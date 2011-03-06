/*
 *  npw-config.c - nspluginwrapper configuration tool
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <dirent.h>


static bool g_auto = false;
static bool g_verbose = false;
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

static int strstart(const char *str, const char *val, const char **ptr)
{
  const char *p, *q;
  p = str;
  q = val;
  while (*q != '\0') {
	if (*p != *q)
	  return 0;
	p++;
	q++;
  }
  if (ptr)
	*ptr = p;
  return 1;
}

/* Implement mkdir -p with default permissions (derived from busybox code) */
static int mkdir_p(const char *path)
{
  char path_copy[strlen(path) + 1];
  char *s = path_copy;
  path = strcpy(s, path);
  for (;;) {
	char c = 0;
	while (*s) {
	  if (*s == '/') {
		while (*++s == '/')
		  ;
		c = *s;
		*s = 0;
		break;
	  }
	  ++s;
	}
	if (mkdir(path, 0755) < 0) {
	  struct stat st;
	  if ((errno != EEXIST && errno != EISDIR) || stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
		break;
	}
	if ((*s = c) == '\0')
	  return 0;
  }
  return -1;
}

static const char *get_user_home_dir(void)
{
  struct passwd *pwent = getpwuid(geteuid());
  if (pwent)
	return pwent->pw_dir;

  return getenv("HOME");
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
#elif defined(__NetBSD__)
	{
	  static const char *netbsd_dirs[] = {
		"/usr/pkg/" LIB "/mozilla/plugins",
		"/usr/pkg/" LIB "/firefox/plugins",
	  };
	  dirs = netbsd_dirs;
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
  static char plugin_path[PATH_MAX];

  if ((home = get_user_home_dir()) == NULL)
	return NULL;

  sprintf(plugin_path, "%s/.mozilla/plugins", home);
  return plugin_path;
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
#if defined(__NetBSD__)
	"/usr/pkg/lib/netscape/plugins",
	"/usr/pkg/lib/firefox/plugins",
	"/usr/pkg/lib/RealPlayer/mozilla",
	"/usr/pkg/Acrobat5/Browsers/intellinux",
	"/usr/pkg/Acrobat7/Browser/intellinux",
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

/* 32-bit ELF base types.  */
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

/* The ELF file header.  This appears at the start of every ELF file.  */
#define EI_NIDENT (16)

typedef struct
{
  unsigned char	e_ident[EI_NIDENT];	/* Magic number and other info */
  Elf32_Half	e_type;			/* Object file type */
  Elf32_Half	e_machine;		/* Architecture */
  Elf32_Word	e_version;		/* Object file version */
  Elf32_Addr	e_entry;		/* Entry point virtual address */
  Elf32_Off	e_phoff;		/* Program header table file offset */
  Elf32_Off	e_shoff;		/* Section header table file offset */
  Elf32_Word	e_flags;		/* Processor-specific flags */
  Elf32_Half	e_ehsize;		/* ELF header size in bytes */
  Elf32_Half	e_phentsize;		/* Program header table entry size */
  Elf32_Half	e_phnum;		/* Program header table entry count */
  Elf32_Half	e_shentsize;		/* Section header table entry size */
  Elf32_Half	e_shnum;		/* Section header table entry count */
  Elf32_Half	e_shstrndx;		/* Section header string table index */
} Elf32_Ehdr;

#define EI_MAG0		0		/* File identification byte 0 index */
#define ELFMAG0		0x7f		/* Magic number byte 0 */
#define EI_MAG1		1		/* File identification byte 1 index */
#define ELFMAG1		'E'		/* Magic number byte 1 */
#define EI_MAG2		2		/* File identification byte 2 index */
#define ELFMAG2		'L'		/* Magic number byte 2 */
#define EI_MAG3		3		/* File identification byte 3 index */
#define ELFMAG3		'F'		/* Magic number byte 3 */
#define EI_CLASS	4		/* File class byte index */
#define ELFCLASS32	1		/* 32-bit objects */
#define ELFCLASS64	2		/* 64-bit objects */
#define EI_DATA		5		/* Data encoding byte index */
#define ELFDATA2LSB	1		/* 2's complement, little endian */
#define ELFDATA2MSB	2		/* 2's complement, big endian */
#define EI_OSABI	7		/* OS ABI identification */
#define ELFOSABI_SYSV	0		/* UNIX System V ABI */
#define ELFOSABI_NETBSD	2		/* NetBSD.  */
#define ELFOSABI_LINUX	3		/* Linux.  */
#define ELFOSABI_SOLARIS 6		/* Sun Solaris.  */
#define ELFOSABI_FREEBSD 9		/* FreeBSD.  */
#define ET_DYN		3		/* Shared object file */
#define EM_386		3		/* Intel 80386 */
#define EM_SPARC	2		/* SUN SPARC */
#define EM_PPC		20		/* PowerPC */
#define EM_PPC64	21		/* PowerPC 64-bit */
#define EM_SPARCV9	43		/* SPARC v9 64-bit */
#define EM_IA_64	50		/* Intel Merced */
#define EM_X86_64	62		/* AMD x86-64 architecture */
#define EV_CURRENT	1		/* Current version */

/* Section header.  */
typedef struct
{
  Elf32_Word	sh_name;		/* Section name (string tbl index) */
  Elf32_Word	sh_type;		/* Section type */
  Elf32_Word	sh_flags;		/* Section flags */
  Elf32_Addr	sh_addr;		/* Section virtual addr at execution */
  Elf32_Off	sh_offset;		/* Section file offset */
  Elf32_Word	sh_size;		/* Section size in bytes */
  Elf32_Word	sh_link;		/* Link to another section */
  Elf32_Word	sh_info;		/* Additional section information */
  Elf32_Word	sh_addralign;		/* Section alignment */
  Elf32_Word	sh_entsize;		/* Entry size if section holds table */
} Elf32_Shdr;

#define SHT_NOBITS	  8		/* Program space with no data (bss) */
#define SHT_DYNSYM	  11		/* Dynamic linker symbol table */

/* Symbol table entry.  */
typedef struct
{
  Elf32_Word	st_name;		/* Symbol name (string tbl index) */
  Elf32_Addr	st_value;		/* Symbol value */
  Elf32_Word	st_size;		/* Symbol size */
  unsigned char	st_info;		/* Symbol type and binding */
  unsigned char	st_other;		/* Symbol visibility */
  Elf32_Half	st_shndx;		/* Section index */
} Elf32_Sym;

#define ELF32_ST_BIND(val)		(((unsigned char) (val)) >> 4)
#define ELF32_ST_TYPE(val)		((val) & 0xf)
#define STB_GLOBAL	1		/* Global symbol */
#define STT_OBJECT	1		/* Symbol is a data object */
#define STT_FUNC	2		/* Symbol is a code object */

/* We handle 32-bit ELF plugins only */
#undef  ELF_CLASS
#define ELF_CLASS	ELFCLASS32
#define ElfW(x)		Elf32_ ## x
#define ELFW(x)		ELF32_ ## x

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

static void elf_swap_ehdr(ElfW(Ehdr) *hdr)
{
  hdr->e_type			= bswap_16(hdr->e_type);
  hdr->e_machine		= bswap_16(hdr->e_machine);
  hdr->e_version		= bswap_32(hdr->e_version);
  hdr->e_entry			= bswap_32(hdr->e_entry);
  hdr->e_phoff			= bswap_32(hdr->e_phoff);
  hdr->e_shoff			= bswap_32(hdr->e_shoff);
  hdr->e_flags			= bswap_32(hdr->e_flags);
  hdr->e_ehsize			= bswap_16(hdr->e_ehsize);
  hdr->e_phentsize		= bswap_16(hdr->e_phentsize);
  hdr->e_phnum			= bswap_16(hdr->e_phnum);
  hdr->e_shentsize		= bswap_16(hdr->e_shentsize);
  hdr->e_shnum			= bswap_16(hdr->e_shnum);
  hdr->e_shstrndx		= bswap_16(hdr->e_shstrndx);
}

static void elf_swap_shdr(ElfW(Shdr) *shdr)
{
  shdr->sh_name			= bswap_32(shdr->sh_name);
  shdr->sh_type			= bswap_32(shdr->sh_type);
  shdr->sh_flags		= bswap_32(shdr->sh_flags);
  shdr->sh_addr			= bswap_32(shdr->sh_addr);
  shdr->sh_offset		= bswap_32(shdr->sh_offset);
  shdr->sh_size			= bswap_32(shdr->sh_size);
  shdr->sh_link			= bswap_32(shdr->sh_link);
  shdr->sh_info			= bswap_32(shdr->sh_info);
  shdr->sh_addralign	= bswap_32(shdr->sh_addralign);
  shdr->sh_entsize		= bswap_32(shdr->sh_entsize);
}

static void elf_swap_sym(ElfW(Sym) *sym)
{
  sym->st_name			= bswap_32(sym->st_name);
  sym->st_value			= bswap_32(sym->st_value);
  sym->st_size			= bswap_32(sym->st_size);
  sym->st_shndx			= bswap_32(sym->st_shndx);
}

static bool is_plugin_fd(int fd, NPW_PluginInfo *out_plugin_info)
{
  int i;
  bool ret = false;

  ElfW(Ehdr) ehdr;
  if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
	return false;

  if (ehdr.e_ident[EI_MAG0] != ELFMAG0
	  || ehdr.e_ident[EI_MAG1] != ELFMAG1
	  || ehdr.e_ident[EI_MAG2] != ELFMAG2
	  || ehdr.e_ident[EI_MAG3] != ELFMAG3)
	return false;

  bool do_swap = (ehdr.e_ident[EI_DATA] == ELFDATA2LSB) && !is_little_endian();
  if (do_swap)
	elf_swap_ehdr(&ehdr);

  if (ehdr.e_ident[EI_CLASS] != ELF_CLASS)
	return false;
  if (ehdr.e_type != ET_DYN)
	return false;
  if (ehdr.e_version != EV_CURRENT)
	return false;

  if (out_plugin_info) {
	const char *target_arch = "";
	switch (ehdr.e_machine) {
	case EM_386:			target_arch = "i386";	break;
	case EM_SPARC:			target_arch = "sparc";	break;
	case EM_PPC:			target_arch = "ppc";	break;
	}
	strcpy(out_plugin_info->target_arch, target_arch);
	const char *target_os = "";
	switch (ehdr.e_ident[EI_OSABI]) {
	case ELFOSABI_LINUX:	target_os = "linux";	break;
	case ELFOSABI_SOLARIS:	target_os = "solaris";	break;
	case ELFOSABI_FREEBSD:	target_os = "freebsd";	break;
	}
	strcpy(out_plugin_info->target_os, target_os);
  }

  ElfW(Shdr) *shdr = (ElfW(Shdr) *)load_data(fd, ehdr.e_shoff, ehdr.e_shnum * sizeof(*shdr));
  if (do_swap) {
	for (i = 0; i < ehdr.e_shnum; i++)
	  elf_swap_shdr(&shdr[i]);
  }

  char **sdata = (char **)calloc(ehdr.e_shnum, sizeof(*sdata));
  for (i = 0; i < ehdr.e_shnum; i++) {
	ElfW(Shdr) *sec = &shdr[i];
	if (sec->sh_type != SHT_NOBITS)
	  sdata[i] =  (char *)load_data(fd, sec->sh_offset, sec->sh_size);
  }

  ElfW(Shdr) *symtab_sec = NULL;
  for (i = 0; i < ehdr.e_shnum; i++) {
	ElfW(Shdr) *sec = &shdr[i];
	if (sec->sh_type == SHT_DYNSYM
		&& strcmp(sdata[ehdr.e_shstrndx] + sec->sh_name, ".dynsym") == 0) {
	  symtab_sec = sec;
	  break;
	}
  }
  if (symtab_sec == NULL)
	goto done;
  ElfW(Sym) *symtab = (ElfW(Sym) *)sdata[symtab_sec - shdr];
  char *strtab = sdata[symtab_sec->sh_link];

  int nb_syms = symtab_sec->sh_size / sizeof(*symtab);
  if (do_swap) {
	for (i = 0; i < nb_syms; i++)
	  elf_swap_sym(&symtab[i]);
  }

  int nb_np_syms;
  int is_wrapper_plugin = 0;
  for (i = 0, nb_np_syms = 0; i < nb_syms; i++) {
	ElfW(Sym) *sym = &symtab[i];
	const char *name = strtab + sym->st_name;
	if (ELFW(ST_BIND)(sym->st_info) != STB_GLOBAL)
	  continue;
	if (ELFW(ST_TYPE)(sym->st_info) == STT_OBJECT && strcmp(name, "NPW_Plugin") == 0)
	  is_wrapper_plugin = 1;
	if (ELFW(ST_TYPE)(sym->st_info) != STT_FUNC)
	  continue;
	if (!strcmp(name, "NP_GetMIMEDescription") ||
		!strcmp(name, "NP_Initialize") ||
		!strcmp(name, "NP_Shutdown"))
	  nb_np_syms++;
  }
  ret = (nb_np_syms == 3) && !is_wrapper_plugin;

 done:
  for (i = 0; i < ehdr.e_shnum; i++)
	free(sdata[i]);
  free(sdata);
  free(shdr);
  return ret;
}

static bool is_plugin_viewer_available(const char *filename, NPW_PluginInfo *out_plugin_info)
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

  for (int i = 0; i < target_arch_table_size; i++) {
	const char *target_arch = target_arch_table[i];
	if (target_arch == NULL)
	  continue;
	char viewer_arch_path[PATH_MAX];
	sprintf(viewer_arch_path, "%s/%s", NPW_LIBDIR, target_arch);
	if (access(viewer_arch_path, F_OK) != 0) {
	  target_arch_table[i] = NULL;		// this target ARCH is not available, skip it for good
	  continue;
	}
	for (int j = 0; j < target_os_table_size; j++) {
	  const char *target_os = target_os_table[j];
	  if (target_os == NULL)
		continue;
	  if (strcmp(target_arch, HOST_ARCH) == 0 && strcmp(target_os, HOST_OS) == 0)
		continue;						// skip viewers that match host OS/ARCH pairs
	  char viewer_path[PATH_MAX];
	  sprintf(viewer_path, "%s/%s/%s", viewer_arch_path, target_os, NPW_VIEWER);
	  if (access(viewer_path, F_OK) != 0)
		continue;
	  int pid = fork();
	  if (pid < 0)
		continue;
	  else if (pid == 0) {
		execl(viewer_path, NPW_VIEWER, "--test", "--plugin", filename, NULL);
		exit(1);
	  }
	  else {
		int status;
		while (waitpid(pid, &status, 0) != pid)
		  ;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		  if (out_plugin_info) {
			strcpy(out_plugin_info->target_arch, target_arch);
			strcpy(out_plugin_info->target_os, target_os);
		  }
		  return true;
		}
	  }
	}
  }
  return false;
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
	strcpy(out_plugin_info->ident, pi->ident);
	strcpy(out_plugin_info->path, pi->path);
	out_plugin_info->mtime = pi->mtime;
	out_plugin_info->target_arch[0] = '\0';
	out_plugin_info->target_os[0] = '\0';
	if (strncmp(pi->ident, "NPW:0.9.90", 10) != 0) {					// additional members in 0.9.91+
	  strcpy(out_plugin_info->target_arch, pi->target_arch);
	  strcpy(out_plugin_info->target_os, pi->target_os);
	}
  }
  return true;
}

