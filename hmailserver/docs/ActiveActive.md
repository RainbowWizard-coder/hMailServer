Active-active nodes: a design
=============================

**Not built.** This is a design, written on 29 September 2026 for 6.3.6, and
active-active itself does not exist in the server: there is no node registry, no
lease, no schema step and no setting for it. Of the first phase of the work below,
which makes one node sturdier on its own, the database layer's row counts and its
UTC clock (AA-01, below) and the transaction scope the persistence layer can use
(AA-02, `DatabaseTransaction`) are in the tree, and nothing in the server uses them
yet. Until it is built, [WarmStandby.md](WarmStandby.md) holds without
exception: **exactly one hMailServer service may run against a database at a
time.** Two active services on one database lose mail, deliver the same mail
twice, fetch the same remote mailbox twice, and can give two messages in one
mailbox the same IMAP UID. Nothing warns when that happens.

It was read again against the code on 12 October 2026 (master 74aed9ab8, schema
6108): what had moved since 29 September is corrected below, the questions the
first version left open are decided, and the work is set out as units in the order
they are to be built. What two nodes do to each other today was measured the same
day, in the lab that the units will be proved in
([build/lab/active-active/](../../build/lab/active-active/README.md)). A review of
that reading, the same evening, found more that the server keeps to one process or
one disk, and this version includes it.

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
| The queue's selection (`SMTPDeliveryManager::LoadPendingMessageList_`, `GetNextMessage_`) | Reads every due, unlocked message into a list, then locks each one as it hands it out. A message being saved is inserted locked until its recipients are written | Both nodes read the same list |
| The queue's lock (`PersistentMessage::LockObject`) | `update hm_messages set messagelocked = 1 where messageid = @MESSAGEID`, with no condition on the lock, and it does not ask how many rows it changed. The database layer can say since unit AA-01 (`DatabaseConnectionManager::ExecuteCount`, below), and nothing asks it yet | Both nodes "win" the same message |
| The database layer's retry (`DALConnection::Execute`) | A statement that meets a connection problem is run again after a reconnect, which since AA-01 it says it did (`ExecuteCount`'s `reconnected`) - but not inside a `DatabaseTransaction`, which runs each of its statements once and is itself run again whole (AA-02), except on a MySQL database that is not transactional, where a scope runs its statements as the pool does | A claim whose commit reached the database but whose reply was lost runs twice, and the second run finds the row already taken. Inside a transaction begun on a connection the pool hands out, the second run is on a new connection, outside it |
| The database session (`ADOConnection::Connect`) | Every SQL Server session runs at READ UNCOMMITTED, "since we've implemented our own locking mechanism": locks inside the process | A reader on one node sees rows another node's transaction has not committed, and may roll back |
| The database's clock (`SQLStatement::GetCurrentTimestamp`) | `GETDATE()`, `NOW()` in the system's time zone, and `current_timestamp` stored into columns without a zone: local wall time. The database's clock in UTC, `GetCurrentUtcTimestamp`, is there since AA-01, and nothing uses it yet | A lease measured on it jumps an hour at a daylight-saving change, on SQL Server and MySQL at both changes and on PostgreSQL in the repeated autumn hour |
| The queue's files | A queued message's file is on the local disk of the node that accepted it. The object store never holds the queue ([ObjectStorage.md](ObjectStorage.md)) | A node that claims a row whose file is on the other node's disk reports HM5006 and **deletes the row** (`SMTPDeliverer::DeliverMessage`). For one local recipient the queue row has already become the mailbox's row (`LocalDelivery::DeliverToLocalAccount_`), so what it deletes is mail already delivered |
| A missing message file (`PersistentMessage::EnsureFileExistance`) | With the file system as the store, a message whose file is missing is given a placeholder ("message undeliverable"), written under its name, and the row takes the placeholder's size | A node asked for a message whose file is on the other node's disk replaces it, on its own disk, with a placeholder, and serves that from then on |
| The other state in the data directory | Sieve scripts, includes and the vacation and duplicate records (`SieveStorage`), the keys the DKIM rotation makes (`<Data>/DKIM`; the domain row stores a path), the quarantine, recoverable items under legal hold, protected and confidential messages, shared files, list archives, eDiscovery exports, and ACME's certificates and account key are files under the data directory | Each exists on the node that wrote it. A Sieve script uploaded on one node does not filter mail delivered on another, mail relayed by another node is not signed with the rotated key, and a quarantine release that reaches another node fails |
| External fetch (`ExternalFetchManager::DoWork`, `PersistentFetchAccount`) | `UnlockAll` at start (`update hm_fetchaccounts set falocked = 0`), and an unconditional `Lock`, held by `FetchAccountLock` | The same as the queue: a remote POP3 mailbox is fetched by both nodes, and one with delete-after-download ends up split between them |
| Migration jobs (`MigrationManager::Start`, `MigrationStore`) | `RequeueRunningJobs` puts every running job back in the queue at start, and `MarkJobRunning` is unconditional | A node that starts takes back the other node's running jobs, and both import the same mailbox |
| A message's UID and mod-sequence (`PersistentMessage::AddObject`, `PersistentIMAPFolder::GetUniqueMessageID`, `GetNextModSeq`) | Under `FolderManipulationLock`, a lock inside the process, the folder's counter is increased by one statement and read back by a second, and the message is inserted by a third. The mod-sequence of a delivery, a flag change and an expunge is taken the same way, and a flag change's and an expunge's outside that lock | Two nodes can read back the same UID. A message can also be committed with a lower UID than one a client has already seen, and a client never fetches such a message (RFC 3501 requires UIDs to be strictly ascending). Even on one node, two flag changes in one folder at once can be given one mod-sequence |
| Flag changes (`FolderManager::ChangeFlagsOf`, `PersistentMessage::SaveFlags`) and a whole-row save (`AddObject`'s update) | A STORE changes the cached message under a lock of this process and writes the resulting flags whole. CONDSTORE's `UNCHANGEDSINCE` is tested against the cached mod-sequence. COM's and the scripts' `Save` writes the whole row from memory, flags and mod-sequence included, and moves no folder counter | A STORE on one node writes back flags as it last read them, undoing what the other node set, and a conditional STORE passes on a stale mod-sequence. A script's `Save` does the same to an IMAP client's flags, on one node too |
| The folder trees (`IMAPFolderContainer`) | Each account's folders are cached in the process, with each folder's current UID and mod-sequence, with no expiry: only the removal of an account's messages (`PersistentAccount::DeleteMessages`), a mailbox move and a shortage of memory uncache them. `SELECT` and `STATUS` answer `UIDNEXT` and `HIGHESTMODSEQ` from those counters | A folder made, renamed or removed on one node is not seen on the other until one of those happens there, or it restarts. A client polling `STATUS` on one node never sees mail delivered by the other, and a CONDSTORE client is told nothing changed |
| The message lists (`MessagesContainer`, `Messages::Refresh`) | Each folder's messages are cached for 30 minutes and read again only when this process marks the folder, and then only the rows above the highest UID already read. The same lists serve IMAP, the webmail's listing and POP3 | A flag change or an expunge made on the other node is never read while the folder is cached, a message committed below a UID already read is never seen, and a POP3 session lists messages the other node has deleted |
| Change notification (`NotificationServer`) | A delivery, an APPEND, an EXPUNGE or a flag change tells every IMAP session of this process that has the folder selected. IDLE waits on it. A mailbox move asks it whether anybody has the account open (`HasMessageSubscribers`) | A session in IDLE on one node is not told about a delivery made on the other, and a move does not see the other node's sessions |
| POP3's mailbox lock (`POP3Sessions`) | A set in memory: one POP3 session per mailbox | Two POP3 sessions on two nodes can have the same mailbox open, and both can delete from it |
| The caches (`CacheContainer`, `MessageCache`, `InboxIDCache`, `AccountSizeCache`) | Accounts, domains, aliases and lists are kept for their TTL, 60 seconds by default. An owner's own setting values are read again after a minute "so that a row another node wrote is seen" (`CacheContainer.h`), and the object store's answers after half a minute. An account's size, which quota is enforced from, is kept with no expiry and moved only by this process | A change made on one node is seen on the other within the TTL, and not before. A disabled account can still sign in on the other node for up to a minute. Each node enforces quota on its own view of the mailbox |
| The settings (`IniSettingStore`, `Configuration::Load`, `Application::Reinitialize`) | Read from the database at start. A change is applied on the node that made it, at once or at its next start or reinitialise, as each setting says. The message store's backend is read again every five seconds ([ObjectStorage.md](ObjectStorage.md)) | A setting changed on one node is not applied on the other until that node saves a setting, reinitialises or restarts |
| The scheduler (`Scheduler`, and the tasks `Application::CreateScheduledTasks_` registers) | Every task runs in the process that registered it: the digests, the scheduled sends, the alerts, the webhooks, the TLS-RPT and DMARC reports, the retention passes, the backup schedule, ACME, the directory sync, the object store's committer, the consistency check and the rest | Each node runs every task. Some are already safe with two nodes, and say so in their code: the DKIM key rotation's state, the complaint and bounce counters, the report importer's de-duplication, the eDiscovery holds, the sandbox's verdict cache, the directory sync's inserts. The others send twice, or work against a local disk that is not the other node's |
| The singletons outside the scheduler | The message indexer (the only writer of `hm_messageindexstate`'s row 0), the sandbox's poller (which submits held messages' files from its own disk), the migration manager's thread, `ResourceBooking`'s lock ("this process's"), the backup's run slot (`BackupManager`), the audit chain (its head read, then a row appended), `DeliveryQueue::Clear` | Two indexers write one row; two pollers and two migration threads work the same rows; two nodes decide for the same resource; two appends fork the audit chain; a clear deletes the other node's queued rows and leaves their files |
| The out-of-office memory (`hm_autoreplies`) | Looked up, then stamped after the reply | Two deliveries from one sender on two nodes can both reply, as two at the same moment on one node already can |
| The reports (`TlsRptReporterTask`, `DmarcRptReporterTask`) | What each report says is collected in memory, per node. A DMARC `report_id` is the day, the domain, the second and the submitter | Each node reports what it saw. Two nodes reporting one domain in the same second give two reports one id, and a receiver discards the second |
| Browser sessions ([DevicesAndSessions.md](DevicesAndSessions.md)) | Kept in the server's memory (`RestApiServer`) | A request that reaches the other node is not signed in. Ending a session ends it on its own node only |
| The account lockout (`AccountLockout`), the second-factor guard (`SecondFactorGuard`), the sign-in states (`SignInState`, `PasskeyStore`), POP3's login delay, the rate limits and the per-destination limits | Counted in memory, "like the sibling stores": the lockout's failures, the step of the last TOTP code taken and the budget of wrong codes, the key the OIDC, SAML and passkey sign-ins are sealed with and the states already spent. The compromised-account holds are in `hm_accountreview`, but loaded only at start (`CompromiseDetector::LoadLocks`) | Each node counts on its own, so two nodes allow twice the attempts. A TOTP code taken on one node is taken again on the other. A sign-in whose callback reaches the other node fails. A hold made on one node is not in force on the other until it restarts |
| Secrets sealed to the machine ([WarmStandby.md](WarmStandby.md), "The DPAPI constraint") | `Crypt::ProtectSecret` seals with DPAPI on Windows and the `LINUX1` key file on Linux: route, per-domain relay and fetch-account passwords, fetch accounts' OAuth tokens, TLS key passphrases, every setting saved encrypted, the inbound and outbound webhooks' secrets, migration jobs' source secrets, the SNMP secrets and the sandbox's token. Only second factors and the `[Settings]` secrets are under the installation's stored-secrets key | A secret entered on one node cannot be read on the other |
| The listeners | Rows of `hm_tcpipports`, which every node reads. The SMTP, IMAP and POP3 listeners each read a PROXY protocol header from the peers on a list of their own, off by default (`TCPServer::HandleAccept`); IMAP and POP3 since unit AA-23, before which only SMTP did | Each node binds every port, as the rehearsal in [WarmStandby.md](WarmStandby.md) saw, which is what a balancer in front wants. Behind a balancer that sends the header to a listener whose switch is on and whose list names it, auto-ban, the IP ranges and the device list act on the client; otherwise on the balancer. The country policy and `OnClientConnect` are asked at the accept, before the header, and judge the balancer |
| The drain (`Application::EnterDrain`) | Closes the SMTP, IMAP, POP3 and ManageSieve listeners and **pauses** the delivery queue while what is in flight finishes. The web server stays open | A drained node keeps its queued messages, deferred and held ones included, on its own disk, and its webmail goes on adding to them: it cannot be stopped for maintenance without a failover |
| A schema upgrade (`SchemaUpgrader`, `Application::OnDatabaseConnected`) | Two walkers on one database are serialised on the version row, or on a named lock on MySQL. A node refuses a database newer than its build when it connects | Two walkers are safe. But a node of a newer build with `[Database] UpgradeOnStart`, `hmailserver --upgrade-database` (which the container image runs at every start), the Windows installer's DBUpdater and the live update's apply all upgrade the schema under a node of the older build that is running, which refuses the database only when it next connects |
| A node's name | There is none. The nearest is the host name, which a container recreated from the shipped Compose file changes every time | Anything keyed to it would lose its queue at a recreate, and at a warm-standby failover onto another machine |

Two smaller facts the work builds on: SQL Server Compact does have transactions
(`SQLCEConnection::BeginTransaction`), and the persistence layer's statements can
run on a transaction's connection inside a `DatabaseTransaction`
(`Common/SQL/DatabaseTransaction.h`, AA-02), where outside one every `Execute` takes
its own connection from the pool; and a MySQL database is transactional only when
every one of its tables is InnoDB (`MySQLConnection::LoadSupportsTransactions_`),
which the create script makes them and an old MyISAM database may not be.

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

### Nodes, their identity, and the database's clock

Each node is a row of a new table, `hm_nodes`: its identity, its name, an id for
this run of the process, its build, the lease protocol it speaks, when it started,
a heartbeat it renews every ten seconds, and its state. A node whose heartbeat is
older than its lease, 60 seconds for example, is taken for dead. Every lease below
is compared with **the database's clock in UTC** (`SYSUTCDATETIME()`,
`UTC_TIMESTAMP(6)`, `statement_timestamp() at time zone 'utc'`), never a node's own
clock and never the database's local time, so two nodes whose clocks disagree still
agree on who holds what, and a daylight-saving change moves no lease. It is the
statement's time on all three, not the transaction's: PostgreSQL's `now()` is the
start of the transaction, and a lease taken inside one would be shorter than it says.
`SQLStatement::GetCurrentUtcTimestamp` writes it (AA-01). SQL Server Compact has no
UTC function at all (its provider refuses `GETUTCDATE()`, `SYSUTCDATETIME()`,
`SYSDATETIME()` and `CURRENT_TIMESTAMP`), and is one node, so there it is the node's
own UTC clock, written into the statement.

A node's **identity** is a random number kept in a file in its data directory,
`node.id`, written at its first start, so that it follows the disk. A container
recreated on the same volume, a machine renamed, a warm standby started on the
replicated data directory and a disk recovered onto another machine are all the
same node, with the same queue. Its name, the host name by default, is only for
people.

A start takes its row at once if the row says stopped or its heartbeat has
expired. If the row is fresh, the node watches it for fifteen seconds and takes it
if it did not move: that is a restart after a crash, which then waits fifteen
seconds rather than a whole lease. A row that moves belongs to another live process
on a copy of the same disk (a copied virtual machine, or a standby started by
mistake on the replica); this process stays closed and says so, and the running
node is undisturbed. A running node whose heartbeat stops matching reads its row:
if another live process holds it, the identity is contested, and this node closes
its listeners and stops claiming work; if the row was simply rolled back (a database
restored from a backup), it registers again. `hmailserver --new-node-id` gives a
copied disk an identity of its own.

A node that cannot renew its heartbeat, because it has lost the database, stops
claiming work at once. It also stops starting deliveries once its leases have run
out. That is how it fences itself.

**While active-active is switched off, exactly one node is active.** That node
holds an `active` lease, claimed with a conditional update so that two nodes
started at the same moment cannot both find the other absent. Any other node is a
standby: its SMTP, IMAP, POP3 and ManageSieve listeners stay closed, the webmail
answers 503, the REST API and COM refuse whatever would put a message, a file or a
queued row on its disk (a send, a mailbox write, an import, a release), it runs no
scheduled task and delivers nothing, and `/readyz` answers 503 `standby`; the rest
of the administration and the health probes stay open. This is the
guard, the first step of the work: it turns [WarmStandby.md](WarmStandby.md)'s
rule into a refusal. It is not a failover. A standby left running takes the lease
once it has expired, with its data directory as replication left it, and the
warm-standby runbook still applies.

The database layer has, since AA-01, the one thing it lacked: **the number of rows
a statement matched**, from all four backends (`DatabaseConnectionManager::ExecuteCount`).
ADO gives it as `RecordsAffected` (SQL Server and SQL Server Compact; under SQL
Server's `NOCOUNT` there is no count, so the server sets `NOCOUNT` off in every
session it opens, which only a server whose `user options` turn it on for every
session notices), MySQL as `mysql_affected_rows` with the connection opened with
`CLIENT_FOUND_ROWS`, and PostgreSQL as `PQcmdTuples`. Without `CLIENT_FOUND_ROWS`,
MySQL and MariaDB count only the rows a statement *changed*: a renewal or a fenced
write that sets a value the row already holds counts 0, and would read as a lost
lease. `ExecuteCount` also says whether the connection was lost while the statement
ran, after which the layer runs it again. On MySQL and MariaDB that is also so,
since AA-01, for a session the database ended under TLS - a restart, a failover, a
`KILL` - which the client reports as TLS's error 2026 rather than 2013, and for one
MySQL 8.0.24 and later closed for being idle (`wait_timeout`, error 4031; MariaDB's
4031 is another error, and is not taken for one): until then the statement after
either failed, once on each connection of the pool, where it is now run again on a
new connection. A caller may forgive one failure, a row the database's own rules
refused (`DALErrorInSQL`), which then counts 0: a duplicate key on all four engines,
and on SQL Server also a `NULL` in a `NOT NULL` column and a foreign key's or a
`CHECK`'s refusal, on SQL Server Compact also a foreign key's. Every other failure
counts -1, and a lost connection is never forgiven. Every claim below is a conditional
`UPDATE` that counts as won only when it matched exactly one row. On 12 October
2026 the lab raced two connections over 300 rows on each of the four engines,
claiming each row with `... where id = @ID and owner = 0`: every row was won
exactly once. A claim also recognises itself: it sets an epoch drawn at random for
that one attempt, and matches its own finished claim as well as a free row, so a
claim that the database layer runs again after a reconnect, whose first run had
committed, still counts as won, while a second attempt that read the same row does
not.

**SQL Server Compact is one node by definition.** It is a file the server's own
process opens from its data directory, with no server for a second machine to
reach. Every step below keeps a working path on it as one node, which is always
the leader and holds every lease.

### The queue: leases with expiry, and fencing

`messagelocked` stays, meaning "a lease is held", so that what reads it keeps
working. Beside it come the node that holds the lease, the time it expires, an
epoch drawn afresh for every claim, and `messagenode`, the node whose disk holds the
message's file.

- **A claim** is
  `update hm_messages set messagelocked = 1, messagelocknode = @me, messagelockuntil = <now + lease>, messagelockepoch = @epoch where messageid = @id and messagetype = 1 and (messagelocked = 0 or messagelockuntil < <now> or messagelockepoch = @epoch)`,
  and it is won only when it matched one row. The selection takes the rows of this
  node and of node 0 whose lease is free or has expired. A row that another node
  claimed between the select and the claim is simply lost, and so is one that has
  left the queue meanwhile: delivered into a mailbox, or held for review.
- **A row being saved** is inserted locked by its node with a lease that never
  expires, until its recipients are written, so no selection takes a message whose
  recipients are not yet saved.
- **Renewal.** A delivery can take minutes, so the lease is renewed every minute
  while it runs, for a node's own rows too. Five minutes is a lease that one slow
  remote server does not outlast.
- **Fencing.** Every write the deliverer makes to the message's row carries
  `and messagelocknode = @me and messagelockepoch = @epoch`: the retry count, the
  next try, the bounce, the hold, the delete, and the reuse of the row as the
  mailbox's message. A write that changes no row means the lease has been lost.
  The deliverer then stops, sends nothing more, and logs it. The lease is also
  checked before each recipient's `DATA` and before the queue file is removed. And
  the deliverer's delete names the queue's states, so that it can never remove a
  row that has become a mailbox's message.
- **At start**, a node releases only the leases its own id holds, left from its
  previous run. It never releases another node's. `UnlockAll` in its present form
  is removed, and so are the same shapes for fetch accounts and migration jobs.
- **The queue's files** stay on the accepting node's local disk. The accept path's
  flush to local disk before the 250 is the durability promise, which a network
  write would weaken. So a row carries the node that accepted it, and **only that
  node claims it** - or any node, once the row says node 0.
- **A planned stop hands the queue over.** The drain closes the listeners, stops
  claiming, and lets what is in flight finish. Then it puts each of its queue files
  into the object store under a queue prefix and sets the file's row to node 0:
  every message not yet delivered, the deferred ones and those held for ETRN, for
  review and for the sandbox, and the inbound hooks' messages with them. From then
  until it stops, anything it still enqueues (a webmail send, its own reports, a
  bounce) is written as node 0, through the store. Last, it releases its fetch and
  migration leases, gives up the leader's lease (below), and only then reports
  `drained`. A node that claims a node-0 row reads the file from the store into its
  own queue first. So maintenance needs no failover, and nothing is written to the
  network on the accept path.
- **When a node dies**, its queued messages wait for its disk. A standby started
  on its replicated data directory is the same node and delivers them, as a
  warm-standby failover does today. Any node that holds the file of a dead node's
  queued message (a disk restored onto it) adopts that row; the others stay listed
  as waiting for their node's disk, and none is deleted for want of its file while
  other nodes are live. An administrator who knows that a disk is gone for good
  removes its messages from the queue.

A lease cannot make delivery exactly-once. A node that is frozen for longer than
its lease (a VM paused, a stalled disk) may be half-way through a transmission
that it then finishes, while a second node has already taken a node-0 message
over. Fencing stops the frozen node from changing the row afterwards. It cannot
stop bytes that are already on the wire. A node killed after the remote server's
250 and before the row's write delivers that message again when it restarts. The
guarantee stays at least once, as it is across a failover today, and the
duplicates are bounded by the deliveries in flight when it happens.

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
and up to 9 rows were committed below a number the reader had already passed. In
one transaction: none of either, on all four engines. A deadlock or a
serialisation failure is retried a bounded number of times, the whole unit of work
and never one statement on a new connection; the folder row is always locked
first.

**A flag change** takes the same transaction: the folder row's lock first, then the
message's row read again, the change applied to what was read, CONDSTORE's
`UNCHANGEDSINCE` tested against the row's mod-sequence, and the row written. A
whole-row save from COM or a script no longer writes flags or a mod-sequence from
memory, and any change it makes that IMAP shows takes a new mod-sequence, so the
folder's counter moves. That also mends two things one node gets wrong today: two
flag changes in one folder given one mod-sequence, and a script's save putting back
flags an IMAP client had just changed.

**Isolation.** The reads the nodes rely on are at READ COMMITTED on every engine:
the folder counters, the message lists, the change log and the read-back of a
claim. SQL Server's sessions stay at READ UNCOMMITTED for everything else, so on
SQL Server these statements carry the `READCOMMITTED` table hint. Without it, a
client could be shown a UID that a transaction then rolled back and a retry gave to
another message.

**Longer holds** go in a lease table, `hm_locks`: a unique name, the node that
holds it, the time it expires and an epoch. A claim is the same conditional update
as the queue's, and a name that has no row yet is taken by an insert that only one
node can win: it counts 1 when it took the name and 0 when the unique name refused
it, and any other failure counts -1. An insert the layer ran again after a
reconnect counts 0 when its first run took the name, so after a reconnect the row is
read back and its epoch decides. These use it:

- the guard's `active` lease (above) and the scheduler's leadership (below);
- POP3's mailbox lock (`pop3:<account>`). Held for the session, and renewed while
  it runs;
- a mailbox move and a restore into a mailbox (`mailbox:<account>`);
- the backup's run slot and a resource's booking decision, which are locks inside
  the process today.

**The audit chain's head** becomes a row of its own, read and moved in the
append's transaction. Its lock puts appends from every node in one order, and an
append that is slow cannot fork the chain, which a lease that expired under it
could.

### Sessions on any node

- **SMTP** holds nothing between connections. Any node takes any connection.
- **IMAP.** A connection stays on one node for its life, but it must see what the
  other nodes do. **The database is the bus**:
  - Every delivery, flag change and expunge moves the folder row's
    `foldercurrentuid` or `foldercurrentmodseq`. While other nodes are live,
    `SELECT`, `EXAMINE` and `STATUS` read those two counters from the database
    before they answer, so `UIDNEXT`, `HIGHESTMODSEQ`, `MESSAGES` and `UNSEEN` are
    the database's, and a client that finds new mail by polling `STATUS` finds it.
  - One watcher per node reads the counters every two seconds, in batches, for the
    folders that have a live listener on that node: those selected, and those the
    webmail is watching. When either has moved, the folder's message list reads the
    rows whose mod-sequence is above the last one it knew, and the expunges recorded
    for QRESYNC above it. The node then tells its own sessions, as it does after a
    local change, so IDLE, NOOP and the untagged responses are unchanged. A
    notification from its own node still arrives at once.
  - The counter is read before the rows, at READ COMMITTED, and the list's last
    known mod-sequence moves to the counter so read: every number commits in order
    with its row, so nothing below it can still be on its way.
  - Folder trees follow the change log (below).
  - `\Recent` may be seen by one session on each node.
  - There is no node-to-node channel: it would need its own authentication, a
    port and its discovery, to save two seconds.
- **The webmail** reads the counters as `STATUS` does before it lists a folder.
- **POP3** takes the mailbox lock from `hm_locks` in place of `POP3Sessions`, and
  reads the Inbox's counters as `SELECT` does when the session starts.
- **Quota** is enforced on an account's size kept for a minute at most while other
  nodes are live, and dropped when its folders' counters move.
- **Browser sessions** (the webmail, the Deck, the portal) move into the database:
  a table of the token's digest, the account, and the times the session policies
  need. Then a request may land on any node, and ending a session ends it
  everywhere. The alternative, a balancer that keeps a browser on one node, still
  leaves ending a session to the node that holds it. It is not recommended.
- **Sign-in security** counts in the database: the account lockout's failures, the
  second-factor guard's last step taken and its budget of wrong codes, and the set
  of sign-in states already spent. The key the OIDC, SAML and passkey sign-ins are
  sealed with is kept there too, sealed under the installation's stored-secrets
  key, so a sign-in started on one node completes on another. A lockout that each
  node counts on its own is a lockout that a second node doubles, and a TOTP code
  taken on one node must not be taken again on another (RFC 6238, section 5.2).
- **Per node, and documented**: the rate limits, the per-destination limits, the IP
  warm-up's reservations for deliveries in flight, and POP3's login delay. A limit
  is a defence against load, which each node bears for itself, and the
  per-destination limits multiply by the number of nodes.

### Invalidating the caches

A change log, `hm_changes`, gets one row for each administrative write through the
persistence layer: the kind of object, its id, the node that wrote it, and a
sequence number. The kinds are accounts, domains, aliases, lists, domain aliases,
routes, security ranges, certificates, listeners, groups, ACLs, an account's
folder tree, setting values (server, domain and account), compromised-account
holds, sessions to end, messages removed from the queue, Sieve scripts and DKIM
keys. The row is written in the same transaction as the change it records, so a
change commits with its row or not at all. Its sequence number is taken from a
counter row inside that transaction, as a UID is: an identity column would commit
out of order, and a reader that had passed 101 would never see 100.
Each node reads the rows after the last one it saw, every two seconds, skips its
own, and acts on each kind:
- it drops the object from `CacheContainer`, `IMAPFolderContainer`, `InboxIDCache`,
  `AccountSizeCache` and the setting values of its owner;
- it applies a hold;
- it puts a security range into its own firewall when auto-ban reaches the
  firewall;
- it ends a revoked device's connections, and an account's sessions before a
  mailbox move;
- it clears its own queue when another node cleared the queue, and removes the
  file of one of its queued messages that another node removed.

The TTLs stay as they are, as a backstop. Messages do not go through the log:
their changes move the folder counters already. A change made outside the server,
by SQL by hand, writes no row, and is seen at each cache's TTL or a restart, as
today. PostgreSQL's `LISTEN`/`NOTIFY`
would be quicker, but the server runs on four backends, and a design that works
on one only would leave the other three with a different server. The log is pruned
after an hour, and the prune is recorded: a node that was behind it has missed
changes, and drops every cache the log feeds.

### Settings

Every write to `hm_settings` and `hm_inisettings` writes a settings row to the
change log, the command line's `--set-setting` included. Each node reloads the
store when it reads one, and applies each setting as that setting says: at once, or
at the next reinitialise or restart. A setting that waits for a restart is named in
the node's log and on the Deck until the node restarts. The sections of
`hMailServer.ini` that are each machine's own stay each machine's own:
`[Directories]`, `[Database]`, `[Security]` and `[SettingsOverride]`.

### Which node runs a scheduled task

The tasks fall into three kinds:

| Kind | Tasks | How they run |
|---|---|---|
| Each node's own | the object store's committer, the free disk space check, the work-queue health check, the log retention, the update check, the closing of idle mailbox keys, the metrics history (whose rows then name their node), the evaluation of the alerts' conditions (this node's disk and queues), the reputation watch, the consistency check (once it counts only its own rows), the sandbox's poller (over the held messages whose files it holds), and the TLS-RPT and DMARC reports (each node's own sessions, with the node's id in the report's id) | On every node, as now |
| Once for the whole set of nodes | the quarantine and list digests, the scheduled sends, the sending of the alerts, the backup schedule, ACME's orders, the directory sync, the DKIM key rotation, the mailbox, archive and expunge retention passes, the greylist, bounce and complaint maintenance, the removal of expired records, the pruning of the change log, of expired leases and of spent sign-in states; and, outside the scheduler, the message indexer and the migration manager | On one node, the leader: the one that holds the `leader` lease in `hm_locks`, renewed every 20 seconds. When the leader dies, another takes the lease once it has expired, within two minutes |
| Any node, row by row | the delivery queue, external fetch, the migration jobs, the outbound and inbound webhooks, the push notifications | Each row claimed with a lease |

