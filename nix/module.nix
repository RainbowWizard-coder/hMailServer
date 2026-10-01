# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# services.hmailserver: the server under systemd on NixOS (hmailserver/docs/Nix.md).
#
# It does what the packaged unit and the .deb's maintainer scripts do, declared:
# the hmailserver account; /etc/hmailserver/hMailServer.ini (0640, root:hmailserver,
# as the package installs it), so that hmailserver's own commands find it without
# --config, its directories and database set from these options at each start; the
# database made and its schema created or upgraded before the server starts, as the
# container's entrypoint does; the first start's secure defaults
# (--apply-new-install-defaults --if-marked, docs/SecureDefaults.md); and the unit's
# sandbox, setting for setting (platform/packaging/hmailserver.service says why each
# is there or not).
#
# With database.createLocally (the default) PostgreSQL runs on the same machine
# and the server reaches it over its Unix socket as the role of its own account,
# which peer authentication lets in with no password. That role may create
# databases, so that hmailserver --create-database makes the database itself and
# marks it as a new installation's, as it does for the Debian package's test. root
# may connect as that role too, so that the administrator's commands run with sudo
# as the packaging guide gives them (sudo hmailserver --create-stored-secrets-key).
{ config, lib, pkgs, ... }:

let
  cfg = config.services.hmailserver;
  db = cfg.database;
  local = db.createLocally && db.type == "PostgreSQL";
  ini = "/etc/hmailserver/hMailServer.ini";
  hmailserver = "${cfg.package}/bin/hmailserver";
  # The auto-ban reaches the firewall: AutoBanFirewall=1, from autoBanFirewall or
  # given in settings. The server then runs the package's autoban-hook, which it
  # finds in the package (it is told where at build time, CMakeLists.txt), as
  # the service's account: the hook needs CAP_NET_ADMIN and nft.
  firewallHook = cfg.autoBanFirewall || toString (cfg.settings.AutoBanFirewall or 0) == "1";

  # As root, before the server's own steps. The first start takes the package's
  # configuration (platform/packaging/hMailServer.ini, its ProgramFolder the
  # package's); every start then sets its [Directories] and [Database] to these
  # options, the password from its file, with nix/ini-merge.awk, and keeps every
  # other line as it is: [Security], where the server and the administrator write
  # the administrator's password hash, the TOTP secret and the stored-secrets key,
  # which the database's sealed values cannot be opened without, and [Settings],
  # which a first start adopts into the database's settings store. The password and
  # its Passwordencryption are set only from a passwordFile: without one they are
  # left as the file has them - empty as packaged, which peer authentication wants,
  # or a password an administrator typed in, which a start must not blank. Written
  # to a file beside it and moved into place. Then the administrator password,
  # hashed by hmailserver --set-admin-password from adminPasswordFile when there is
  # one.
  prepare = pkgs.writeShellScript "hmailserver-prepare" ''
    set -euo pipefail
    install -d -m 0750 -o root -g ${cfg.group} /etc/hmailserver
    base=${ini}
    if [ ! -f "$base" ]; then
      base=${cfg.package}/etc/hmailserver/hMailServer.ini
    fi
    ${lib.optionalString (db.passwordFile != null) ''
      HM_PASSWORD=$(cat ${lib.escapeShellArg db.passwordFile})
      HM_PASSWORDENCRYPTION=0
      export HM_PASSWORD HM_PASSWORDENCRYPTION
    ''}
    new=$(mktemp /etc/hmailserver/.hMailServer.ini.XXXXXX)
    chmod 0640 "$new"
    chown root:${cfg.group} "$new"
    HM_PROGRAMFOLDER=${cfg.package}/share/hmailserver \
    HM_DATAFOLDER=${lib.escapeShellArg cfg.dataDir} \
    HM_LOGFOLDER=${lib.escapeShellArg cfg.logDir} \
    HM_TEMPFOLDER=${lib.escapeShellArg "${cfg.dataDir}/temp"} \
    HM_EVENTFOLDER=${lib.escapeShellArg "${cfg.dataDir}/events"} \
    HM_DATABASEFOLDER=${lib.escapeShellArg "${cfg.dataDir}/database"} \
    HM_TYPE=${db.type} \
    HM_SERVER=${lib.escapeShellArg db.host} \
    HM_PORT=${toString db.port} \
    HM_DATABASE=${lib.escapeShellArg db.name} \
    HM_USERNAME=${lib.escapeShellArg db.user} \
      ${pkgs.gawk}/bin/awk -f ${./ini-merge.awk} "$base" > "$new"
    unset HM_PASSWORD HM_PASSWORDENCRYPTION
    mv -f "$new" ${ini}
    ${lib.optionalString (cfg.adminPasswordFile != null) ''
      ${hmailserver} --config ${ini} --set-admin-password < ${lib.escapeShellArg cfg.adminPasswordFile} > /dev/null
    ''}
  '';

  # As the service's account: the database and its schema, as the Debian package's
  # test makes them, so that what --create-database writes into the data directory
  # is the account's.
  database = pkgs.writeShellScript "hmailserver-database" ''
    set -euo pipefail
    ${hmailserver} --config ${ini} --create-database
    ${hmailserver} --config ${ini} --upgrade-database
  '';

  # As root, the settings: --set-setting writes the server's copy of a setting into
  # the configuration as well as into the database, and refuses an account that
  # cannot write the file, which the service's cannot.
  settings = pkgs.writeShellScript "hmailserver-settings" ''
    set -euo pipefail
    ${lib.concatStringsSep "\n" (lib.mapAttrsToList (name: value:
      "${hmailserver} --config ${ini} --set-setting ${lib.escapeShellArg "${name}=${toString value}"} > /dev/null"
    ) cfg.settings)}
  '';