static bool is_wrapper_plugin(const char *plugin_path, NPW_PluginInfo *out_plugin_info)
{
  void *handle = dlopen(plugin_path, RTLD_LAZY);
  if (handle == NULL)
	return false;

  bool ret = is_wrapper_plugin_handle(handle, out_plugin_info);
  dlclose(handle);
  return ret;
}

static bool is_wrapper_plugin_0(const char *plugin_path)
{
  NPW_PluginInfo plugin_info;
  return is_wrapper_plugin(plugin_path, &plugin_info)
	&& strcmp(plugin_info.path, NPW_DEFAULT_PLUGIN_PATH) != 0			// exclude OS/ARCH npwrapper.so
	&& strcmp(plugin_info.path, NPW_OLD_DEFAULT_PLUGIN_PATH) != 0;		// exclude ARCH npwrapper.so
}

static bool is_system_wide_wrapper_plugin(const char *plugin_path)
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
		  && strcmp(plugin_info.ident, NPW_PLUGIN_IDENT) == 0
		  && plugin_info.mtime == st.st_mtime);
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

  int plugin_path_length = 256;
  char *plugin_path = (char *)malloc(plugin_path_length);
  int plugin_dir_length = strlen(plugin_dir);

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
	int len = plugin_dir_length + 1 + strlen(ent->d_name) + 1;
	if (len > plugin_path_length) {
	  plugin_path_length = len;
	  plugin_path = (char *)realloc(plugin_path, plugin_path_length);
	}
	sprintf(plugin_path, "%s/%s", plugin_dir, ent->d_name);
	NPW_PluginInfo plugin_info;
	if (test(plugin_path, &plugin_info))
	  process(plugin_path, &plugin_info);
  }

  free(plugin_path);
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

  int mode = 0700;
  if (geteuid() == 0 && strcmp(plugin_dir, "/root") != 0)
	mode = 0755;

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

  NPW_PluginInfo *pi = (NPW_PluginInfo *)(plugin_data + ofs - NPW_PLUGIN_IDENT_SIZE);
  pi->mtime = st.st_mtime;
  strcpy(pi->target_arch, plugin_info->target_arch);
  strcpy(pi->target_os, plugin_info->target_os);

  int d_fd = open(d_plugin_path, O_CREAT | O_WRONLY, mode);
  if (d_fd < 0)
	return 4;

  if (write(d_fd, plugin_data, w_size) != w_size)
	return 13;
  close(d_fd);

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

  ret = do_install_plugin(plugin_path, get_system_mozilla_plugin_dir(), plugin_info);
  if (ret == 0)
	return 0;

  // don't install plugin in user home dir if already available system-wide
  if (is_system_wide_wrapper_plugin(plugin_path)) {
	if (g_verbose)
	  printf(" ... already installed system-wide, skipping\n");
	return 0;
  }

  const char *user_plugin_dir = get_user_mozilla_plugin_dir();
  if (access(user_plugin_dir, R_OK | W_OK) < 0 && mkdir_p(user_plugin_dir) < 0)
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

