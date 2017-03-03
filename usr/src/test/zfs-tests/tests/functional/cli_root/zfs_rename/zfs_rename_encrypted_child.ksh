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
# 'zfs rename' should not move an encrypted child dataset outside of its
# encryption root.
#
# STRATEGY:
# 1. Create two encryption roots and a child and grandchild of the first
#    dataset
# 2. Attempt to rename the grandchild under an unencrypted parent
# 3. Attempt to rename the grandchild under a different encrypted parent
# 4. Attempt to rename the grandchild under the current parent
# 5. Attempt to rename the grandchild to a child
#

verify_runnable "both"

function cleanup
{
	datasetexists $TESTPOOL/$TESTFS2 && \
		log_must $ZFS destroy -r $TESTPOOL/$TESTFS2
	datasetexists $TESTPOOL/$TESTFS3 && \
		log_must $ZFS destroy -r $TESTPOOL/$TESTFS3
}
log_onexit cleanup

log_assert "'zfs rename' should not move an encrypted child outside of its" \
	"encryption root"

log_must eval "$ECHO $PASSPHRASE | $ZFS create -o encryption=on" \
	"-o keyformat=passphrase -o keylocation=prompt $TESTPOOL/$TESTFS2"
log_must $ZFS create $TESTPOOL/$TESTFS2/child
log_must $ZFS create $TESTPOOL/$TESTFS2/child/grandchild
log_must eval "$ECHO $PASSPHRASE1 | $ZFS create -o encryption=on" \
	"-o keyformat=passphrase -o keylocation=prompt $TESTPOOL/$TESTFS3"

log_mustnot $ZFS rename $TESTPOOL/$TESTFS2/child/grandchild \
	$TESTPOOL/grandchild
log_mustnot $ZFS rename $TESTPOOL/$TESTFS2/child/grandchild \
	$TESTPOOL/$TESTFS3/grandchild
log_must $ZFS rename $TESTPOOL/$TESTFS2/child/grandchild \
	$TESTPOOL/$TESTFS2/child/grandchild2
log_must $ZFS rename $TESTPOOL/$TESTFS2/child/grandchild2 \
	$TESTPOOL/$TESTFS2/grandchild2

log_pass "'zfs rename' does not move an encrypted child outside of its" \
	"encryption root"
