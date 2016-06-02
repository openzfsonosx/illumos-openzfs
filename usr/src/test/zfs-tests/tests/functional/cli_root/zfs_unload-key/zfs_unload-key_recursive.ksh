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
# 'zfs unload-key -r' should recursively unload keys.
#
# STRATEGY:
# 1. Create a parent encrypted dataset
# 3. Create a sibling encrypted dataset
# 2. Create a child dataset as an encryption root
# 3. Unmount all datasets
# 4. Attempt to unload all dataset keys under parent
# 5. Verify parent and child have their keys unloaded
# 6. Verify sibling has its key loaded
# 7. Attempt to mount all datasets
#

verify_runnable "both"

function cleanup
{
	datasetexists $TESTPOOL/$TESTFS1 && \
		log_must $ZFS destroy -r $TESTPOOL/$TESTFS1
}
log_onexit cleanup

log_assert "'zfs unload-key -r' should recursively unload keys"

log_must eval "$ECHO $PASSPHRASE > /$TESTPOOL/pkey"
log_must $ZFS create -o encryption=on -o keyformat=passphrase \
	-o keylocation=file:///$TESTPOOL/pkey $TESTPOOL/$TESTFS1
log_must $ZFS create -o keyformat=passphrase \
	-o keylocation=file:///$TESTPOOL/pkey $TESTPOOL/$TESTFS1/child
log_must eval "$ECHO $PASSPHRASE1 | $ZFS create -o encryption=on" \
	"-o keyformat=passphrase -o keylocation=prompt $TESTPOOL/$TESTFS2"

log_must $ZFS unmount $TESTPOOL/$TESTFS1
log_must $ZFS unmount $TESTPOOL/$TESTFS2

log_must $ZFS unload-key -r $TESTPOOL/$TESTFS1

log_must key_unavailable $TESTPOOL/$TESTFS1
log_must key_unavailable $TESTPOOL/$TESTFS1/child

log_must key_available $TESTPOOL/$TESTFS2

log_mustnot $ZFS mount $TESTPOOL/$TESTFS1
log_mustnot $ZFS mount $TESTPOOL/$TESTFS1/child
log_must $ZFS mount $TESTPOOL/$TESTFS2

log_pass "'zfs unload-key -r' recursively unloads keys"
