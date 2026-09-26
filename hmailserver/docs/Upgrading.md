Upgrading
=========

How to move an existing hMailServer installation to this fork, or from one release
of it to the next, and what the database upgrade actually does. Written because the
upgrade is the one operation where an unpleasant surprise costs you mail rather than
time.

The short version
-----------------

Run the new installer over the existing installation. It preserves your
configuration and your mail, stops the service, upgrades the database schema,
and starts the service again. There is no separate migration step and no export
and re-import.

**Take a backup first anyway.** Not because the upgrade is unreliable — the chain
below has run continuously since hMailServer 4 — but because the one thing an
upgrade cannot do is undo itself. See [Rolling back](#rolling-back).

**Sealed secrets are off after an upgrade until you turn them on.** From schema 6103
each account's second factor and the `[Settings]` secrets can be sealed in the
database under the installation's stored-secrets key, but an upgrade makes no key:
the server goes on storing them as written, says so in the application log at each
start, and the setup checklist offers the step. To turn sealing on, from an elevated
prompt on one server (`sudo hmailserver ...` on Linux):

```powershell
.\hMailServer.exe --create-stored-secrets-key
.\hMailServer.exe --export-stored-secrets-key D:\Escrow\hmailserver-stored-secrets.key
Restart-Service hMailServer
```

then `--import-stored-secrets-key` the same file on every other server that shares the
database, and restart each. Keep the exported file with the database backups, stored
apart from them: from then on a database-level backup restored without it opens none
of those secrets ([SecretsAtRest.md](SecretsAtRest.md)). A new installation's installer
makes the key itself and says to export it.

Where you can upgrade from
--------------------------

The upgrade chain is continuous, from schema version `0` through to whatever
version this build requires, applied one step at a time. A database at any
intermediate version is brought forward step by step, so there is no "you must
first upgrade to 5.x" hop to plan around.

**The step count and the target version are deliberately not written here.**
They have gone stale on this page four times now, and a version number is
exactly the sort of fact a reader trusts without checking. Read them from the
tree instead: `REQUIRED_DB_VERSION` in
`hmailserver/source/Server/Common/Application/Constants.h` is the version this
build demands, and `build\check-schema-versions.ps1` prints both numbers and
proves the chain reaches one in exactly the other.

That includes databases created by the *original* upstream project. This fork did
not branch the schema; it extended it. The current version is a superset,
reached by the same mechanism upstream used.

**How far back the chain reaches depends on your backend, and this is the one
qualification worth knowing before you start.** The steps are registered once, for
every backend, but the SQL scripts they run are per-dialect and they do not all
exist:

| Backend | Steps that ship | Reaches back to |
|---|---|---|
| MySQL / MariaDB | every step | schema `0` (hMailServer 1.0) |
| Microsoft SQL Server | every step | schema `0` (hMailServer 1.0) |
| PostgreSQL | every step from `5001` onwards | schema `5001` |
| SQL Server Compact (internal) | every step from `5001` onwards | schema `5001` |

In practice that is not a gap, because neither PostgreSQL nor the internal SQL CE
database was a supported backend before hMailServer 5, so no database older than
`5001` exists on either. But it is why the answer is "any version of *your*
backend" rather than "any version". If a step's script is missing, DBUpdater says
so by name and stops before touching anything — it checks that every file on the
path exists before it runs the first one.

What the upgrade touches
------------------------

* **The schema only.** Messages live on disk, not in the database — the database
  holds accounts, domains, settings and message *metadata*. No upgrade step rewrites
  message files.
* **Every supported backend.** Separate script sets exist for MySQL/MariaDB, MS SQL
  Server, PostgreSQL and the embedded SQL CE. The installer picks the set matching
  your configured backend.
* **Nothing is deleted that carries live data.** One consequence is worth knowing
  because it looks alarming and is not: a schema comparison between an upgraded
  database and a freshly created one can show an extra table. The concrete case is
  `hm_adsynchronization`, a long-dead table that step 5004 → 5005 drops on MS SQL
  Server, MySQL and SQL CE but not on PostgreSQL, and which no `CreateTables` script
  creates on any backend. So an upgraded PostgreSQL database keeps it and a fresh one
  never had it. It is unused either way, and its presence is not a sign of a partial
  upgrade.

The account the service runs as
-------------------------------

An upgrade keeps the account the service runs as - LocalSystem, for an
installation that began before 25 September 2026 - unless the installer's
*Service account* page is answered otherwise; a new installation runs it as the
virtual account `NT SERVICE\hMailServer`. The page offers the way each side, and
`/serviceaccount=virtual|localsystem|keep` does the same for a silent upgrade.
A move is made after the database is upgraded, and only once the service's folders
are granted cleanly: an attended installation asks about any folder or key file
the configuration names outside the program folder, a silent one leaves the
service on LocalSystem when there is one. Under the virtual account the live
update is checked and an administrator installs it from the release page, since
the service cannot run an installer itself, and `hMailServer.INI` is read-only to
the service - the administrator password is then changed with
`hMailServer.exe /SetAdministratorPassword`. [ServiceAccount.md](ServiceAccount.md)
has the rest.

Doing it
--------

1. **Back up.** Both halves, and they are separate things:
   * the **database** — with your backend's own tools (`mysqldump`, `pg_dump`,
     SQL Server backup). The built-in backup covers this too, but a native dump is
     what you want if you need to restore into a different server.
   * the **data directory** — the message files. This is the part that cannot be
     reconstructed from anything else.
