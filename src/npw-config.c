/*
 *  npw-config.c - nspluginwrapper configuration tool
 *
 *  nspluginwrapper (C) 2005-2009 Gwenole Beauchesne
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
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <dirent.h>

#include <glib.h>

static bool g_auto = false;
static bool g_verbose = false;
static bool g_allow_native = false;
static const char NPW_CONFIG[] = "nspluginwrapper";

static void error(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  fprintf(stderr, "%s: ", NPW_CONFIG);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
  exit(1);
}

static const char *strnstr(const char *str, int len, const char *substr)
{
  const char *match = strstr(str, substr);
  if (len > 0 && (match + strlen(substr) > str + len))
	match = NULL;
  return match;
}

typedef struct {
  const char *name;
  const char *value;
} Var;

static int strexpand(char *dst, int dstlen, const char *src, int srclen, const Var *vars)
{
  if (dst == NULL || dstlen < 1 || src == NULL)
	return -1;

  if (srclen <= 0)
	srclen = strlen(src);

  int n = 0;
  for (int i = 0; i < srclen; i++) {
	char ch = src[i];
	if (ch != '%') {
	  dst[n++] = ch;
	  if (n >= dstlen - 1)
		return -1;
	}
	else {
	  char var[16];
	  const char *str = &src[i + 1];
	  const char *end = strchr(str, '%');
	  if (end == NULL)
		error("unterminated var '%s'", str);

	  int len = end - str;
	  if (len >= sizeof(var) - 1) {
		len = sizeof(var) - 1;
		memcpy(var, str, len);
		var[len] = '\0';
		error("unsupported var '%s...'", var);
	  }
	  memcpy(var, str, len);
	  var[len] = '\0';

	  str = NULL;
	  for (int j = 0; vars[j].name != NULL; j++) {
		if (strcmp(vars[j].name, var) == 0) {
		  str = vars[j].value;
		  break;
		}
	  }
	  if (str == NULL)
		error("could not expand var '%s'", var);
	  i += len + 1;
	  len = strlen(str);
	  memcpy(&dst[n], str, len);
	  n += len;
	}
  }
  dst[n] = '\0';
  return 0;
}

static const char *get_system_mozilla_plugin_dir(void)
{
  static const char default_dir[] = LIBDIR "/mozilla/plugins";
  static const char *dir = NULL;

  if (dir == NULL) {
	const char **dirs = NULL;

#if defined(__FreeBSD__)
	{
	  static const char *freebsd_dirs[] = {
		"/usr/X11R6/" LIB "/browser_plugins",
		"/usr/X11R6/" LIB "/firefox/plugins",
	  };
	  dirs = freebsd_dirs;
	}
#elif defined(__DragonFly__)
	{
	  static const char *dragonfly_dirs[] = {
		"/usr/pkg/" LIB "/mozilla/plugins",
		"/usr/pkg/" LIB "/firefox/plugins",
	  };
	  dirs = dragonfly_dirs;
	}
#elif defined(__NetBSD__)
	{
	  static const char *netbsd_dirs[] = {
		"/usr/pkg/" LIB "/mozilla/plugins",
		"/usr/pkg/" LIB "/firefox/plugins",
	  };
	  dirs = netbsd_dirs;
	}
#elif defined(__sun__)
	{
	  static const char *solaris_dirs[] = {
		LIBDIR "/firefox/plugins",
		"/usr/sfw/" LIB "/mozilla/plugins",
	  };
	  dirs = solaris_dirs;
	}
#elif defined(__linux__)
	if (access("/etc/SuSE-release", F_OK) == 0) {
	  static const char *suse_dirs[] = {
		LIBDIR "/browser-plugins",
		LIBDIR "/firefox/plugins",
		LIBDIR "/seamonkey/plugins",
		"/opt/MozillaFirefox/" LIB "/plugins",
	  };
	  dirs = suse_dirs;
	}
	else if (access("/etc/debian_version", F_OK) == 0) {
	  static const char *debian_dirs[] = {
		"/usr/lib/mozilla/plugins",				// XXX how unfortunate
	  };
	  dirs = debian_dirs;
	}
	else if (access("/etc/gentoo-release", F_OK) == 0) {
	  static const char *gentoo_dirs[] = {
		LIBDIR "/nsbrowser/plugins",
	  };
	  dirs = gentoo_dirs;
	}
#endif

	if (dirs) {
	  while ((dir = *dirs++) != NULL) {
		if (access(dir, F_OK) == 0)
		  break;
	  }
	}

	if (dir == NULL)
	  dir = default_dir;
  }

  return dir;
}

static const char *get_user_mozilla_plugin_dir(void)
{
  const char *home;
  static char *plugin_path = NULL;

  if (plugin_path != NULL)
	return plugin_path;

  if ((home = g_get_home_dir()) == NULL)
	return NULL;

  return (plugin_path = g_build_filename(home, ".mozilla", "plugins", NULL));
}

static const char **get_mozilla_plugin_dirs(void)
{
  static const char *default_dirs[] = {
	"/usr/lib/mozilla/plugins",
	"/usr/lib32/mozilla/plugins",				// XXX how unfortunate
	"/usr/lib64/mozilla/plugins",
	"/usr/lib/browser-plugins",
	"/usr/lib64/browser-plugins",
	"/usr/lib/firefox/plugins",
	"/usr/lib64/firefox/plugins",
	"/usr/lib/seamonkey/plugins",
	"/usr/lib64/seamonkey/plugins",
	"/opt/MozillaFirefox/lib/plugins",
	"/opt/MozillaFirefox/lib64/plugins",
	"/usr/lib/nsbrowser/plugins",
	"/usr/lib32/nsbrowser/plugins",				// XXX how unfortunate
	"/usr/lib64/nsbrowser/plugins",
#if defined(__FreeBSD__)
	"/usr/X11R6/lib/browser_plugins",
	"/usr/X11R6/lib/firefox/plugins",
	"/usr/X11R6/lib/linux-mozilla/plugins",
	"/usr/local/lib/npapi/linux-flashplugin",
	"/usr/X11R6/Adobe/Acrobat7.0/ENU/Browser/intellinux",
#endif
#if defined(__DragonFly__)
	"/usr/pkg/lib/netscape/plugins",
	"/usr/pkg/lib/firefox/plugins",
	"/usr/pkg/lib/RealPlayer/mozilla",
	"/usr/pkg/Acrobat5/Browsers/intellinux",
	"/usr/pkg/Acrobat7/Browser/intellinux",
#endif
#if defined(__NetBSD__)
	"/usr/pkg/lib/netscape/plugins",
	"/usr/pkg/lib/firefox/plugins",
	"/usr/pkg/lib/RealPlayer/mozilla",
	"/usr/pkg/Acrobat5/Browsers/intellinux",
	"/usr/pkg/Acrobat7/Browser/intellinux",
#endif
#if defined(__sun__)
	"/usr/sfw/lib/mozilla/plugins",
#endif
  };

  const int n_default_dirs = (sizeof(default_dirs) / sizeof(default_dirs[0]));
  const char **dirs = malloc((n_default_dirs + 2) * sizeof(dirs[0]));
  int i, j;
  for (i = 0, j = 0; i < n_default_dirs; i++) {
	const char *dir = default_dirs[i];
	if (dir && access(dir, F_OK) == 0)
	  dirs[j++] = dir;
  }
  dirs[j++] = get_user_mozilla_plugin_dir();
  dirs[j] = NULL;
  return dirs;
}

/* ELF decoder derived from QEMU code */

