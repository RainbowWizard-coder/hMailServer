The active-active lab
=====================

Two or more hMailServer nodes on one database, in containers on server2 (10.0.0.19),
for [ActiveActive.md](../../../hmailserver/docs/ActiveActive.md) and the units of its
build plan. Every unit of Programme B proves itself here, as well as on the gates.
The lab is the negative control first: on a tree without those units, it shows what
two nodes do to each other today.

Run it **on server2**, as a member of the docker group, from a copy of the tree
(`/home/chris/hmlab/agent/<name>/src`, with a `.commit` file naming what it is).
Nothing needs sudo, nothing listens on the host, and everything it makes is named
`hmlab-p-aa-*`, or `$AA_PREFIX-*` for a lab of a unit's own beside the planner's (with
an `AA_HOME` of its own too): the owner's own containers and `hmlab-runner` (the
GitLab runner) are never touched. `aa-lab.sh` with no command prints its usage.

What it is made of
------------------

| Container | What it is | Network |
|---|---|---|
| `hmlab-p-aa-postgresql`, `-mariadb`, `-mysql`, `-mssql` | The databases, from the images `build/lab/docker-compose.yml` pins (PostgreSQL 17, MariaDB 11.8, MySQL 8.4, SQL Server 2022), each with the application user `hmlab` | back |
| `hmlab-p-aa-minio` | The object store, behind the lab's own CA, with the bucket `hmlab-aa` | back |
| `hmlab-p-aa-node1` ... `-nodeN` | The Linux server built from the tree (`aa-lab.sh build`, clang, in the CI image), one per container, on database `hmlab_aa`. node1 lays the database out with the server's own `--create-database`; the others join it as it is | back and front |
| `hmlab-p-aa-sink` | A remote mail server that takes every message and remembers it (`sink.py`): the route for `sink.test` sends there. It can be slow (`AA_SINK_DATA_DELAY_MS`) or answer every Nth message 451 (`AA_SINK_TEMPFAIL_EVERY`) | front |
| `hmlab-p-aa-balancer` | HAProxy, round robin, with the PROXY protocol v2 to SMTP, IMAP and POP3 (`haproxy.cfg`); to SMTP alone with `AA_PROXY_MAIL=0`, for a tree before unit AA-23 | front |
| `hmlab-p-aa-load` | The load (`aa-load.py send`): numbered messages, each with `X-AA-Seq`, straight to the nodes (with a PROXY v1 header, which a node wants from the front network) or through the balancer | front |
| `hmlab-p-aa-client-a`, `-client-b` | Two clients behind the balancer, each with an address of its own, for the `proxy` scenario (`aa-load.py logon`) | front |

The databases are reached only on the back network; no port is published on the
host. The credentials are generated once into `~/hmlab/aa/secrets.env` (mode 600)
and reach the containers through the environment by name, never as an argument.

The Linux server has no SQL Server backend (HM6390), so here SQL Server takes part
through the engine checks (`sqlcheck.py`, over TDS) alone. Whole nodes on SQL Server
are the Windows lab's, below.

The commands
------------

```
bash build/lab/active-active/aa-lab.sh init                 # once: ~/hmlab/aa, credentials, the lab CA
bash build/lab/active-active/aa-lab.sh image                # the node image (CI image + client tools)
bash build/lab/active-active/aa-lab.sh build                # the server of this tree (about 4 minutes)
bash build/lab/active-active/aa-lab.sh up postgresql mariadb mysql mssql
bash build/lab/active-active/aa-lab.sh sqlcheck <db>        # the engine checks, below
bash build/lab/active-active/aa-lab.sh up-dst <db>          # postgresql, mariadb or mysql in Europe/London, its clock movable
bash build/lab/active-active/aa-lab.sh dst <db> autumn|spring   # a lease on each clock across that change
bash build/lab/active-active/aa-lab.sh baseline <db> [--shared-data]   # scenario S0
bash build/lab/active-active/aa-lab.sh nodes <db> [n] [--shared-data]
bash build/lab/active-active/aa-lab.sh load <tag> <count> <to> [hosts]
bash build/lab/active-active/aa-lab.sh check <tag> | queue | imap <account> | errors
bash build/lab/active-active/aa-lab.sh fault kill|freeze|thaw|cutdb|heal|restart <node>
bash build/lab/active-active/aa-lab.sh proxy                # unit AA-23: whom the auto-ban bans behind the balancer
bash build/lab/active-active/aa-lab.sh down [all]           # 'all' takes the databases and the networks too
```