A leader that loses its lease part-way through a pass must not leave a half-done
pass to be done twice. So each task that sends mail records what it has sent with
a compare-and-set on the state it already keeps, before it sends:
- the digest's state row, `hm_digeststate`;
- the scheduled send's row, `hm_scheduled`;
- the alert's `alerteventnotified`;
- the out-of-office memory, `hm_autoreplies`.

The next leader then skips what is already done. The tasks already written for a
second node, such as the complaint counters, keep their own guards.

A leader that is frozen or cut off must not keep working beside the new one. A node
counts itself leader only until the renewal it last sent, plus the lease, less a
margin, measured on its own monotonic clock, and stops the leader's work before
then. The indexer's and the migration manager's writes to their state also carry
the leader's epoch, so a leader that wakes too late changes nothing. They are
started and stopped from a thread of their own, never the one that renews the
lease, because stopping the indexer waits for its batch.

ACME's HTTP-01 challenge is answered by whichever node the certificate
authority's request reaches, so the challenge's token goes in the database. The
certificate the leader obtains goes there too, sealed, with the ACME account's key,
and each node deploys it.

### State on each node's disk

Under the design every node has a data directory of its own. Messages go to the
object store (below), and the queue is handed over at a drain. Everything else that
the server keeps as files moves too, kind by kind:

- Sieve scripts, their includes, and the vacation and duplicate records into the
  database, imported once from each node's disk;
- the keys the DKIM rotation makes into the database, sealed under the
  installation's key; each node writes them to its own disk before it signs with
  them;
- the quarantine, recoverable items under legal hold, protected and confidential
  messages, shared files, list archives and eDiscovery exports into the object
  store;
- ACME's certificate and account key into the database, as above.

What an administrator puts on each machine stays the administrator's to copy, as
for a warm standby: TLS certificate and key files, DKIM keys set by hand, the event
scripts and `PasswordPepper`. Switching active-active on refuses a set whose nodes
do not each read every path the database names, or do not hold the same pepper or
the same event scripts.

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
versions 1 or 2 (`ProxyProtocol.h`). The SMTP, IMAP and POP3 listeners each read
it from the peers on a list of their own: `SMTPProxyProtocolTrustedIPs`, and
since unit AA-23 `IMAPProxyProtocolTrustedIPs` and `POP3ProxyProtocolTrustedIPs`,
each with its switch. The country policy and `OnClientConnect` are asked at the
accept, before the header, so they see the balancer; in this topology the
geographic policy belongs on the balancer ([CountryBlocking.md](CountryBlocking.md)).
The balancer's health check is `/readyz` on the metrics listener
([HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)), which reads no PROXY
header, so that the check sends none; a drained or standby node answers it with 503.

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
- a message whose row was the queue's (one local recipient) has its `messagenode`
  set to 0 once it is in the store, where it would otherwise keep the accepting
  node from its queue life;
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