#undef bswap_16
#define bswap_16(x) \
	((((x) >> 8) & 0xff) | (((x) & 0xff) << 8))

#undef bswap_32
#define bswap_32(x) \
     ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) | \
      (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))

#undef bswap_64
#define bswap_64(x) \
     (              \
      (((x) & 0xff00000000000000) >> 56)  | (((x) & 0x00ff000000000000) >> 40) | \
      (((x) & 0x00000000000000ff) << 56)  | (((x) & 0x000000000000ff00) << 40) | \
      (((x) & 0x0000ff0000000000) >> 24)  | (((x) & 0x000000ff00000000) >>  8) | \
      (((x) & 0x0000000000ff0000) << 24)  | (((x) & 0x00000000ff000000) <<  8) \
     )

/* Base structure - used to distinguish between 32/64 bit version */
typedef struct
{
  unsigned char	e_ident[EI_NIDENT];	/* Magic number and other info */
} Elf_hdr_base;

void *load_data(int fd, long offset, unsigned int size)
{
  char *data = (char *)malloc(size);
  if (!data)
	return NULL;

  lseek(fd, offset, SEEK_SET);
  if (read(fd, data, size) != size) {
	free(data);
	return NULL;
  }

  return data;
}

static bool is_little_endian(void)
{
  union { uint32_t i; uint8_t b[4]; } x;
  x.i = 0x01020304;
  return x.b[0] == 0x04;
}

#define ELF_CLASS ELFCLASS32
#include "npw-config-template.h"

