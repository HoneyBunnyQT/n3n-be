Summary: n3n-be peer-to-peer VPN
Name: n3n-be
Version: %{VERSION}
Release: 1
License: AGPL-3.0-only AND GPL-3.0-only AND GPL-2.0-only AND GPL-2.0-or-later AND LGPL-2.1-only
Group: Networking/Utilities
URL: https://github.com/HoneyBunnyQT/n3n-be
Source: n3n-be-%{version}.tgz
Packager: Honey Bunny QT <honeybunnyqt.official@proton.me>
# Temporary location where the RPM will be built
BuildRoot:  %{_tmppath}/%{name}-%{version}-root
Requires: libzstd

# Make sure .build-id is not part of the package
%define _build_id_links none

# n3n-be-networkmanager, the plugin of NetworkManager (nm/): built with
# "--with networkmanager", which packages/rpm/Makefile passes where libnm
# is there
%bcond_with networkmanager
%if %{with networkmanager}
BuildRequires: NetworkManager-libnm-devel
BuildRequires: libnma-devel
BuildRequires: libnma-gtk4-devel
BuildRequires: gtk3-devel
BuildRequires: gtk4-devel
%endif

%description
n3n peer-to-peer VPN: n3n-be (Bunny Edition), a fork of n3n.  Its
programs, configuration (/etc/n3n-be) and services have names of their
own, so it installs next to n3n.

%if %{with networkmanager}
%package networkmanager
Summary: n3n-be as a VPN of NetworkManager: GNOME's Settings, nmcli
Requires: %{name} = %{version}-%{release}
Requires: NetworkManager

%description networkmanager
n3n-be as a VPN of NetworkManager: set up in GNOME's Settings or with
nmcli, switched on in the top bar, the key in the system's or the user's
keyring; and a GNOME Shell extension with the peers in the quick
settings, the exits among them marked, one to route all traffic
through chosen there.  See docs/quick_start/GNOME.md.
%endif

%prep

# This is expecting to build from the checked-out source, so there is no prep

%build

cd %TOPDIR
./autogen.sh
%configure %{?with_networkmanager:--with-network-manager}
%make_build

%install

cd %TOPDIR
# where the system has its sbin: /usr/bin since Fedora 42
%make_install CONFIG_SBINDIR=$RPM_BUILD_ROOT%{_sbindir}
chmod u+w $RPM_BUILD_ROOT%{_sbindir}/*

#find $RPM_BUILD_ROOT -name ".git" | xargs /bin/rm -rf
#find $RPM_BUILD_ROOT -name ".svn" | xargs /bin/rm -rf
#find $RPM_BUILD_ROOT -name "*~"   | xargs /bin/rm -f

%if %{with networkmanager}
%files networkmanager
%{_libexecdir}/nm-n3n-be-service
%{_libexecdir}/nm-n3n-be-auth-dialog
%{_libdir}/NetworkManager/libnm-vpn-plugin-n3n-be.so
%{_libdir}/NetworkManager/libnm-vpn-plugin-n3n-be-editor.so
%{_libdir}/NetworkManager/libnm-gtk4-vpn-plugin-n3n-be-editor.so
/usr/lib/NetworkManager/VPN/nm-n3n-be-service.name
/usr/share/dbus-1/system.d/nm-n3n-be-service.conf
/usr/share/gnome-shell/extensions/n3n-be@honeybunnyqt.github.io/
%endif

%clean
# Clean out our build directory
rm -fr $RPM_BUILD_ROOT

%files
/usr/bin/n3n-be-ctl
/usr/lib/n3n-be/migrate
/usr/lib/systemd/system/n3n-be-edge.service
/usr/lib/systemd/system/n3n-be-edge@.service
/usr/lib/systemd/system/n3n-be-supernode.service
/usr/lib/sysusers.d/n3n-be.conf
%{_sbindir}/n3n-be
%{_sbindir}/n3n-be-edge
%{_sbindir}/n3n-be-supernode
/usr/share/doc/n3n-be/Contributing.md
/usr/share/doc/n3n-be/FAQ.md
/usr/share/doc/n3n-be/LICENSE.md
/usr/share/doc/n3n-be/README.md
/usr/share/doc/n3n-be/SECURITY.md
/usr/share/doc/n3n-be/Scripts.md
/usr/share/doc/n3n-be/Tools.md
/usr/share/doc/n3n-be/community.list.sample
/usr/share/doc/n3n-be/edge.conf.sample
/usr/share/doc/n3n-be/supernode.conf.sample
/usr/share/man/man7/n3n-be.7.gz
/usr/share/man/man8/n3n-be.8.gz
/usr/share/man/man8/n3n-be-edge.8.gz
/usr/share/man/man8/n3n-be-supernode.8.gz

# Set the default attributes of all of the files specified to have an
# owner and group of root and to inherit the permissions of the file
# itself.
%defattr(-, root, root)

%changelog
* Sun Oct 31 2021 Hamish Coleman <hamish@zot.org> 3.1.0
- Last stable release

# Execution order:
# install:    pre -> (copy) -> post
# upgrade:    pre -> (copy) -> post -> preun (old) -> (delete old) -> postun (old) -> posttrans
# un-install:                          preun       -> (delete)     -> postun

%pre

if ! getent group n3n-be > /dev/null 2>&1; then
  echo 'Creating n3n-be group'
  /usr/sbin/groupadd -r n3n-be
fi

if ! /usr/bin/id -u n3n-be > /dev/null 2>&1; then
  echo 'Creating n3n-be user'
  /usr/sbin/useradd -M -N -g n3n-be -r -s /bin/false n3n-be
fi

# an upgrade: once the old package is gone, %posttrans carries over what it
# had under n3n's names (/usr/lib/n3n-be/migrate)
if [ "$1" -gt 1 ]; then
  mkdir -p /var/lib/n3n-be
  touch /var/lib/n3n-be/upgrading
fi

%post
if [ -f /bin/systemctl ]; then
  if [ ! -f /.dockerenv ]; then
      /bin/systemctl daemon-reload
      # NOTE: do not enable any services during first installation
  fi
fi

%preun
if [ -f /bin/systemctl ]; then
  if [ ! -f /.dockerenv ]; then
      # possibly remove the installed services
      %systemd_preun n3n-be-supernode.service n3n-be-edge.service 'n3n-be-edge@*.service'
  fi
fi

%postun
if [ -f /bin/systemctl ]; then
  if [ ! -f /.dockerenv ]; then
      # possibly restart the running services
      %systemd_postun_with_restart n3n-be-supernode.service n3n-be-edge.service 'n3n-be-edge@*.service'
  fi
fi

%posttrans
if [ -e /var/lib/n3n-be/upgrading ]; then
  /usr/lib/n3n-be/migrate || true
  rm -f /var/lib/n3n-be/upgrading
fi