2. **Note your current versions.** The server version is in the Control Panel. The
   schema version is in a table of its own — `select value from hm_dbversion`, a
   single-column, single-row table, *not* a row in the settings table. (The Control
   Panel shows it on the **Server status** page and DBUpdater reads it too; the SQL
   is here for when the server will not start and neither of them will connect.)
   Write both down — if you need support, they are the first two questions.
3. **Run the new installer.** It stops the service, installs, upgrades the schema,
   and restarts.
4. **Check it came up.** The service should be running and listening on your
   configured ports. Then check the ERROR log for the window around the upgrade —
   an empty ERROR log is the signal you want.
5. **Send a message to yourself**, end to end, and read it back over IMAP or POP3.
   A service that starts is not the same as a server that delivers.

An upgrade without a window
---------------------------

Since 23 September 2026 an upgrade no longer needs a booked window: the server
drains before it stops, and it can bring its own schema forward when it starts.
What a single node can and cannot promise is written down at the end of this
section, because the difference matters when you plan.

**The sequence.** Whether the live update runs it (`POST /api/v1/update/install`,
`Status.InstallUpdate`, or the unattended window) or you do:

1. **Drain.** The SMTP, IMAP, POP3 and ManageSieve listeners are closed and the
   delivery queue is paused. A connection attempted from now on is *refused at the
   socket* - not accepted and then cut - which every MTA on the internet treats as
   "try again later" and retries on its own schedule, so no mail is lost and no
   sender sees a bounce. The sessions already open finish at their own pace; a
   delivery already talking to a remote finishes, and a queued message not yet
   started stays in the queue, unlocked. `/readyz` answers `503 not ready:
   draining`; the metrics listener, the REST API and the web services stay up, so
   the Deck, a load balancer's probe and the update helper can all still see the
   server. The application log says `Drain: listeners closed; N session(s) and M
   delivery(ies) in flight; ...`.

   What a person sees, from the code: a mail client's IMAP, POP3 and SMTP
   submission are refused and retried by the client, and so is a Sieve client on
   ManageSieve; the **webmail keeps working** - it reads mail over the REST
   listener in-process, and its send (`POST /api/v1/me/messages`) writes the
   message into the delivery queue rather than opening an SMTP session, so a
   message sent from it during the drain is accepted and waits, unlocked, in the
   queue until the drain ends; **CalDAV and CardDAV** are served by the web-services
   listener and keep answering; the Deck and every `/api/v1` route keep answering.
2. **Stop.** The service stop finds the doors already closed and waits up to
   `ShutdownDrainSeconds` for what is still in flight (nothing, usually, by now:
   *a stop after a drain does not wait again for what has already drained*), then
   tears the servers down. With `ShutdownDrainSeconds` at its default of `0` the
   doors still close first, but nothing is waited for - **set it on a production
   server** (30 is a reasonable figure; the HA runbook says the same), or a session
   mid-DATA at the moment of the stop is cut.