#define ELF_CLASS ELFCLASS64
#include "npw-config-template.h"

static bool is_plugin_fd(int fd, NPW_PluginInfo *out_plugin_info)
{
  Elf_hdr_base ehdr;
	
  if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
	return false;

  if (ehdr.e_ident[EI_MAG0] != ELFMAG0
	  || ehdr.e_ident[EI_MAG1] != ELFMAG1
	  || ehdr.e_ident[EI_MAG2] != ELFMAG2
	  || ehdr.e_ident[EI_MAG3] != ELFMAG3)
	return false;

  lseek(fd, 0, SEEK_SET);

  switch (ehdr.e_ident[EI_CLASS]) {
  case ELFCLASS32: return is_elf_plugin_fd_32(fd, out_plugin_info);
  case ELFCLASS64: return is_elf_plugin_fd_64(fd, out_plugin_info);
  }
  return false;
}

enum {
  EXIT_VIEWER_NOT_FOUND	= -2,
  EXIT_VIEWER_ERROR		= -1,
  EXIT_VIEWER_OK		= 0,
  EXIT_VIEWER_NATIVE	= 20
};

static bool is_plugin_viewer_ok(const char *viewer_path, const char *filename)
{
  int pid = fork();
  if (pid < 0)
	return false;
  if (pid == 0) {
	if (!g_verbose) {
	  // don't spit out errors in non-verbose mode, we only need
	  // to know whether there is a valid viewer or not
	  if (freopen("/dev/null", "w", stderr) == NULL) {
		fprintf(stderr, "failed to silence stderr\n");
	  }
	}
	execl(viewer_path, NPW_VIEWER, "--test", "--plugin", filename, NULL);
	exit(1);
  }
  else {
	int status;
	while (waitpid(pid, &status, 0) != pid)
	  ;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
	  return true;
  }
  return false;
}

static int detect_plugin_viewer(const char *filename, NPW_PluginInfo *out_plugin_info)
{
  static const char *target_arch_table[] = {
	NULL,
	"i386",
	NULL
  };
  const int target_arch_table_size = sizeof(target_arch_table) / sizeof(target_arch_table[0]);

  if (out_plugin_info && out_plugin_info->target_arch[0] != '\0')
	target_arch_table[0] = out_plugin_info->target_arch;
  else
	target_arch_table[0] = NULL;

  static const char *target_os_table[] = {
	NULL,
	"linux",
	NULL
  };
  const int target_os_table_size = sizeof(target_os_table) / sizeof(target_os_table[0]);

  if (out_plugin_info && out_plugin_info->target_os[0] != '\0')
	target_os_table[0] = out_plugin_info->target_os;
  else
	target_os_table[0] = NULL;

  // don't wrap plugins for host OS/ARCH
  if (!g_allow_native
	  && out_plugin_info
	  && out_plugin_info->target_arch && strcmp(out_plugin_info->target_arch, HOST_ARCH) == 0
	  && out_plugin_info->target_os && strcmp(out_plugin_info->target_os, HOST_OS) == 0)
	return EXIT_VIEWER_NATIVE;

  enum { VAR_ARCH, VAR_OS, VAR_COUNT };
  Var vars[VAR_COUNT+1];
  vars[VAR_ARCH].name = "ARCH";
  vars[VAR_OS].name = "OS";
  vars[VAR_COUNT].name = NULL;
  vars[VAR_COUNT].value = NULL;

  char viewer_path[sizeof(out_plugin_info->viewer_path)];
  const int viewer_path_len = sizeof(viewer_path) - strlen(NPW_VIEWER) - 1;
  const char viewer_paths[] = NPW_VIEWER_PATHS;
  const char *viewer_path_spec_end, *viewer_path_spec = viewer_paths;
  do {
	int len;
	if ((viewer_path_spec_end = strchr(viewer_path_spec, ':')) != NULL)
	  len = viewer_path_spec_end - viewer_path_spec;
	else
	  len = strchr(viewer_path_spec, '\0') - viewer_path_spec;

	for (int i = 0; i < target_arch_table_size; i++) {
	  const char *target_arch = target_arch_table[i];
	  if (target_arch == NULL)
		continue;
	  vars[VAR_ARCH].value = target_arch;

	  for (int j = 0; j < target_os_table_size; j++) {
		const char *target_os = target_os_table[j];
		if (target_os == NULL)
		  continue;
		vars[VAR_OS].value = target_os;

		if (strexpand(viewer_path, viewer_path_len, viewer_path_spec, len, vars) < 0)
		  continue;
		strcat(viewer_path, "/" NPW_VIEWER);
		if (access(viewer_path, F_OK) != 0)
		  continue;

		if (is_plugin_viewer_ok(viewer_path, filename)) {
		  strcpy(out_plugin_info->target_arch, target_arch);
		  strcpy(out_plugin_info->target_os, target_os);
		  strcpy(out_plugin_info->viewer_path, viewer_path);
		  return EXIT_VIEWER_OK;
		}

		if (strnstr(viewer_path_spec, len, "%OS%") == NULL)
		  break; // don't iterate over OS table if there is no "%OS%" pattern
	  }

	  if (strnstr(viewer_path_spec, len, "%ARCH%") == NULL)
		break; // don't iterate over ARCH table if there is no "%ARCH%" pattern
	}

	viewer_path_spec += len + 1;
  } while (viewer_path_spec_end != NULL);

  return EXIT_VIEWER_NOT_FOUND;
}

