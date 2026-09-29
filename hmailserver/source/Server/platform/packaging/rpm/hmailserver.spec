# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The RPM source package for Fedora, EL and openSUSE (hmailserver/docs/SourcePackages.md):
# built from the release's source tarball as a distribution builds it - without
# the live update, which the distribution's package manager stands in for, naming
# its command instead, and against the distribution's shared Boost rather than
# the static archives the project's own .rpm carries. Nothing is fetched while it
# builds. Source1 is rpm/hmailserver.sysusers beside this file in the tarball.
#
# Version and the %%changelog entry are the release's, stamped with Version.h
# (build/check-linux-version-stamp.sh holds them to it).

# The CMake project is not at the top of the tarball. openSUSE's %%cmake builds in
# ./build unless told otherwise, and the tarball's own build/ is the project's
# scripts, so the build directory is named apart there.
%global _vpath_srcdir hmailserver/source/Server
%if 0%{?suse_version}
%global __sourcedir hmailserver/source/Server
%global __builddir _build
%global __builder ninja
%global hm_builddir _build
%global hm_update_command zypper update hmailserver
%else
%global hm_builddir %{__cmake_builddir}
%global hm_update_command dnf upgrade hmailserver
%endif
%global quickjs_version 2025.04.26
# No link-time optimisation, for memory: six programs link the whole static core,
# and with LTO the linker alone held 3.0 GB on the laptop, besides the LTRANS
# compilers it starts, where Fedora 44's link without it peaks at 1.5 GB; the CI
# jobs have 8 GB. Not for a GCC 16 defect: the one failed LTO link
# ("ltrans9.o: section ... is missing") was on a laptop whose file system was then
# found to lose written data (hmailserver/docs/SourcePackages.md), so GCC 16's LTO
# link of the server is untested, not known to fail.
%global _lto_cflags %{nil}

Name:           hmailserver
Version:        6.3.5
Release:        1%{?dist}
Summary:        Mail server for SMTP, IMAP and POP3
# The server is the project's (AGPL-3.0-or-later); compiled into it are QuickJS
# (MIT), the MIME coder (BSD-1-Clause), CStdString (its own permissive notice)
# and the Public Suffix List's rules (MPL-2.0); tlds.txt, SURBL's 2006 list of
# two-level country-code domains, states no licence.
License:        AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND LicenseRef-Fedora-UltraPermissive
URL:            https://gitlab.com/Progressiverobot/hmailserver
Source0:        https://gitlab.com/api/v4/projects/86730559/packages/generic/hmailserver/%{version}/hmailserver-%{version}.tar.gz
Source1:        hmailserver.sysusers
%if 0%{?suse_version}
Group:          Productivity/Networking/Email/Servers
%endif

BuildRequires:  cmake >= 3.22
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(ldap)
BuildRequires:  pkgconfig(libpq)
BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(zlib)
BuildRequires:  systemd-rpm-macros
%if 0%{?suse_version}
BuildRequires:  libboost_chrono-devel >= 1.83
BuildRequires:  libboost_filesystem-devel >= 1.83
BuildRequires:  libboost_headers-devel >= 1.83
BuildRequires:  libboost_thread-devel >= 1.83
BuildRequires:  ninja
BuildRequires:  sysuser-tools
%{?sysusers_requires}
%else
BuildRequires:  boost-devel >= 1.83
BuildRequires:  ninja-build
%endif
%{?systemd_requires}
%if (0%{?fedora} && 0%{?fedora} < 42) || (0%{?rhel} && 0%{?rhel} < 11)
%{?sysusers_requires_compat}
%endif
Requires(post): util-linux
# /etc/logrotate.d/hmailserver rotates the server's logs.
Requires:       logrotate

# QuickJS, vendored unmodified under hmailserver/source/Server/quickjs and linked
# into the server: no distribution this builds for ships it as a library.
Provides:       bundled(quickjs) = %{quickjs_version}
Recommends:     %{name}-cli = %{version}-%{release}
Suggests:       p7zip
Suggests:       postgresql-server
%if 0%{?suse_version}
Suggests:       libmariadb3
%else
Suggests:       mariadb-connector-c
%endif

