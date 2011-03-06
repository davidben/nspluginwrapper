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

# Set a new LD_LIBRARY_PATH that is TARGET specific
export LD_LIBRARY_PATH=$NPW_VIEWER_DIR

case $ARCH in
i?86)
    ARCH=i386
    ;;
amd64)
    ARCH=x86_64
    ;;
esac

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

# Don't wrap host plugins
case " $@ " in
*" --test "*|*" -t "*)
    if test "$TARGET_OS" = "$OS" -a "$TARGET_ARCH" = "$ARCH"; then
	exit 20 # EXIT_VIEWER_NATIVE
    fi
    ;;
esac

# Expand PATH for RealPlayer package on NetBSD (realplay)
if test "$OS" = "NetBSD"; then
    REALPLAYER_HOME="/usr/pkg/lib/RealPlayer"
    if test -x "$REALPLAYER_HOME/realplay"; then
	export PATH=$PATH:$REALPLAYER_HOME
    fi
fi

# Use sound wrappers wherever possible (Flash 9 plugin)
case " $@ " in
*" --test "*|*" -t "*)
    # do nothing
    ;;
*)
    # XXX: detect QEMU target soundwrapper differently
    case "$LOADER" in
    *linux32)
	if test "$OS" = "Linux"; then
	    soundwrapper=`which soundwrapper`
	    if test -x "$soundwrapper"; then
		LOADER="$LOADER $soundwrapper"
	    elif ps aux | grep artsd | grep -vq grep; then
		soundwrapper=`which artsdsp`
		if test -x "$soundwrapper"; then
		    LOADER="$LOADER $soundwrapper"
		fi
	    fi
	fi
	;;
    esac
    ;;
esac

exec $LOADER $NPW_VIEWER_DIR/npviewer.bin ${1+"$@"}