A unit of the build plan runs a lab of its own beside the others: `AA_PREFIX` names
everything it makes (it must begin `hmlab-`; `hmlab-p-aa` by default) and `AA_HOME`
is where it keeps its credentials and results (`~/hmlab/aa` by default).

`down all` leaves the volumes (`docker volume ls --filter name=hmlab-p-aa`); remove
them by name when the lab is finished with. `~/hmlab/aa/runs/` keeps every run's
results, `~/hmlab/aa/results/` the ones a document quotes.

The engine checks (`sqlcheck`)
------------------------------

What each engine does with the statements the design builds on, through the client
library the server itself uses and in the same calls: libpq's `PQexec` and
`PQcmdTuples`, libmariadb's `mysql_real_connect`, `mysql_query` and
`mysql_affected_rows`, and for SQL Server the row count of the TDS `DONE` token,
which is what ADO hands back as `RecordsAffected`.

Run on 12 October 2026 against PostgreSQL 17.11, MariaDB 11.8.9, MySQL 8.4.11 and
SQL Server 2022 CU27 (`~/hmlab/aa/runs/sqlcheck-*.jsonl`):

| Check | PostgreSQL | MariaDB | MySQL | SQL Server |
|---|---|---|---|---|
| Row count of an update that matches a row and changes nothing | 1 | 1 with `CLIENT_FOUND_ROWS`, 0 without | 1 with `CLIENT_FOUND_ROWS`, 0 without | 1 |
| 300 rows claimed by two connections with `... where id = @ID and owner = 0`, won on a count of 1 | every row won once | every row won once | every row won once | every row won once |
| A claim held until the database's clock + 2 s: refused before, taken after | yes | yes | yes | yes |
| UIDs as the server takes them today (counter raised, read back, row inserted: three statements), two writers and a reader, 800 rows | 140 and 157 numbers given twice; 9 and 5 rows the reader never saw | 288 and 300 given twice | 0 and 2 given twice; 0 and 1 never seen (its commit is slower; the race needs a wider window) | 396 given twice |
| The same in one transaction | 0 and 0 | 0 and 0 | 0 and 0 | 0 and 0 |

Two limits of these runs. They used each engine's default isolation, which for SQL
Server is READ COMMITTED, where the server's own sessions run at READ UNCOMMITTED
(`ADOConnection::Connect`); and the databases' clocks were in UTC, so the local
time the server's `GETDATE()`, `NOW()` and `current_timestamp` give never crossed a
daylight-saving change. Unit AA-01 of the build plan added both, below.

### What AA-01 added (13 October 2026)

In the same `sqlcheck` run against the same four engines (`counts`, `named`,
`rollback`; AA-01's own instance, `AA_PREFIX=hmlab-aa-aa-01`, results in its
`runs/`):

| Check | PostgreSQL | MariaDB | MySQL | SQL Server |
|---|---|---|---|---|
| A claim that recognises itself (`... and (owner = 0 or token = @epoch)`), run a second time as the database layer runs a statement again after a reconnect | 1 | 1 with `CLIENT_FOUND_ROWS`, 0 without | 1 with `CLIENT_FOUND_ROWS`, 0 without | 1 |
| The same claim without its own epoch, run again; another epoch's | 0; 0 | 0; 0 | 0; 0 | 0; 0 |
| `SET NOCOUNT ON`, then an update of one row | | | | no count (`None`), 1 again with it off |
| 300 rows claimed by two connections both at READ UNCOMMITTED | | | | every row won once |
| 100 named leases with no row yet, taken by insert-or-claim by two connections | every name won once | every name won once | every name won once | every name won once, at READ UNCOMMITTED too |
| A lease on the UTC clock + 2 s: refused before, taken after | yes | yes | yes | yes |
| UIDs in one transaction, every fourth rolled back, a reader at the engine's own isolation: rows seen that were never committed / rows never seen | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| The same reader at READ UNCOMMITTED, as the server's sessions run | | | | **89** seen that were rolled back, 88 of their numbers then given to another row, 88 never seen (the negative control) |
| The same reader at READ UNCOMMITTED with the `READCOMMITTED` hint | | | | 0 / 0 |