static bool is_plugin_viewer_available(const char *filename, NPW_PluginInfo *out_plugin_info)
{
  return detect_plugin_viewer(filename, out_plugin_info) == EXIT_VIEWER_OK;
}

static bool is_plugin(const char *filename, NPW_PluginInfo *out_plugin_info)
{
  int fd = open(filename, O_RDONLY);
  if (fd < 0)
	return false;

  bool ret = is_plugin_fd(fd, out_plugin_info);
  close(fd);
  return ret;
}

static bool is_compatible_plugin(const char *filename, NPW_PluginInfo *out_plugin_info)
{
  return is_plugin(filename, out_plugin_info) && is_plugin_viewer_available(filename, out_plugin_info);
}

static bool is_wrapper_plugin_handle(void *handle, NPW_PluginInfo *out_plugin_info)
{
  if (dlsym(handle, "NP_Initialize") == NULL)
	return false;
  if (dlsym(handle, "NP_Shutdown") == NULL)
	return false;
  if (dlsym(handle, "NP_GetMIMEDescription") == NULL)
	return false;
  NPW_PluginInfo *pi;
  if ((pi = (NPW_PluginInfo *)dlsym(handle, "NPW_Plugin")) == NULL)
	return false;
  if (out_plugin_info) {
	int plugin_info_version = 0;
	if (strncmp(pi->ident, "NPW:0.9.90", 10) != 0)
	  plugin_info_version = 1;
	if (strncmp(pi->ident, "NPW:X:", 6) == 0)
	  plugin_info_version = pi->struct_version;
	out_plugin_info->struct_version = plugin_info_version;
	strcpy(out_plugin_info->ident, pi->ident);
	strcpy(out_plugin_info->path, pi->path);
	out_plugin_info->mtime = pi->mtime;
	if (plugin_info_version >= 1) {		// additional members in 0.9.91+
	  strcpy(out_plugin_info->target_arch, pi->target_arch);
	  strcpy(out_plugin_info->target_os, pi->target_os);
	}
	else {
	  out_plugin_info->target_arch[0] = '\0';
	  out_plugin_info->target_os[0] = '\0';
	}
	if (plugin_info_version >= 2)		// additional members in 1.3.0+
	  strcpy(out_plugin_info->viewer_path, pi->viewer_path);
	else
	  out_plugin_info->viewer_path[0] = '\0';
  }
  return true;
}

static bool is_wrapper_plugin(const char *plugin_path, NPW_PluginInfo *out_plugin_info)
{
  void *handle = dlopen(plugin_path, RTLD_LAZY);
  if (handle == NULL)
	return false;

  bool ret = is_wrapper_plugin_handle(handle, out_plugin_info);
  /* Intentionally leak the handle; many libraries crash when unloaded. */
  /* dlclose(handle); */
  return ret;
}

static bool is_master_wrapper_plugin(const char *plugin_path)
{
  static const char *master_plugin_paths[] = {
	NPW_LIBDIR "/" HOST_ARCH "/" NPW_WRAPPER,
	NPW_LIBDIR "/" HOST_ARCH "/" HOST_OS "/" NPW_WRAPPER,
	NPW_DEFAULT_PLUGIN_PATH,
	NULL
  };
  for (int i = 0; master_plugin_paths[i] != NULL; i++) {
	if (strcmp(master_plugin_paths[i], plugin_path) == 0)
	  return true;
  }
  return false;
}

static bool is_wrapper_plugin_0(const char *plugin_path)
{
  NPW_PluginInfo plugin_info;
  return is_wrapper_plugin(plugin_path, &plugin_info)
	&& !is_master_wrapper_plugin(plugin_path);
}