3. **Install.** The binaries are replaced. The Windows installer runs DBUpdater as
   it always has; the `.deb` and `.rpm` run `hmailserver --upgrade-database` in
   their post-install step; the container's entrypoint runs it at every start.
4. **The schema step, in the service's own start.** When the database is older
   than the binary and `[Database]` in `hMailServer.ini` carries
   `UpgradeOnStart=1`, the service applies the upgrade chain itself as it starts -
   the same code `hmailserver --upgrade-database` runs, one step at a time, one
   log line per step (`Schema: 6086 -> 6087 (Upgrade6086to6087MSSQLCE.sql)`), each
   step in a transaction where the backend has one - and then continues the start.
   Without it, or when a step fails, the start is refused with `HM5011` as before,
   and the message names the failing step and the version the database is at; the
   steps before it have been applied and a second start resumes there. The key is
   in the file, not the settings store, by necessity: it has to be read before the
   database is at a version this build can open. It is off by default. Nothing is
   backed up first - *Take a backup* above is the rule, and this is the step that
   makes it matter. **One node at a time:** set it on the node you start first; a
   second node that starts against the same database reads the new version and
   does nothing. Two nodes that do start together are serialised step by step -
   on PostgreSQL and SQL Server by the version row, which each step locks
   (`select ... for update`, `with (updlock, holdlock)`) for the length of its
   transaction; on MySQL and MariaDB, whose first DDL statement in a step commits
   the transaction and frees that lock, by a named lock
   (`GET_LOCK('hmailserver_schema_upgrade')`) held until the step is over and
   waited on for up to four hours - so the second blocks, then finds the step
   done and moves on - but that is the net, not the plan. Until 26 September
   2026 MySQL had only the row lock, so the second node ran the same step beside
   the first and failed on a table or a column already there. A server that
   refuses `GET_LOCK` altogether - MariaDB Galera with wsrep on - still upgrades,
   with only the row lock, and the upgrade's output says so for each step: there,
   upgrade from one node at a time.
5. **Start.** `/readyz` answers 200 again once the database probe agrees, and the
   load balancer routes to the node again.

**What is refused, and for how long.** From the drain to the moment the new
version is listening: the length of the install plus the service's start, which is
seconds to a couple of minutes on a single node, spent as *refused connections*,
which senders retry. That is the honest limit of a single node. A rolling restart
with no refusal at all needs a second node (section 7 of Roadmap2, the HA runbook's
planned failover), and the drain is what that failover now uses.

**Maintenance mode without an upgrade.** The drain is a state of its own, with
three routes, all server-wide (`manage_server` to enter and leave, `view_configuration`
to read; refused to a domain-restricted or read-only key; recorded on the audit
trail as `server` / `drain` / `entered` and `left`):

```
POST   /api/v1/server/drain     enter: listeners closed, queue paused, /readyz 503 "draining"
GET    /api/v1/server/drain     {"draining":true,"since":"2026-09-23 10:15:02","sessions":1,"deliveries":0,"drained":false}
DELETE /api/v1/server/drain     leave: listeners reopened on the same ports, queue resumed, /readyz 200
```