And across a daylight-saving change (`up-dst`, `dst`): the database in
`Europe/London`, its whole clock put by libfaketime (0.9.10, built by the lab, its
threaded form) a minute before the change of 25 October 2026 (`autumn`) or of 28
March 2027 (`spring`) - these two changes and no others, the clock put forward to
them before they come and back to them once they are past (libfaketime reads a
signed offset, which `dst` writes either way) -
a lease of 120 seconds taken on the local wall clock (`NOW(6)`, `localtimestamp`)
and one on the UTC clock, and both looked at 30 seconds after the change and again
after the lease's length. On PostgreSQL 17.11, MariaDB 11.8.9 and MySQL 8.4.11, in
both seasons, the local clock moved by the hour and the UTC lease was held for its
length and no longer; the wall-clock lease was expired 30 seconds after the spring
change and still held after its length in autumn - an hour early and an hour late
(`runs/sqlcheck-<db>-dst-<season>-*.jsonl`). On PostgreSQL the server's own
`current_timestamp` is compared as an instant, so there only the repeated autumn
hour is wrong; the check reads the wall clock to show the shape MySQL's `NOW()` and
SQL Server's `GETDATE()` have. With a change already past - a copy of the check with
the spring change put at 29 March 2026, measured on 13 October - the clock was put
back to it and the check passed on PostgreSQL; written as `+-N`, as `dst` wrote a
negative offset until then, libfaketime took it for none and the check stopped with
the clock where it was. SQL Server was not taken across a change, and cannot be
this way: `up-dst` makes the three Linux engines only, and libfaketime preloaded
into SQL Server 2022 on Linux leaves its clock as it is (13 October 2026, a
container of the image the lab pins: an offset of 30 days ahead and of 30 days back
each left `SYSDATETIME()` and `SYSUTCDATETIME()` at the host's time). The Windows
lab's `hmlab-aa-mssql`, in `Europe/London`, crosses the change of 25 October 2026
itself, which is when AA-06's S13 can watch a lease across it.

Through the server's own database layer, `database-claim-tests` (built by
`linux-build` beside the server) against the same PostgreSQL, MariaDB and MySQL,
over TLS where the engine offers it, which the server asks for by default
(`[Database] TlsMode` prefer): every count as above, a claim run again 1, 200 rows
and 50 named leases raced by two connections each won once, a connection closed,
ended (`pg_terminate_backend`, `KILL`) and closed for idling under a claim each
reconnected, said and counted 1, the UTC lease refused, renewed, taken and fenced,
and the UTC clock 0 s from the machine's. Its negative controls, each a build with
one line of `MySQLConnection.cpp` taken out: without `CLIENT_FOUND_ROWS` an update
that leaves its row as it was counted 0 on MariaDB and MySQL; without TLS's error
2026 among the connection problems the statement after a `KILL` failed on both;
without 4031 the statement after MySQL closed an idle session failed there. With
`TlsMode=disable` a `KILL` is answered 2013, which the layer reconnected already:
without the 2026 case both passed with TLS off.

