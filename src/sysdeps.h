/*
 *  sysdeps.h - System dependent definitions
 *
 *  nspluginwrapper (C) 2005-2006 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#if !defined(__STDC__) || (__STDC_VERSION__ < 199901L)
#error "Your compiler is not ISO. Get a real one."
#endif

#include "config.h"

/* XXX detect popen() et al. at configure time */
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <limits.h>

// NSPluginWrapper paths and identification strings
#define NPW_CONNECTION_PATH "/org/wrapper/NSPlugins"
#define NPW_VIEWER_BASE "npviewer"
#define NPW_VIEWER NPW_VIEWER_BASE
#define NPW_WRAPPER_BASE "npwrapper"
#define NPW_WRAPPER NPW_WRAPPER_BASE ".so"
#define NPW_OLD_DEFAULT_PLUGIN_PATH NPW_LIBDIR "/" HOST_ARCH "/" NPW_WRAPPER
#define NPW_DEFAULT_PLUGIN_PATH NPW_LIBDIR "/" HOST_ARCH "/" HOST_OS "/" NPW_WRAPPER
#define NPW_PLUGIN_IDENT "NPW:" NPW_VERSION
#define NPW_PLUGIN_IDENT_SIZE 32
typedef struct __attribute__((packed)) {
  char ident[NPW_PLUGIN_IDENT_SIZE];
  char path[PATH_MAX];
  time_t mtime;
  char target_arch[65];
  char target_os[65];
} NPW_PluginInfo;

#if defined(BUILD_XPCOM)
#define NPW_COMPONENT_NAME "XPCOM  "
#elif defined(BUILD_WRAPPER)
#define NPW_COMPONENT_NAME "Wrapper"
#elif defined(BUILD_VIEWER)
#define NPW_COMPONENT_NAME "Viewer "
#endif

// Boolean types
#ifndef __cplusplus
#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#else
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#define bool _Bool
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#endif
#endif
#endif

#endif /* SYSDEPS_H */
