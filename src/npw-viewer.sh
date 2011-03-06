#!/bin/sh
#
#  nsplugin viewer wrapper script (C) 2005-2006 Gwenole Beauchesne
#
OS="`uname -s`"
ARCH="`uname -m`"
NPW_LIBDIR="%NPW_LIBDIR%"

if test -z "$TARGET_OS"; then
    echo "*** NSPlugin Viewer *** error, TARGET_OS not initialized"
    exit 1
fi

if test -z "$TARGET_ARCH"; then
    echo "*** NSPlugin Viewer *** error, TARGET_ARCH not initialized"
    exit 1
fi

NPW_VIEWER_DIR=$NPW_LIBDIR/$TARGET_ARCH/$TARGET_OS

case $ARCH in
i?86)
    ARCH=i386
    ;;
amd64)
    ARCH=x86_64
    ;;
esac

if test -n "$LD_LIBRARY_PATH"; then
    LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$NPW_VIEWER_DIR
else
    LD_LIBRARY_PATH=$NPW_VIEWER_DIR
fi
export LD_LIBRARY_PATH

if test "$ARCH" != "$TARGET_ARCH"; then
    case $TARGET_ARCH in
    i386)
	if test "$ARCH" = "x86_64"; then
	    case "$OS" in
	    Linux)
		LOADER=`which linux32`
		;;
	    FreeBSD | NetBSD)
		# XXX check that COMPAT_LINUX is enabled or fail otherwise
		LOADER="none"
	        ;;
	    esac
	elif test "$ARCH" = "ia64"; then
	    # XXX check that IA-32 EL or HW emulator is enabled or fail
	    # otherwise (use QEMU?)
	    LOADER="none"
	else
	    LOADER=`which qemu-i386`
	fi
	;;
    ppc)
	if test "$ARCH" = "ppc64"; then
	    case "$OS" in
	    Linux)
		LOADER=`which linux32`
	        ;;
	    esac
	else
	    LOADER=`which qemu-ppc`
	fi
	;;
    esac
    if test "$LOADER" = "none"; then
	unset LOADER
    elif test -z "$LOADER" -o ! -x "$LOADER"; then
	echo "*** NSPlugin Viewer *** preloader not found"
	exit 1
    fi
fi

# XXX qemu doesn't support NPTL
case " $LOADER " in
*qemu*)
    if test -f "/etc/mandrake-release"; then
	# glibc --enable-kernel (as of 2.3.4-8mdk)
	case $ARCH in
	ppc64)	KERNEL_VERSION=2.4.21;;
	ia64)	KERNEL_VERSION=2.4.0;;
	x86_64)	KERNEL_VERSION=2.4.0;;
	*)	KERNEL_VERSION=2.2.5;;
	esac
    else
	# this generally brings floating-stacks
	KERNEL_VERSION=2.4.1
    fi
    ;;
*none*)
    unset LOADER
    ;;
esac
if test -n "$KERNEL_VERSION"; then
    export LD_ASSUME_KERNEL=$KERNEL_VERSION
fi

# Expand PATH for RealPlayer package on NetBSD (realplay)
if test "$OS" = "NetBSD"; then
    REALPLAYER_HOME="/usr/pkg/lib/RealPlayer"
    if test -x "$REALPLAYER_HOME/realplay"; then
	export PATH=$PATH:$REALPLAYER_HOME
    fi
fi

exec $LOADER $NPW_VIEWER_DIR/npviewer.bin ${1+"$@"}
