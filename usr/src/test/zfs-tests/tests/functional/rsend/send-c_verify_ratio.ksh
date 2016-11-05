#!/usr/bin/ksh

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

#
# Copyright (c) 2015 by Delphix. All rights reserved.
#

. $STF_SUITE/tests/functional/rsend/rsend.kshlib
. $STF_SUITE/include/properties.shlib

#
# Description:
# Verify that the amount of data in a send -c stream matches compressratio.
#
# Strategy:
# 1. For random compression types, and compressible / incompressible data:
# 2. Create a snap with data
# 3. Compare the size of the stream with the data on the dataset, adjusted
#    by compressratio for normal send, and compared to used for send -c.
#

verify_runnable "both"

log_onexit cleanup_pool $POOL2

typeset sendfs=$POOL2/$FS
typeset megs=128

for prop in $(get_rand_compress_any 6); do
	for compressible in 'yes' 'no'; do
		log_must $ZFS create -o compress=$prop $sendfs

		if [[ $compressible = 'yes' ]]; then
			write_compressible $(get_prop mountpoint $sendfs) \
			    ${megs}m
		else
			typeset file="$(get_prop mountpoint $sendfs)/ddfile"
			log_must $DD if=/dev/urandom of=$file bs=1024k \
			    count=$megs
		fi

		log_must $ZFS snapshot $sendfs@snap

		# Calculate the sizes and verify the compression ratio.
		log_must eval "$ZFS send $sendfs@snap >$BACKDIR/uncompressed"
		verify_stream_size $BACKDIR/uncompressed $sendfs

		log_must eval "$ZFS send -c $sendfs@snap >$BACKDIR/compressed"
		verify_stream_size $BACKDIR/compressed $sendfs

		log_must $RM $BACKDIR/uncompressed $BACKDIR/compressed
		log_must $ZFS destroy -r $sendfs
	done
done

log_pass "Verify send -c streams are compressed"
