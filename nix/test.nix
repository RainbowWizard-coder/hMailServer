# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The NixOS test, checks.<system>.nixos (hmailserver/docs/Nix.md): one machine with
# services.hmailserver and the PostgreSQL it makes by default. It waits for the
# server, asks SMTP and IMAP for their greetings as the Debian package's
# autopkgtest does (nix/probe.sh), asks the REST API - turned on by the module's
# settings option, with the administrator password from adminPasswordFile - for
# its status, has root make the stored-secrets key as the packaging guide says, and
# asks again after a restart, which sets the configuration's directories and
# database anew over a database that is already there and must keep the key and a
# database password typed into the file by hand. Before that restart the service
# must not have been restarted once: its first start is ordered after PostgreSQL's
# setup, which makes its role. With autoBanFirewall, an auto-ban placed through the
# REST API reaches the firewall - the server runs the package's autoban-hook,
# which puts the address into its nftables set - and leaves it when the ban is
# deleted; the hook without the capability the option grants cannot. The flake
# builds it with pkgs.testers.runNixOSTest.
{ self }:

{
  name = "hmailserver";

  nodes.machine = { pkgs, ... }: {
    imports = [ self.nixosModules.default ];

    services.hmailserver = {
      enable = true;
      # A test's secret is in the store; a real one is not (Nix.md).
      adminPasswordFile = pkgs.writeText "hmailserver-admin-password" "nixos-test-admin\n";
      settings = {
        RestApiPort = 8045;
      };
      autoBanFirewall = true;
    };

    environment.systemPackages = [
      pkgs.curl
      pkgs.nftables
      (pkgs.writeShellScriptBin "hmailserver-probe" (builtins.readFile ./probe.sh))
    ];

    virtualisation.memorySize = 2048;
    documentation.enable = false;
  };

  testScript = ''
    import json

    machine.wait_for_unit("postgresql.service")
    machine.wait_for_unit("hmailserver.service")
    machine.wait_for_open_port(25)
    machine.wait_for_open_port(143)
    print(machine.succeed("hmailserver --version"))
    print(machine.succeed("hmailserver-probe"))

    machine.wait_for_open_port(8045)
    status = machine.succeed(
        "curl -sf -u Administrator:nixos-test-admin http://127.0.0.1:8045/api/v1/status"
    )
    print(status)

    # The configuration the module wrote: the service's group reads it, no one else,
    # as the .deb installs it.
    machine.succeed("test \"$(stat -c '%a %U %G' /etc/hmailserver)\" = '750 root hmailserver'")
    machine.succeed("test \"$(stat -c '%a %U %G' /etc/hmailserver/hMailServer.ini)\" = '640 root hmailserver'")

    # The administrator's step after the first start, as the packaging guide gives it:
    # root, through the peer map the module adds, makes the stored-secrets key, which
    # goes into the configuration's [Security] - and must still be there after the
    # restart below, which sets the configuration's directories and database again.
    machine.succeed("hmailserver --create-stored-secrets-key")
    key = machine.succeed("grep '^StoredSecretsKey=.' /etc/hmailserver/hMailServer.ini")

    # The first boot's start went through at the first try: no restart for a role
    # postgresql-setup.service had not yet made (the unit is ordered after
    # postgresql.target), nor for anything else.
    restarts = machine.succeed("systemctl show -p NRestarts --value hmailserver.service").strip()
    assert restarts == "0", f"hmailserver.service was restarted {restarts} time(s) before it started"

    # A database password typed into the configuration by hand, with no passwordFile:
    # the restart must keep it (peer authentication ignores it here).
    machine.succeed("sed -i 's/^Password=.*/Password=typed-by-hand/' /etc/hmailserver/hMailServer.ini")

    machine.succeed("systemctl restart hmailserver.service")
    machine.wait_for_unit("hmailserver.service")
    machine.wait_for_open_port(25)
    machine.wait_for_open_port(143)
    print(machine.succeed("hmailserver-probe"))
    machine.wait_for_open_port(8045)
    machine.succeed("curl -sf -u Administrator:nixos-test-admin http://127.0.0.1:8045/api/v1/status")
    after = machine.succeed("grep '^StoredSecretsKey=.' /etc/hmailserver/hMailServer.ini")
    assert after == key, f"the stored-secrets key changed across the restart: {key!r} became {after!r}"
    machine.succeed("grep -qx 'Password=typed-by-hand' /etc/hmailserver/hMailServer.ini")
    machine.succeed("test \"$(stat -c '%a %U %G' /etc/hmailserver/hMailServer.ini)\" = '640 root hmailserver'")

    # The auto-ban below the server (autoBanFirewall). NixOS has no
    # /usr/lib/hmailserver, where the server looked for the hook until 6.3.6; it
    # runs the one in its own package, where the build installed it.
    machine.fail("test -e /usr/lib/hmailserver/autoban-hook")
    package = machine.succeed("dirname \"$(dirname \"$(readlink -f \"$(command -v hmailserver)\")\")\"").strip()
    hook = package + "/lib/hmailserver/autoban-hook"
    machine.succeed(f"test -x {hook}")
    caps = machine.succeed("systemctl show -p AmbientCapabilities --value hmailserver.service")
    assert "cap_net_admin" in caps, f"the unit's ambient capabilities are {caps!r}, without cap_net_admin"
    # Without that capability the hook can change nothing: the service's account
    # alone is refused, as the hook says it will be.
    machine.fail(f"runuser -u hmailserver -- {hook} check")

    # A ban as the auto-ban places one - an expiring range for one address, named
    # "Auto-ban: " - through the REST API. The server reconciles the firewall with
    # the bans once a minute; within that the address is in the hook's set.
    expires = machine.succeed("date -d '+30 min' '+%F %T'").strip()
    body = json.dumps({"name": "Auto-ban: 192.0.2.7", "lower": "192.0.2.7", "upper": "192.0.2.7",
                       "expires": True, "expires_time": expires})
    created = machine.succeed(
        "curl -sf -u Administrator:nixos-test-admin -H 'Content-Type: application/json' "
        f"--data-binary '{body}' http://127.0.0.1:8045/api/v1/ipranges"
    )
    range_id = json.loads(created)["id"]
    machine.wait_until_succeeds("nft list set inet hmailserver autoban4 | grep -F 192.0.2.7", timeout=180)
    print(machine.succeed("nft list table inet hmailserver"))

    # And deleting the ban takes the address out of the set again.
    machine.succeed(f"curl -sf -u Administrator:nixos-test-admin -X DELETE http://127.0.0.1:8045/api/v1/ipranges/{range_id}")
    machine.wait_until_succeeds("! nft list set inet hmailserver autoban4 | grep -F 192.0.2.7", timeout=180)
  '';
}
