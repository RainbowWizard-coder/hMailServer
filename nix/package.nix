# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The server as a Nix package, built from the tree with the CMake build that
# build/ci/linux-build.sh runs (hmailserver/docs/Nix.md). It is a distribution's
# build, as the Debian source package and the RPM spec are: the live update off,
# which in a Nix store could replace nothing, naming the command that updates it;
# the distribution's shared Boost (HM_BOOST_STATIC OFF); and the tester programs run
# in checkPhase.
#
# Two outputs: out, the server, and cli, hmctl, which is Python and so kept out of
# the server's closure, as the Debian source package keeps it in hmailserver-cli.
{ lib
, stdenv
, cmake
, ninja
, boost
, openssl
, zlib
, libpq
, openldap
, mariadb-connector-c
, python3
, src
, version
  # What the update check names, since this build cannot update itself: the step
  # that takes a newer release into a NixOS configuration whose flake names this
  # one as its input "hmailserver" (Nix.md, "Updating"). HM_UPDATE_COMMAND holds
  # letters, digits, spaces and ._/:=+@,~- only (CMakeLists.txt).
, updateCommand ? "nix flake update hmailserver"
}:

stdenv.mkDerivation {
  pname = "hmailserver";
  inherit version src;

  outputs = [ "out" "cli" ];

  nativeBuildInputs = [ cmake ninja ];
  buildInputs = [ boost openssl zlib libpq openldap python3 ];

  # The CMake project is hmailserver/source/Server; it reads the tree above it
  # (DBScripts, WebAdmin, Extras, LICENSE), so the whole source is unpacked and the
  # build directory is build/ at its root.
  cmakeDir = "../hmailserver/source/Server";
  cmakeFlags = [
    "-DHM_LIVE_UPDATE=OFF"
    "-DHM_BOOST_STATIC=OFF"
  ];
  # A flag with spaces in it goes through the array, which the CMake hook passes
  # word for word; cmakeFlags would be split at the spaces.
  preConfigure = ''
    cmakeFlagsArray+=(${lib.escapeShellArg "-DHM_UPDATE_COMMAND=${updateCommand}"})
  '';

  # First the server itself, which has to get as far as printing its version (every
  # static constructor runs before main), then every tester program the build
  # makes beside it, found rather than listed, as linux-build.sh finds them, so a
  # new one runs here the day it is added. None needs a database or the network;
  # network-deadline-tests connects to peers of its own on 127.0.0.1 and writes a
  # configuration of its own under $TMPDIR, and http-server-tests answers itself
  # on 127.0.0.1, which the sandbox's loopback and its temporary directory give
  # them.
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    echo "== hmailserver --version"
    ./hmailserver --version
    testers=$(find . -maxdepth 1 -type f -name '*-tests' -perm -u+x | sort)
    if [ -z "$testers" ]; then
      echo "no *-tests program in the build directory: the CMake build no longer makes the testers it did"
      exit 1
    fi
    for tester in $testers; do
      echo "== $tester"
      "$tester"
    done
    runHook postCheck
  '';

  # The CMake install writes the configuration, the logrotate rule, the fail2ban
  # filter and jail and the systemd unit to absolute paths (/etc, /usr/lib/systemd),
  # which a Nix build cannot write. So it installs into a staging directory, and
  # $out takes the prefix's part, and the absolute part as examples: the
  # configuration under $out/etc with ProgramFolder pointing into $out, the rest as
  # they are. The unit is left out: it names /usr/bin/hmailserver, and the NixOS
  # module declares its own (nix/module.nix).
  installPhase = ''
    runHook preInstall
    DESTDIR="$PWD/stage" cmake --install .
    mkdir -p "$out" "$cli/bin"
    cp -a "stage$out/." "$out/"
    mv "$out/bin/hmctl" "$cli/bin/hmctl"
    # CMake's FILES_MATCHING install of DBScripts leaves the one subdirectory that
    # holds no .sql behind, empty.
    find "$out/share/hmailserver" -type d -empty -delete
    mkdir -p "$out/etc"
    cp -a stage/etc/. "$out/etc/"
    substituteInPlace "$out/etc/hmailserver/hMailServer.ini" \
      --replace-fail "ProgramFolder=/usr/share/hmailserver" "ProgramFolder=$out/share/hmailserver"
    runHook postInstall
  '';

  # The MySQL and MariaDB client is opened with dlopen("libmariadb.so.3") when such
  # a database is configured, and is not linked (MySQLInterface.cpp). A Nix binary
  # finds a library only on its RUNPATH, so the connector's directory is added to
  # it - after the fixup, whose RUNPATH shrinking would take out an entry no linked
  # library needs.
  postFixup = ''
    patchelf --add-rpath ${lib.getLib mariadb-connector-c}/lib/mariadb "$out/bin/hmailserver"
  '';

  meta = {
    description = "Mail server for SMTP, IMAP and POP3";
    longDescription = ''
      hMailServer is a mail server for SMTP, IMAP and POP3, with anti-spam, DKIM,
      DMARC and ARC, a REST API and a web administration page. This is its Linux
      server, which keeps its settings and mail index in PostgreSQL or MySQL/MariaDB.
    '';
    homepage = "https://gitlab.com/hmailserver/hmailserver";
    changelog = "https://gitlab.com/hmailserver/hmailserver/-/blob/v${version}/CHANGELOG.md";
    # The server's own licence, and those of what is compiled into it: QuickJS
    # (MIT), the MIME coder (BSD-1-Clause), the Public Suffix List's rules
    # (MPL-2.0); CStdString's notice is permissive and has no identifier
    # (hmailserver/source/Server/platform/packaging/debian-src/debian/copyright).
    license = with lib.licenses; [ agpl3Plus mit bsd1 mpl20 ];
    platforms = lib.platforms.linux;
    mainProgram = "hmailserver";
  };
}