static int remove_plugin(const char *plugin_path, ...)
{
  if (g_verbose)
	printf("Remove plugin %s\n", plugin_path);

  if (unlink(plugin_path) < 0)
	return 1;

  return 0;
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
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)remove_plugin);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int update_plugin(const char *plugin_path, ...)
{
  if (g_verbose)
	printf("Update plugin %s\n", plugin_path);

  int ret = 0;
  NPW_PluginInfo plugin_info;
  is_wrapper_plugin(plugin_path, &plugin_info);

  struct stat st;

  if (access(plugin_info.path, F_OK) < 0) {
	if (g_verbose)
	  printf("  NS4 plugin %s is no longer available, removing wrapper\n", plugin_info.path);
	ret = remove_plugin(plugin_path);
  }
  else if (is_system_wide_wrapper_plugin(plugin_info.path)
		   && !strstart(plugin_path, get_system_mozilla_plugin_dir(), NULL)) {
	if (g_verbose)
	  printf("  NS4 plugin %s is already installed system-wide, removing wrapper\n", plugin_info.path);
	ret = remove_plugin(plugin_path);
  }
  else if (stat(plugin_info.path, &st) == 0 && st.st_mtime > plugin_info.mtime) {
	if (g_verbose)
	  printf("  NS4 plugin %s was modified, reinstalling plugin\n", plugin_info.path);
	ret = install_plugin(plugin_info.path, &plugin_info);
  }
  else if (strcmp(plugin_info.ident, NPW_PLUGIN_IDENT) != 0) {
	if (g_verbose)
	  printf("  nspluginwrapper ident mismatch, reinstalling plugin\n");
	ret = install_plugin(plugin_info.path, &plugin_info);
  }

  return ret;
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
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)update_plugin);
	}
  }
  free(plugin_dirs);
  return 0;
}