### SQL Server

The lab proves the design on PostgreSQL, MySQL and MariaDB with whole nodes. The
Linux server has no SQL Server backend, and the Windows gate runs on SQL Server
Compact, so the lab's checks on SQL Server are of the engine's own behaviour, not of
the server's code that talks to it. Of that code, AA-02's transaction scope has
been run on SQL Server: its checks and their controls, through the server's own ADO
code (MSOLEDBSQL 18) on a Windows node against SQL Server 2022, on 13 October 2026.
Active-active is not offered on SQL Server until two Windows nodes have been run on
it; until then a SQL Server installation keeps the warm standby, with the guard.

### Upgrades

A set of nodes is upgraded stopped: the nodes are drained and stopped, the
database is upgraded once, and they start on the new build. There is no rolling
upgrade. The refusal is in the schema upgrader itself: while a live node of
another build is registered, `UpgradeOnStart`, `hmailserver --upgrade-database` and
the live update refuse to upgrade, and the Windows installer's DBUpdater asks the
same before it starts. For the first upgrade, when nodes of the older build have
never registered, the upgrader refuses, unless told to go on, while the database
has other sessions under the server's own login that name no program, which is how
an older node's sessions look; from then on every connection names itself. The commands that rewrite the message store refuse while
any node is live.

Schema changes it would take
----------------------------

