#!/usr/bin/env bash
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The schema-upgrade gate: prove that a database at schema 6029 holding orphaned
# rows upgrades to this build's schema through the real upgrade scripts, on a real
# backend, and that what it upgrades to is the schema a new installation gets.
# Nothing in CI executed an upgrade before 11 September 2026, which is how the
# 6029 -> 6030 step shipped sweeping children before parents (issue #93's report
# led to it). This runs on the Ubuntu VM and in GitLab CI's linux-db-upgrade-pgsql
# and linux-db-upgrade-mysql jobs (build/ci/linux-db-upgrade.sh).
#
#   db-upgrade-gate.sh pgsql|mysql
#
# Environment (all have defaults for the CI services):
#   HM_BIN          the server binary (default: hmailserver on PATH)
#   HM_SCRIPTS      the DBScripts directory of the tree under test
#   DB_HOST DB_PORT DB_USER DB_PASSWORD DB_NAME
#
# Steps:
#   1. A database at schema 6029, made by the create script of 6029 itself:
#      build/db-upgrade-6029/CreateTables<dialect>.sql, the files
#      hmailserver/source/DBScripts/CreateTables{MYSQL,PGSQL}.sql of commit
#      6eeb54bdf (5 September 2026), the last whose create scripts made 6029,
#      byte for byte (git blobs 72ca64c4 and 4aff719a; .gitattributes keeps
#      them unconverted). The database is made as that version's server made
#      it - utf8 on MySQL, UTF8 on PostgreSQL - and the script is run by the
#      backend's own client.
#   2. The orphans: a row with an absent parent in each of the seventeen tables
#      the 6030 step constrains, and the chains that fail only in the wrong
#      order (build/seed-orphans.py, from that same script). And greylisting
#      triplets as a server before schema 6072 wrote them, with no key, some
#      cut to the old columns' 200 characters, one triplet twice once its
#      domain is folded (build/db-upgrade-greylist.py).
#   3. --upgrade-database, exactly as the installer's post-install step runs it.
#   4. A second database made by --create-database at this build's schema, the
#      way a new installation is made.
#   5. The verdict: the upgrade exited 0, the version is REQUIRED_DB_VERSION,
#      and the upgraded database has the new one's schema - every table, every
#      column with its type, nullability and default (and on MySQL its
#      collation), every index with its columns, every constraint and foreign
#      key. A difference fails the gate: the upgraded server would run on a
#      schema nothing else tests. And the greylisting triplets are what schema
#      6107 makes of them - keyed by the documented function, computed apart
#      from the server, the cuts and the second row gone - in a table that
#      refuses a row without its key and a second row for a triplet, IPv4 too.
#   6. The steps that change rows rather than the schema, which step 5 cannot
#      see: Upgrade6105to6106<dialect>.sql run by the backend's own client over
#      external accounts of every kind it treats differently, in the upgraded
#      database put back to 6105, and each value compared with what
#      FetchAccount::DaysToKeepStoredBefore6106 says it becomes. A value that
#      changes wrongly here changes what is deleted on somebody's remote server.
#
# Until 26 September 2026 step 1 made the database at this build's schema and
# wound it back to 6029, dropping what it read each later step's script to add.
# The comment above it said no older create script survived; they do, under
# the MYSQL spelling of the name that commit a8ddb65ba changed. The rewind went
# stale three times, silently until an upgrade stopped on it: indexes a later
# step made on an old table (6089), a unique index MariaDB would not let a
# column go under (6086), and the second column of a step that adds two in one
# ALTER TABLE (6101), on the first GitLab pipeline. It also never undid a
# column's widening (6039, 6072, 6103) or MySQL's utf8 (6102), so those steps
# ran on columns and tables already in their final shape. A real 6029 database
# has none of those faults, and step 5 checks the whole chain against the
# create script, which the rewind could not.
set -euo pipefail

backend="${1:-}"
case "$backend" in pgsql|mysql) ;; *) echo "usage: $0 pgsql|mysql" >&2; exit 64 ;; esac