in
{
  options.services.hmailserver = {
    enable = lib.mkEnableOption "hMailServer, a mail server for SMTP, IMAP and POP3";

    package = lib.mkOption {
      type = lib.types.package;
      description = "The hMailServer package; the flake's own by default.";
    };

    user = lib.mkOption {
      type = lib.types.str;
      default = "hmailserver";
      description = "The account the server runs as.";
    };

    group = lib.mkOption {
      type = lib.types.str;
      default = "hmailserver";
      description = "The group of that account, which may read the configuration.";
    };

    dataDir = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/hmailserver";
      description = "The message store, and the stored-secrets key beside it: back it up with the database.";
    };

    logDir = lib.mkOption {
      type = lib.types.str;
      default = "/var/log/hmailserver";
      description = "Where the server writes its logs.";
    };

    database = {
      type = lib.mkOption {
        type = lib.types.enum [ "PostgreSQL" "MySQL" ];
        default = "PostgreSQL";
        description = "The database backend. MySQL covers MariaDB too, through the MariaDB connector the package opens.";
      };
      host = lib.mkOption {
        type = lib.types.str;
        default = "/run/postgresql";
        description = "The database server: a host name, or for PostgreSQL the directory of its Unix socket.";
      };
      port = lib.mkOption {
        type = lib.types.port;
        default = 5432;
        description = "The database server's port (for a Unix socket, the number in its file name).";
      };
      name = lib.mkOption {
        type = lib.types.str;
        default = "hmailserver";
        description = "The database; hmailserver --create-database makes it when it is not there.";
      };
      user = lib.mkOption {
        type = lib.types.str;
        default = "hmailserver";
        description = "The database role.";
      };
      passwordFile = lib.mkOption {
        type = lib.types.nullOr lib.types.path;
        default = null;
        description = "A file holding the database password, read as root at each start, named as a string (\"/run/secrets/...\"): a Nix path literal would copy the password into the world-readable Nix store. None for peer authentication; without one the configuration's Password is left as it is.";
      };
      createLocally = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Run PostgreSQL on this machine, with a role for the server's account that may create its database.";
      };
    };

    adminPasswordFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = ''
        A file holding the administrator password, which gates the REST API, named
        as a string ("/run/secrets/..."): a Nix path literal would copy the password
        into the world-readable Nix store. It is hashed into the configuration at
        each start. Without it the hash set by hand (hmailserver
        --set-admin-password) is kept from start to start.
      '';
    };

    settings = lib.mkOption {
      type = with lib.types; attrsOf (oneOf [ str int ]);
      default = { };
      example = { RestApiPort = 8045; };
      description = ''
        Settings stored in the database with hmailserver --set-setting, as root,
        at each start. Not for secrets, which that command refuses on its command
        line.
      '';
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Open SMTP (25, 465, 587), IMAP (143, 993) and POP3 (110, 995) in the firewall.";
    };

    autoBanFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Block each address the server auto-bans in the firewall below it, for as
        long as the ban lasts: AutoBanFirewall=1, with which the server runs the
        package's autoban-hook, which keeps an nftables set with a timeout. It
        stores the setting, and gives the service CAP_NET_ADMIN, which it
        otherwise does not have, and nft. The same is given when settings sets
        AutoBanFirewall to 1. fail2ban, with the package's filter and jail
        (etc/fail2ban), is the other way, and needs neither.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = !local || (db.user == cfg.user && db.host == "/run/postgresql");
        message = "services.hmailserver.database.createLocally reaches PostgreSQL over its socket with peer authentication, so database.user must be the service's account and database.host /run/postgresql.";
      }
    ];

    users.users.${cfg.user} = {
      isSystemUser = true;
      group = cfg.group;
      home = cfg.dataDir;
      description = "hMailServer";
    };
    users.groups.${cfg.group} = { };

    services.hmailserver.settings = lib.mkIf cfg.autoBanFirewall { AutoBanFirewall = lib.mkDefault 1; };

    environment.systemPackages = [ cfg.package ];

    services.postgresql = lib.mkIf local {
      enable = true;
      ensureUsers = [{
        name = db.user;
        ensureClauses.createdb = true;
      }];
      # Peer authentication for the role, with root let in as it too: the lines
      # land before NixOS's own "local all all peer", and the first line that
      # matches decides.
      identMap = ''
        hmailserver ${cfg.user} ${db.user}
        hmailserver root ${db.user}
      '';
      authentication = ''
        local all ${db.user} peer map=hmailserver
      '';
    };

    systemd.tmpfiles.rules = [
      "d ${cfg.dataDir} 0750 ${cfg.user} ${cfg.group} - -"
      "d ${cfg.dataDir}/temp 0750 ${cfg.user} ${cfg.group} - -"
      "d ${cfg.dataDir}/events 0750 ${cfg.user} ${cfg.group} - -"
      "d ${cfg.dataDir}/database 0750 ${cfg.user} ${cfg.group} - -"
      "d ${cfg.logDir} 0750 ${cfg.user} ${cfg.group} - -"
    ];

    networking.firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall [ 25 110 143 465 587 993 995 ];

    systemd.services.hmailserver = {
      description = "hMailServer, a mail server for SMTP, IMAP and POP3";
      documentation = [ "https://gitlab.com/hmailserver/hmailserver" ];
      wantedBy = [ "multi-user.target" ];
      wants = [ "network-online.target" ];
      # After postgresql.target, not only postgresql.service: since NixOS 25.11 the
      # ensureUsers above - the role --create-database connects as - are made by
      # postgresql-setup.service, which starts once postgresql.service is up, and
      # the target is reached only when it has run. Ordered after the service alone,
      # the first boot's start could ask for the role before it existed. Where a
      # NixOS has no such target (before 25.11, when the service made the roles
      # itself), After= on it orders nothing and the service still does.
      after = [ "network-online.target" "postgresql.service" "postgresql.target" "mysql.service" ];
      requires = lib.optional local "postgresql.service";
      # The auto-ban hook's firewall tool, which it finds on PATH.
      path = lib.optional firewallHook pkgs.nftables;

      serviceConfig = {
        Type = "simple";
        User = cfg.user;
        Group = cfg.group;
        # The two with "+" run as root and outside the sandbox below: they write
        # /etc. The other two run as the account; the first start's defaults are
        # forgiven a failure, as in the packaged unit, and the server's own start
        # says what is wrong.
        ExecStartPre = [
          "+${prepare}"
          "${database}"
          "+${settings}"
          "-${hmailserver} --config ${ini} --apply-new-install-defaults --if-marked"
        ];
        ExecStart = "${hmailserver} --config ${ini} --foreground";
        ExecReload = "${pkgs.coreutils}/bin/kill -HUP $MAINPID";
        TimeoutStartSec = 300;
        TimeoutStopSec = 300;
        Restart = "on-failure";
        RestartSec = 5;

        WorkingDirectory = cfg.dataDir;
        ReadWritePaths = [ cfg.dataDir cfg.logDir ];
        RuntimeDirectory = "hmailserver";
        RuntimeDirectoryMode = "0750";

        # And CAP_NET_ADMIN for the auto-ban hook, only when the firewall is asked
        # for: the packaged unit leaves it to a drop-in, because a mail server that
        # can rewrite the host's firewall is a bigger grant than one that can bind
        # port 25, and here the option is that deliberate step.
        AmbientCapabilities = [ "CAP_NET_BIND_SERVICE" ] ++ lib.optional firewallHook "CAP_NET_ADMIN";
        CapabilityBoundingSet = [ "CAP_NET_BIND_SERVICE" ] ++ lib.optional firewallHook "CAP_NET_ADMIN";
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        PrivateDevices = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectKernelLogs = true;
        ProtectControlGroups = true;
        ProtectClock = true;
        ProtectHostname = true;
        ProtectProc = "invisible";
        ProcSubset = "pid";
        RestrictNamespaces = true;
        RestrictRealtime = true;
        RestrictSUIDSGID = true;
        LockPersonality = true;
        RemoveIPC = true;
        RestrictAddressFamilies = [ "AF_INET" "AF_INET6" "AF_UNIX" "AF_NETLINK" ];
        SystemCallArchitectures = "native";
        SystemCallFilter = [ "@system-service" ];
        SystemCallErrorNumber = "EPERM";
        UMask = "0027";
        LimitNOFILE = 65535;
      };
    };
  };
}