The checks AA-01's repair added, after an adversarial check found that a caller's
`DALErrorInSQL` forgave every failure but a lost connection on SQL Server and
Compact, and on MySQL and MariaDB a lost connection too, and that MariaDB's 4031 is
not MySQL's: an insert into a table that does not exist, told to forgive a
duplicate, counts -1, and so does one of a `NULL` into a `NOT NULL` column (0 on
SQL Server, where that is an integrity violation as a duplicate is); an insert told
to forgive a duplicate on a session the database ended is reconnected, counted 1
and written; a statement MariaDB refuses with 4031 (a `CREATE TRIGGER` that
`FOLLOWS` a trigger there is not, its number read with `GET DIAGNOSTICS`) fails at
once, counted -1, and is not run again; and a run whose tables cannot be dropped at
the end says so and fails. All passed on the same three engines. Their negative
controls: the bitwise test of what to forgive put back - the insert into a missing
table was forgiven on MariaDB and MySQL, and with that check left out the insert
after a `KILL` was not reconnected and wrote nothing; 4031 a lost connection on
every server - on MariaDB `TryExecute` answered a lost connection, and
`ExecuteCount` ran the statement again on new connections for some six seconds
before it failed, while MySQL passed; a drop at the end that cannot succeed - the
tester as AA-01's first commit had it passed, said nothing and left `aaclaimprobe`
on the database, where it now fails and says so.

And SQL Server's half through the server's own code, on node W1 of the Windows lab,
whose database is `hmlab-aa-mssql`: `Utilities.RunTestSuite` over COM, so
that `DatabaseClaimTester` ran inside the service through `ADOConnection` and the
Microsoft OLE DB Driver 18 on SQL Server 2022 CU27, at READ UNCOMMITTED, after its
SQL Server Compact half (the Compact runtime installed on W1 for the run and removed
after it). Every check held there: an update that leaves its row as it was counted
1, a claim run again 1, 200 rows and 50 names raced each won once, a reconnect said,
the UTC lease, and the UTC clock 0 s from W1's. With the server's `user options`
set to 512 (`NOCOUNT` on for every session) this build passed again, and a build
without `ADOConnection::Connect`'s `SET NOCOUNT OFF` failed at the first count, "An
INSERT counted: -1"; with `user options` at 0 that build passed. The internal suite
itself stopped after the tester, at AdminUpdateTester, whose installer fixtures only
the Windows gate's machines have.

Scenario S0: what two nodes do today
-----------------------------------

`aa-lab.sh baseline <db>`: two nodes of master 74aed9ab8 on one database, each with
its own data directory, 2,000 messages relayed to the sink through both, then 1,000
into one mailbox through both, eight connections at a time. Measured on 12 October
2026 (`~/hmlab/aa/results/s0-*.log`):

| | PostgreSQL 17.11 | MariaDB 11.8.9 | MySQL 8.4.11 |
|---|---|---|---|
| Relayed: delivered once | 1,894 of 2,000 | 1,878 | 1,971 |
| Relayed: delivered twice | 0 | 0 | 0 |
| Relayed: **never delivered** | **106** | **122** | **29** |
| Into one mailbox: **left in the mailbox** | **118 of 1,000** (`UIDNEXT` 930) | **226** (`UIDNEXT` 918) | **82** (`UIDNEXT` 986) |
| UIDs given to two messages | 0 | 5 (10 rows): `SELECT` showed 221 where `STATUS` said 226 | 0 |
| Served as a placeholder by one of the two nodes | all 118 | all 221 | all 82 |

Why:

1. Both nodes read the same due rows and both "lock" them: the lock is an
   unconditional update, and the database layer counts no rows.
2. The node that has no file for a row reports HM5006 and deletes the row by its id.
3. For one local recipient the queue row has already become the mailbox's row, so
   what it deletes is mail that was delivered (its file is left on the other disk).
4. A node asked for a message whose file is on the other disk writes a placeholder
   over the name (HM5026) and serves that.

With one data directory shared by both nodes (`--shared-data`, PostgreSQL), which
WarmStandby.md forbids, nothing was lost but 1,489 of the 2,000 relayed messages went
out twice.

Scenarios the units add
-----------------------

Each unit of Programme B adds its scenario here, as a command of `aa-lab.sh` or a
script beside it, and records its result in its commit message. The scenarios, and
what each must show, are listed in ActiveActive.md under "How it would be tested".

### Unit AA-23: the client behind the balancer (`proxy`)