None is made or reserved. Each would be a numbered step on all four backends,
with the create scripts to match, and SQL Server Compact would get the tables and
use them as one node. Every lease and heartbeat column holds UTC. No column is
dropped, so a database can still be taken back by restoring it alone, and a node
whose row the restore rolled back registers again.

| Table | Change |
|---|---|
| `hm_nodes` | New: id, identity, name, run id, build, lease protocol, started, heartbeat, state |
| `hm_settings` | Rows for the switch (`ActiveActiveEnabled`, 0) and the lease and heartbeat lengths |
| `hm_locks` | New: a unique name, the node that holds it, its expiry, its epoch; with the guard's `active` row |
| `hm_messages` | Beside `messagelocked`: the node that holds the lease, its expiry and its epoch, and `messagenode`, the node whose disk holds the file (0 when it is in the store) |
| `hm_fetchaccounts`, `hm_migrationjobs` | The same lease beside `falocked` and `mjstate` |
| `hm_webhookqueue`, `hm_inboundhookqueue`, `hm_scheduled`, the push queue | The same lease; the inbound hooks also the accepting node |
| The audit chain | New: its head, one row |
| `hm_changes` | New: a sequence, the kind of object, its id, the node, the time; and a counter row the sequence is taken from |
| `hm_metricsamples` | The node that sampled |
| Browser sessions | New: the token's digest, the account, created, last used, and the address bound to it |
| Sign-in security | New: the lockout's failures per name in a window, the second factor's last step and wrong codes, the sign-in states spent, the sealed key they are sealed with |
| Sieve | New: the scripts and the vacation and duplicate records |
| DKIM | The rotation's keys, sealed |
| ACME | New: the challenge tokens, the certificate and the account key |

