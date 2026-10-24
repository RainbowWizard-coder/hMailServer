# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The project's own Nix flake (hmailserver/docs/Nix.md): the Linux server as a Nix
# package, a NixOS module that runs it, and a NixOS test that starts it against
# PostgreSQL. Nothing is submitted anywhere for it to be used: a flake in the
# project's repository is a channel of its own.
#
#   nix build gitlab:hmailserver/hmailserver/v<version>     the server of a release
#   nix run gitlab:hmailserver/hmailserver -- --version
#   nix build .#hmailserver                                  from a checkout
#   nix build .#checks.x86_64-linux.nixos                    the NixOS test
#
# nixpkgs is NixOS's own release tarball from releases.nixos.org, where
# channels.nixos.org's nixos-26.05 pointed on 29 September 2026 - never
# github:NixOS/nixpkgs, the usual reference, which the project may not download
# from - and flake.lock pins it by its NAR hash. It is taken as a plain source tree
# (flake = false) and imported, so neither nixpkgs' own flake.nix nor the flake
# registry, which names GitHub, is read. Build with the registry off:
#
#   nix --extra-experimental-features 'nix-command flakes' --option flake-registry '' build .#hmailserver
#
# The libraries are the distribution's (nixpkgs'): Boost, OpenSSL, libpq, OpenLDAP
# and zlib, and the MariaDB connector the server opens when a MySQL or MariaDB
# database is configured. QuickJS is compiled from the copy in the tree. The live
# update is off: Nix replaces the package, not the server (nix/package.nix).
{
  description = "Progressive Robot's fork of hMailServer, a mail server for SMTP, IMAP and POP3";

  inputs.nixpkgs = {
    url = "https://releases.nixos.org/nixos/26.05/nixos-26.05.10882.7fc6f2c20af0/nixexprs.tar.xz";
    flake = false;
  };

  outputs = { self, nixpkgs }:
    let
      lib = import (nixpkgs + "/lib");

      # Where the server is built and tested. aarch64-linux evaluates and builds as
      # x86_64-linux does; the pipeline builds and tests x86_64-linux (Nix.md).
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: lib.genAttrs systems f;

      # The one version, Version.h's, as the CMake build reads it.
      versionHeader = builtins.readFile ./hmailserver/source/Server/Common/Application/Version.h;
      version = builtins.head (builtins.match ".*#define HMAILSERVER_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" versionHeader);

      # What the CMake build under hmailserver/source/Server reads, and nothing else,
      # so that a change to the documents, the Windows tools or the CI does not make
      # a new package.
      src = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [
          ./LICENSE
          ./hmailserver/source/Server
          ./hmailserver/source/DBScripts
          ./hmailserver/source/Tools/Cli/hmctl
          ./hmailserver/installation/WebAdmin
          ./hmailserver/installation/Extras/dh2048.pem
          # Feather's notice, which the install rules put among the licences; a
          # path literal cannot hold its name's spaces.
          (./hmailserver/docs/Licenses + "/License - Feather icons.txt")
        ];
      };

      overlay = final: prev: {
        hmailserver = final.callPackage ./nix/package.nix { inherit src version; };
      };

      pkgsFor = system: import nixpkgs {
        inherit system;
        overlays = [ overlay ];
      };
    in
    {
      overlays.default = overlay;

      packages = forAllSystems (system:
        let pkgs = pkgsFor system;
        in {
          hmailserver = pkgs.hmailserver;
          default = pkgs.hmailserver;
        });

      # services.hmailserver, with this flake's package unless the configuration
      # names another (nix/module.nix).
      nixosModules.default = { lib, pkgs, ... }: {
        imports = [ ./nix/module.nix ];
        services.hmailserver.package =
          lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.hmailserver;
      };
      nixosModules.hmailserver = self.nixosModules.default;

      checks = forAllSystems (system:
        let pkgs = pkgsFor system;
        in {
          hmailserver = pkgs.hmailserver;
          # A NixOS machine with the module and PostgreSQL, whose SMTP and IMAP are
          # asked for their greetings, as the Debian package's autopkgtest asks
          # (nix/test.nix).
          nixos = pkgs.testers.runNixOSTest (import ./nix/test.nix { inherit self; });
        });
    };
}