%description
hMailServer is a mail server: SMTP for sending and receiving, IMAP and
POP3 for reading, with SPF, DKIM, DMARC and ARC, DNS block lists and
gray listing, TLS with certificates from ACME, and a REST API with a
browser-based administration page, the Control Deck. Its accounts,
messages' metadata and settings are kept in PostgreSQL, or in MySQL or
MariaDB through the MariaDB client library, which it loads when it is
configured to.

This is a maintained fork of the Windows mail server of that name,
ported to Linux. This build has no live update of its own: the package
manager updates it, and the server's update check names that command.

%package cli
Summary:        Command-line client for hMailServer's REST API
%if 0%{?suse_version}
Group:          Productivity/Networking/Email/Servers
%endif
BuildArch:      noarch
Requires:       python3

%description cli
hmctl administers an hMailServer over its REST API - domains, accounts,
aliases, settings, the queue, certificates and the rest - from a shell, on the
server itself or on another machine, Linux or Windows. It is one Python 3 file
that needs nothing beyond the standard library.

%prep
%autosetup -p1
# What the project's tarball already leaves out (build/source-exclusions.txt), so
# that a source made another way - a git archive of the tag - builds from the
# same files (hmailserver/docs/SourceTarball.md). On the project's tarball it
# removes nothing.
while IFS= read -r path; do
  case "$path" in
    '' | '#'*) ;;
    *) rm -rf -- "$path" ;;
  esac
done < build/source-exclusions.txt

%build
%if 0%{?suse_version}
%sysusers_generate_pre %{SOURCE1} hmailserver hmailserver.conf
%else
# Ninja on EL as on Fedora, whose %%cmake already passes -G Ninja: EL 10's writes
# Makefiles, which know nothing of the link pool below and would run as many
# links at once as -j allows (seen on AlmaLinux 10, 29 September 2026). A -G on
# the command line, Fedora's, overrides this.
export CMAKE_GENERATOR=Ninja
%endif
# One link at a time: six programs link the whole static core, and with debug
# information each link takes 1.5 GB (3 GB with LTO); several at once ran a 10 GB
# machine out of memory. The compilers still run in parallel.
%cmake \
  -DHM_LIVE_UPDATE=OFF \
  "-DHM_UPDATE_COMMAND=%{hm_update_command}" \
  -DHM_BOOST_STATIC=OFF \
  -DCMAKE_JOB_POOLS=link=1 \
  -DCMAKE_JOB_POOL_LINK=link
%cmake_build

%install
%cmake_install
install -D -p -m 0644 %{SOURCE1} %{buildroot}%{_sysusersdir}/hmailserver.conf
# hmctl names the interpreter python3 installs, not env's search for one.
sed -i '1s|^#!/usr/bin/env python3$|#!/usr/bin/python3|' %{buildroot}%{_bindir}/hmctl
install -d -m 0750 %{buildroot}%{_localstatedir}/lib/hmailserver
install -d -m 0750 %{buildroot}%{_localstatedir}/lib/hmailserver/temp
install -d -m 0750 %{buildroot}%{_localstatedir}/lib/hmailserver/events
install -d -m 0750 %{buildroot}%{_localstatedir}/lib/hmailserver/database
install -d -m 0750 %{buildroot}%{_localstatedir}/log/hmailserver

%check
# First the server itself, which has to get as far as printing its version: every
# static constructor runs before main, so a server that cannot start fails the
# build here and not at its first start after installation. The testers alone let
# through, on 29 September 2026, a server that died before main (a linked file the
# build machine's file system had corrupted; SourcePackages.md).
# Then the tester programs the build makes beside the server: the RFC 7208 suite and
# the SPF testers, the JavaScript binding, the SNMP agent, the object store's
# request signatures, the singletons, the legacy Blowfish scheme, the backup
# index's XML, and the date class with the date parsers built on it - the list
# debian/rules runs. No socket, no configuration, no database; seconds.
cd %{hm_builddir}
echo "== hmailserver --version"
./hmailserver --version
for tester in spf-tests script-binding-tests snmp-tests object-store-signature-tests singleton-tests legacy-blowfish-tests xml-tree-tests datetime-tests; do
  echo "== $tester"
  ./$tester