static int list_plugin(const char *plugin_path, ...)
{
  NPW_PluginInfo plugin_info;
  is_wrapper_plugin(plugin_path, &plugin_info);

  printf("%s\n", plugin_path);
  printf("  Original plugin: %s\n", plugin_info.path);
  char *str = strtok(plugin_info.ident, ":");
  if (str && strcmp(str, "NPW") == 0) {
	str = strtok(NULL, ":");
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

static void print_usage(void)
{
  printf("%s, configuration tool.  Version %s\n", NPW_CONFIG, NPW_VERSION);
  printf("\n");
  printf("   usage: %s [flags] [command [plugin(s)]]\n", NPW_CONFIG);
  printf("\n");
  printf("   -h --help               print this message\n");
  printf("   -v --verbose            flag: set verbose mode\n");
  printf("   -a --auto               flag: set automatic mode for plugins discovery\n");
  printf("   -l --list               list plugins currently installed\n");
  printf("   -u --update             update plugin(s) currently installed\n");
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

static int process_list(int argvc, char *argv[])
{
  const char **plugin_dirs = get_mozilla_plugin_dirs();
  if (plugin_dirs) {
	int i;
	for (i = 0; plugin_dirs[i] != NULL; i++) {
	  const char *plugin_dir = plugin_dirs[i];
	  if (g_verbose)
		printf("List plugins in %s\n", plugin_dir);
	  process_plugin_dir(plugin_dir, (is_plugin_cb)is_wrapper_plugin_0, (process_plugin_cb)list_plugin);
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
  int i;

  if (g_auto)
	return auto_install_plugins();

  if (argc < 1)
	error("expected plugin(s) file name to install");

  for (i = 0; i < argc; i++) {
	NPW_PluginInfo plugin_info;
	const char *plugin_path = argv[i];
	if (!is_compatible_plugin(plugin_path, &plugin_info))
	  error("%s is not a valid NPAPI plugin", plugin_path);
	int ret = install_plugin(plugin_path, &plugin_info);
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
