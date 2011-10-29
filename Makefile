#
#  nspluginwrapper Makefile (C) 2005-2009 Gwenole Beauchesne
#                           (C) 2011 David Benjamin
#
-include config.mak

ifeq ($(SRC_PATH),)
SRC_PATH = .
endif

PACKAGE = nspluginwrapper
ifeq ($(SNAPSHOT),2)
VERSION_SUFFIX = -$(SVNDATE)
endif

ifeq ($(INSTALL),)
INSTALL = install
ifneq (,$(findstring $(OS),solaris))
INSTALL = $(SRC_PATH)/utils/install.sh
endif
endif

ifeq ($(ALLOW_STRIP), yes)
STRIP_OPT = -s
endif

LN_S = ln -sf

ifeq ($(LD_soname),)
LD_soname = -soname
ifeq ($(TARGET_OS),solaris)
LD_soname = -h
endif
endif

ifneq (,$(findstring $(OS),linux))
libdl_LIBS = -ldl
endif

libpthread_LIBS = -lpthread
ifeq ($(OS),dragonfly)
libpthread_LDFLAGS = -pthread
endif

ifneq (,$(findstring $(OS),solaris))
libsocket_LIBS = -lsocket -lnsl
endif

PIC_CFLAGS = -fPIC
DSO_LDFLAGS = -shared
ifeq ($(COMPILER),xlc)
PIC_CFLAGS = -qpic
DSO_LDFLAGS = -qmkshrobj
endif

ARCH_32 = $(ARCH)
ifeq ($(build_biarch), yes)
ARCH_32 = $(TARGET_ARCH)
LSB_LIBS = $(LSB_OBJ_DIR)/libc.so $(LSB_OBJ_DIR)/libgcc_s_32.so
LSB_LIBS += $(LSB_CORE_STUBS:%=$(LSB_OBJ_DIR)/%.so)
LSB_LIBS += $(LSB_CORE_STATIC_STUBS:%=$(LSB_OBJ_DIR)/%.a)
LSB_LIBS += $(LSB_DESKTOP_STUBS:%=$(LSB_OBJ_DIR)/%.so)
endif

LSB_TOP_DIR = $(SRC_PATH)/lsb-build
LSB_INC_DIR = $(LSB_TOP_DIR)/headers
LSB_SRC_DIR = $(LSB_TOP_DIR)/stub_libs
LSB_OBJ_DIR = lsb-build-$(ARCH_32)
LSB_CORE_STUBS = $(shell cat $(LSB_SRC_DIR)/core_filelist)
LSB_CORE_STATIC_STUBS = $(shell cat $(LSB_SRC_DIR)/core_static_filelist)
LSB_DESKTOP_STUBS = $(shell cat $(LSB_SRC_DIR)/desktop_filelist)

ifeq (i386,$(TARGET_ARCH))
TARGET_ELF_ARCH = elf32-i386
endif
ifeq (ppc,$(TARGET_ARCH))
TARGET_ELF_ARCH = elf32-powerpc
endif

ifeq ($(build_biarch), yes)
CFLAGS_32     += -I$(LSB_INC_DIR)
LDFLAGS_32    += -L$(LSB_OBJ_DIR)
GLIB_CFLAGS_32 = -I$(LSB_INC_DIR)/glib-2.0
GLIB_LIBS_32   = -lgobject-2.0 -lgthread-2.0 -lglib-2.0
GTK_CFLAGS_32  = -I$(LSB_INC_DIR)/gtk-2.0
GTK_LIBS_32    = -lgtk-x11-2.0 -lgdk-x11-2.0
X_LIBS_32      = -lX11 -lXt
else
GLIB_CFLAGS_32 = $(GLIB_CFLAGS)
GLIB_LIBS_32   = $(GLIB_LIBS)
GTK_CFLAGS_32  = $(GTK_CFLAGS)
GTK_LIBS_32    = $(GTK_LIBS)
X_CFLAGS_32    = $(X_CFLAGS)
X_LIBS_32      = $(X_LIBS)
endif

MOZILLA_CFLAGS = -I$(SRC_PATH)/npapi

