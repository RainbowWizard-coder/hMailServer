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
bash build/lab/active-active/aa-lab.sh baseline <db> [--shared-data]   # scenario S0
bash build/lab/active-active/aa-lab.sh nodes <db> [n] [--shared-data]
bash build/lab/active-active/aa-lab.sh load <tag> <count> <to> [hosts]
bash build/lab/active-active/aa-lab.sh check <tag> | queue | imap <account> | errors
bash build/lab/active-active/aa-lab.sh fault kill|freeze|thaw|cutdb|heal|restart <node>
bash build/lab/active-active/aa-lab.sh down [all]           # 'all' takes the databases and the networks too
```

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
