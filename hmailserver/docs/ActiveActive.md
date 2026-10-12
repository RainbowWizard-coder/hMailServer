Active-active nodes: a design
=============================

**Not built.** This is a design, written on 29 September 2026 for 6.3.6, and
nothing in it exists in the server. There is no code for it, no schema step and
no setting. Until it is built, [WarmStandby.md](WarmStandby.md) holds without
exception: **exactly one hMailServer service may run against a database at a
time.** Two active services on one database lose mail, deliver the same mail
twice, fetch the same remote mailbox twice, and can give two messages in one
mailbox the same IMAP UID. Nothing warns when that happens.

It was read again against the code on 12 October 2026 (master 74aed9ab8, schema
6108): what had moved since 29 September is corrected below, the questions the
first version left open are decided, and the work is set out as units in the order
they are to be built. What two nodes do to each other today was measured the same
day, in the lab that the units will be proved in
([build/lab/active-active/](../../build/lab/active-active/README.md)).

What is meant here by *active-active* is two or more nodes that serve the same
domains at the same time from one database, behind one address. Any node takes
SMTP, IMAP, POP3 and the web pages, and any node delivers the queue. A node can
then be taken down for maintenance without a failover. This is the Roadmap2 row
"Active-active nodes" and work item #6. The owner closed #6 on 27 September 2026
as work that could not be finished in 6.3.5. For 6.3.6 the maintainers took the
design alone, because the work is new server code, not a change to what exists.
On 12 October 2026 the owner put it into 6.3.7.

This document says what the server assumes today, taken from the code, what two
nodes do today, and what each assumption has to become. It then lists the
decisions, the schema changes, the failure modes, the bench that proves it, and
the order the work takes.

What the server assumes today
-----------------------------

Every one of these assumes a single process. Each is correct for one node, and
each goes wrong with two.