The first version planned a table recording each unit of scheduled work done, and
a generation number for each account's folder tree. Neither is needed: the
compare-and-set on the state each task already keeps does the first, and the
change log does the second.

Failure modes
-------------

| What fails | What happens | What limits it |
|---|---|---|
| A node dies part-way through a delivery | Its own messages wait for its disk: a standby on its replicated data directory is the same node and delivers them. A message it held of node 0's is taken by another node once its lease has expired. The recipient may get one twice | The lease's length. This is the same cost as a warm-standby failover today |
| A node is killed after the remote server's 250 and before the row's write | Its restart delivers that message again | The deliveries in flight at that moment |
| A node freezes for longer than its lease | Its own messages wait for it, since only it holds their files. A node-0 message it had claimed is taken by another node. When the frozen node wakes, its writes to that row change nothing and it stops, but a transmission it was part-way through can finish | At least once, not exactly once. Checking the lease before `DATA` narrows it |
| A node restarts seconds after a crash | It watches its own row for fifteen seconds, takes it over and releases its own leases | Its identity |
| Two processes run with one identity (a copied machine, a standby started on the replica) | The incumbent keeps its row; the newcomer stays closed and says so. If the newcomer had already taken it, the incumbent's heartbeat finds the row held, and it closes its listeners | Fifteen seconds; one heartbeat |
| The database is restored from a backup | A node's row names an old run; its heartbeat finds that, and it registers again | One heartbeat |
| Two nodes' clocks disagree, or a daylight-saving change comes | Nothing. Every lease is compared with the database's clock in UTC | The database's clock |
| A node loses the database | It cannot renew, so it stops claiming. Its leases expire and the others take the work. Its IMAP and POP3 sessions fail as they do today | The heartbeat and the lease |
| A node loses its clients but not the database | The balancer's health check takes it out of service. It goes on delivering its own queue | `/readyz` |
| The database fails over | The leases are rows, so they survive the failover. A transaction in flight rolls back and is tried again whole. A claim whose reply was lost is run again and recognises itself | The backend's own availability |
| The leader dies, or freezes | The tasks that run once pause until the `leader` lease expires, then another node runs them. A frozen leader has stopped its own leader work before its lease ran out, and its late writes change nothing. What was already recorded as sent is not sent again | Two minutes |
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
engine checks, run as the server's sessions run, at READ UNCOMMITTED. Testers that
race two connection pools in one process run in the Windows gate, whose database
is SQL Server Compact. The engine checks are also run through the server's own
database layer: `database-claim-tests` against the Linux suite's PostgreSQL and the
database matrix's MySQL and MariaDB before the server starts, and the same checks
inside the Windows server on a SQL Server Compact database of their own
(`DatabaseClaimTester`). Faults are made on purpose:
- a node killed (`docker kill`);
- a node frozen past its lease (`docker pause`, then `unpause`);
- a node cut off from the database;
- a slow sink, and a sink that answers 451;
- a database in a time zone with daylight saving, its clock moved across a change;
- a node recreated without a fixed host name, and a copy of a node's disk started.

