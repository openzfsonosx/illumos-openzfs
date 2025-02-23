#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright (c) 2018, Joyent, Inc.

LIBRARY	=	libdhcpagent.a
VERS =		.1
OBJECTS	=	dhcp_hostconf.o dhcpagent_ipc.o dhcpagent_util.o dhcp_stable.o

include ../../Makefile.lib

# install this library in the root filesystem
include ../../Makefile.rootfs

LIBS =		$(DYNLIB) $(LINTLIB)

LDLIBS +=	-lc -lsocket -ldhcputil -luuid -ldlpi -lcontract

SRCDIR =	../common
$(LINTLIB) :=	SRCS = $(SRCDIR)/$(LINTSRC)

CFLAGS += 	$(CCVERBOSE)
CERRWARN +=	-_gcc=-Wno-type-limits

# needs work
SMOFF += allocating_enough_data

.KEEP_STATE:

all: $(LIBS)

lint: lintcheck

include ../../Makefile.targ
