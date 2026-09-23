#!/usr/bin/env bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The container's entrypoint. The environment becomes the configuration, each
# part only when it is set, each idempotent, and then the server runs:
#
#   HM_DB_TYPE (PostgreSQL|MySQL), HM_DB_HOST, HM_DB_PORT, HM_DB_NAME,
#   HM_DB_USER, HM_DB_PASSWORD   -> the [Database] section of hMailServer.ini,
#                                   rewritten from these on every start so the
#                                   environment is the truth, not a stale file
#   HM_ADMIN_PASSWORD            -> [Security] AdministratorPassword, through the
#                                   server's own --set-admin-password, so the file
#                                   holds a PBKDF2 hash and never the password.
#                                   Without it the REST API refuses to start, and
#                                   on Linux the REST API is the administration.
#   HM_SETTING_<key>             -> any [Settings] key the server reads (the
#                                   keys of build/ini-settings-baseline.txt,
#                                   matched without case), for example
#                                   HM_SETTING_MaxSubmissionsPerIPPerMinute=30
#   HM_PROPERTY_<name>           -> any hm_settings row the server reads (the
#                                   names of SettingsDefaults.h), for example
#                                   HM_PROPERTY_maxmessagesize=20480
#   HM_SETTING_<key>_FILE,
#   HM_PROPERTY_<name>_FILE      -> the same, the value read from the file named
#                                   (a Kubernetes Secret or a Compose secret
#                                   mounted as a file; its trailing line break
#                                   is not part of the value). Setting both the
#                                   plain form and the _FILE form is refused.
#                                   All of these are stored in the database - the
#                                   settings store - by the server's own
#                                   --apply-environment once the database is
#                                   there, checked first, and none stored when any
#                                   is refused: the container then does not
#                                   start, and the log says which and why. The
#                                   environment wins at EVERY start, so a change
#                                   made in the Deck or over the REST API to a
#                                   setting named here would be undone at the
#                                   next one - the server refuses it instead
#                                   (409), until a start without the variable.
#   HM_REST_PORT, HM_REST_BIND_ADDRESS, HM_REST_CERTIFICATE_FILE,
#   HM_REST_PRIVATE_KEY_FILE     -> RestApiPort, RestApiBindAddress,
#                                   RestApiCertificateFile, RestApiPrivateKeyFile:
#                                   the names these four always had, now aliases
#                                   of HM_SETTING_RestApiPort and the other three
#                                   (the last two are paths the server reads, not
#                                   _FILE forms). The packaged file has the port
#                                   at 0 (off) and the bind at 127.0.0.1, which
#                                   inside a container is the container's own
#                                   loopback; to reach the API from outside, bind
#                                   0.0.0.0 with a certificate and key (the server
#                                   requires TLS off loopback), or run with host
#                                   networking.
#   HM_DB_PASSWORD_FILE, HM_ADMIN_PASSWORD_FILE
#                                -> the same two secrets read from a file (a Docker
#                                   or Compose secret) instead of the environment
#   HM_CREATE_DATABASE (default 1) -> hmailserver --create-database, which is a
#                                   no-op when the database already exists
#   HM_UPGRADE_DATABASE (default 1) -> hmailserver --upgrade-database, which is a
#                                   no-op when the schema is current
#   HM_PROFILE (office|gateway|relay|sender)
#                                -> hmailserver --apply-profile NAME --once: the
#                                   setting profile of that name (docs/Profiles.md)
#                                   applied at the first start, after the settings
#                                   above, and never again - a later start, and a
#                                   start after an administrator reverted it, leave
#                                   the settings alone. A profile entry that closes
#                                   a listener, trusts an address range or turns a
#                                   filter off is applied only with
#   HM_PROFILE_CONFIRM=yes       -> --confirm; without it such a profile does not
#                                   apply, the container does not start, and the
#                                   log names the entries.
#
# then exec the command - hmailserver --foreground unless told otherwise - so
# signals reach the server and it is PID 1's child, not a grandchild.
set -eu

INI="${HM_INI:-/etc/hmailserver/hMailServer.ini}"

# VAR_FILE, when set, is where VAR's value is read from; setting both is a
# mistake, not a precedence question.
file_env() {
   local var="$1" file_var="${1}_FILE"
   if [ -n "${!file_var:-}" ]; then
      if [ -n "${!var:-}" ]; then
         echo "$var and $file_var are both set; set one of them" >&2
         exit 64
      fi
      if [ ! -r "${!file_var}" ]; then
         echo "$file_var names ${!file_var}, which cannot be read" >&2
         exit 66
      fi
      export "$var"="$(cat "${!file_var}")"
   fi
}

needs_writable_ini() {
   if [ ! -w "$INI" ]; then
      echo "$INI is not writable by $(id -un); $1 cannot be written from the environment" >&2
      exit 1
   fi
}

write_database_section() {
   # Everything except the [Database] section is kept as it is; that section is
   # replaced. awk drops the old section (from its header to the next header)
   # and the new one is appended.
   local tmp
   tmp="$(mktemp)"
   awk 'BEGIN{skip=0} /^\[/{skip=($0=="[Database]")} !skip{print}' "$INI" > "$tmp"
   {
      cat "$tmp"
      printf '\n[Database]\nType=%s\nServer=%s\nPort=%s\nDatabase=%s\nUsername=%s\nPassword=%s\nPasswordencryption=0\n' \
         "$HM_DB_TYPE" "${HM_DB_HOST:-db}" "${HM_DB_PORT:-$default_port}" "${HM_DB_NAME:-hmailserver}" \
         "${HM_DB_USER:-hmailserver}" "${HM_DB_PASSWORD:-}"
   } > "$INI"
   rm -f "$tmp"
}