What it must show:

1. **No lost mail, and duplicates bounded.** Mail is sent through every node to
   many recipients while nodes are killed and frozen at random. The sink holds
   every message, and each once, except at most the deliveries in flight at each
   kill and the node-0 messages whose lease expired under a delivery, matched by
   their sequence numbers.
2. **UIDs.** Mail is delivered into one mailbox from every node at once, and
   appended on every node. An IMAP client on each node sees UIDs that are unique
   and ascending, a `UIDNEXT` that agrees with them, and every message as it was
   sent.
3. **IMAP, the webmail and POP3 across nodes.** A client in IDLE on one node is
   told of a delivery, a flag change and an expunge made on another within the
   watcher's interval. `STATUS` polled on one node, and the webmail's listing,
   show another node's delivery at once. A message deleted over POP3 on one node is
   gone on the other. Two nodes changing different flags on one message keep both.
4. **POP3.** A second POP3 session on another node to the same mailbox is
   refused while the first is open.
5. **Fetch.** Each external account is fetched once in each interval, with
   delete-after-download on, while nodes are killed.
6. **Scheduled tasks.** The digests, reports, alerts, webhooks and scheduled sends
   go out once in each period, with the leader killed, and frozen, part-way
   through a pass.
7. **Caches, settings, sessions and local state.** A change made on one node is
   seen on each of the others within the stated bound: an account disabled, a
   password changed, a setting that applies at once, a browser session ended, a
   TOTP code used, a Sieve script uploaded. A sign-in started on one node
   completes on another.