| Where | What it does | What two nodes do to it |
|---|---|---|
| The delivery queue's start (`SMTPDeliveryManager::DoWork`, `PersistentMessage::UnlockAll`) | `update hm_messages set messagelocked = 0 where messagetype = 1 and messagelocked = 1`, once, when delivery starts: a lock left by a crash would otherwise hold its message for ever | A node that starts releases every message the other node is delivering at that moment |
| The queue's selection (`SMTPDeliveryManager::LoadPendingMessageList_`, `GetNextMessage_`) | Reads every due, unlocked message into a list, then locks each one as it hands it out | Both nodes read the same list |
| The queue's lock (`PersistentMessage::LockObject`) | `update hm_messages set messagelocked = 1 where messageid = @MESSAGEID`, with no condition on the lock and no count of the rows it changed. No backend's `TryExecute` returns a count: ADO's `Execute` is called with no `RecordsAffected`, `mysql_affected_rows` is not loaded and the MySQL connection is opened without `CLIENT_FOUND_ROWS`, and `PQcmdTuples` is not called | Both nodes "win" the same message |
| The queue's files | A queued message's file is on the local disk of the node that accepted it. The object store never holds the queue ([ObjectStorage.md](ObjectStorage.md)) | A node that claims a row whose file is on the other node's disk reports HM5006 and **deletes the row** (`SMTPDeliverer::DeliverMessage`). For one local recipient the queue row has already become the mailbox's row (`LocalDelivery::DeliverToLocalAccount_`), so what it deletes is mail already delivered |
| A missing message file (`PersistentMessage::EnsureFileExistance`) | With the file system as the store, a message whose file is missing is given a placeholder ("message undeliverable"), written under its name, and the row takes the placeholder's size | A node asked for a message whose file is on the other node's disk replaces it, on its own disk, with a placeholder, and serves that from then on |
| External fetch (`ExternalFetchManager::DoWork`, `PersistentFetchAccount`) | `UnlockAll` at start (`update hm_fetchaccounts set falocked = 0`), and an unconditional `Lock`, held by `FetchAccountLock` | The same as the queue: a remote POP3 mailbox is fetched by both nodes, and one with delete-after-download ends up split between them |
| Migration jobs (`MigrationManager::Start`, `MigrationStore`) | `RequeueRunningJobs` puts every running job back in the queue at start, and `MarkJobRunning` is unconditional | A node that starts takes back the other node's running jobs, and both import the same mailbox |
| A message's UID and mod-sequence (`PersistentMessage::AddObject`, `PersistentIMAPFolder::GetUniqueMessageID`, `GetNextModSeq`) | Under `FolderManipulationLock`, a lock inside the process, the folder's counter is increased by one statement and read back by a second, and the message is inserted by a third. The mod-sequence of a delivery, a flag change and an expunge is taken the same way | Two nodes can read back the same UID. A message can also be committed with a lower UID than one a client has already seen, and a client never fetches such a message (RFC 3501 requires UIDs to be strictly ascending) |
| The folder trees (`IMAPFolderContainer`) | Each account's folders are cached in the process, with each folder's current UID and mod-sequence, with no expiry: only the removal of an account's messages (`PersistentAccount::DeleteMessages`), a mailbox move and a shortage of memory uncache them | A folder made, renamed or removed on one node is not seen on the other until one of those happens there, or it restarts |
| The message lists (`MessagesContainer`, `Messages::Refresh`) | Each folder's messages are cached for 30 minutes and read again only when this process marks the folder, and then only the rows above the highest UID already read | A flag change or an expunge made on the other node is never read while the folder is cached, and a message committed below a UID already read is never seen |
| Change notification (`NotificationServer`) | A delivery, an APPEND, an EXPUNGE or a flag change tells every IMAP session of this process that has the folder selected. IDLE waits on it. A mailbox move asks it whether anybody has the account open (`HasMessageSubscribers`) | A session in IDLE on one node is not told about a delivery made on the other, and a move does not see the other node's sessions |
| POP3's mailbox lock (`POP3Sessions`) | A set in memory: one POP3 session per mailbox | Two POP3 sessions on two nodes can have the same mailbox open, and both can delete from it |
| The caches (`CacheContainer`, `MessageCache`, `InboxIDCache`, `AccountSizeCache`) | Accounts, domains, aliases and lists are kept for their TTL, 60 seconds by default. An owner's own setting values are read again after a minute "so that a row another node wrote is seen" (`CacheContainer.h`), and the object store's answers after half a minute | A change made on one node is seen on the other within the TTL, and not before. A disabled account can still sign in on the other node for up to a minute |
| The settings (`IniSettingStore`, `Configuration::Load`, `Application::Reinitialize`) | Read from the database at start. A change is applied on the node that made it, at once or at its next start or reinitialise, as each setting says. The message store's backend is read again every five seconds ([ObjectStorage.md](ObjectStorage.md)) | A setting changed on one node is not applied on the other until that node saves a setting, reinitialises or restarts |
| The scheduler (`Scheduler`, and the tasks `Application::CreateScheduledTasks_` registers) | Every task runs in the process that registered it: the digests, the scheduled sends, the alerts, the webhooks, the TLS-RPT and DMARC reports, the retention passes, the backup schedule, ACME, the directory sync, the object store's committer, the consistency check and the rest | Each node runs every task. Some are already safe with two nodes, and say so in their code: the DKIM key rotation, the complaint and bounce counters, the report importer's de-duplication, the eDiscovery holds, the sandbox's verdict cache, the directory sync's inserts. The others send twice, or work against a local disk that is not the other node's |
| The singletons outside the scheduler | The message indexer (the only writer of `hm_messageindexstate`'s row 0), the sandbox's poller, the migration manager's thread, `ResourceBooking`'s lock ("this process's"), the backup's run slot (`BackupManager`), the audit chain (its head read, then a row appended), `DeliveryQueue::Clear` | Two indexers write one row; two pollers and two migration threads work the same rows; two nodes decide for the same resource; two appends fork the audit chain; a clear deletes the other node's queued rows and leaves their files |
| The reports (`TlsRptReporterTask`, `DmarcRptReporterTask`) | What each report says is collected in memory, per node. A DMARC `report_id` is the day, the domain, the second and the submitter | Each node reports what it saw. Two nodes reporting one domain in the same second give two reports one id, and a receiver discards the second |
| Browser sessions ([DevicesAndSessions.md](DevicesAndSessions.md)) | Kept in the server's memory (`RestApiServer`) | A request that reaches the other node is not signed in. Ending a session ends it on its own node only |
| The account lockout (`AccountLockout`) and the rate limits | Counted in memory, "like the sibling stores". The compromised-account holds are in `hm_accountreview`, but loaded only at start (`CompromiseDetector::LoadLocks`) | Each node counts on its own, so two nodes allow twice the attempts. A hold made on one node is not in force on the other until it restarts |
| Secrets sealed to the machine ([WarmStandby.md](WarmStandby.md), "The DPAPI constraint") | `Crypt::ProtectSecret` seals with DPAPI on Windows and the `LINUX1` key file on Linux: route, per-domain relay and fetch-account passwords, fetch accounts' OAuth tokens, TLS key passphrases, every setting saved encrypted, the inbound and outbound webhooks' secrets, migration jobs' source secrets, the SNMP secrets and the sandbox's token. Only second factors and the `[Settings]` secrets are under the installation's stored-secrets key | A secret entered on one node cannot be read on the other |
| The listeners | Rows of `hm_tcpipports`, which every node reads. Only the SMTP listener reads a PROXY protocol header (`TCPServer`: `sessionType_ == STSMTP`) | Each node binds every port, as the rehearsal in [WarmStandby.md](WarmStandby.md) saw, which is what a balancer in front wants. Behind a balancer, IMAP and POP3 see the balancer's address, so auto-ban, the IP ranges and the device list act on the balancer |
| The drain (`Application::EnterDrain`) | Closes the listeners and **pauses** the delivery queue while what is in flight finishes | A drained node keeps its queued messages, deferred ones included, on its own disk: it cannot be stopped for maintenance without a failover |
| A schema upgrade (`SchemaUpgrader`, `Application::OnDatabaseConnected`) | Two walkers on one database are serialised on the version row, or on a named lock on MySQL. A node refuses a database newer than its build when it connects | Two walkers are safe. But a node of a newer build with `[Database] UpgradeOnStart`, or the live update's apply, upgrades the schema under a node of the older build that is running, which refuses the database only when it next connects |

Two smaller facts the work builds on: SQL Server Compact does have transactions
(`SQLCEConnection::BeginTransaction`), but no backend lets the persistence layer
run its statements on a transaction's connection, since every `Execute` takes its
own from the pool; and a MySQL database is transactional only when every one of its
tables is InnoDB (`MySQLConnection::LoadSupportsTransactions_`), which the create
script makes them and an old MyISAM database may not be.

What two nodes do today
-----------------------

Measured on 12 October 2026 in the lab: two Linux nodes of master 74aed9ab8 on one
database, each with its own data directory, 2,000 messages relayed to a sink that
records what it is given, and 1,000 into one mailbox, sent through both nodes
(`aa-lab.sh baseline`).

| | PostgreSQL 17 | MariaDB 11.8 | MySQL 8.4 |
|---|---|---|---|
| Relayed and never delivered | 106 of 2,000 | 122 | 29 |
| Relayed twice | 0 | 0 | 0 |
| Left in the mailbox, of 1,000 | 118 | 226 | 82 |
| UIDs given to two messages | 0 | 5 | 0 |
| Messages one of the two nodes served as a placeholder | all of them | all of them | all of them |

With one data directory shared by both, which [WarmStandby.md](WarmStandby.md)
forbids, nothing was lost but 1,489 of the 2,000 relayed messages went out twice.

What it would take
------------------

### Nodes, and the database's clock

Each node is a row of a new table, `hm_nodes`: its name, an id for this run of the
process, its build, the lease protocol it speaks, when it started, a heartbeat it
renews every ten seconds, and its state. A node whose heartbeat is older than its
lease, 60 seconds for example, is taken for dead. Every lease below is compared
with **the database's clock** (`CURRENT_TIMESTAMP` or the engine's equivalent in
the statement), never a node's own clock, so two nodes whose clocks disagree still
agree on who holds what.