file_env HM_DB_PASSWORD
file_env HM_ADMIN_PASSWORD

if [ -n "${HM_DB_TYPE:-}" ]; then
   case "$HM_DB_TYPE" in
      PostgreSQL|postgresql|postgres|pgsql) HM_DB_TYPE=PostgreSQL; default_port=5432 ;;
      MySQL|mysql|MariaDB|mariadb)          HM_DB_TYPE=MySQL;      default_port=3306 ;;
      MSSQL|mssql)                          HM_DB_TYPE=MSSQL;      default_port=1433 ;;
      *) echo "HM_DB_TYPE must be PostgreSQL, MySQL or MSSQL, not '$HM_DB_TYPE'" >&2; exit 64 ;;
   esac
   needs_writable_ini "the [Database] section"
   write_database_section
   echo "[Database] written from the environment: $HM_DB_TYPE on ${HM_DB_HOST:-db}:${HM_DB_PORT:-$default_port}, database ${HM_DB_NAME:-hmailserver}"
fi

if [ -n "${HM_ADMIN_PASSWORD:-}" ]; then
   needs_writable_ini "AdministratorPassword"
   # The server's own verb: one line on standard input, a PBKDF2 hash in the
   # file, the file's owner and mode kept. The password itself is in no file.
   printf '%s\n' "$HM_ADMIN_PASSWORD" | hmailserver --config "$INI" --set-admin-password
   echo "[Security] AdministratorPassword written as a hash from HM_ADMIN_PASSWORD"
fi

if [ "${HM_CREATE_DATABASE:-1}" != 0 ]; then
   # Exit status 0 also when the database already holds this build's schema or
   # an older one, which the upgrade below brings forward - so this runs at every
   # start. A schema from a newer build, or a database that cannot be reached,
   # is a reason not to start.
   hmailserver --config "$INI" --create-database
fi

if [ "${HM_UPGRADE_DATABASE:-1}" != 0 ]; then
   hmailserver --config "$INI" --upgrade-database
fi

# Every setting the environment names - HM_SETTING_*, HM_PROPERTY_*, their _FILE
# forms and the four HM_REST_* - stored in the database, which is the settings
# store: from schema 6042 the [Settings] section of hMailServer.ini is the
# server's copy of it, so a line written there is taken only at the first start
# and a changed one is named as HM5804 and put back. The server's own verb checks
# every value before it stores any, prints one line per variable (set,
# unchanged, or refused and why - never a value from a file or a secret), and
# records which names the environment manages, so that the Deck and the REST API
# refuse a change the next start would undo; a run without a variable gives its
# setting back. It needs the database at this build's schema, so it follows the
# create and upgrade steps above, and a refusal stops the container, as a
# database the server could not start on would.
#
# Run when there is anything for it to do: a variable of this family other than
# the ones this script reads itself - which includes a misspelt prefix such as
# HM_SETTINGS_, which the verb refuses by name rather than leave a setting
# silently unset - or the file it keeps in the data directory while the
# environment manages anything, so that the first start without the variables
# gives those settings back. Otherwise the database is not opened here, as it was
# not before settings came from the environment, and a database that is a moment
# late does not stop a container that has nothing to apply.
applies_environment() {
   local name data
   # compgen -e: the names of the exported variables, one per line, and never a
   # value - a value with a line break in it cannot pass for a name here.
   for name in $(compgen -e); do
      case "$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')" in
         HM_DB_TYPE|HM_DB_HOST|HM_DB_PORT|HM_DB_NAME|HM_DB_USER|HM_DB_PASSWORD|HM_DB_PASSWORD_FILE) ;;
         HM_ADMIN_PASSWORD|HM_ADMIN_PASSWORD_FILE|HM_INI|HM_CREATE_DATABASE|HM_UPGRADE_DATABASE) ;;
         HM_PROFILE|HM_PROFILE_CONFIRM) ;;
         HM_*) return 0 ;;
      esac
   done

   data="$(sed -n 's/^[[:space:]]*DataFolder[[:space:]]*=[[:space:]]*//p' "$INI" 2>/dev/null | head -n 1 | tr -d '\r')"
   case "$data" in
      /*) ;;
      *) data=/var/lib/hmailserver ;;
   esac
   [ -e "$data/.hmailserver-environment" ]
}

if applies_environment; then
   hmailserver --config "$INI" --apply-environment
fi

# A setting profile, applied once: --once does nothing on a database a profile has
# been applied to before (the hm_settings row ProfileApplied says so, and it is
# not in a backup), so a restart, or a start after an administrator reverted the
# profile in the Deck, does not apply it again. After the environment's settings,
# so that a setting the environment manages is skipped and said to be. A profile
# that would close a listener, trust an address range or turn a filter off asks
# for --confirm, which HM_PROFILE_CONFIRM=yes gives; without it the verb exits 3
# having changed nothing, and the container does not start on a shape nobody
# confirmed.
if [ -n "${HM_PROFILE:-}" ]; then
   case "${HM_PROFILE_CONFIRM:-no}" in
      yes|YES|Yes|1|true|TRUE|True) hmailserver --config "$INI" --apply-profile "$HM_PROFILE" --once --confirm ;;
      *) hmailserver --config "$INI" --apply-profile "$HM_PROFILE" --once ;;
   esac
fi

if [ "${1:-}" = "hmailserver" ] && ! printf '%s\n' "$@" | grep -q -- '--config'; then
   shift
   set -- hmailserver --config "$INI" "$@"
fi
exec "$@"