`aa-lab.sh proxy`, on the nodes and the balancer that run. Since unit AA-23 the
IMAP and POP3 listeners read the PROXY protocol as SMTP's does, from the peers on
lists of their own; the nodes trust the front network for all three (node.sh
writes the four `hm_settings` rows when it lays the database out, before any node
starts), and the balancer sends version 2 to all three. Two clients behind the
balancer, A and B, are containers with addresses of their own. With the auto-ban on
every node at three failures, A fails three IMAP logons through the balancer; the
auto-ban ranges are read, and B and then A sign in once each. The same follows over
POP3, as other accounts, after the bans and the failures are cleared.

Measured on 13 October 2026 with two nodes of the unit's build
(`~/hmlab/aa/results/aa23-lab-*.log` on server2):

| | PostgreSQL 17.11 | MariaDB 11.8.9 | MySQL 8.4.11 | PostgreSQL and MySQL, `AA_PROXY_MAIL=0` (IMAP and POP3 as before AA-23) |
|---|---|---|---|---|
| IMAP: the address the auto-ban range names | client A alone | client A alone | client A alone | **the balancer** |
| IMAP: client B, who failed nothing, signs in | yes | yes | yes | **no** |
| IMAP: client A signs in with the right password | no, closed at its header | no | no | no |
| POP3: the same three | A alone; B yes; A no | A alone; B yes; A no | A alone; B yes; A no | **the balancer; B no**; A no |

Without the header every client behind the balancer is the balancer, so one
client's three wrong passwords shut out every other client for the length of the
ban. The country policy and `OnClientConnect` are still asked about the balancer,
at the accept; this scenario does not cover them.

The Windows lab (`windows/`): SQL Server through the server's own code
---------------------------------------------------------------------

Two Windows nodes of the server on one SQL Server 2022, so that SQL Server is proved
the way a Windows installation uses it: the server as a Windows service, ADO and the
Microsoft OLE DB Driver 18, sessions at READ UNCOMMITTED, the database password
sealed for each machine with DPAPI. Set up on 13 October 2026 on two VMs the owner
gave the lab, both with other roles. It is driven from the machine that hosts them
(the owner's laptop), over SSH:

| Machine | What it is | What the lab put there |
|---|---|---|
| W1 | The installer smoke VM, Windows Server 2025 (4 vCPU, 8 GB), its address by DHCP (`aa-win.py` asks `vmrun`) | The server as a real installation has it: `C:\hmlab-aa` (Bin, DBScripts, Languages, Events, WebAdmin, Data, Logs, Temp), `/RegisterTypeLib` and `/Register` (service `hMailServer`, LocalSystem, the COM server), `InstallLocation`; the service set to Manual; MSOLEDBSQL 18.7.6; a firewall rule `hmlab-aa: hMailServer lab ports` (TCP 2525, 2587, 1110, 1143 to `hMailServer.exe`, local subnet) |
| W2 | The AD lab's domain controller, Windows Server 2025 (6 vCPU, 5 GB), 192.168.11.149, which runs an hMailServer of its own (`C:\Program Files\hMailServer`, on SQL Server Compact) | The same `C:\hmlab-aa`, as a second service `hMailServerAA` (made as `/Register` makes one: own process, `"<exe>" RunAsService`, after RPCSS, LocalSystem; Manual), not registered with COM; MSOLEDBSQL 18.7.6. Its firewall is off, and stays as it was |
| server2 | 10.0.0.19 | `hmlab-aa-mssql`: SQL Server 2022 CU27 Developer edition (the image `build/lab/docker-compose.yml` pins), long-lived (`--restart unless-stopped`, 4 GB, volume `hmlab-aa-mssql-data`), published on `10.0.0.19:14330`, in `TZ=Europe/London`, the zone of the nodes. During a run, `hmlab-aa-sink` (`sink.py`) on `10.0.0.19:14325`. `~/hmlab/aa-win` holds the scripts, the runs and python-tds for the checks |

**One installation per machine.** On Windows the server finds `hMailServer.ini`
through `HKLM\SOFTWARE\hMailServer\InstallLocation` (the 32-bit view the installer
writes) before its own folder (`Utilities::GetBinDirectory`). A second installation
on a machine that has one reads the first's configuration, and runs on the first's
database; the first run on W2 did exactly that, and W2 was reverted to its snapshot.
So the node on W2 runs in **turns**: while its service runs, and for the server's own
verbs (`--set-admin-password`, `--upgrade-database`), the AD lab's service is stopped
and Disabled and `InstallLocation` names `C:\hmlab-aa`; when it stops, both are put
back and the AD lab's service is started again. `node.ps1` keeps what a turn changed
in `C:\hmlab-aa\lab\turn.json` before it changes it, and `node.ps1 release` puts it
back after any failure. Disabled, not Manual: during a turn a COM activation of
`hMailServer.Application` would otherwise start the AD lab's service, which would read
the lab's INI. The AD lab's hMailServer is down for as long as the W2 node runs.

The lab database, `hmlab_aa`, is **made by the server itself**: `aa-win.py provision`
has W1's server create it over COM, as DBSetup does (`Database.CreateExternalDatabase`,
which runs `CREATE DATABASE` with the instance's collation, SQL_Latin1_General_CP1_CI_AS,
and `CreateTablesMSSQL.sql`, then `Reinitialize`), and configures it over COM: the
listeners moved to 2525, 2587, 1110 and 1143 (W2's own hMailServer has 25, 110, 143
and 587), auto-ban off, `example.test` with `account1@` to `account4@`, and the route
that sends `sink.test` to the sink. The nodes' login, `hmlab_aa_node`, holds dbcreator
only until then, and owns the database and nothing else; the engine checks have a
login and a database of their own. A golden backup of the database as provisioning
left it is what `reset` restores. The new-installation defaults are not applied, as in
the Linux lab: the listeners have no TLS.