A node's name is set per machine and defaults to its host name. A start takes over
the row of its own name, whatever its heartbeat says, because that row is its own
previous run: a node restarted by the service controller seconds after a crash
does not wait for itself. Two live processes under one name (a copied virtual
machine) are caught by the heartbeat itself, a conditional update that then
changes no row, and the second stops claiming work.

A node that cannot renew its heartbeat, because it has lost the database, stops
claiming work at once. It also stops starting deliveries once its leases have run
out. That is how it fences itself.

The database layer needs one thing it does not have: **the number of rows a
statement matched**, from all four backends. ADO gives it as `RecordsAffected`
(SQL Server and SQL Server Compact), MySQL as `mysql_affected_rows` with the
connection opened with `CLIENT_FOUND_ROWS`, and PostgreSQL as `PQcmdTuples`.
Without `CLIENT_FOUND_ROWS`, MySQL and MariaDB count only the rows a statement
*changed*: a renewal or a fenced write that sets a value the row already holds
counts 0, and would read as a lost lease. Every claim below is a conditional
`UPDATE` that counts as won only when it matched exactly one row. On 12 October
2026 the lab raced two connections over 300 rows on each of the four engines,
claiming each row with `... where id = @ID and owner = 0`: every row was won
exactly once.

**SQL Server Compact is one node by definition.** It is a file the server's own
process opens from its data directory, with no server for a second machine to
reach. Every step below keeps a working path on it as one node, which is always
the leader and holds every lease.