8. **Fencing.** A node frozen past its lease while delivering node-0 messages
   changes no row when it wakes, and says so in its log.
9. **Drain.** A drained node hands its queue over, deferred and held messages
   included, and stops with an empty queue, while the other nodes deliver it; a
   webmail send made on it after the hand-over is delivered too.
10. **The message store.** A message delivered on one node is read on another at
    once. With the store stopped, no node writes a placeholder. A quarantined
    message is released, and a shared file read, from another node.
11. **The guard and the identity.** Two nodes started at once with active-active
    off: one serves, the other stays closed, through a balancer that checks
    `/readyz`. A node recreated without its host name keeps its queue. A copy of a
    node's disk is held closed.
12. **Clocks.** Across a daylight-saving change no lease is taken early and none
    held an hour late.
13. **Regressions.** The Linux regression suite passes against one node of a set
    of two, on PostgreSQL, MySQL and MariaDB - the database matrix - and a single
    node passes the Windows and Linux gates unchanged at every step.
14. **The client behind the balancer.** Failed logons over IMAP, and over POP3,
    from one client through the balancer ban that client and not the balancer,
    and another client behind it is still served (`aa-lab.sh proxy`, unit AA-23).

The warm-standby rehearsal of 9 September 2026 ([WarmStandby.md](WarmStandby.md),
"The rehearsal") showed the problem this design solves: a second service's start
set a message's lock back to 0 while the first was delivering it. The lab's
baseline, above, shows how much of it there is.

The order of the work
---------------------

Thirty-one units, each small enough for one person or agent in a few days. The
first phase makes one node sturdier, and mends two things one node gets wrong
today. Most of the rest change nothing on one node: one node is its own leader,
wins every claim, skips its own change-log rows and has no folder to watch.

1. **One node, sturdier** (no schema step):
   - AA-01, the database layer's row counts on all four backends, the UTC clock,
     and the engine checks run as the server runs (built; nothing uses the counts
     or the clock until AA-03 and AA-06);
   - AA-02, a transaction scope the persistence layer can use - built, as
     `DatabaseTransaction`. No statement inside it is run again after a reconnect,
     except on a MySQL database that is not transactional, where a scope runs its
     statements as the pool does. A statement that fails in it for a reason a
     retry would not mend is reported as the pool reports one (`HM5032`,
     `HM5033`), and a `BEGIN` or a `COMMIT` the database refuses so as `HM8502`;
     `DatabaseTransaction::Run` reports a unit of work it gave up after its
     attempts (`HM8500`) and one whose connection was lost during its `COMMIT`,
     which it does not run again since whether it committed is not known (`HM8501`).
     On SQL Server Compact the pool holds the server's one connection, so an open
     scope holds it: a scope that fails gives it back before the failure is
     reported, and a statement its own thread makes outside it while it is open
     (an OnError script's) is refused at once (`HM8503`), where it would have waited
     for a connection only that thread could give back. Its SQL Server half was run
     on a Windows node through the server's own ADO code (MSOLEDBSQL 18; "SQL
     Server", above);
   - AA-03, conditional claims on today's lock columns, and a queue delete that
     cannot remove a delivered row;
   - AA-04, UIDs and mod-sequences in one transaction;
   - AA-26, flag changes and whole-row saves under the folder row's lock;
   - AA-23, the PROXY protocol for IMAP and POP3.
2. **The guard:**
   - AA-05, nodes register in `hm_nodes` with an identity that follows the disk,
     and while active-active is off one node is active and the others keep their
     doors shut. This turns the rule of [WarmStandby.md](WarmStandby.md) from a
     sentence into a refusal. It was offered for 6.3.6 as the owner's option, and
     was left out by the maintainers' decision of 28 September 2026 (design only);
   - AA-27, every schema upgrade, and every command that rewrites the store,
     refuses while a node of another build, or any node, is live.
3. **Leases:**
   - AA-06, `hm_locks` and the row lease on the database's clock in UTC;
   - AA-07, the queue as leases, with node affinity and adoption;
   - AA-08, the deliverer renewed and fenced;
   - AA-09, fetch accounts and migration jobs.
4. **One leader:**
   - AA-10, the leader, fenced, and the scheduler's kinds of task;
   - AA-11, work done once (sends, digests, alerts, webhooks, push, auto-replies);
   - AA-12, records that name their node;
   - AA-13, the named leases (POP3, moves, restores, backup, booking) and the
     audit chain's head.
5. **What one node changes, the others see:**
   - AA-14, the change log, in order;
   - AA-15, caches, folder trees, holds, bans and session ends;
   - AA-16, settings;
   - AA-18, IMAP, the webmail and POP3 reading the other nodes' changes;
   - AA-17, browser sessions in the database;
   - AA-28, sign-in security in the database.
6. **Secrets:** AA-19, every stored secret under the installation's key.
7. **The message store, the local state and the drain:**
   - AA-20, write-through and no placeholders while other nodes are live;
   - AA-29, Sieve in the database;
   - AA-30, the rotated DKIM keys in the database;
   - AA-31, the quarantine and the other kept files in the object store;
   - AA-21, the drain that hands everything over;
   - AA-22, ACME across nodes.
8. **Proof and switch-on:**
   - AA-24, every scenario above, on PostgreSQL, MySQL and MariaDB;
   - AA-25, the switch (`ActiveActiveEnabled`) and its checks: an engine the lab
     has proved, InnoDB only, not SQL Server Compact, the object store, the
     stored-secrets key, every kind of local state moved, and the administrator's
     own files the same on every node. Then the node list on the Deck, and the
     warm-standby and high-availability documents rewritten for a set of nodes.

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
