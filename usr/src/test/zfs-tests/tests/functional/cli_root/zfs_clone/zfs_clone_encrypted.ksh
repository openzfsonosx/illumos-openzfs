#!/bin/ksh -p
#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#

#
# Copyright (c) 2017 Datto, Inc. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zfs_load-key/zfs_load-key_common.kshlib

#
# DESCRIPTION:
# 'zfs clone' should create encrypted clones of encrypted datasets
#
# STRATEGY:
# 1. Create an encrypted dataset
# 2. Create a snapshot of the dataset
# 3. Attempt to clone the snapshot as an unencrypted dataset
# 4. Attempt to clone the snapshot as an encryption root without a new key
# 5. Attempt to clone the snapshot as an encryption root with a new key
# 6. Attempt to clone the snapshot as a encrypted child dataset
# 7. Unmount all datasets and unload their keys
# 8. Attempt to load each dataset's key
# 9. Verify each dataset's key is loaded
# 10. Attempt to mount each dataset
#

verify_runnable "both"

function cleanup
{
	datasetexists $TESTPOOL/$TESTFS2 && \
		log_must $ZFS destroy $TESTPOOL/$TESTFS2
	datasetexists $TESTPOOL/$TESTFS1 && \
		log_must $ZFS destroy -r $TESTPOOL/$TESTFS1
}
log_onexit cleanup

log_assert "'zfs clone' should create encrypted clones of encrypted datasets"

log_must eval "$ECHO $PASSPHRASE | $ZFS create -o encryption=on" \
	"-o keyformat=passphrase -o keylocation=prompt $TESTPOOL/$TESTFS1"
log_must $ZFS snapshot $TESTPOOL/$TESTFS1@now

log_mustnot $ZFS clone -o encryption=off $TESTPOOL/$TESTFS1@now \
	$TESTPOOL/$TESTFS2
log_mustnot $ZFS clone $TESTPOOL/$TESTFS1@now $TESTPOOL/$TESTFS2
log_must eval "$ECHO $PASSPHRASE1 | $ZFS clone -o keyformat=passphrase" \
	"$TESTPOOL/$TESTFS1@now $TESTPOOL/$TESTFS2"
log_must $ZFS clone $TESTPOOL/$TESTFS1@now $TESTPOOL/$TESTFS1/child

log_must $ZFS unmount $TESTPOOL/$TESTFS1
log_must $ZFS unmount $TESTPOOL/$TESTFS2
log_must $ZFS unload-key -a

log_must eval "$ECHO $PASSPHRASE | $ZFS load-key $TESTPOOL/$TESTFS1"
log_must eval "$ECHO $PASSPHRASE1 | $ZFS load-key $TESTPOOL/$TESTFS2"

log_must key_available $TESTPOOL/$TESTFS1
log_must key_available $TESTPOOL/$TESTFS1/child
log_must key_available $TESTPOOL/$TESTFS2

log_must $ZFS mount $TESTPOOL/$TESTFS1
log_must $ZFS mount $TESTPOOL/$TESTFS1/child
log_must $ZFS mount $TESTPOOL/$TESTFS2

log_pass "'zfs clone' creates encrypted clones of encrypted datasets"