### The queue: leases with expiry, and fencing

`messagelocked` stays, meaning "a lease is held", so that what reads it keeps
working. Beside it come the node that holds the lease, the time it expires, an
epoch that goes up by one at every claim, and `messagenode`, the node whose disk
holds the message's file.

- **A claim** is
  `update hm_messages set messagelocked = 1, messagelocknode = @me, messagelockuntil = <now + lease>, messagelockepoch = messagelockepoch + 1 where messageid = @id and (messagelocked = 0 or messagelockuntil < <now>)`,
  and it is won only when it matched one row. The selection can stay as it is,
  narrowed to `messagenode = @me or messagenode = 0`. A row that another node
  claimed between the select and the claim is simply lost.
- **Renewal.** A delivery can take minutes, so the lease is renewed every minute
  while it runs. Five minutes is a lease that one slow remote server does not
  outlast.
- **Fencing.** Every write the deliverer makes to the message's row carries
  `and messagelocknode = @me and messagelockepoch = @epoch`: the retry count, the
  next try, the bounce, the hold, the delete. A write that changes no row means the
  lease has been lost. The deliverer then stops, sends nothing more, and logs it.
  The lease is also checked before each recipient's `DATA` and before the queue
  file is removed. And the deliverer's delete names the queue's states, so that it
  can never remove a row that has become a mailbox's message.
- **At start**, a node releases only the leases its own id holds, left from its
  previous run. It never releases another node's. `UnlockAll` in its present form
  is removed, and so are the same shapes for fetch accounts and migration jobs.
- **The queue's files** stay on the accepting node's local disk. The accept path's
  flush to local disk before the 250 is the durability promise, which a network
  write would weaken. So a row carries the node that accepted it, and **only that
  node claims it** - or any node, once the row says node 0.
- **A planned stop hands the queue over.** The drain closes the listeners, stops
  claiming, and lets what is in flight finish. Then it puts each of its queue files
  into the object store under a queue prefix and sets the file's row to node 0.
  Last, it releases its fetch and migration leases, gives up the leader's lease
  (below), and only then reports `drained`. A node that claims a node-0 row reads
  the file from the store into its own queue first. So maintenance needs no
  failover, and nothing is written to the network on the accept path. When a node
  dies, its queued messages wait for its disk, as they wait in a warm-standby
  failover today.

A lease cannot make delivery exactly-once. A node that is frozen for longer than
its lease (a VM paused, a stalled disk) may be half-way through a transmission
that it then finishes, while a second node has already taken the message over.
Fencing stops the frozen node from changing the row afterwards. It cannot stop
bytes that are already on the wire. The guarantee stays at least once, as it is
across a failover today. Duplicates happen only when a lease expires under a
delivery, and the lease's length is chosen to make that rare.

External fetch takes the same lease on `hm_fetchaccounts`, and the migration jobs
on `hm_migrationjobs`. So do the database queues that are worked from a table and
claim nothing today:
- the outbound webhooks (`hm_webhookqueue`);
- the inbound hooks (`hm_inboundhookqueue`), whose rows carry the accepting node as
  the queue's do, since the message they POST is a local file;
- the scheduled sends (`hm_scheduled`);
- the push notifications.

### Locks that hold across nodes

**UIDs and mod-sequences** do not need a lease. The folder's counter is increased
and the message inserted **in one transaction**:
`update hm_imapfolders set foldercurrentuid = foldercurrentuid + 1 where folderid = @id`,
then the counter is read and the row inserted, then the transaction commits.
PostgreSQL, MySQL (InnoDB) and SQL Server all hold the folder row's lock until the
commit. That keeps UIDs unique, and it also makes them commit in ascending order: a
second delivery into the same folder waits for the first to commit, which is what
`FolderManipulationLock` does inside one process today. The same holds for
`foldercurrentmodseq`, taken by deliveries, flag changes and expunges. On 12
October 2026 the lab had two writers take 800 numbers from one counter while a
reader read as an IMAP session does. Taken as the server takes them today, up to
396 numbers were given twice (2 on MySQL, whose slower commit narrows the race),
and up to 9 rows were committed below a number the reader had already passed. In one transaction: none of either, on all four engines.
A deadlock or a serialisation failure is retried a bounded number of times; the
folder row is always locked first.