npwrapper_LIBRARY = npwrapper.so
npwrapper_RAWSRCS = npw-wrapper.c npw-common.c npw-malloc.c npw-rpc.c rpc.c debug.c utils.c npruntime.c
npwrapper_SOURCES = $(npwrapper_RAWSRCS:%.c=$(SRC_PATH)/src/%.c)
npwrapper_OBJECTS = $(npwrapper_RAWSRCS:%.c=npwrapper-%.os)
npwrapper_CFLAGS  = $(CFLAGS) $(X_CFLAGS) $(MOZILLA_CFLAGS) $(GLIB_CFLAGS)
npwrapper_LDFLAGS = $(LDFLAGS) $(libpthread_LDFLAGS)
npwrapper_LIBS    = $(X_LIBS) $(libpthread_LIBS) $(libsocket_LIBS)
npwrapper_LIBS   += $(GLIB_LIBS)

npviewer_PROGRAM  = npviewer.bin
npviewer_RAWSRCS  = npw-viewer.c npw-common.c npw-malloc.c npw-rpc.c rpc.c debug.c utils.c npruntime.c
npviewer_SOURCES  = $(npviewer_RAWSRCS:%.c=$(SRC_PATH)/src/%.c)
npviewer_OBJECTS  = $(npviewer_RAWSRCS:%.c=npviewer-%.o)
npviewer_CFLAGS   = $(CFLAGS_32)
npviewer_CFLAGS  += $(GTK_CFLAGS_32)
npviewer_CFLAGS  += $(GLIB_CFLAGS_32)
npviewer_CFLAGS  += $(X_CFLAGS_32)
npviewer_CFLAGS  += $(MOZILLA_CFLAGS)
npviewer_LDFLAGS  = $(LDFLAGS_32)
npviewer_LDFLAGS += $(libpthread_LDFLAGS)
npviewer_LIBS     = $(GTK_LIBS_32) $(GLIB_LIBS_32) $(X_LIBS_32)
npviewer_LIBS    += $(libdl_LIBS) $(libpthread_LIBS)
ifeq ($(TARGET_OS):$(TARGET_ARCH),linux:i386)
npviewer_MAPFILE  = $(SRC_PATH)/src/npw-viewer.map
endif
ifneq ($(npviewer_MAPFILE),)
npviewer_LDFLAGS += -Wl,--export-dynamic
npviewer_LDFLAGS += -Wl,--version-script,$(npviewer_MAPFILE)
endif
ifeq ($(TARGET_OS):$(TARGET_ARCH),linux:i386)
npviewer_SOURCES += $(SRC_PATH)/src/cxxabi-compat.cpp
npviewer_OBJECTS += npviewer-cxxabi-compat.o
npviewer_LIBS    += -lsupc++
endif
ifeq ($(TARGET_OS):$(TARGET_ARCH),solaris:i386)
npviewer_LIBS    += $(libsocket_LIBS)
endif

npplayer_PROGRAM  = npplayer
npplayer_SOURCES  = npw-player.c debug.c rpc.c utils.c glibcurl.c gtk2xtbin.c $(tidy_SOURCES)
npplayer_OBJECTS  = $(npplayer_SOURCES:%.c=npplayer-%.o)
npplayer_CFLAGS   = $(CFLAGS)
npplayer_CFLAGS  += $(GTK_CFLAGS) $(GLIB_CFLAGS) $(MOZILLA_CFLAGS) $(CURL_CFLAGS) $(X_CFLAGS)
npplayer_LDFLAGS  = $(LDFLAGS)
npplayer_LDFLAGS += $(libpthread_LDFLAGS)
npplayer_LIBS     = $(GTK_LIBS) $(GLIB_LIBS) $(CURL_LIBS) $(X_LIBS)
npplayer_LIBS    += $(libdl_LIBS) $(libpthread_LIBS) $(libsocket_LIBS)

libnoxshm_LIBRARY = libnoxshm.so
libnoxshm_RAWSRCS = libnoxshm.c
libnoxshm_SOURCES = $(libnoxshm_RAWSRCS:%.c=$(SRC_PATH)/src/%.c)
libnoxshm_OBJECTS = $(libnoxshm_RAWSRCS:%.c=libnoxshm-%.o)
libnoxshm_CFLAGS  = $(CFLAGS_32) $(PIC_CFLAGS)
libnoxshm_LDFLAGS = $(LDFLAGS_32)

npconfig_PROGRAM = npconfig
npconfig_RAWSRCS = npw-config.c
npconfig_SOURCES = $(npconfig_RAWSRCS:%.c=$(SRC_PATH)/src/%.c)
npconfig_OBJECTS = $(npconfig_RAWSRCS:%.c=npconfig-%.o)
npconfig_CFLAGS  = $(CFLAGS)
npconfig_CFLAGS += $(GLIB_CFLAGS)
npconfig_LIBS    = $(libdl_LIBS)
npconfig_LIBS   += $(GLIB_LIBS)
npconfig_LDFLAGS = $(LDFLAGS)
ifneq (,$(findstring $(OS),netbsd dragonfly))
# We will try to dlopen() the native plugin library. If that lib is
# linked against libpthread, then so must our program too.
# XXX use the ELF decoder for native plugins too?
npconfig_LDFLAGS += $(libpthread_LDFLAGS)
npconfig_LIBS    += $(libpthread_LIBS)
endif