static bool has_system_wide_wrapper_plugin(const char *plugin_path, bool check_ident)
{
  char *plugin_base = strrchr(plugin_path, '/');
  if (plugin_base == NULL)
	return false;
  plugin_base += 1;

  char s_plugin_path[PATH_MAX];
  int n = snprintf(s_plugin_path, sizeof(s_plugin_path), "%s/%s.%s", get_system_mozilla_plugin_dir(), NPW_WRAPPER_BASE, plugin_base);
  if (n < 0 || n >= sizeof(s_plugin_path))
	return false;

  struct stat st;
  if (stat(plugin_path, &st) < 0)
	return false;

  NPW_PluginInfo plugin_info;
  return (is_wrapper_plugin(s_plugin_path, &plugin_info)
		  && strcmp(plugin_info.path, plugin_path) == 0
		  && (check_ident ?
			  strcmp(plugin_info.ident, NPW_PLUGIN_IDENT) == 0 :
			  true)
		  && plugin_info.mtime == st.st_mtime);
}

static bool match_path_prefix(const char *path, const char *prefix)
{
  if (path == NULL || prefix == NULL)
	return false;
  int prefix_len = strlen(prefix);
  return strncmp(path, prefix, prefix_len) == 0 && path[prefix_len] == '/';
}

static bool is_user_home_path(const char *path)
{
  const char *homedir;
  if ((homedir = g_get_home_dir()) == NULL)
	return false;
  return match_path_prefix(path, homedir);
}

static bool is_root_path(const char *path)
{
  struct passwd *pwent = getpwuid(0);
  return pwent && match_path_prefix(path, pwent->pw_dir);
}

static bool is_root_only_accessible_plugin(const char *plugin_path)
{
  /* XXX: this is very primitive and doesn't account for non ~root/
	 directories that actually are not accessible by non root users */
  return plugin_path && is_root_path(plugin_path);
}

typedef bool (*is_plugin_cb)(const char *plugin_path, NPW_PluginInfo *plugin_info);
typedef int (*process_plugin_cb)(const char *plugin_path, NPW_PluginInfo *plugin_info);

static int process_plugin_dir(const char *plugin_dir, is_plugin_cb test, process_plugin_cb process)
{
  if (g_verbose)
	printf("Looking for plugins in %s\n", plugin_dir);

  DIR *dir = opendir(plugin_dir);
  if (dir == NULL)
	return -1;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
	char *plugin_path = g_build_filename(plugin_dir, ent->d_name, NULL);
	NPW_PluginInfo plugin_info;
	if (test(plugin_path, &plugin_info))
	  process(plugin_path, &plugin_info);
	g_free(plugin_path);
  }

  closedir(dir);
  return 0;
}