done

# The account: rpm makes it from the sysusers.d file in the payload before the
# files that name it go in, on Fedora 42 and later; elsewhere %%pre does.
%if 0%{?suse_version}
%pre -f hmailserver.pre
%service_add_pre hmailserver.service
%endif
%if (0%{?fedora} && 0%{?fedora} < 42) || (0%{?rhel} && 0%{?rhel} < 11)
%pre
%sysusers_create_compat %{SOURCE1}
%endif

%post
%if 0%{?suse_version}
%service_add_post hmailserver.service
%else
%systemd_post hmailserver.service
%endif
# An upgrade of a configured server: the schema brought to this build's before
# %%postun restarts a server that was running (platform/packaging/rpm/post does the
# same for the project's own .rpm).
if [ "$1" -gt 1 ] && grep -q '^Type=[A-Za-z]' %{_sysconfdir}/hmailserver/hMailServer.ini 2> /dev/null; then
  if ! runuser -u hmailserver -- %{_bindir}/hmailserver --upgrade-database; then
    echo "hmailserver: the database schema was not upgraded; run" >&2
    echo "   sudo -u hmailserver hmailserver --upgrade-database" >&2
    echo "and read what it says before starting the service." >&2
  fi
fi

%preun
%if 0%{?suse_version}
%service_del_preun hmailserver.service
%else
%systemd_preun hmailserver.service
%endif

%postun
%if 0%{?suse_version}
%service_del_postun hmailserver.service
%else
%systemd_postun_with_restart hmailserver.service
%endif

%files
%license LICENSE
%doc hmailserver/source/Server/platform/packaging/README.md
%{_bindir}/hmailserver
%{_bindir}/tlds.txt
%{_bindir}/dh2048.pem
%dir %{_prefix}/lib/hmailserver
%{_prefix}/lib/hmailserver/autoban-hook
%{_unitdir}/hmailserver.service
%{_sysusersdir}/hmailserver.conf
%dir %{_datadir}/hmailserver
%{_datadir}/hmailserver/DBScripts
%{_datadir}/hmailserver/WebAdmin
%dir %{_datadir}/hmailserver/licenses
%license %{_datadir}/hmailserver/licenses/QuickJS.txt
%license %{_datadir}/hmailserver/licenses/AGPL-3.0.txt
%dir %attr(0750,root,hmailserver) %{_sysconfdir}/hmailserver
%config(noreplace) %attr(0640,root,hmailserver) %{_sysconfdir}/hmailserver/hMailServer.ini
%config(noreplace) %{_sysconfdir}/logrotate.d/hmailserver
%dir %{_sysconfdir}/fail2ban
%dir %{_sysconfdir}/fail2ban/filter.d
%dir %{_sysconfdir}/fail2ban/jail.d
%config(noreplace) %{_sysconfdir}/fail2ban/filter.d/hmailserver.conf
%config(noreplace) %{_sysconfdir}/fail2ban/jail.d/hmailserver.conf
%dir %attr(0750,hmailserver,hmailserver) %{_localstatedir}/lib/hmailserver
%dir %attr(0750,hmailserver,hmailserver) %{_localstatedir}/lib/hmailserver/temp
%dir %attr(0750,hmailserver,hmailserver) %{_localstatedir}/lib/hmailserver/events
%dir %attr(0750,hmailserver,hmailserver) %{_localstatedir}/lib/hmailserver/database
%dir %attr(0750,hmailserver,hmailserver) %{_localstatedir}/log/hmailserver

%files cli
%license LICENSE
%{_bindir}/hmctl

%changelog
* Mon Sep 28 2026 Christopher Holloway <christopher.j.holloway@outlook.com> - 6.3.5-1
- The project's own spec, built from the release's source tarball with the live
  update off and the distribution's shared Boost (hmailserver/docs/SourcePackages.md).