nploader_PROGRAM = npviewer.sh
nploader_RAWSRCS = npw-viewer.sh
nploader_SOURCES = $(nploader_RAWSRCS:%.sh=$(SRC_PATH)/src/%.sh)

test_rpc_RAWSRCS		 = test-rpc-common.c debug.c rpc.c
test_rpc_client_OBJECTS	 = $(test_rpc_RAWSRCS:%.c=%-client.o)
test_rpc_server_OBJECTS	 = $(test_rpc_RAWSRCS:%.c=%-server.o)
test_rpc_client_CPPFLAGS = $(CPPFLAGS) -I$(SRC_PATH)/src -DBUILD_CLIENT -DNPW_COMPONENT_NAME="\"Client\""
test_rpc_server_CPPFLAGS = $(CPPFLAGS) -I$(SRC_PATH)/src -DBUILD_SERVER -DNPW_COMPONENT_NAME="\"Server\""
test_rpc_CFLAGS			 = $(CFLAGS) -I$(SRC_PATH)/src $(GLIB_CFLAGS)
test_rpc_LDFLAGS		 = $(LDFLAGS) $(libpthread_LDFLAGS)
test_rpc_LIBS			 = $(GLIB_LIBS) $(libpthread_LIBS) $(libsocket_LIBS)
test_rpc_RAWPROGS		 = \
	test-rpc-types \
	test-rpc-nested-1 \
	test-rpc-nested-2 \
	test-rpc-concurrent
test_rpc_PROGRAMS		 = \
	$(test_rpc_RAWPROGS:%=%-client) \
	$(test_rpc_RAWPROGS:%=%-server)

CPPFLAGS	= -I. -I$(SRC_PATH)
TARGETS		= $(npconfig_PROGRAM)
TARGETS		+= $(nploader_PROGRAM)
TARGETS		+= $(npwrapper_LIBRARY)
ifeq ($(build_viewer),yes)
TARGETS		+= $(npviewer_PROGRAM)
TARGETS		+= $(libnoxshm_LIBRARY)
endif
ifeq ($(build_player),yes)
TARGETS		+= $(npplayer_PROGRAM)
endif
TARGETS		+= $(test_rpc_PROGRAMS)

archivedir	= files/
SRCARCHIVE	= $(PACKAGE)-$(VERSION)$(VERSION_SUFFIX).tar

all: $(TARGETS)

clean:
	rm -f $(TARGETS) *.o *.os
	rm -rf $(LSB_OBJ_DIR)

distclean: clean
	rm -f config-host.* config.*

uninstall: uninstall.player uninstall.wrapper uninstall.viewer uninstall.libnoxshm uninstall.loader uninstall.config uninstall.dirs
uninstall.dirs:
	rmdir -p $(DESTDIR)$(nptargetdir) || :
	rmdir -p $(DESTDIR)$(nphostdir) || :
	rmdir -p $(DESTDIR)$(npcommondir) || :
uninstall.player:
	rm -f $(DESTDIR)$(nphostdir)/$(npplayer_PROGRAM)
uninstall.wrapper:
	rm -f $(DESTDIR)$(nphostdir)/$(npwrapper_LIBRARY)
uninstall.viewer:
	rm -f $(DESTDIR)$(nptargetdir)/$(npviewer_PROGRAM)
	rm -f $(DESTDIR)$(nptargetdir)/$(npviewer_PROGRAM:%.bin=%)
uninstall.libnoxshm:
	rm -f $(DESTDIR)$(nptargetdir)/$(libnoxshm_LIBRARY)
uninstall.loader:
	rm -f $(DESTDIR)$(npcommondir)/$(nploader_PROGRAM)
uninstall.config:
	rm -f $(DESTDIR)$(bindir)/nspluginwrapper
	rm -f $(DESTDIR)$(nphostdir)/$(npconfig_PROGRAM)
uninstall.mkruntime:
	rm -f $(DESTDIR)$(npcommondir)/mkruntime