static int do_install_plugin(const char *plugin_path, const char *plugin_dir, NPW_PluginInfo *plugin_info)
{
  if (plugin_dir == NULL)
	return 1;

  char *plugin_base = strrchr(plugin_path, '/');
  if (plugin_base == NULL)
	return 2;
  plugin_base += 1;

  char d_plugin_path[PATH_MAX];
  int n = snprintf(d_plugin_path, sizeof(d_plugin_path), "%s/%s.%s", plugin_dir, NPW_WRAPPER_BASE, plugin_base);
  if (n < 0 || n >= sizeof(d_plugin_path))
	return 3;

  NPW_PluginInfo w_plugin_info;
  if (!is_wrapper_plugin(NPW_DEFAULT_PLUGIN_PATH, &w_plugin_info))
	return 5;
  const char *w_plugin_path, *plugin_ident;
  plugin_ident = w_plugin_info.ident;
  w_plugin_path = w_plugin_info.path;
  if (strcmp(w_plugin_path, NPW_DEFAULT_PLUGIN_PATH) != 0)
	return 6;
  int w_plugin_path_length = strlen(w_plugin_path);
  int plugin_ident_length = strlen(plugin_ident);

  int w_fd = open(w_plugin_path, O_RDONLY);
  if (w_fd < 0)
	return 7;

  ssize_t w_size = lseek(w_fd, 0, SEEK_END);
  if (w_size < 0)
	return 8;
  lseek(w_fd, 0, SEEK_SET);

  char *plugin_data = malloc(w_size);
  if (plugin_data == NULL)
	return 9;

  if (read(w_fd, plugin_data, w_size) != w_size)
	return 10;
  close(w_fd);

  int i, ofs = -1;
  for (i = NPW_PLUGIN_IDENT_SIZE; i < w_size - PATH_MAX; i++) {
	if (memcmp(plugin_data + i, w_plugin_path, w_plugin_path_length) == 0 &&
		memcmp(plugin_data + i - NPW_PLUGIN_IDENT_SIZE, NPW_PLUGIN_IDENT, plugin_ident_length) == 0) {
	  ofs = i;
	  break;
	}
  }
  if (ofs < 0)
	return 11;
  strcpy(plugin_data + ofs, plugin_path);

  struct stat st;
  if (stat(plugin_path, &st) < 0)
	return 12;

  if (plugin_info == NULL)
	return 14;
  if (plugin_info->target_arch[0] == '\0' || plugin_info->target_os[0] == '\0') {
	if (!is_plugin_viewer_available(plugin_path, plugin_info))
	  return 15;
  }
  if (plugin_info->viewer_path[0] == '\0') {
	if (!is_plugin_viewer_available(plugin_path, plugin_info))
	  return 16;
  }

  NPW_PluginInfo *pi = (NPW_PluginInfo *)(plugin_data + ofs - NPW_PLUGIN_IDENT_SIZE);
  pi->mtime = st.st_mtime;
  strcpy(pi->target_arch, plugin_info->target_arch);
  strcpy(pi->target_os, plugin_info->target_os);
  pi->struct_version = w_plugin_info.struct_version;
  strcpy(pi->viewer_path, plugin_info->viewer_path);

  int mode = 0700;
  if (!is_user_home_path(d_plugin_path) &&
	  !is_root_only_accessible_plugin(plugin_dir))
	mode = 0755;

  // TODO: Don't swallow the error message. Also get rid of these ridiculous
  // return codes. They're never acted on anyway. Use GError or something.
  if (!g_file_set_contents(d_plugin_path, plugin_data, w_size, NULL))
    return 4;

  if (g_verbose)
	printf("  into %s\n", d_plugin_path);

  free(plugin_data);
  return 0;
}

static int install_plugin(const char *plugin_path, NPW_PluginInfo *plugin_info)
{
  int ret;

  if (g_verbose)
	printf("Install plugin %s\n", plugin_path);

  // don't install plugin system-wide if it is only accessible by root
  if (!is_root_only_accessible_plugin(plugin_path)) {
	ret = do_install_plugin(plugin_path, get_system_mozilla_plugin_dir(), plugin_info);
	if (ret == 0)
	  return 0;
  }

  // don't install plugin in user home dir if already available system-wide
  if (has_system_wide_wrapper_plugin(plugin_path, true)) {
	if (g_verbose)
	  printf(" ... already installed system-wide, skipping\n");
	return 0;
  }

  const char *user_plugin_dir = get_user_mozilla_plugin_dir();
  if (access(user_plugin_dir, R_OK | W_OK) < 0 &&
	  g_mkdir_with_parents(user_plugin_dir, 0755) < 0)
	return 1;

  ret = do_install_plugin(plugin_path, user_plugin_dir, plugin_info);
  if (ret == 0)
	return 0;

  return ret;
}