HM_BIN="${HM_BIN:-hmailserver}"
HM_SCRIPTS="${HM_SCRIPTS:-$(cd "$(dirname "$0")/.." && pwd)/hmailserver/source/DBScripts}"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_USER="${DB_USER:-hmailserver}"
# Empty is a password too (the CI job's MariaDB root has none): hmtest only when unset.
DB_PASSWORD="${DB_PASSWORD-hmtest}"
DB_NAME="${DB_NAME:-hmupgrade}"
fresh_name="${DB_NAME}_fresh"
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ "$backend" = pgsql ]; then
   DB_PORT="${DB_PORT:-5432}"
   ini_type=PostgreSQL
   base_script="$here/db-upgrade-6029/CreateTablesPGSQL.sql"
   # sql_in <database> <psql arguments>
   sql_in() { local db="$1"; shift; PGPASSWORD="$DB_PASSWORD" psql -v ON_ERROR_STOP=1 -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$db" -q -t -A "$@"; }
   query_in() { sql_in "$1" -c "$2" </dev/null; }
   run_file() { sql_in "$DB_NAME" -f "$1" </dev/null >/dev/null; }
   drop_database() { query_in postgres "drop database if exists $1;" >/dev/null; }
   create_6029() { query_in postgres "create database \"$1\" ENCODING = 'UTF8';" >/dev/null; }
   this_schema="'public'"
   count_fks="select count(*) from information_schema.table_constraints where constraint_type = 'FOREIGN KEY' and table_schema = 'public';"
   schema_queries=(
      "select concat_ws(' ', 'table', table_name) from information_schema.tables where table_schema = 'public' and table_type = 'BASE TABLE';"
      "select concat_ws(' ', 'column', table_name, column_name, data_type, coalesce(character_maximum_length::text, '-'), case when is_nullable = 'YES' then 'null' else 'not-null' end, 'default=' || coalesce(column_default, '(none)')) from information_schema.columns where table_schema = 'public';"
      "select concat_ws(' ', 'index', tablename, indexname, indexdef) from pg_indexes where schemaname = 'public';"
      "select concat_ws(' ', 'constraint', conrelid::regclass::text, conname, pg_get_constraintdef(oid)) from pg_constraint where connamespace = 'public'::regnamespace;"
      "select concat_ws(' ', 'function', proname, pg_get_function_identity_arguments(oid)) from pg_proc where pronamespace = 'public'::regnamespace;"
   )
else
   DB_PORT="${DB_PORT:-3306}"
   ini_type=MySQL
   base_script="$here/db-upgrade-6029/CreateTablesMySQL.sql"
   # MariaDB 11.4's client verifies the server's certificate, and the one a new
   # server makes for itself can be proved only by a password: with none, the
   # connection is still TLS, and the certificate is not checked.
   verify=()
   [ -n "$DB_PASSWORD" ] || verify=(--skip-ssl-verify-server-cert)
   # sql_in <database, or "" for none> <mariadb arguments>
   sql_in() {
      local database=()
      [ -z "$1" ] || database=(--database="$1")
      shift
      mariadb --host="$DB_HOST" --port="$DB_PORT" --user="$DB_USER" --password="$DB_PASSWORD" "${verify[@]}" "${database[@]}" --batch --skip-column-names "$@"
   }
   query_in() { sql_in "$1" -e "$2" </dev/null; }
   run_file() { sql_in "$DB_NAME" < "$1"; }
   drop_database() { query_in "" "drop database if exists $1;"; }
   # As SQLStatement.cpp of 6029 made it.
   create_6029() { query_in "" "create database $1 character set 'utf8';"; }
   this_schema="database()"
   count_fks="select count(*) from information_schema.referential_constraints where constraint_schema = database();"
   schema_queries=(
      "select concat_ws(' ', 'table', table_name, engine, table_collation) from information_schema.tables where table_schema = database() and table_type = 'BASE TABLE';"
      "select concat_ws(' ', 'column', table_name, column_name, column_type, if(is_nullable = 'YES', 'null', 'not-null'), concat('default=', coalesce(column_default, '(none)')), coalesce(collation_name, '-'), extra) from information_schema.columns where table_schema = database();"
      "select concat_ws(' ', 'index', table_name, index_name, if(non_unique = 0, 'unique', 'non-unique'), group_concat(concat(column_name, if(sub_part is null, '', concat('(', sub_part, ')'))) order by seq_in_index separator ',')) from information_schema.statistics where table_schema = database() group by table_name, index_name, non_unique;"
      "select concat_ws(' ', 'foreign-key', k.table_name, k.constraint_name, group_concat(k.column_name order by k.ordinal_position separator ','), '->', k.referenced_table_name, group_concat(k.referenced_column_name order by k.ordinal_position separator ','), concat('update=', r.update_rule), concat('delete=', r.delete_rule)) from information_schema.key_column_usage k join information_schema.referential_constraints r on r.constraint_schema = k.constraint_schema and r.constraint_name = k.constraint_name and r.table_name = k.table_name where k.table_schema = database() and k.referenced_table_name is not null group by k.table_name, k.constraint_name, k.referenced_table_name, r.update_rule, r.delete_rule;"
      "select concat_ws(' ', 'check', table_name, constraint_name, check_clause) from information_schema.check_constraints where constraint_schema = database();"
   )
fi
query() { query_in "$DB_NAME" "$1"; }

# Every table, column, index and constraint of a database, one per line, sorted.
schema_of() {
   local q
   for q in "${schema_queries[@]}"; do
      query_in "$1" "$q"
   done | LC_ALL=C sort
}

# An installation's directories and hMailServer.ini, for the database named.
make_installation() {
   local dir="$1" db="$2"
   mkdir -p "$dir/program" "$dir/db" "$dir/data" "$dir/logs" "$dir/temp" "$dir/events"
   cp -r "$HM_SCRIPTS" "$dir/program/DBScripts"
   cat > "$dir/program/hMailServer.ini" <<INI
[Directories]
ProgramFolder=$dir/program
DatabaseFolder=$dir/db
DataFolder=$dir/data
LogFolder=$dir/logs
TempFolder=$dir/temp
EventFolder=$dir/events

[Database]
Type=$ini_type
Server=$DB_HOST
Port=$DB_PORT
Username=$DB_USER
Password=$DB_PASSWORD
Database=$db
Passwordencryption=0

[Settings]
INI
}

# The schema this build needs: from Constants.h when the scripts are the tree's,
# else from the database the binary creates, which is that schema by definition.
constants="$HM_SCRIPTS/../Server/Common/Application/Constants.h"
if [ -f "$constants" ]; then
   required=$(grep -oE 'REQUIRED_DB_VERSION\s+[0-9]+' "$constants" | grep -oE '[0-9]+$')
else
   required=""
fi
[ -f "$base_script" ] || { echo "no $base_script: the 6029 create script this gate starts from is missing" >&2; exit 1; }
echo "== gate: $backend, binary $("$HM_BIN" --version </dev/null 2>/dev/null | head -1), scripts in $HM_SCRIPTS"

# 1. The 6029 database.
echo "-- dropping $DB_NAME and $fresh_name, creating $DB_NAME at schema 6029 from ${base_script#"$here"/}"
drop_database "$DB_NAME"
drop_database "$fresh_name"
create_6029 "$DB_NAME"
run_file "$base_script"
version=$(query "select value from hm_dbversion;")
[ "$version" = 6029 ] || { echo "the 6029 create script made a database at $version" >&2; exit 1; }
fks=$(query "$count_fks")
[ "$fks" = 0 ] || { echo "a 6029 database has no foreign key, and this one has $fks" >&2; exit 1; }
echo "   $(query "select count(*) from information_schema.tables where table_schema = $this_schema and table_type = 'BASE TABLE';") tables, schema 6029"

# 2. The orphans.
echo "-- seeding orphaned rows"
python3 "$here/seed-orphans.py" "$base_script" "$backend" > "$work/seed.sql"
run_file "$work/seed.sql"
echo "   $(grep -c '^insert' "$work/seed.sql") rows in $(grep -c '^insert' "$work/seed.sql") tables, every one with a parent that does not exist"
python3 "$here/db-upgrade-greylist.py" seed > "$work/greylist.sql"
run_file "$work/greylist.sql"
echo "   $(grep -c '^insert' "$work/greylist.sql") greylisting triplets as a server before schema 6072 wrote them: no key, two cut to 200 characters, one triplet twice"

# 3. The upgrade, exactly as the installer's post-install step runs it.
echo "-- upgrading"
make_installation "$work/upgraded" "$DB_NAME"
set +e
"$HM_BIN" --config "$work/upgraded/program/hMailServer.ini" --upgrade-database </dev/null
rc=$?
set -e
version=$(query "select value from hm_dbversion;")

# 4. A new installation's database, at this build's schema.
echo "-- creating $fresh_name as a new installation makes it"
make_installation "$work/fresh" "$fresh_name"
"$HM_BIN" --config "$work/fresh/program/hMailServer.ini" --create-database </dev/null
fresh_version=$(query_in "$fresh_name" "select value from hm_dbversion;")
[ -n "$required" ] || required="$fresh_version"
[ "$fresh_version" = "$required" ] || { echo "the new database is at $fresh_version, expected $required" >&2; exit 1; }

# 5. The verdict.
fks=$(query "$count_fks")
expected_fks=$(query_in "$fresh_name" "$count_fks")
schema_of "$fresh_name" > "$work/schema-new.txt"
schema_of "$DB_NAME" > "$work/schema-upgraded.txt"
echo "-- upgrade exit $rc; schema now $version of $required; foreign keys $fks of $expected_fks;" \
     "$(wc -l < "$work/schema-new.txt") schema objects in a new database"

# Two things an upgraded database has that a new one does not, by design, and
# nothing else:
#  - the helper functions ScriptPrerequisites<dialect>.sql makes for the upgrade
#    steps to call (hm_drop_column_objects on PostgreSQL), which --upgrade-database
#    runs first and --create-database never does;
#  - on MySQL, a column of the TEXT family one size wider: CONVERT TO CHARACTER
#    SET, moving a table from utf8 (three bytes a character) to utf8mb4 (four),
#    makes a TEXT a MEDIUMTEXT and a MEDIUMTEXT a LONGTEXT so that it still holds
#    as many characters, and the 6102 step converts every table so. The wider
#    column holds everything the new database's does. The same column narrower,
#    or of any other type, is still a difference.
prerequisites="$HM_SCRIPTS/ScriptPrerequisites$( [ "$backend" = pgsql ] && echo PGSQL || echo MySQL ).sql"
if [ -f "$prerequisites" ]; then
   while read -r helper; do
      if ! grep -q "^function $helper " "$work/schema-new.txt"; then
         sed -i "/^function $helper /d" "$work/schema-upgraded.txt"
         echo "   the upgrade's helper function $helper, from ${prerequisites##*/}: not a difference"
      fi
   done < <(grep -ioE 'create (or replace )?function \w+' "$prerequisites" | awk '{print tolower($NF)}' | sort -u)
fi
if [ "$backend" = mysql ]; then
   python3 - "$work/schema-new.txt" "$work/schema-upgraded.txt" <<'PY'
import sys

WIDER = {"text": "mediumtext", "mediumtext": "longtext"}
new_path, upgraded_path = sys.argv[1], sys.argv[2]
new = {}
with open(new_path, encoding="utf-8") as f:
    for line in f:
        fields = line.rstrip("\n").split(" ")
        if fields[0] == "column":
            new[(fields[1], fields[2])] = fields
lines, widened = [], []
with open(upgraded_path, encoding="utf-8") as f:
    for line in f:
        fields = line.rstrip("\n").split(" ")
        if fields[0] == "column":
            was = new.get((fields[1], fields[2]))
            if was and len(was) == len(fields) and WIDER.get(was[3]) == fields[3] and was[4:] == fields[4:]:
                widened.append("%s.%s" % (fields[1], fields[2]))
                fields[3] = was[3]
        lines.append(" ".join(fields))
with open(upgraded_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
if widened:
    print("   %d TEXT-family column(s) one size wider than a new database's, as utf8mb4's conversion leaves them: not a difference" % len(widened))
PY
fi
failed=""
if [ "$rc" -ne 0 ] || [ "$version" != "$required" ] || [ "$fks" != "$expected_fks" ]; then
   failed=yes
fi

# The greylisting triplets, where the upgrade reached schema 6107 (work item #50).
if [ "$version" -ge 6107 ] 2>/dev/null; then
   echo "-- the greylisting triplets after the upgrade"
   if ! query "select concat_ws('|', glsenderaddress, glrecipientaddress, glipaddress1, coalesce(glipaddress2, -1), glpassedcount, glsenderhash, glrecipienthash) from hm_greylisting_triplets order by glid;" \
         | python3 "$here/db-upgrade-greylist.py" check; then
      echo "The upgrade did not make of the greylisting triplets what schema 6107 must." >&2
      failed=yes
   fi
   while IFS=$'\t' read -r expect statement; do
      if query "$statement" >/dev/null 2>&1; then outcome=accept; else outcome=refuse; fi
      if [ "$outcome" != "$expect" ]; then
         echo "The upgraded hm_greylisting_triplets did $outcome, and must $expect: $statement" >&2
         failed=yes
      fi
   done < <(python3 "$here/db-upgrade-greylist.py" constraints)
   query "delete from hm_greylisting_triplets where glrecipientaddress = 'other@example.test';" >/dev/null
   echo "   a row without its key and a second row for a triplet, IPv4 and IPv6, refused; another triplet accepted"
fi
if ! diff -u --label "a new database ($fresh_name)" --label "the upgraded one ($DB_NAME)" \
        "$work/schema-new.txt" "$work/schema-upgraded.txt" > "$work/schema.diff"; then
   echo "The upgraded database's schema is not a new database's ($(grep -c '^-[^-]' "$work/schema.diff") line(s) only in the new, $(grep -c '^+[^+]' "$work/schema.diff") only in the upgraded):"
   cat "$work/schema.diff"
   failed=yes
fi
if [ -n "$failed" ]; then
   echo "GATE FAILED: a 6029 database holding orphaned rows did not upgrade cleanly to a new database's schema on $backend" >&2
   exit 1
fi

# 6. The 6106 step's rows: DaysToKeepMessages given the one meaning both fetchers
#    read (Server/Common/BO/FetchAccount.h). An IMAP account's 0 was "delete" and
#    becomes -1, its negative number was "keep" and becomes 0; a POP3 account made
#    or moved by somebody other than a server administrator (faorigin not 0) has a
#    negative number made 0; everything else is as it was. Each row is
#    "name servertype origin stored expected". The rows belong to no account, so
#    their foreign key is set aside - for the session on MySQL; on PostgreSQL, where
#    that needs a superuser, by dropping the constraint from this database, which
#    the gate has finished judging and drops on its next run.
step6106="$HM_SCRIPTS/Upgrade6105to6106$( [ "$backend" = pgsql ] && echo PGSQL || echo MySQL ).sql"
if [ -f "$step6106" ]; then
   echo "-- the 6106 step's translation of DaysToKeepMessages, by ${step6106##*/}"
   retention_rows=(
      "imap-zero 1 0 0 -1"
      "imap-minus-one 1 1 -1 0"
      "imap-minus-five 1 0 -5 0"
      "imap-seven 1 2 7 7"
      "pop3-admin-minus-one 0 0 -1 -1"
      "pop3-admin-zero 0 0 0 0"
      "pop3-admin-seven 0 0 7 7"
      "pop3-reader-minus-one 0 1 -1 0"
      "pop3-restricted-minus-one 0 2 -1 0"
      "pop3-reader-zero 0 1 0 0"
      "pop3-reader-thirty 0 1 30 30"
   )
   {
      if [ "$backend" = pgsql ]; then
         echo "alter table hm_fetchaccounts drop constraint if exists fk_hm_fetchaccounts_account;"
      else
         echo "set foreign_key_checks = 0;"
      fi
      echo "delete from hm_fetchaccounts_uids;"
      echo "delete from hm_fetchaccounts;"
      for row in "${retention_rows[@]}"; do
         read -r name type origin stored _ <<< "$row"
         echo "insert into hm_fetchaccounts (faactive, faaccountid, faaccountname, faserveraddress, faserverport, faservertype, fausername, fapassword, faminutes, fanexttry, fadaystokeep, falocked, faprocessmimerecipients, faprocessmimedate, faconnectionsecurity, fauseantispam, fauseantivirus, faenablerouterecipients, faoauthtoken, famirrorskip, famirrorseen, faorigin) values (0, 1, '$name', 'remote.example', 110, $type, 'user', '', 30, '2026-10-01 00:00:00', $stored, 0, 0, 0, 0, 1, 1, 0, '', '', '', $origin);"
      done
      echo "update hm_dbversion set value = 6105;"
   } > "$work/retention-seed.sql"
   run_file "$work/retention-seed.sql"
   run_file "$step6106"
   step_version=$(query "select value from hm_dbversion;")
   [ "$step_version" = 6106 ] || { echo "GATE FAILED: ${step6106##*/} left the version at $step_version" >&2; exit 1; }
   wrong=0
   for row in "${retention_rows[@]}"; do
      read -r name type origin stored expected <<< "$row"
      got=$(query "select fadaystokeep from hm_fetchaccounts where faaccountname = '$name';")
      if [ "$got" != "$expected" ]; then
         echo "   $name (server type $type, origin $origin) stored $stored: became $got, should be $expected"
         wrong=$((wrong + 1))
      fi
   done
   query "delete from hm_fetchaccounts;" > /dev/null
   if [ "$wrong" -ne 0 ]; then
      echo "GATE FAILED: the 6106 step gave $wrong of ${#retention_rows[@]} external accounts the wrong DaysToKeepMessages on $backend" >&2
      exit 1
   fi
   echo "   ${#retention_rows[@]} external accounts, each given the value it should have"
fi
echo "GATE PASSED on $backend: 6029 to $required, the same schema as a new database, and the steps' rows as they should be"