`POST` while draining, or when the server is not running, answers `409` with the
reason; so does `DELETE` when nothing is draining. `drained` is true once nothing
is in flight, which is when a stop would be instantaneous. If a port cannot be
reopened on `DELETE` - something else took it while the listeners were closed -
the answer names it in `not_reopened`, the error log carries `HM7150`, and the rest
are open; a restart tries every port again. If none could be reopened because the
io threads did not answer, the state stays `draining` and `DELETE` answers `500`
with the same document and `error: the listeners could not be reopened`, so that
a `running` is never declared over closed ports; try again, or restart. The state
shows as `draining` in `/healthz`, `hmailserver_state`, the Deck's dashboard and
its update view (which carries **Drain** and **Resume** buttons), the Control
Panel's status page and `Application.ServerState` (`hStateDraining`, 5). A live
update refuses to apply while somebody else's drain is in progress ("already
draining"): leave it, or wait for it to end, and apply again. **The update's own
drain is watched.** The helper is meant to stop the service, after which the
outcome it writes is read at the next start. The helper writes it only once it
has seen the service running again, and the service says it is running before
it has loaded anything, so the first thing a start does is look: when the
helper's log in the Updates directory (`apply-<version>.log`, or on Linux the
`apply.XXXXXX` it writes until it exits) is newer than the last outcome
reported and there is no outcome yet, the start-up update task looks for it
every second, for `UpdateServiceWaitSeconds` plus two minutes, and reports it
as soon as it is there rather than at the quarter-hour run after. A helper that finishes without
stopping it - an installer that failed while the service stayed up, or one that
did nothing - or that never gets that far would otherwise leave the server
drained for ever. So after the helper is started the server polls for its
outcome every five seconds, for up to **ten minutes** (a constant,
`UPDATE_APPLY_DRAIN_MINUTES` in `UpdateInstaller.cpp`, not a setting): an outcome
written while the service is still running, or the ten minutes passing with no
stop, and the server leaves the drain itself, records `left` on the audit trail
as `server`, logs why, and reports the outcome then rather than at the next start
(`HM7151` when the helper wrote nothing at all). **A reinitialise
ends maintenance mode**: SIGHUP, the Control Panel's Reinitialize, `POST
/api/v1/server/reinitialize`, a backup restore and an ACME deployment all stop
and restart the servers in place, and the restarted servers accept again; the
server records the `left` event itself (actor `server`), so the trail pairs your
`entered` with what ended it. **Apply now**, on the Control Panel's pending bar
and the Deck's Settings view ([SettingsApply.md](SettingsApply.md)), is a
reinitialise too: while the server is draining its question says the drain ends,
and `POST /api/v1/server/reinitialize` answers `drained: true` when it did.

**On Windows, `hMailServer.exe --upgrade-database`** now runs the same chain from
the command line, for all four backends, without DBUpdater's window or the
administrator password; it attaches to the prompt it was started from and exits
0 (done, or nothing to do), 1 (failed; the output names the step) or 2 (no
`[Database]` configured). Stop the service first. On Linux the option is what it
was; it now runs the backend's prerequisites script before the chain, as
DBUpdater always has.

**What is thinner than a window-free upgrade.** A single node refuses
connections for the length of the install; there is no rolling restart without a
second node; the in-process schema step does not run DBUpdater's after-the-fact
schema probes (`SchemaVerification.cs`), which the installer's path keeps; and the
drain's state is per process, so a warm-standby pair drains one node at a time.

If the upgrade fails
--------------------

The installer checks both that the database tool launched *and* its exit code, so a
failed or cancelled schema upgrade is reported instead of passing silently: on a
non-zero exit code the installer shows an error dialog naming the exit code and
telling you to re-run `DBSetupQuick.exe`, and then **fails the install** — it
raises, so setup ends with a non-zero exit code rather than finishing as though
nothing had happened (it used to carry on and exit 0, which a scripted deployment
could not tell from success). If you script installs, treat a non-zero exit code,
or a `hm_dbversion` that is still on the old value afterwards, as the failure; do
not treat "the installer finished" as success.

If it does fail, the service may be installed but the schema only partly upgraded.
The server does not paper over that: on startup it compares the database's version
against the version it requires and **refuses to run** if they differ, in either
direction, with one of two messages worth knowing because they are what you will
search for:

* `The database is too old for this version of hMailServer. Please run hMailServer
  Database updater (DBUpdater.exe) to upgrade it.`
* `The database is too new for this version of hMailServer. Please upgrade
  hMailServer.`

For the first, run **`DBUpdater.exe`** from the installation's `Bin` folder — that is
the standalone updater, and it is what the error message names. (The installer drives
`DBSetupQuick.exe` for the same job during an install; either will bring the schema
forward.) It picks up from whichever version the database actually reports, so a
retry after a partial upgrade resumes rather than restarting. If it fails again, the
log names the step it stopped on, and that step number is the useful thing to put in
an issue.

The second message means the database has already been upgraded past this binary —
you are running an older server against a newer schema. Install the matching or newer
server rather than trying to downgrade the database.

Rolling back
------------

There is no downgrade path, and this is the one genuine sharp edge in the process.
The schema upgrade is one-way: an older server will refuse to run against a newer
`dbversion` rather than misinterpret it, which is the correct behaviour but means
"just reinstall the old version" does not work on its own.

To roll back you need the pre-upgrade database backup. Restore it, then reinstall the
older server. The data directory does not normally need restoring — message files are
not rewritten by an upgrade — but if you restore an older database against a newer
data directory, any mail that arrived after the backup will exist on disk with no
metadata row, and will be invisible.

Which is the real argument for the backup: not fear of the upgrade, but the fact that
rollback is only as good as the snapshot you took first.

**Restore `hMailServer.ini` with it, when the version you roll back from sealed the
stored secrets** (schema 6103 on, [SecretsAtRest.md](SecretsAtRest.md)). Such a server
writes the `[Settings]` secrets into the file as `HMK1:` envelopes, and an older server
reads the file's lines as they are: its `/Register` - which the older installer runs -
hands `ServiceAccountPassword`'s envelope to Windows as the service account's password,
and the service then cannot log on and does not start; its first moments, before the
restored database puts its rows back into the file, run SRS, BATV and OAuth2 on the
envelopes. So restore the pre-upgrade `hMailServer.ini` beside the database, or, where
you have none, set `ServiceAccountPassword` (and any other of those secrets you use)
again with the older version's Control Panel once it runs, and run `hMailServer.exe
/Register` again.

Special cases
-------------

**Moving to a different database backend** is not an upgrade and the chain does not
do it. There is no supported in-place conversion between backends. The route is a new
installation against the new backend and a migration of accounts and mail — and there
is no first-class tool for that yet, which is honestly recorded as a gap in
[Roadmap.md](../../Roadmap.md).

**Moving to a new machine**: install the same version on the new machine, restore the
database and the data directory, and confirm the data-directory path in the settings
matches where you actually put it. Upgrade afterwards, as a separate step, so that if
something goes wrong you know which of the two changes caused it.

Two values that are not in the database go with it by hand, before the new machine's
service first starts on the restored database: `PasswordPepper`, whose value is copied
into the new machine's `hMailServer.ini` ([Backup.md](Backup.md)), and - from schema
6103, where the installation has one - the **stored-secrets key** every account's second
factor and the `[Settings]` secrets are sealed under. Export it on the old machine
(`hMailServer.exe --export-stored-secrets-key FILE`, elevated, or `sudo hmailserver
--export-stored-secrets-key FILE` on Linux) - or take the file exported when the key was
made, if the old machine is gone - and import it on the new one with
`--import-stored-secrets-key FILE`. The new machine's installer made a key of its own for
the empty database it installed with: remove that `[Security] StoredSecretsKey` line from
the new machine's `hMailServer.ini` first, or the import is refused. Never copy the line
from the old machine, which is sealed under its own DPAPI. Without the key every second
factor refuses every code on the new machine and those settings read as not set, and the
error log says `HM7495` and names this step; with the key lost for good,
`--forget-sealed-secrets KEYID` clears what it sealed ([SecretsAtRest.md](SecretsAtRest.md)). A
move through a backup archive rather than the database does not need the key: the archive
carries those secrets opened, and the restore seals them under the new machine's own. It
still needs the pepper.

**Upgrading silently** — see the [unattended install](../../README.md#unattended-install)
notes. Until 6.2.23 a silent upgrade of an installation with an administrator password
set hung: `DBSetupQuick.exe` forwarded only `/SilentIfOk` and `/silent` to
`DBUpdater.exe`, not the password, and DBUpdater then opened a modal password dialog
nobody was there to answer. That is fixed. DBSetupQuick forwards the password on the
upgrade path as well as the create path, the installer accepts it as
`/adminpassword=<password>` or, from 6.3.4, in the environment variable
`HM_ADMIN_PASSWORD`, and under `/silent` the shared authenticator fails with an exit
code instead of prompting. From 6.3.4 the installer hands the password to
`DBSetupQuick.exe`, and DBSetupQuick to `DBUpdater.exe`, in `HM_ADMIN_PASSWORD`; until
then it went on their command lines unquoted, so a password with a space, a double
quote or backslashes before one reached them changed and the database step failed with
exit code 3. So:

* administrator password empty → a silent upgrade works;
* administrator password set → pass `/adminpassword=<password>` or set
  `HM_ADMIN_PASSWORD`; without it the database upgrade fails and the installer reports
  the non-zero exit code rather than waiting. A password with a double quote goes in
  `HM_ADMIN_PASSWORD`: Inno Setup drops every double quote from its command line. Set
  the variable in an elevated prompt: setup started without administrator rights
  elevates itself through the UAC prompt, and the variable does not cross it.

*(New in 6.2.28: the live update's helper
passes a single-use `/upgradetoken=<hex>` in place of the password, so an unattended
update never handles the administrator password at all.)*

**The .NET 10 runtime.** From the first release after 6.2.18, the Control Panel and
the setup tools require the .NET 10 Desktop Runtime, which the installer bundles and
installs when missing; 6.2.18 and earlier used .NET 8. Nothing about the server
itself changed — it is native code with no .NET dependency, so an upgrade that
fails to install the runtime still leaves you with a working mail server, just
without the Control Panel. The runtime is bundled as
`windowsdesktop-runtime-10.0-win-x64.exe` and installed `/quiet /norestart`, only
when it is missing, and the setup tools (`DBSetup`, `DBSetupQuick`, `DBUpdater`)
need it as well as the Control Panel does.

**MySQL and MariaDB: schema 6102 rebuilds every table.** The step converts every
table to `utf8mb4` under `utf8mb4_general_ci`, InnoDB and `ROW_FORMAT=DYNAMIC`, so
that a character outside the Basic Multilingual Plane - an emoji - is stored instead
of refused; [MigratingDatabaseBackend.md](MigratingDatabaseBackend.md#mysql-and-mariadb)
says why each of the three. What it means for the upgrade:

* **It takes as long as copying the database.** Each table is rebuilt, one after the
  other, and the largest - `hm_messages`, `hm_message_metadata`,
  `hm_messageindexterms`, `hm_messagerecipients`, `hm_audit` - take the time. With
  the schema empty it is one to five seconds; on a large installation plan minutes
  or more, and have free disk space of the order of the largest table, since InnoDB
  writes the new copy before it drops the old.
* **It needs MySQL 5.7.9 or MariaDB 10.2.2.** So do steps 6041 and 6101 before it,
  whose new tables have keys of the same size. Each of the three opens with a
  statement that needs such a key - 6041 and 6101 make and drop a table
  `hm_upgradeprobe` with one, 6102 converts `hm_accounts` - so on an older server
  the upgrade from 6.3.3 stops at the first, 6040 to 6041, with error 1071, and
  nothing has been changed; upgrade the database server, then the schema. Until 26
  September 2026 steps 6041 and 6101 made their first tables before the index that
  failed, and MySQL commits a CREATE or ALTER TABLE at once, so they stopped half
  done and every later run failed on a table already there.
* **Repairing a database an earlier build left half done.** No upgrade path repairs
  it - `UpgradeOnStart`, `hmailserver --upgrade-database` and DBUpdater all run the
  same step again and stop on the same table - so it is put back by hand, once the
  database server can hold the keys (MySQL 5.7.9 or MariaDB 10.2.2 or later, InnoDB
  as the default engine, `innodb_large_prefix` on where the server still has it), or
  the probe stops the step again. Stop the hMailServer service and take a backup of
  the database first.

  *At 6040, from 6.3.3* - `select value from hm_dbversion` answers `6040` and
  `show tables like 'hm_calendar%'` lists `hm_calendars`. The old step 6041 made that
  table and stopped at its index; nothing at schema 6040 uses it, so it is empty
  (`select count(*) from hm_calendars` answers 0). Drop what the step made and run
  the upgrade again:

  ```sql
  DROP TABLE IF EXISTS hm_calendarobjects;
  DROP TABLE IF EXISTS hm_calendars;
  ```

  *At 6100* - `select value from hm_dbversion` answers `6100` and
  `show tables like 'hm_booking%'` lists `hm_bookingpolicies` or `hm_bookinganswers`.
  A server that took the keys of step 6041 takes these as well, so this needs a
  database server changed between the two steps; if it happened, the old step 6101
  had added a column to `hm_accounts`, made `hm_bookingpolicies`, added two columns
  to `hm_messages` and made `hm_bookinganswers`. Drop those - each column only if
  `show columns from <table> like '<column>'` lists it, since MySQL has no
  `DROP COLUMN IF EXISTS` - and run the upgrade again:

  ```sql
  DROP TABLE IF EXISTS hm_bookingdecisions;
  DROP TABLE IF EXISTS hm_bookingqueue;
  DROP TABLE IF EXISTS hm_bookinganswers;
  DROP TABLE IF EXISTS hm_bookingpolicies;
  ALTER TABLE hm_messages DROP COLUMN messagesubmitterid, DROP COLUMN messageauthdomains;
  ALTER TABLE hm_accounts DROP COLUMN accountkind;
  ```

  Dropping the two `hm_messages` columns rebuilds that table, which takes as long as
  copying it. Neither repair touches a row the server had before the step.
* **It needs the ALTER privilege on the database**, not only on its tables, because
  it also changes the database's default character set. An account granted
  `ALL PRIVILEGES ON <database>.*` has it.
* **It can stop at a duplicate.** Most tables compared under `utf8_general_ci`, of
  which `utf8mb4_general_ci` is the twin, and a unique index over them cannot meet a
  duplicate it did not meet before. Some compared under a collation that tells apart
  a few things `utf8mb4_general_ci` does not: every `utf8` table on MariaDB 11.5 and
  later (`utf8mb3_uca1400_ai_ci`, which keeps `straße` apart from `strase`), the
  tables that were `utf8mb4` already - the calendar tables, and the IMAP metadata
  table of a database upgraded through 6014 - under the server's own `utf8mb4`
  collation (`utf8mb4_0900_ai_ci` on MySQL 8, `utf8mb4_uca1400_ai_ci` on MariaDB 11.4
  and 11.8, both of which keep apart two names that differ only in an emoji), and,
  in a database an administrator made, the fifteen
  tables that named no character set and took the server's default
  (`latin1_swedish_ci`, for one, keeps `Ä` apart from `A`). Two such rows under one
  unique index stop the step with error 1062, which names the index and the value.
  Rename or remove one and run the upgrade again: every statement of the step can
  run twice, a table already converted is converted again at no harm, and the step
  finishes.
* **Afterwards a `text` column may read `mediumtext`**, and a `mediumtext` one
  `longtext`: MySQL widens a column it converts to four bytes a character so that it
  holds as many characters as it did. A fresh database keeps the declared types; the
  server reads both alike.

**Scripts that change the configuration over COM.** Since 6.3.4, a
COM change to a route (and a route's addresses), an incoming relay, a DNS black
list, a SURBL server, a remote domain policy, a server message, a blocked
attachment, an SSL certificate or a group takes effect when the object's `Save()` is
called, and not before. Setting a property writes the object's own copy of the item;
`Save()` writes the copy to the database and the server reads the list again. Two
things a script written for an earlier release may have relied on no longer happen:
a property set and never saved used to change the running server until the next
restart, and now changes nothing; and another object for the same item, read before
the `Save()`, used to show the new value, and now keeps the value it read - read the
item again from its collection after the save. The Control Panel already reads,
changes and saves each item in turn, and is not affected. The reason is in
[ARCHITECTURE.md](../../ARCHITECTURE.md) ("A COM change to an item of a shared
collection is written onto a copy"): the old behaviour wrote the fields of the very
object the server's sessions were reading.

Verified against the code
-------------------------

Re-checked 21 August 2026, and this time the argument was acted on rather than
restated. The two numbers had gone stale a FOURTH time - the page said 73 steps
to 6022 while the tree had moved on - after a previous revision had already
written down that deriving them beats transcribing them, and then transcribed
them anyway. They are gone from the prose now; the sources below are the answer.
Against: `formMain.LoadScripts` in `DBUpdater` (the registered steps and the
version at each end), `Constants.h`'s `REQUIRED_DB_VERSION`,
`hmailserver/source/DBScripts` (which dialects ship which steps, and the
`hm_adsynchronization` asymmetry), `DatabaseConnectionManager::GetCurrentDatabaseVersion`
(`select … from hm_dbversion`), `Application::OnDatabaseConnected` (the two refusal
messages, verbatim), `hMailServerInnoExtension.iss` (the exit-code check and what
follows it, and the .NET 10 bundling), `DBSetupQuick`'s `UpgradeDatabase` (which
arguments are forwarded) and `Authenticator.AuthenticateUser` (fails instead of
prompting under `/silent`).
