/*
 *  debug.c - Debugging utilities
 *
 *  nspluginwrapper (C) 2005-2008 Gwenole Beauchesne
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
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

#include "debug.h"


static int g_debug_level = -1;

void npw_dprintf(const char *format, ...)
{
  if (g_debug_level < 0) {
	g_debug_level = 0;
	const char *debug_str = getenv("NPW_DEBUG");
	if (debug_str) {
	  errno = 0;
	  long v = strtol(debug_str, NULL, 10);
	  if ((v != LONG_MIN && v != LONG_MAX) || errno != ERANGE)
		g_debug_level = v;
	}
  }

  if (g_debug_level > 0) {
	va_list args;
	va_start(args, format);
	npw_vprintf(format, args);
	va_end(args);
  }
}


static FILE *g_log_file = NULL;

static FILE *npw_log_file(void)
{
  if (g_log_file == NULL) {
	const char *log_file = getenv("NPW_LOG");
	if (log_file) {
	  const char *mode = "w";
#ifdef BUILD_VIEWER
	  /* the wrapper plugin has the responsability to create the file,
		 thus the viewer is only opening it for appending data.  */
	  mode = "a";
#endif
	  g_log_file = fopen(log_file, mode);
	}
	if (log_file == NULL)
	  g_log_file = stderr;
  }
  if (g_log_file != stderr)
	fseek(g_log_file, 0, SEEK_END);
  return g_log_file;
}

static void __attribute__((destructor)) log_file_sentinel(void)
{
  if (g_log_file && g_log_file != stderr) {
	fclose(g_log_file);
	g_log_file = stderr;
  }
}


void npw_vprintf(const char *format, va_list args)
{
  FILE *log_file = npw_log_file();
  fprintf(log_file, "*** NSPlugin %s *** ", NPW_COMPONENT_NAME);
  vfprintf(log_file, format, args);
  fflush(log_file);
}

void npw_printf(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  npw_vprintf(format, args);
  va_end(args);
}