**Longer holds** go in a lease table, `hm_locks`: a unique name, the node that
holds it, the time it expires and an epoch. A claim is the same conditional update
as the queue's. These use it:

- POP3's mailbox lock (`pop3:<account>`). Held for the session, and renewed while
  it runs;
- a mailbox move and a restore into a mailbox (`mailbox:<account>`);
- the backup's run slot, a resource's booking decision and the audit chain's
  append, which are locks inside the process today;
- the scheduler's leadership (below).

### Sessions on any node

- **SMTP** holds nothing between connections. Any node takes any connection.
- **IMAP.** A connection stays on one node for its life, but it must see what the
  other nodes do. **The database is the bus**:
  - Every delivery, flag change and expunge already moves the folder row's
    `foldercurrentuid` or `foldercurrentmodseq`. While other nodes are live, one
    watcher per node reads those two counters every two seconds, in batches, for
    the folders selected on that node. When either has moved, the folder's message
    list reads the rows whose mod-sequence is above the last one it knew, and the
    expunges recorded for QRESYNC above it. The node then tells its own sessions,
    as it does after a local change, so IDLE, NOOP and the untagged responses are
    unchanged. A notification from its own node still arrives at once.
  - Folder trees follow the change log (below).
  - `\Recent` may be seen by one session on each node.
  - There is no node-to-node channel: it would need its own authentication, a
    port and its discovery, to save two seconds.
- **POP3** takes the mailbox lock from `hm_locks` in place of `POP3Sessions`.
- **Browser sessions** (the webmail, the Deck, the portal) move into the database:
  a table of the token's digest, the account, and the times the session policies
  need. Then a request may land on any node, and ending a session ends it
  everywhere. The alternative, a balancer that keeps a browser on one node, still
  leaves ending a session to the node that holds it. It is not recommended.
- **The account lockout** counts in the database. A lockout that each node counts
  on its own is a lockout that a second node doubles. The rate limits stay per
  node, and say so: a limit is a defence against load, which each node bears for
  itself.

### Invalidating the caches

A change log, `hm_changes`, gets one row for each administrative write through the
persistence layer: the kind of object, its id, the node that wrote it, and a
sequence number. The kinds are accounts, domains, aliases, lists, domain aliases,
routes, security ranges, certificates, listeners, groups, ACLs, an account's
folder tree, setting values (server, domain and account), compromised-account
holds, and sessions to end. Each node reads the rows after the last one it saw,
every two seconds, skips its own, and acts on each kind:
- it drops the object from `CacheContainer`, `IMAPFolderContainer`, `InboxIDCache`,
  `AccountSizeCache` and the setting values of its owner;
- it applies a hold;
- it puts a security range into its own firewall when auto-ban reaches the
  firewall;
- it ends a revoked device's connections.

The TTLs stay as they are, as a backstop. Messages do not go through the log:
their changes move the folder counters already. PostgreSQL's `LISTEN`/`NOTIFY`
would be quicker, but the server runs on four backends, and a design that works
on one only would leave the other three with a different server. The log is pruned
after an hour.

### Settings

Every write to `hm_settings` and `hm_inisettings` writes a settings row to the
change log. Each node reloads the store when it reads one, and applies each
setting as that setting says: at once, or at the next reinitialise or restart. A
setting that waits for a restart is named in the node's log and on the Deck until
the node restarts. The sections of `hMailServer.ini` that are each machine's own
stay each machine's own: `[Directories]`, `[Database]`, `[Security]` and
`[SettingsOverride]`.

### Which node runs a scheduled task

The tasks fall into three kinds:

| Kind | Tasks | How they run |
|---|---|---|
| Each node's own | the object store's committer, the free disk space check, the work-queue health check, the log retention, the update check, the closing of idle mailbox keys, the metrics history (whose rows then name their node), the evaluation of the alerts' conditions (this node's disk and queues), the reputation watch, the consistency check (once it counts only its own rows), and the TLS-RPT and DMARC reports (each node's own sessions, with the node's name in the report's id) | On every node, as now |
| Once for the whole set of nodes | the quarantine and list digests, the scheduled sends, the sending of the alerts, the backup schedule, ACME's orders, the directory sync, the DKIM key rotation, the mailbox, archive and expunge retention passes, the greylist, bounce and complaint maintenance, the removal of expired records, the change log's pruning; and, outside the scheduler, the message indexer, the sandbox's poller and the migration manager | On one node, the leader: the one that holds the `leader` lease in `hm_locks`, renewed every 20 seconds. When the leader dies, another takes the lease once it has expired, within two minutes |
| Any node, row by row | the delivery queue, external fetch, the migration jobs, the outbound and inbound webhooks, the push notifications | Each row claimed with a lease |

A leader that loses its lease part-way through a pass must not leave a half-done
pass to be done twice. So each task that sends mail records what it has sent with
a compare-and-set on the state it already keeps, before it sends:
- the digest's state row, `hm_digeststate`;
- the scheduled send's row, `hm_scheduled`;
- the alert's `alerteventnotified`.

The next leader then skips what is already done. The tasks already written for a
second node, such as the DKIM rotation and the complaint counters, keep their own
guards.

ACME's HTTP-01 challenge is answered by whichever node the certificate
authority's request reaches, so the challenge's token goes in the database. The
certificate the leader obtains goes there too, sealed, and each node deploys it.

### Secrets

Every node imports the installation's stored-secrets key, as a standby does
today ([WarmStandby.md](WarmStandby.md), setup step 5). Every secret
`Crypt::ProtectSecret` seals is then sealed under that key instead of the
machine's, and a command reseals, on a node that can open them, the values still
sealed with DPAPI or the Linux key file. Without that move, a secret entered on
one node fails on every other node. A node that does not hold the recorded key
may not join.

### In front of the nodes

A TCP load balancer passes the client's address with the PROXY protocol,
versions 1 or 2 (`ProxyProtocol.h`). Today only the SMTP listener reads it, so
IMAP and POP3 are taught to as well. The balancer's health check is `/readyz`
([HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)), which a drained or
standby node answers with 503.

### The message store

**Active-active requires the object store** ([ObjectStorage.md](ObjectStorage.md)).
Every node reads the local disk first and the store second, and the committer puts
a message into the store up to a minute after its row is saved. So while other
nodes are live:
- a delivered or appended message is put into the store *before* its row is saved,
  with a bounded wait;
- if the store does not take it in time, the file stays on the local disk, the row
  carries `messagenode`, and the other nodes answer IMAP `NO [UNAVAILABLE]` or
  POP3 `-ERR [SYS/TEMP]` for it until the committer has put it;
- no node writes a placeholder over a message it cannot find.

This write is on the delivery path, not the accept path, so the 250 promise is
untouched.

**A shared file system is not supported**, for [WarmStandby.md](WarmStandby.md)'s
reasons and two more of the code's:
- the sweep of abandoned APPENDs at start would delete another node's APPEND in
  flight;
- the consistency check would report the other nodes' files.

Nor is file replication, which gives no read-after-write across nodes.

**SQL Server Compact is excluded**, as above.

### Upgrades

A set of nodes is upgraded stopped: the nodes are drained and stopped, the
database is upgraded once, and they start on the new build. There is no rolling
upgrade. While a live node of another build is registered, a node refuses
`UpgradeOnStart` and the live update refuses to apply.

Schema changes it would take
----------------------------

None is made or reserved. Each would be a numbered step on all four backends,
with the create scripts to match, and SQL Server Compact would get the tables and
use them as one node. No column is dropped, so a database can still be taken back
by restoring it alone.

| Table | Change |
|---|---|
| `hm_nodes` | New: id, name, run id, build, lease protocol, started, heartbeat, state |
| `hm_settings` | Rows for the switch (`ActiveActiveEnabled`, 0) and the lease and heartbeat lengths |
| `hm_messages` | Beside `messagelocked`: the node that holds the lease, its expiry and its epoch, and `messagenode`, the node whose disk holds the file (0 when it is in the store) |
| `hm_fetchaccounts`, `hm_migrationjobs` | The same lease beside `falocked` and `mjstate` |
| `hm_webhookqueue`, `hm_inboundhookqueue`, `hm_scheduled`, the push queue | The same lease; the inbound hooks also the accepting node |
| `hm_locks` | New: a unique name, the node that holds it, its expiry, its epoch |
| `hm_changes` | New: a sequence, the kind of object, its id, the node, the time |
| `hm_metricsamples` | The node that sampled |
| Browser sessions | New: the token's digest, the account, created, last used, and the address bound to it |
| The account lockout | New: failures per name, in a window |
| ACME | New: the challenge tokens, and the certificate |

The first version planned a table recording each unit of scheduled work done, and
a generation number for each account's folder tree. Neither is needed: the
compare-and-set on the state each task already keeps does the first, and the
change log does the second.

Failure modes
-------------

| What fails | What happens | What limits it |
|---|---|---|
| A node dies part-way through a delivery | Its lease expires and another node takes the message, if it holds the file, which a node-0 row in the store is. Otherwise the message waits for the dead node's disk. The recipient may get it twice | The lease's length. This is the same cost as a warm-standby failover today |
| A node freezes for longer than its lease | Another node takes its messages. When the frozen node wakes, its row updates change nothing and it stops, but a transmission it was part-way through can finish | At least once, not exactly once. Checking the lease before `DATA` narrows it |
| A node restarts seconds after a crash | It takes over its own row and releases its own leases | Its name |
| Two machines run under one name | The second one's heartbeat changes no row; it stops claiming and says so | One heartbeat |
| Two nodes' clocks disagree | Nothing. Every lease is compared with the database's clock | The database's clock |
| A node loses the database | It cannot renew, so it stops claiming. Its leases expire and the others take the work. Its IMAP and POP3 sessions fail as they do today | The heartbeat and the lease |
| A node loses its clients but not the database | The balancer's health check takes it out of service. It goes on delivering its own queue | `/readyz` |
| The database fails over | The leases are rows, so they survive the failover. A transaction in flight rolls back and is tried again | The backend's own availability |
| The leader dies | The tasks that run once pause until the `leader` lease expires, then another node runs them. What was already recorded as sent is not sent again | Two minutes |
| Deliveries into one mailbox from several nodes | The folder row's lock puts them in order, as the lock inside one process does now | The time of one insert |
| Cache staleness | A change reaches the other nodes within the change log's interval. An account disabled on one node can sign in on another for that long | Two seconds, where it is 60 seconds today |
| A node of another build or lease protocol | It refuses the database at start, or refuses to join when `hm_nodes` shows a different protocol; nobody upgrades the schema under it | The schema pin, the protocol column and the guard |
| The object store fails | Reads of messages that are only in the store fail on every node. New deliveries go on, onto each node's local disk, and the other nodes answer `[UNAVAILABLE]` for those until the store is back | [ObjectStorage.md](ObjectStorage.md) |

How it would be tested
----------------------

The bench exists: [build/lab/active-active/](../../build/lab/active-active/README.md),
in containers on a Linux host. It has:
- two or more Linux nodes;
- PostgreSQL 17, MariaDB 11.8, MySQL 8.4 and SQL Server 2022;
- MinIO as the object store;
- HAProxy with the PROXY protocol;
- a sink SMTP server that records every message it is given;
- a load that numbers what it sends.

The Linux server has no SQL Server backend, so SQL Server takes part through the
engine checks, and through testers that race two connection pools in one process,
run by the Windows gate. Faults are made on purpose:
- a node killed (`docker kill`);
- a node frozen past its lease (`docker pause`, then `unpause`);
- a node cut off from the database;
- a slow sink, and a sink that answers 451.

What it must show:

1. **No lost mail, and duplicates only where a lease expired.** Mail is sent
   through every node to many recipients while nodes are killed and frozen at
   random. The sink holds every message, and it holds each one once, except for
   messages whose lease expired under a delivery. The log names each of those.
2. **UIDs.** Mail is delivered into one mailbox from every node at once, and
   appended on every node. An IMAP client on each node sees UIDs that are unique
   and ascending, a `UIDNEXT` that agrees with them, and every message as it was
   sent.
3. **IMAP across nodes.** A client in IDLE on one node is told of a delivery, a
   flag change and an expunge made on another within the watcher's interval.
4. **POP3.** A second POP3 session on another node to the same mailbox is
   refused while the first is open.
5. **Fetch.** Each external account is fetched once in each interval, with
   delete-after-download on, while nodes are killed.
6. **Scheduled tasks.** The digests, reports, alerts, webhooks and scheduled sends
   go out once in each period, with the leader killed part-way through a pass.
7. **Caches, settings and sessions.** A change made on one node is seen on each of
   the others within the stated bound: an account disabled, a password changed, a
   setting that applies at once, a browser session ended.
8. **Fencing.** A node frozen past its lease changes no row when it wakes, and
   says so in its log.
9. **Drain.** A drained node hands its queue over, deferred messages included, and
   stops with an empty queue, while the other nodes deliver it.
10. **The message store.** A message delivered on one node is read on another at
    once. With the store stopped, no node writes a placeholder.
11. **Regressions.** The Linux regression suite passes against one node of a set
    of two, on PostgreSQL, MySQL and MariaDB - the database matrix - and a single
    node passes the Windows and Linux gates unchanged at every step.

The warm-standby rehearsal of 9 September 2026 ([WarmStandby.md](WarmStandby.md),
"The rehearsal") showed the problem this design solves: a second service's start
set a message's lock back to 0 while the first was delivering it. The lab's
baseline, above, shows how much of it there is.

The order of the work
---------------------

Twenty-five units, each small enough for one person or agent in a few days. The
first four make one node sturdier and change nothing else, so they come first.
Most of the rest change nothing on one node either: one node is its own leader,
wins every claim, skips its own change-log rows and has no folder to watch.

1. **One node, sturdier** (no schema step):
   - AA-01, the database layer's row counts on all four backends;
   - AA-02, a transaction scope the persistence layer can use;
   - AA-03, conditional claims on today's lock columns, and a queue delete that
     cannot remove a delivered row;
   - AA-04, UIDs and mod-sequences in one transaction. This also protects one node
     from writers in other processes: the command-line tools and COM scripts.
2. **The guard:** AA-05, nodes register in `hm_nodes`. A node that finds another
   live node refuses to start its delivery queue, external fetch and the
   leader's work, and says so. This turns the rule of
   [WarmStandby.md](WarmStandby.md) from a sentence into a refusal. It was offered
   for 6.3.6 as the owner's option, and was left out by the maintainers' decision
   of 28 September 2026 (design only). It also stops a newer node from upgrading
   the schema under an older one.
3. **Leases:**
   - AA-06, `hm_locks` and the row lease on the database's clock;
   - AA-07, the queue as leases, with node affinity;
   - AA-08, the deliverer fenced;
   - AA-09, fetch accounts and migration jobs.
4. **One leader:**
   - AA-10, the leader and the scheduler's kinds of task;
   - AA-11, work done once (sends, digests, alerts, webhooks, push);
   - AA-12, records that name their node;
   - AA-13, the named locks (POP3, moves, restores, backup, booking, the audit chain).
5. **What one node changes, the others see:**
   - AA-14, the change log;
   - AA-15, caches, folder trees, holds and bans;
   - AA-16, settings;
   - AA-17, browser sessions and the lockout in the database;
   - AA-18, IMAP's watch on the other nodes.
6. **Secrets and the edge:**
   - AA-19, every stored secret under the installation's key;
   - AA-23, the PROXY protocol for IMAP and POP3.
7. **The message store and the drain:**
   - AA-20, write-through and no placeholders while other nodes are live;
   - AA-21, the drain that hands the queue over;
   - AA-22, ACME across nodes.
8. **Proof and switch-on:**
   - AA-24, every scenario above, on PostgreSQL, MySQL and MariaDB;
   - AA-25, the switch (`ActiveActiveEnabled`) and its checks: the object store,
     the stored-secrets key, InnoDB only, not SQL Server Compact. Then the node
     list on the Deck, and the warm-standby and high-availability documents
     rewritten for a set of nodes.

What the warm-standby guidance keeps saying
-------------------------------------------

Until every step above has shipped and passed that bench, [WarmStandby.md](WarmStandby.md)
and [HighAvailabilityRunbook.md](HighAvailabilityRunbook.md) keep saying these,
and nothing here softens them:

- **One service per database.** A standby's service is set to Manual and left
  stopped.
- **Two active services on one database lose mail and deliver twice.** The second
  one's start releases every message the first is delivering. The two select the
  queue without coordinating, and the lock is taken after the selection and
  without a condition. A node without a message's file deletes its row. External
  fetch and the migration jobs do the same, and IMAP UIDs can collide.
- **A failover starts the standby only once the primary is confirmed stopped**:
  powered off, its service stopped, or cut off from the network.
- **The message store is replicated, not shared** over SMB or a Cluster Shared
  Volume. The object store is the one store that nodes can share.
- **Secrets sealed to the machine do not travel**, and the stored-secrets key is
  imported by hand.
