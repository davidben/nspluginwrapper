%define name	nspluginwrapper
%define version	0.9.91.1
%define release	1
#define svndate 20061227

# define 32-bit arch of multiarch platforms
%define arch_32 %{nil}
%ifarch x86_64
%define arch_32 i386
%endif
%ifarch ppc64
%define arch_32 ppc
%endif
%ifarch sparc64
%define arch_32 sparc
%endif

# define target architecture of plugins we want to support
%define target_arch i386

# define target operating system of plugins we want to support
%define target_os linux

# define nspluginswrapper libdir (invariant, including libdir)
%define pkglibdir %{_prefix}/lib/%{name}

# define mozilla plugin dir
%define plugindir %{_libdir}/mozilla/plugins

# define to build a biarch package
# XXX really build one package and handle upgrades
%define build_biarch		0
%if "%{_arch}:%{arch_32}" == "x86_64:i386"
%define build_biarch		1
%endif
%{expand: %{?_with_biarch:	%%global build_biarch 1}}
%{expand: %{?_without_biarch:	%%global build_biarch 0}}

Summary:	A compatibility layer for Netscape 4 plugins
Name:		%{name}
Version:	%{version}
Release:	%{release}
Source0:	%{name}-%{version}%{?svndate:-%{svndate}}.tar.bz2
License:	GPL
Group:		Networking/WWW
Url:		http://gwenole.beauchesne.info/projects/nspluginwrapper/
BuildRequires:	gtk+2-devel
Provides:	%{name}-%{_arch} = %{version}-%{release}
Requires:	%{name}-%{target_arch} = %{version}-%{release}
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-buildroot

%description
nspluginwrapper makes it possible to use Netscape 4 compatible plugins
compiled for %{target_arch} into Mozilla for another architecture, e.g. x86_64.

This package consists in:
  * npviewer: the plugin viewer
  * npwrapper.so: the browser-side plugin
  * nspluginwrapper: a tool to manage plugins installation and update

%if %{build_biarch}
%package %{target_arch}
Summary:	A viewer for %{target_arch} compiled Netscape 4 plugins
Group:		Networking/WWW

%description %{target_arch}
nspluginwrapper makes it possible to use Netscape 4 compatible plugins
compiled for %{target_arch} into Mozilla for another architecture, e.g. x86_64.

This package consists in:
  * npviewer: the plugin viewer
  * npwrapper.so: the browser-side plugin
  * nspluginwrapper: a tool to manage plugins installation and update

This package provides the npviewer program for %{target_arch}.
%endif

%prep
%setup -q

%build
%if %{build_biarch}
biarch="--with-biarch"
%else
biarch="--without-biarch"
%endif
mkdir objs
pushd objs
../configure --prefix=%{_prefix} $biarch
make
popd

%install
rm -rf $RPM_BUILD_ROOT

make -C objs install DESTDIR=$RPM_BUILD_ROOT

mkdir -p $RPM_BUILD_ROOT%{plugindir}
ln -s %{pkglibdir}/%{_arch}/%{_os}/npwrapper.so $RPM_BUILD_ROOT%{plugindir}/npwrapper.so

%clean
rm -rf $RPM_BUILD_ROOT

%post
if [ $1 = 1 ]; then
  %{_bindir}/%{name} -v -a -i
else
  %{_bindir}/%{name} -v -a -u
fi

%preun
if [ $1 = 0 ]; then
  %{_bindir}/%{name} -v -a -r
fi

%files
%defattr(-,root,root)
%doc README COPYING NEWS
%{_bindir}/%{name}
%{plugindir}/npwrapper.so
%dir %{pkglibdir}
%dir %{pkglibdir}/noarch
%{pkglibdir}/noarch/npviewer
%{pkglibdir}/noarch/mkruntime
%dir %{pkglibdir}/%{_arch}
%dir %{pkglibdir}/%{_arch}/%{_os}
%{pkglibdir}/%{_arch}/%{_os}/npconfig
%if ! %{build_biarch}
%{pkglibdir}/%{_arch}/%{_os}/npviewer
%{pkglibdir}/%{_arch}/%{_os}/npviewer.bin
%{pkglibdir}/%{_arch}/%{_os}/libxpcom.so
%endif
%{pkglibdir}/%{_arch}/%{_os}/npwrapper.so

%if %{build_biarch}
%files %{target_arch}
%defattr(-,root,root)
%dir %{pkglibdir}/%{target_arch}
%dir %{pkglibdir}/%{target_arch}/%{target_os}
%{pkglibdir}/%{target_arch}/%{target_os}/npviewer
%{pkglibdir}/%{target_arch}/%{target_os}/npviewer.bin
%{pkglibdir}/%{target_arch}/%{target_os}/libxpcom.so
%endif

%changelog
* Tue Dec 26 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.91.1-1
- fix NPRuntime bridge (VLC plugin)
- fix Mozilla plugins dir creation on NetBSD and FreeBSD hosts
- fix potential buffer overflow in RPC marshalers
- handle empty args for plugin creation (flasharcade.com)

* Thu Dec 21 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.91-1
- add scripting support through npruntime
- add XEMBED support (mplayer plug-in)
- add NPN_RequestRead() support (Acrobat Reader)
- add support for NetBSD, FreeBSD and non-x86 Linux hosts
- fix ppc64 / ppc32 support
- fix focus problems
- fix some rare hangs (add delayed requests)
- fix libstdc++2 compat glue for broken plugins
- create user mozilla plugins dir if it does not exist yet

* Wed Nov 18 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.90.4-1
- add printing support (NPP_Print)
- add initial support for Konqueror
- fix post data to a URL (NPN_PostURL, NPN_PostURLNotify)
- reduce plugin load times
- robustify error condition (Darryl L. Miles)

* Tue Sep 19 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.90.3-1
- fix acrobat reader 7 plugin

* Sun Sep 17 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.90.2-1
- use a bidirectional communication channel

* Sun Jun  4 2006 Gwenole Beauchesne <gb.public@free.fr> 0.9.90.1-1
- relicense under GPL
- don't use QEMU on IA-64
- handle SuSE Linux Mozilla paths
- portability fixes to non-Linux platforms

* Tue Oct 25 2005 Gwenole Beauchesne <gb.public@free.fr> 0.9.90-1
- first public beta version