static int auto_install_plugins(void)
{
  const char **plugin_dirs = get_mozilla_plugin_dirs();
  if (plugin_dirs) {
	int i;
	for (i = 0; plugin_dirs[i] != NULL; i++) {
	  const char *plugin_dir = plugin_dirs[i];
	  if (g_verbose)
		printf("Auto-install plugins from %s\n", plugin_dir);
	  process_plugin_dir(plugin_dir, is_compatible_plugin, (process_plugin_cb)install_plugin);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int remove_plugin(const char *plugin_path)
{
  if (g_verbose)
	printf("Remove plugin %s\n", plugin_path);

  if (unlink(plugin_path) < 0)
	return 1;

  return 0;
}

static int remove_plugin_cb(const char *plugin_path, void *unused)
{
  return remove_plugin(plugin_path);
}

static int auto_remove_plugins(void)
{
  const char **plugin_dirs = get_mozilla_plugin_dirs();
  if (plugin_dirs) {
	int i;
	for (i = 0; plugin_dirs[i] != NULL; i++) {
	  const char *plugin_dir = plugin_dirs[i];
	  if (g_verbose)
		printf("Auto-remove plugins from %s\n", plugin_dir);
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)remove_plugin_cb);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int update_plugin(const char *plugin_path)
{
  if (g_verbose)
	printf("Update plugin %s\n", plugin_path);

  int ret = 0;
  NPW_PluginInfo plugin_info;
  memset(&plugin_info, 0, sizeof(plugin_info));
  is_wrapper_plugin(plugin_path, &plugin_info);

  struct stat st;

  if (access(plugin_info.path, F_OK) < 0) {
	if (g_verbose)
	  printf("  NPAPI plugin %s is no longer available, removing wrapper\n", plugin_info.path);
	ret = remove_plugin(plugin_path);
  }
  else if (has_system_wide_wrapper_plugin(plugin_info.path, true)
		   && !g_str_has_prefix(plugin_path, get_system_mozilla_plugin_dir())) {
	if (g_verbose)
	  printf("  NPAPI plugin %s is already installed system-wide, removing wrapper\n", plugin_info.path);
	ret = remove_plugin(plugin_path);
  }
  else if (has_system_wide_wrapper_plugin(plugin_info.path, false)
		   && is_root_only_accessible_plugin(plugin_info.path)) {
	/* Don't check for an exact ident, we only need to know if there
	   is a system-wide plugin to be removed. It will then be
	   re-installed to the user (root) private mozilla plugins dir */
	if (g_verbose)
	  printf("  NPAPI plugin %s is accessible by root only, removing wrapper\n", plugin_info.path);
	if ((ret = remove_plugin(plugin_path)) != 0)
	  return ret;
	if (g_verbose)
	  printf ("  ... but re-installing it to root private mozilla plugins dir\n");
	ret = install_plugin(plugin_info.path, &plugin_info);
  }
  else if (stat(plugin_info.path, &st) == 0 && st.st_mtime > plugin_info.mtime) {
	if (g_verbose)
	  printf("  NPAPI plugin %s was modified, reinstalling plugin\n", plugin_info.path);
	ret = install_plugin(plugin_info.path, &plugin_info);
  }
  else if (strcmp(plugin_info.ident, NPW_PLUGIN_IDENT) != 0) {
	if (g_verbose)
	  printf("  nspluginwrapper ident mismatch, reinstalling plugin\n");
	ret = install_plugin(plugin_info.path, &plugin_info);
  }
  else {
	if (g_verbose)
	  printf("  wrapper ident matches and NPAPI plugin is unmodified, skipping\n");
  }

  return ret;
}

static int update_plugin_cb(const char *plugin_path, void *unused)
{
  return update_plugin(plugin_path);
}

static int auto_update_plugins(void)
{
  const char **plugin_dirs = get_mozilla_plugin_dirs();
  if (plugin_dirs) {
	int i;
	for (i = 0; plugin_dirs[i] != NULL; i++) {
	  const char *plugin_dir = plugin_dirs[i];
	  if (g_verbose)
		printf("Auto-update plugins from %s\n", plugin_dir);
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)update_plugin_cb);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int list_plugin(const char *plugin_path)
{
  NPW_PluginInfo plugin_info;
  memset(&plugin_info, 0, sizeof(plugin_info));
  is_wrapper_plugin(plugin_path, &plugin_info);

  printf("%s\n", plugin_path);
  printf("  Original plugin: %s\n", plugin_info.path);
  if (plugin_info.struct_version >= 2 && plugin_info.viewer_path[0] != '\0')
	printf("  Plugin viewer: %s\n", plugin_info.viewer_path);
  char *str = strtok(plugin_info.ident, ":");
  if (str && strcmp(str, "NPW") == 0) {
	str = strtok(NULL, ":");
	if (plugin_info.struct_version >= 2) { /* skip 'X' */
	  if (str[0] != 'X')
		error("invalid NPW_PluginInfo format");
	  str = strtok(NULL, ":");
	}
	if (str) {
	  printf("  Wrapper version string: %s", str);
	  str = strtok(NULL, ":");
	  if (str)
		printf(" (%s)", str);
	  printf("\n");
	}
  }

  return 0;
}

static int list_plugin_cb(const char *plugin_path, void *unused)
{
  return list_plugin(plugin_path);
}

static void print_usage(void)
{
  printf("%s, configuration tool.  Version %s\n", NPW_CONFIG, NPW_VERSION);
  printf("\n");
  printf("   usage: %s [flags] [command [plugin(s)]]\n", NPW_CONFIG);
  printf("\n");
  printf("   -h --help               print this message\n");
  printf("   -v --verbose            flag: set verbose mode\n");
  printf("   -a --auto               flag: set automatic mode for plugins discovery\n");
  printf("   -n --native             flag: allow native plugin(s) to be wrapped\n");
  printf("   -l --list               list plugins currently installed\n");
  printf("   -u --update [FILE(S)]   update plugin(s) currently installed\n");
  printf("   -i --install [FILE(S)]  install plugin(s)\n");
  printf("   -r --remove [FILE(S)]   remove plugin(s)\n");
  printf("\n");
}

static int process_help(int argc, char *argv[])
{
  print_usage();
  return 0;
}

static int process_verbose(int argc, char *argv[])
{
  g_verbose = true;
  return 0;
}

static int process_auto(int argc, char *argv[])
{
  g_auto = true;
  return 0;
}

static int process_native(int argc, char *argv[])
{
  g_allow_native = true;
  return 0;
}

static int process_list(int argvc, char *argv[])
{
  const char **plugin_dirs = get_mozilla_plugin_dirs();
  if (plugin_dirs) {
	int i;
	for (i = 0; plugin_dirs[i] != NULL; i++) {
	  const char *plugin_dir = plugin_dirs[i];
	  if (g_verbose)
		printf("List plugins in %s\n", plugin_dir);
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)list_plugin_cb);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int process_update(int argc, char *argv[])
{
  int i;

  if (g_auto)
	return auto_update_plugins();

  if (argc < 1)
	error("expected plugin(s) file name to update");

  for (i = 0; i < argc; i++) {
	const char *plugin_path = argv[i];
	if (!is_wrapper_plugin_0(plugin_path))
	  error("%s is not a valid nspluginwrapper plugin", plugin_path);
	int ret = update_plugin(plugin_path);
	if (ret != 0)
	  return ret;
  }

  return 0;
}

static int process_install(int argc, char *argv[])
{
  int i, ret;

  if (g_auto)
	return auto_install_plugins();

  if (argc < 1)
	error("expected plugin(s) file name to install");

  for (i = 0; i < argc; i++) {
	NPW_PluginInfo plugin_info;
	memset(&plugin_info, 0, sizeof(plugin_info));

	const char *plugin_path = argv[i];
	if (!is_plugin(plugin_path, &plugin_info))
	  error("%s is not a valid NPAPI plugin", plugin_path);

	ret = detect_plugin_viewer(plugin_path, &plugin_info);
	if (ret != EXIT_VIEWER_OK) {
	  if (ret == EXIT_VIEWER_NATIVE)
		return 0; /* silently ignore exit status */
	  error("no appropriate viewer found for %s", plugin_path);
	}

	ret = install_plugin(plugin_path, &plugin_info);
	if (ret != 0)
	  return ret;
  }

  return 0;
}

static int process_remove(int argc, char *argv[])
{
  int i;

  if (g_auto)
	return auto_remove_plugins();

  if (argc < 1)
	error("expected plugin(s) file name to remove");

  for (i = 0; i < argc; i++) {
	const char *plugin_path = argv[i];
	if (!is_wrapper_plugin_0(plugin_path))
	  error("%s is not a valid nspluginwrapper plugin", plugin_path);
	int ret = remove_plugin(plugin_path);
	if (ret != 0)
	  return ret;
  }

  return 0;
}

int main(int argc, char *argv[])
{
  char **args;
  int i, j, n_args;

  n_args = argc - 1;
  args = argv + 1;

  if (n_args < 1) {
	print_usage();
	return 1;
  }

  if (args[0][0] != '-') {
	print_usage();
	return 1;
  }

  static const struct option {
	char short_option;
	const char *long_option;
	int (*process_callback)(int argc, char *argv[]);
	bool terminal;
  }
  options[] = {
	{ 'h', "help",		process_help,		1 },
	{ 'v', "verbose",	process_verbose,	0 },
	{ 'a', "auto",		process_auto,		0 },
	{ 'n', "native",	process_native,		0 },
	{ 'l', "list",		process_list,		1 },
	{ 'u', "update",	process_update,		1 },
	{ 'i', "install",	process_install,	1 },
	{ 'r', "remove",	process_remove,		1 },
	{  0,   NULL,		NULL,				1 }
  };

  for (i = 0; i < n_args; i++) {
	const char *arg = args[i];
	const struct option *opt = NULL;
	for (j = 0; opt == NULL && options[j].process_callback != NULL; j++) {
	  if ((arg[0] == '-' && arg[1] == options[j].short_option && arg[2] == '\0') ||
		  (arg[0] == '-' && arg[1] == '-' && strcmp(&arg[2], options[j].long_option) == 0))
		opt = &options[j];
	}
	if (opt == NULL) {
	  fprintf(stderr, "invalid option %s\n", arg);
	  print_usage();
	  return 1;
	}
	int ret = opt->process_callback(n_args - i - 1, args + i + 1);
	if (opt->terminal)
	  return ret;
  }

  return 0;
}