install: install.dirs install.player install.wrapper install.viewer install.libnoxshm install.loader install.config
install.dirs:
	mkdir -p $(DESTDIR)$(npcommondir) || :
	mkdir -p $(DESTDIR)$(nphostdir) || :
	mkdir -p $(DESTDIR)$(nptargetdir) || :
ifeq ($(build_player),yes)
install.player: install.dirs $(npplayer_PROGRAM)
	$(INSTALL) -m 755 $(STRIP_OPT) $(npplayer_PROGRAM) $(DESTDIR)$(nphostdir)/$(npplayer_PROGRAM)
	mkdir -p $(DESTDIR)$(bindir)
	$(LN_S) $(nphostdir)/$(npplayer_PROGRAM) $(DESTDIR)$(bindir)/nspluginplayer
else
install.player:
endif
install.wrapper: install.dirs $(npwrapper_LIBRARY)
	$(INSTALL) -m 755 $(STRIP_OPT) $(npwrapper_LIBRARY) $(DESTDIR)$(nphostdir)/$(npwrapper_LIBRARY)
ifeq ($(build_viewer),yes)
install.viewer: install.dirs install.viewer.bin install.viewer.glue
install.libnoxshm: install.dirs do.install.libnoxshm
else
install.viewer:
install.libnoxshm:
endif
install.viewer.bin: install.dirs $(npviewer_PROGRAM)
	$(INSTALL) -m 755 $(STRIP_OPT) $(npviewer_PROGRAM) $(DESTDIR)$(nptargetdir)/$(npviewer_PROGRAM)
install.viewer.glue:: install.dirs
	p=$(DESTDIR)$(nptargetdir)/$(npviewer_PROGRAM:%.bin=%);	\
	echo "#!/bin/sh" > $$p;								\
	echo "TARGET_OS=$(TARGET_OS)" >> $$p;						\
	echo "TARGET_ARCH=$(TARGET_ARCH)" >> $$p;					\
	echo ". $(npcommondir)/$(nploader_PROGRAM)" >> $$p;			\
	chmod 755 $$p
do.install.libnoxshm: install.dirs $(libnoxshm_LIBRARY)
	$(INSTALL) -m 755 $(STRIP_OPT) $(libnoxshm_LIBRARY) $(DESTDIR)$(nptargetdir)/$(libnoxshm_LIBRARY)
install.config: install.dirs $(npconfig_PROGRAM)
	$(INSTALL) -m 755 $(STRIP_OPT) $(npconfig_PROGRAM) $(DESTDIR)$(nphostdir)/$(npconfig_PROGRAM)
	mkdir -p $(DESTDIR)$(bindir)
	$(LN_S) $(nphostdir)/$(npconfig_PROGRAM) $(DESTDIR)$(bindir)/nspluginwrapper
install.loader: install.dirs $(nploader_PROGRAM)
	$(INSTALL) -m 755 $(nploader_PROGRAM) $(DESTDIR)$(npcommondir)/$(nploader_PROGRAM)
install.mkruntime: install.dirs $(SRC_PATH)/utils/mkruntime.sh
	$(INSTALL) -m 755 $< $(DESTDIR)$(npcommondir)/mkruntime

$(npwrapper_LIBRARY): $(npwrapper_OBJECTS)
	$(CC) $(DSO_LDFLAGS) $(npwrapper_LDFLAGS) -o $@ $(npwrapper_OBJECTS) $(npwrapper_LIBS)

npwrapper-%.os: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(PIC_CFLAGS) $(CPPFLAGS) $(npwrapper_CFLAGS) -DBUILD_WRAPPER

$(npviewer_PROGRAM): $(npviewer_OBJECTS) $(npviewer_MAPFILE) $(LSB_OBJ_DIR) $(LSB_LIBS)
	$(CC) $(npviewer_LDFLAGS) -o $@ $(npviewer_OBJECTS) $(npviewer_LIBS)

npviewer-%.o: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(CPPFLAGS) $(npviewer_CFLAGS) -DBUILD_VIEWER

npviewer-%.o: $(SRC_PATH)/src/%.cpp
	$(CXX) -o $@ -c $< $(CPPFLAGS) $(npviewer_CFLAGS) -DBUILD_VIEWER

$(npplayer_PROGRAM): $(npplayer_OBJECTS) $(npplayer_MAPFILE) $(LSB_OBJ_DIR) $(LSB_LIBS)
	$(CC) $(npplayer_LDFLAGS) -o $@ $(npplayer_OBJECTS) $(npplayer_LIBS)