**Secrets** are in `~/.config/hmailserver/aa-lab-mssql` on the driving machine
(owner-only), made by `aa-win.py secrets`, and in each node's INI as the server keeps
them: the database password sealed with DPAPI for that machine
(`PasswordEncryption=6`; W1's written by `CreateExternalDatabase`, W2's by `node.ps1
set-database` in the same format, and both checked to open to the same password), the
administrator's as a PBKDF2 hash written by `--set-admin-password`. Each reaches the
machine that needs it on standard input, never as an argument; server2 is given only
the database's. sa's first password, which the container's environment shows, is a
throwaway replaced at the first start.

```
python build/lab/active-active/windows/aa-win.py secrets
python build/lab/active-active/windows/aa-win.py kit --server1 <commit>      # make-kit.ps1 on server1
python build/lab/active-active/windows/aa-win.py provision --kit <zip> --msi <msoledbsql.msi>
python build/lab/active-active/windows/aa-win.py deploy-build --kit <zip> [--node W1|W2|all] [--upgrade-database] [--start]
python build/lab/active-active/windows/aa-win.py start W1|W2 | stop W1|W2|all | status
python build/lab/active-active/windows/aa-win.py reset [--recreate]
python build/lab/active-active/windows/aa-win.py smoke W1|W2
python build/lab/active-active/windows/aa-win.py baseline
python build/lab/active-active/windows/aa-win.py sqlcheck
python build/lab/active-active/windows/aa-win.py ado-probe W1|W2
```

`start` refuses a node while the other runs or holds a session on the database:
until the units land, WarmStandby.md holds. `start all --both` and `baseline` are the
controlled measurements that break that rule on purpose; `baseline` ends with W2
stopped and W1 alone. A kit is a build of the Windows server laid out as the installer
lays out an installation (`make-kit.ps1`: `build\build.ps1 -Configuration Release`,
post-build events off, on the build machine; the C++ runtime app-local, OpenSSL,
libpq, the MariaDB client, `SHA256SUMS.txt`, which the nodes check). `deploy-build`
keeps each node's INI, and with `--upgrade-database` stops both nodes and moves the
schema through `hMailServer.exe --upgrade-database` on W1. The VMs have snapshots
`before-aa-lab` (each as it was) and `aa-lab-base` (the lab provisioned, both services
stopped, the database at its golden backup); after a revert to `aa-lab-base`, run
`aa-win.py reset`, since the database is on server2 and not in the snapshot. W1's
`firstsnapshot` is the installer smoke test's and is not the lab's.

