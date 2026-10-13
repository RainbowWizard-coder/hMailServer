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
`hmlab-p-aa-*`: the owner's own containers and `hmlab-runner` (the GitLab runner) are
never touched. `aa-lab.sh` with no command prints its usage.

What it is made of
------------------

| Container | What it is | Network |
|---|---|---|
| `hmlab-p-aa-postgresql`, `-mariadb`, `-mysql`, `-mssql` | The databases, from the images `build/lab/docker-compose.yml` pins (PostgreSQL 17, MariaDB 11.8, MySQL 8.4, SQL Server 2022), each with the application user `hmlab` | back |
| `hmlab-p-aa-minio` | The object store, behind the lab's own CA, with the bucket `hmlab-aa` | back |
| `hmlab-p-aa-node1` ... `-nodeN` | The Linux server built from the tree (`aa-lab.sh build`, clang, in the CI image), one per container, on database `hmlab_aa`. node1 lays the database out with the server's own `--create-database`; the others join it as it is | back and front |
| `hmlab-p-aa-sink` | A remote mail server that takes every message and remembers it (`sink.py`): the route for `sink.test` sends there. It can be slow (`AA_SINK_DATA_DELAY_MS`) or answer every Nth message 451 (`AA_SINK_TEMPFAIL_EVERY`) | front |
| `hmlab-p-aa-balancer` | HAProxy, round robin, with the PROXY protocol v2 to SMTP (`haproxy.cfg`) | front |
| `hmlab-p-aa-load` | The load (`aa-load.py send`): numbered messages, each with `X-AA-Seq`, straight to the nodes (with a PROXY v1 header, which a node wants from the front network) or through the balancer | front |

The databases are reached only on the back network; no port is published on the
host. The credentials are generated once into `~/hmlab/aa/secrets.env` (mode 600)
and reach the containers through the environment by name, never as an argument.

The Linux server has no SQL Server backend (HM6390), so SQL Server takes part through
the engine checks (`sqlcheck.py`, over TDS) and not with whole nodes.

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

What it cannot show yet
-----------------------

- **A node's host name changing.** `nodes` starts each node with `--hostname
  node<i>`; the Compose file the project ships sets none, so a recreated container
  there is a new host. Unit AA-05 adds a node recreated without one.
- **`/readyz`.** The balancer's check is a TCP connect (`haproxy.cfg`), because the
  nodes do not open the metrics listener that serves `/readyz`. Unit AA-05 opens it
  and switches the check.
- **Clock skew between nodes.** The containers share server2's clock; a faketime
  offset per node is the nearest the lab can come.
- **A database failover that loses its last commit**, unless a PostgreSQL replica
  is added to promote.
- **SQL Server through the server's own code.** The Linux server has no SQL Server
  backend, so SQL Server takes part through `sqlcheck` alone here.
  `DatabaseClaimTester` runs inside a Windows server (ClassTester): on SQL Server
  Compact in the Windows gate, and on the server's own database when that is SQL
  Server.