npplayer-%.o: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(CPPFLAGS) $(npplayer_CFLAGS) -DBUILD_PLAYER
npplayer-%.o: $(SRC_PATH)/src/tidy/%.c
	$(CC) -o $@ -c $< $(CPPFLAGS) $(npplayer_CFLAGS) -DBUILD_PLAYER

$(libnoxshm_LIBRARY): $(libnoxshm_OBJECTS) $(LSB_OBJ_DIR) $(LSB_LIBS)
	$(CC) $(DSO_LDFLAGS) $(libnoxshm_LDFLAGS) -o $@ $(libnoxshm_OBJECTS) -Wl,$(LD_soname),libnoxshm.so

libnoxshm-%.o: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(CPPFLAGS) $(libnoxshm_CFLAGS)

$(npconfig_PROGRAM): $(npconfig_OBJECTS)
	$(CC) $(npconfig_LDFLAGS) -o $@ $(npconfig_OBJECTS) $(npconfig_LIBS)

npconfig-%.o: $(SRC_PATH)/src/%.c
	$(CC) $(npconfig_CFLAGS) -o $@ -c $< $(CPPFLAGS)

$(nploader_PROGRAM): $(nploader_SOURCES)
	sed -e 's|%NPW_VIEWER_DIR%|$(nptargetdir_var)|' $< > $@
	chmod 755 $@

$(LSB_OBJ_DIR):
	@[ -d $(LSB_OBJ_DIR) ] || mkdir $(LSB_OBJ_DIR) > /dev/null 2>&1

$(LSB_OBJ_DIR)/%.o: $(LSB_SRC_DIR)/%.c | $(LSB_OBJ_DIR)
	$(CC) $(CFLAGS_32) -nostdinc -fno-builtin -I. -I$(LSB_INC_DIR) -c $< -o $@

$(LSB_OBJ_DIR)/%.a: $(LSB_OBJ_DIR)/%.o | $(LSB_OBJ_DIR)
	$(AR) rc $@ $<

$(LSB_OBJ_DIR)/libc.so: $(LSB_OBJ_DIR)/libc_main.so $(LSB_OBJ_DIR)/libc_nonshared.a | $(LSB_OBJ_DIR)
	@echo "OUTPUT_FORMAT($(TARGET_ELF_ARCH))" > $@
	@echo "GROUP ( $(LSB_OBJ_DIR)/libc_main.so $(LSB_OBJ_DIR)/libc_nonshared.a )" >> $@

$(LSB_OBJ_DIR)/libgcc_s_32.so: $(LSB_OBJ_DIR)/libgcc_s.so | $(LSB_OBJ_DIR)
	$(LN_S) libgcc_s.so $@

$(LSB_OBJ_DIR)/%.so: $(LSB_OBJ_DIR)/%.o | $(LSB_OBJ_DIR)
	$(CC) $(LDFLAGS_32) -nostdlib $(DSO_LDFLAGS) $< -o $@ \
		-Wl,--version-script,$(patsubst $(LSB_OBJ_DIR)/%.o,$(LSB_SRC_DIR)/%.Version,$<) \
		-Wl,-soname,`grep "$(patsubst $(LSB_OBJ_DIR)/%.o,%,$<) " $(LSB_SRC_DIR)/LibNameMap.txt | cut -f2 -d' '`

test-rpc-%-client: test-rpc-%-client.o $(test_rpc_client_OBJECTS)
	$(CC) $(test_rpc_LDFLAGS) -o $@ $< $(test_rpc_client_OBJECTS) $(test_rpc_LIBS)
test-rpc-%-client.o: $(SRC_PATH)/tests/test-rpc-%.c
	$(CC) -o $@ -c $< $(test_rpc_client_CPPFLAGS) $(test_rpc_CFLAGS)
%-client.o: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(test_rpc_client_CPPFLAGS) $(test_rpc_CFLAGS)
test-rpc-%-server: test-rpc-%-server.o $(test_rpc_server_OBJECTS)
	$(CC) $(test_rpc_LDFLAGS) -o $@ $< $(test_rpc_server_OBJECTS) $(test_rpc_LIBS)
test-rpc-%-server.o: $(SRC_PATH)/tests/test-rpc-%.c
	$(CC) -o $@ -c $< $(test_rpc_server_CPPFLAGS) $(test_rpc_CFLAGS)
%-server.o: $(SRC_PATH)/src/%.c
	$(CC) -o $@ -c $< $(test_rpc_server_CPPFLAGS) $(test_rpc_CFLAGS)