Measured on 13 October 2026 (master 74aed9ab8, schema 6108; results in
`~/.cache/hmlab-aa-win/runs/` on the driving machine and `~/hmlab/aa-win/runs/` on
server2):

- **Each node alone** (`smoke`): the service starts, 5 sessions from its host, a
  message delivered over SMTP to `account2@` and read back over IMAP from the
  driving machine, one relayed to the sink. On W2 the other node's message in the
  same mailbox logged HM4403 (its file is on W1's disk), and HM5080 is logged at
  every start there (`CoResumeClassObjects`: `CO_E_WRONG_SERVER_IDENTITY`, the COM
  classes being the AD lab's service's).
- **`RecordsAffected` through ADO** (`ado-probe`), on both nodes, through
  MSOLEDBSQL and SQLOLEDB, at READ UNCOMMITTED: 0, 1, 1 for a row matched and left
  unchanged, 2, 1 for an insert, 1 for a delete; a claim that recognises itself 1
  and 1 again, a plain one run again 0.
- **The engine checks at READ UNCOMMITTED** (`sqlcheck`: `windows/sqlcheck-mssql.py`
  runs `sqlcheck.py`'s checks in sessions set up as `ADOConnection::Connect` sets the
  server's): counts, claims (300, each won once) and the lease as above; UIDs in three
  statements 302 numbers given twice in 800 rows, in one transaction none; and with
  writers that roll back every third number, the reader at READ UNCOMMITTED saw 109
  rows that were rolled back and never saw 108 committed ones, the reader with the
  `READCOMMITTED` hint neither.
- **Scenario S0** (`baseline`), two runs: of 2,000 relayed, 314 and 316 never
  delivered and none twice; of 1,000 into one mailbox, 218 and 260 left (`UIDNEXT`
  863 and 839), 14 and 18 UIDs given to two messages each (`SELECT` showed 204 and
  242 where `STATUS` said 218 and 260), and every message a placeholder on one of the
  two nodes; HM5006 logged 2,273 and 2,240 times. Worse than any Linux engine.

What the Windows lab cannot show yet: the faults (a node killed or frozen, cut off
from the database: `Stop-Process`, a debugger's suspend and a firewall rule would
make them, and AA-24 adds them), a balancer in front of the Windows nodes, the
object store (MinIO is on the Linux lab's own network), clock skew (both VMs keep
the laptop's time) and a machine renamed. The laptop has little memory to spare with
four VMs running, so the Windows nodes are for runs like these, not for the long
suites.

What it cannot show yet
-----------------------

- **A node's host name changing.** `nodes` starts each node with `--hostname
  node<i>`; the Compose file the project ships sets none, so a recreated container
  there is a new host. Unit AA-05 adds a node recreated without one.
- **`/readyz`.** The balancer's check is a TCP connect (`haproxy.cfg`), because the
  nodes do not open the metrics listener that serves `/readyz`. Unit AA-05 opens it
  and switches the check, which then names the metrics port and must not have
  `check-send-proxy`: the metrics listener reads no PROXY header
  ([HighAvailabilityRunbook.md](../../../hmailserver/docs/HighAvailabilityRunbook.md),
  section 3).
- **Clock skew between nodes.** The containers share server2's clock; a faketime
  offset per node is the nearest the lab can come.
- **A database failover that loses its last commit**, unless a PostgreSQL replica
  is added to promote.
- **SQL Server through the server's own code**, here: the Linux server has no SQL
  Server backend, so SQL Server takes part through `sqlcheck` alone. The Windows lab
  (above) runs it, and `DatabaseClaimTester` runs inside a Windows server
  (ClassTester): on SQL Server Compact in the Windows gate, and on the server's own
  database when that is SQL Server.
