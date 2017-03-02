/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2017, Datto, Inc. All rights reserved.
 */

#ifndef	_SYS_ZIO_CRYPT_H
#define	_SYS_ZIO_CRYPT_H

#include <sys/dmu.h>
#include <sys/refcount.h>
#include <sys/crypto/api.h>
#include <sys/nvpair.h>
#include <sys/avl.h>
#include <sys/zio.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* forward declarations */
struct zbookmark_phys;

#define	WRAPPING_KEY_LEN 32
#define	WRAPPING_IV_LEN DATA_IV_LEN
#define	WRAPPING_MAC_LEN 16

#define	SHA_256_DIGEST_LEN 32
#define	HMAC_SHA256_KEYLEN 32

#define	L2ARC_DEFAULT_CRYPT ZIO_CRYPT_AES_256_CCM

#define	ZIO_NO_ENCRYPTION_NEEDED -1

/*
 * After encrypting many blocks with the same key we may start to run up
 * against the theoretical limits of how much data can securely be encrypted
 * with a single key using the supported encryption modes. The most obvious
 * limitation is that our risk of generating 2 equivalent 96 bit IVs increases
 * the more IVs we generate (which both GCM and CCM modes strictly forbid).
 * This risk actually grows surprisingly quickly over time according to the
 * Birthday Problem. With a total IV space of 2^(96 bits), and assuming we have
 * generated n IVs with a cryptographically secure RNG, the approximate
 * probability p(n) of a collision is given as:
 *
 * p(n) ~= e^(-n(n-1)/(2*(2^96)))
 *
 * [http://www.math.cornell.edu/~mec/2008-2009/TianyiZheng/Birthday.html]
 *
 * Assuming that we want to ensure that p(n) never goes over 1 / 1 trillion
 * we must not write more than 398065730 blocks with the same encryption key,
 * which is significantly less than the zettabyte of data that ZFS claims to
 * be able to store. To counteract this, we rotate our keys after 400000000
 * blocks have been written by generating a new random 64 bit salt for our
 * HKDF encryption key generation function.
 */
#define	ZIO_CRYPT_MAX_SALT_USAGE 400000000

#define	BITS_TO_BYTES(x)	((x + NBBY - 1) / NBBY)
#define	BYTES_TO_BITS(x)	(x * NBBY)

typedef enum zio_crypt_type {
	ZC_TYPE_NONE = 0,
	ZC_TYPE_CCM,
	ZC_TYPE_GCM
} zio_crypt_type_t;

/* table of supported crypto algorithms, modes and keylengths. */
typedef struct zio_crypt_info {
	/* mechanism name, needed by ICP */
	crypto_mech_name_t ci_mechname;

	/* cipher mode type (GCM, CCM) */
	zio_crypt_type_t ci_crypt_type;

	/* length of the encryption key */
	size_t ci_keylen;

	/* human-readable name of the encryption alforithm */
	char *ci_name;
} zio_crypt_info_t;

extern zio_crypt_info_t zio_crypt_table[ZIO_CRYPT_FUNCTIONS];

/* ZAP entry keys for DSL Encryption Keys stored on disk */
#define	DSL_CRYPTO_KEY_CRYPTO_SUITE "DSL_CRYPTO_SUITE"
#define	DSL_CRYPTO_KEY_IV "DSL_CRYPTO_IV"
#define	DSL_CRYPTO_KEY_MAC "DSL_CRYPTO_MAC"
#define	DSL_CRYPTO_KEY_MASTER_KEY "DSL_CRYPTO_MASTER_KEY"
#define	DSL_CRYPTO_KEY_HMAC_KEY "DSL_CRYPTO_HMAC_KEY"

/* in memory representation of an unwrapped key that is loaded into memory */
typedef struct zio_crypt_key {
	/* encryption algorithm */
	uint64_t zk_crypt;

	/* buffer for master key */
	uint8_t zk_master_keydata[MAX_MASTER_KEY_LEN];

	/* buffer for hmac key */
	uint8_t zk_hmac_keydata[HMAC_SHA256_KEYLEN];

	/* buffer for currrent encryption key derived from master key */
	uint8_t zk_current_keydata[MAX_MASTER_KEY_LEN];

	/* current 64 bit salt for deriving an encryption key */
	uint8_t zk_salt[DATA_SALT_LEN];

	/* count of how many times the current salt has been used */
	uint64_t zk_salt_count;

	/* illumos crypto api current encryption key */
	crypto_key_t zk_current_key;

	/* template of current encryption key for illumos crypto api */
	crypto_ctx_template_t zk_current_tmpl;

	/* illumos crypto api current hmac key */
	crypto_key_t zk_hmac_key;

	/* template of hmac key for illumos crypto api */
	crypto_ctx_template_t zk_hmac_tmpl;

	/* lock for changing the salt and dependant values */
	krwlock_t zk_salt_lock;
} zio_crypt_key_t;

void zio_crypt_key_destroy(zio_crypt_key_t *key);
int zio_crypt_key_init(uint64_t crypt, zio_crypt_key_t *key);
int zio_crypt_key_get_salt(zio_crypt_key_t *key, uint8_t *salt_out);

int zio_crypt_key_wrap(crypto_key_t *cwkey, zio_crypt_key_t *key, uint8_t *iv,
    uint8_t *mac, uint8_t *keydata_out, uint8_t *hmac_keydata_out);
int zio_crypt_key_unwrap(crypto_key_t *cwkey, uint64_t crypt, uint8_t *keydata,
    uint8_t *hmac_keydata, uint8_t *iv, uint8_t *mac, zio_crypt_key_t *key);
int zio_crypt_generate_iv(uint8_t *ivbuf);
int zio_crypt_generate_iv_salt_dedup(zio_crypt_key_t *key, uint8_t *data,
    uint_t datalen, uint8_t *ivbuf, uint8_t *salt);

void zio_crypt_encode_params_bp(blkptr_t *bp, uint8_t *salt, uint8_t *iv);
void zio_crypt_decode_params_bp(const blkptr_t *bp, uint8_t *salt, uint8_t *iv);
void zio_crypt_encode_mac_bp(blkptr_t *bp, uint8_t *mac);
void zio_crypt_decode_mac_bp(const blkptr_t *bp, uint8_t *mac);
void zio_crypt_encode_mac_zil(void *data, uint8_t *mac);
void zio_crypt_decode_mac_zil(const void *data, uint8_t *mac);
void zio_crypt_copy_dnode_bonus(uint8_t *src, uint8_t *dst, uint_t datalen);

int zio_do_crypt_data(boolean_t encrypt, zio_crypt_key_t *key, uint8_t *salt,
    dmu_object_type_t ot, uint8_t *iv, uint8_t *mac, uint_t datalen,
    uint8_t *plainbuf, uint8_t *cipherbuf);


#ifdef	__cplusplus
}
#endif

#endif /* _SYS_ZIO_CRYPT_H */
