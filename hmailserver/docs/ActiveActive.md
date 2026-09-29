Active-active nodes: a design
=============================

**Not built.** This is a design, written on 29 September 2026 for 6.3.6, and
nothing in it exists in the server. There is no code for it, no schema step and
no setting. Until it is built, [WarmStandby.md](WarmStandby.md) holds without
exception: **exactly one hMailServer service may run against a database at a
time.** Two active services on one database deliver the same mail twice, fetch
the same remote mailbox twice, and can give two messages in one mailbox the same
IMAP UID. Nothing warns when that happens.

What is meant here by *active-active* is two or more nodes that serve the same
domains at the same time from one database, behind one address. Any node takes
SMTP, IMAP, POP3 and the web pages, and any node delivers the queue. A node can
then be taken down for maintenance without a failover. This is the Roadmap2 row
"Active-active nodes" and work item #6. The owner closed #6 on 27 September 2026
as work that could not be finished in 6.3.5. For 6.3.6 the maintainers took the
design alone, because the work is new server code, not a change to what exists.

This document says what the server assumes today, taken from the code, and what
each assumption would have to become. It then lists the schema changes, the
failure modes, the bench that would prove it, and the order the work would take.

What the server assumes today
-----------------------------

Every one of these assumes a single process. Each is correct for one node, and
each goes wrong with two.

| Where | What it does | What two nodes do to it |
|---|---|---|
| The delivery queue's start (`SMTPDeliveryManager::DoWork`, `PersistentMessage::UnlockAll`) | `update hm_messages set messagelocked = 0 where messagetype = 1 and messagelocked = 1`, once, when delivery starts: a lock left by a crash would otherwise hold its message for ever | A node that starts releases every message the other node is delivering at that moment, and both deliver them |
| The queue's selection (`SMTPDeliveryManager::LoadPendingMessageList_`, `GetNextMessage_`) | Reads every due, unlocked message into a list, then locks each one as it hands it out | Both nodes read the same list |
| The queue's lock (`PersistentMessage::LockObject`) | `update hm_messages set messagelocked = 1 where messageid = @MESSAGEID`, with no condition on the lock and no count of the rows it changed. The database layer's `Execute` returns true or false and no count | Both nodes "win" the same message, and both deliver it |
| The queue's files | A queued message's file is on the local disk of the node that accepted it. The object store never holds the queue ([ObjectStorage.md](ObjectStorage.md)) | Only the accepting node can deliver a message, whichever node claims its row |
| External fetch (`ExternalFetchManager::DoWork`, `PersistentFetchAccount`) | `UnlockAll` at start (`update hm_fetchaccounts set falocked = 0`), and an unconditional `Lock` | The same as the queue: a remote POP3 mailbox is fetched by both nodes, and one with delete-after-download ends up split between them |
| A message's UID (`PersistentMessage::SaveObject`, `PersistentIMAPFolder::GetUniqueMessageID`) | Under `FolderManipulationLock`, a lock inside the process, the folder's counter is increased by one statement and read back by a second, and the message is inserted | Two nodes can read back the same UID. A message can also be committed with a lower UID than one a client has already seen, and a client never fetches such a message (RFC 3501 requires UIDs to be strictly ascending) |
| The folder trees (`IMAPFolderContainer`) | Each account's folders are cached in the process, with each folder's current UID and mod-sequence | A folder made, renamed or removed on one node is not seen on the other |
| Change notification (`NotificationServer`) | A delivery, an APPEND, an EXPUNGE or a flag change tells every IMAP session of this process that has the folder selected. IDLE waits on it | A session in IDLE on one node is not told about a delivery made on the other |
| POP3's mailbox lock (`POP3Sessions`) | A set in memory: one POP3 session per mailbox | Two POP3 sessions on two nodes can have the same mailbox open, and both can delete from it |
| The caches (`CacheContainer`, `MessageCache`, `InboxIDCache`, `AccountSizeCache`) | Accounts, domains, aliases and lists are kept for their TTL, 60 seconds by default. An owner's own setting values are read again after a minute "so that a row another node wrote is seen" (`CacheContainer.h`), and the object store's answers after half a minute | A change made on one node is seen on the other within the TTL, and not before. A disabled account can still sign in on the other node for up to a minute |
| The settings (`IniFileSettings`, `IniSettingStore`, `Application::Reinitialize`) | Read from the database at start. A change is applied on the node that made it, at once or at its next start or reinitialise, as each setting says. The message store's backend is read again every five seconds ([ObjectStorage.md](ObjectStorage.md)) | A setting changed on one node is not applied on the other until that node restarts or reinitialises |
| The scheduler (`Scheduler`, and the tasks `Application.cpp` registers) | Every task runs every minute in the process that registered it: the digests, the scheduled sends, the TLS-RPT and DMARC reports, the retention passes, the backup schedule, ACME, the directory sync, the object store's committer, the consistency check and the rest | Each node runs every task. Some are already safe with two nodes: the DKIM key rotation, the complaint and bounce counters, the report importer's de-duplication and the eDiscovery holds say so in their code. The others send twice, or work against a local disk that is not the other node's |
| Browser sessions ([DevicesAndSessions.md](DevicesAndSessions.md)) | Kept in the server's memory | A request that reaches the other node is not signed in. Ending a session ends it on its own node only |
| The account lockout (`AccountLockout`) and the rate limits | Counted in memory, "like the sibling stores" | Each node counts on its own, so two nodes allow twice the attempts |
| Secrets sealed with DPAPI ([WarmStandby.md](WarmStandby.md), "The DPAPI constraint") | Route, fetch-account and per-domain relay passwords, and TLS key passphrases, are sealed to the machine | A password entered on one node cannot be read on the other |
| The listeners | Rows of `hm_tcpipports`, which every node reads | Each node binds every port, as the rehearsal in [WarmStandby.md](WarmStandby.md) saw. That is what a balancer in front wants |
| A schema upgrade (`SchemaUpgrader`) | Two walkers on one database are serialised on the version row, or on a named lock on MySQL | Already safe |

What it would take
------------------

### Nodes, and the database's clock

Each node is a row of a new table, `hm_nodes`: its id, name, build, the lease
protocol it speaks, when it started, and a heartbeat it renews every ten seconds.
A node whose heartbeat is older than its lease, 60 seconds for example, is taken
for dead. Every lease below is compared with **the database's clock**
(`CURRENT_TIMESTAMP` in the statement), never a node's own clock, so two nodes
whose clocks disagree still agree on who holds what.

A node that cannot renew its heartbeat, because it has lost the database, stops
claiming work at once. It also stops starting deliveries once its leases have run
out. That is how it fences itself.

The database layer needs one thing it does not have: **the number of rows a
statement changed**. ADO gives it as `RecordsAffected`, MySQL as
`mysql_affected_rows` and PostgreSQL as `PQcmdTuples`. Every claim below is a
conditional `UPDATE` that counts as won only when it changed exactly one row.

### The queue: leases with expiry, and fencing

`messagelocked` becomes a lease. Its columns are the node that holds it, the time
it expires, and an epoch that goes up by one at every claim.

- **A claim** is
  `update hm_messages set lockednode = @me, lockuntil = <now + lease>, lockepoch = lockepoch + 1 where messageid = @id and (lockednode = 0 or lockuntil < <now>)`,
  and it is won only when it changed one row. The selection can stay as it is. A
  row that another node claimed between the select and the claim is simply lost.
- **Renewal.** A delivery can take minutes, so the lease is renewed every minute
  while it runs. Five minutes is a lease that one slow remote server does not
  outlast.
- **Fencing.** Every write the deliverer makes to the message's row carries
  `and lockednode = @me and lockepoch = @epoch`: the retry count, the next try,
  the bounce, the delete. A write that changes no row means the lease has been
  lost. The deliverer then stops, sends nothing more, and logs it. The lease is
  also checked before each recipient's `DATA` and before the queue file is
  removed.
- **At start**, a node releases only the leases its own id holds, left from its
  previous run. It never releases another node's. `UnlockAll` in its present form
  is removed.
- **The queue's files** stay on the accepting node's local disk. The object store
  does not hold the queue, and the accept path's flush to local disk before the
  250 is the durability promise, which a network write would weaken. So a row
  carries the node that accepted it. **Only that node claims it while its
  heartbeat is fresh.** A planned stop drains the node first (the drain the live
  update already uses, `POST /api/v1/server/drain`), so maintenance needs no
  failover. When a node dies, its queued messages wait for its disk, as they wait
  in a warm-standby failover today. Moving the queue into the object store, so
  that any node could deliver any message, is a separate and later step. It
  would put a network write before the 250.

A lease cannot make delivery exactly-once. A node that is frozen for longer than
its lease (a VM paused, a stalled disk) may be half-way through a transmission
that it then finishes, while a second node has already taken the message over.
Fencing stops the frozen node from changing the row afterwards. It cannot stop
bytes that are already on the wire. The guarantee stays at least once, as it is
across a failover today. Duplicates happen only when a lease expires under a
delivery, and the lease's length is chosen to make that rare.

External fetch takes the same lease on `hm_fetchaccounts`. So do the database
queues that are already worked from a table: the outbound webhooks
(`hm_webhookqueue`), the inbound hooks (`hm_inboundhookqueue`) and the scheduled
sends (`hm_scheduled`).

### Locks that hold across nodes

**UIDs and mod-sequences** do not need a lease. The folder's counter is increased
and the message inserted **in one transaction**:
`update hm_imapfolders set foldercurrentuid = foldercurrentuid + 1 where folderid = @id`,
then the counter is read and the row inserted, then the transaction commits.
PostgreSQL, MySQL and SQL Server all hold the folder row's lock until the commit.
That keeps UIDs unique, and it also makes them commit in ascending order: a
second delivery into the same folder waits for the first to commit, which is what
`FolderManipulationLock` does inside one process today. The same holds for
`foldercurrentmodseq`.

**Longer holds** go in a lease table, `hm_locks`: a unique name, the node that
holds it, the time it expires and an epoch. A claim is the same conditional
update as the queue's. These use it:

- POP3's mailbox lock (`pop3:<account>`). Held for the session, and renewed while
  it runs;
- a mailbox move, a restore into a mailbox, and the backup's run slot, which is a
  lock inside the process today (`BackupManager`);
- the scheduler's leadership (below).

### Sessions on any node

- **SMTP** holds nothing between connections. Any node takes any connection.
- **IMAP.** A connection stays on one node for its life, but it must see what the
  other nodes do. A selected folder is watched through its
  `foldercurrentmodseq` and `foldercurrentuid`, which one indexed read gives. IDLE
  and NOOP read them every few seconds, and when either has moved, the session
  reads the changes as it does after a local notification. A notification from
  its own node still arrives at once. Each account's folder tree carries a
  generation number, raised by every create, rename, delete and subscription
  change. `LIST` and `SELECT` read it, and read the tree again when it has moved.
- **POP3** takes the mailbox lock from `hm_locks` in place of `POP3Sessions`.
- **Browser sessions** (the webmail, the Deck, the portal) move into the database:
  a table of the token's digest, the account, and the times the session policies
  need. Then a request may land on any node, and ending a session ends it
  everywhere. The alternative, a balancer that keeps a browser on one node, still
  leaves ending a session to the node that holds it. It is not recommended.
- **The account lockout** counts in the database. A lockout that each node counts
  on its own is a lockout that a second node doubles. The rate limits may stay
  per node, each set to the total divided by the number of nodes.

### Invalidating the caches

A change log, `hm_changes`, gets one row for each write through the persistence
layer: the kind of object, its id and a sequence number. Each node reads the rows
after the last one it saw, every two to five seconds, and drops those objects
from `CacheContainer`, `IMAPFolderContainer`, `InboxIDCache`, `AccountSizeCache`
and the setting values of their owners. The TTLs stay as they are, as a backstop.
PostgreSQL's `LISTEN`/`NOTIFY` would be quicker, but the server runs on four
backends, and a design that works on one only would leave the other three with a
different server. The log is pruned after an hour.

### Settings

Every write to `hm_settings` and `hm_inisettings` raises a settings generation.
Each node reads it with the change log, reloads the store when it has moved, and
applies each setting as that setting says: at once, or at the next restart. A
setting that needs a restart is named in the node's log and on the Deck until
the node restarts. The sections of `hMailServer.ini` that are each machine's own
stay each machine's own: `[Directories]`, `[Database]`, `[Security]` and
`[SettingsOverride]`.

### Which node runs a scheduled task

The tasks fall into three kinds:

| Kind | Tasks | How they run |
|---|---|---|
| Each node's own | the object store's committer, the free disk space check, the work-queue health check, the log retention, the update check, the removal of expired records (which also closes this node's idle mailbox keys), the metrics history (whose rows then name their node), the alert evaluation (whose conditions are this node's disk and queues) and the reputation watch (a node may send from an address of its own) | On every node, as now |
| Once for the whole set of nodes | the quarantine and list digests, the scheduled sends, the TLS-RPT and DMARC reports, the backup schedule, ACME, the directory sync, the DKIM key rotation, the mailbox, archive and expunge retention passes, the greylist and bounce and complaint maintenance, the outbound and inbound webhooks and the push notifications | On one node, the leader: the one that holds the `scheduler` lease in `hm_locks`, renewed each minute. When the leader dies, another takes the lease once it has expired, within two minutes |
| Neither, as it stands | the message store's consistency check | It compares the database with the local disk, and so reports the other nodes' queue files and not-yet-committed files as missing. It would have to be taught which files are whose first |

A leader that loses its lease part-way through a pass must not leave a half-done
pass to be done twice. So each task that sends mail also records what it has
sent, one row for each unit of work: an account's digest for a period, a
domain's report for a day. The next leader then skips what is already done. The
tasks already written for a second node, such as the DKIM rotation and the
complaint counters, keep their own guards.

ACME's HTTP-01 challenge is answered by whichever node the certificate
authority's request reaches, so the challenge's token goes in the database.

### Secrets

Every node imports the installation's stored-secrets key, as a standby does
today ([WarmStandby.md](WarmStandby.md), setup step 5). The passwords still
sealed with DPAPI (routes, fetch accounts, per-domain relays, TLS key
passphrases) move under that key first. Without that move, a password entered
on one node fails on every other node.

### In front of the nodes

A TCP load balancer passes the client's address with the PROXY protocol, whose
versions 1 and 2 the listeners already read (`ProxyProtocol.h`). Its
health check is `/readyz` ([HighAvailabilityRunbook.md](HighAvailabilityRunbook.md)).
The message store is the object store ([ObjectStorage.md](ObjectStorage.md)).
Every node reads the local disk first and the store second. So a message
delivered on one node is missing on the other nodes until that node's committer
has put it into the store, which runs every minute. Before active-active, either
the committer puts a delivered message into the store before its row is saved,
or a node reads that message from the node that holds it. **SQL Server Compact is
excluded**: it is one local file.

Schema changes it would take
----------------------------

None is made or reserved. Each would be a numbered step on all four backends,
with the create scripts to match, and SQL Server Compact would get the tables
without using them.

| Table | Change |
|---|---|
| `hm_nodes` | New: id, name, build, lease protocol, started, heartbeat, state |
| `hm_messages` | A lease in place of `messagelocked`: the node that holds it, its expiry and its epoch. Also the node that accepted the message |
| `hm_fetchaccounts` | The same lease in place of `falocked` |
| `hm_webhookqueue`, `hm_inboundhookqueue`, `hm_scheduled` | The same lease, where each takes its rows today |
| `hm_locks` | New: a unique name, the node that holds it, its expiry, its epoch |
| `hm_changes` | New: a sequence, the kind of object, its id, the time |
| `hm_imapfolders` or `hm_accounts` | A folder-tree generation for each account |
| The settings | A settings generation, as a row of `hm_settings` |
| Browser sessions | New: the token's digest, the account, created, last used, and the address bound to it |
| The account lockout | New: failures per name, in a window |
| The scheduled tasks | New: a row for each unit of work done (task, key, period) |

An upgrade would take the old lock columns away in the same step as it adds the
leases. A node of the previous build then refuses the database
(`REQUIRED_DB_VERSION`), so a set of nodes cannot run two builds at once. An
upgrade stops every node, upgrades the database once, and starts them on the new
build.

Failure modes
-------------

| What fails | What happens | What limits it |
|---|---|---|
| A node dies part-way through a delivery | Its lease expires and another node takes the message, if it holds the file. Otherwise the message waits for the dead node's disk. The recipient may get it twice | The lease's length. This is the same cost as a warm-standby failover today |
| A node freezes for longer than its lease | Another node takes its messages. When the frozen node wakes, its row updates change nothing and it stops, but a transmission it was part-way through can finish | At least once, not exactly once. Checking the lease before `DATA` narrows it |
| Two nodes' clocks disagree | Nothing. Every lease is compared with the database's clock | The database's clock |
| A node loses the database | It cannot renew, so it stops claiming. Its leases expire and the others take the work. Its IMAP and POP3 sessions fail as they do today | The heartbeat and the lease |
| A node loses its clients but not the database | The balancer's health check takes it out of service. It goes on delivering its own queue | `/readyz` |
| The database fails over | The leases are rows, so they survive the failover. A transaction in flight rolls back and is tried again | The backend's own availability |
| The leader dies | The tasks that run once pause until the `scheduler` lease expires, then another node runs them. The units of work already recorded are not done again | Two minutes |
| Deliveries into one mailbox from several nodes | The folder row's lock puts them in order, as the lock inside one process does now | The time of one insert |
| Cache staleness | A change reaches the other nodes within the change log's interval. An account disabled on one node can sign in on another for that long | Two to five seconds, where it is 60 seconds today |
| A node of another build or lease protocol | It refuses the database at start, or refuses to join when `hm_nodes` shows a different protocol | The schema pin and the protocol column |
| The object store fails | Reads of messages that are only in the store fail on every node. New deliveries go on, onto each node's local disk | [ObjectStorage.md](ObjectStorage.md) |

How it would be tested
----------------------

This needs a bench of its own:

- three nodes, as Linux containers or VMs;
- PostgreSQL, MySQL and SQL Server;
- an S3-compatible store (MinIO) as the message store;
- a TCP load balancer (HAProxy) with the PROXY protocol, and a sink SMTP server
  that records every message it is given.

Faults are made on purpose: a node killed (`kill -9`), a node frozen past its
lease (`SIGSTOP`, then `SIGCONT`), a node cut off from the database or from its
clients (firewall rules), clocks set minutes apart, and the database failed over.

What it must show:

1. **No lost mail, and duplicates only where a lease expired.** Mail is sent
   through every node to many recipients while nodes are killed and frozen at
   random. The sink holds every message, and it holds each one once, except for
   messages whose lease expired under a delivery. The log names each of those.
2. **UIDs.** Mail is delivered into one mailbox from every node at once. An IMAP
   client on each node sees UIDs that are unique and ascending, a `UIDNEXT` that
   agrees with them, and no message it fails to fetch.
3. **IDLE across nodes.** A client in IDLE on one node is told of a delivery made
   on another within the polling interval.
4. **POP3.** A second POP3 session on another node to the same mailbox is
   refused while the first is open.
5. **Fetch.** Each external account is fetched once in each interval, with
   delete-after-download on, while nodes are killed.
6. **Scheduled tasks.** The digests, reports and scheduled sends go out once in
   each period, with the leader killed part-way through a pass.
7. **Caches and settings.** A change made on one node is seen on each of the
   others within the stated bound: an account disabled, a password changed, a
   setting that applies at once.
8. **Fencing.** A node frozen past its lease changes no row when it wakes, and
   says so in its log.
9. **Drain.** A drained node hands its work over and stops with an empty queue,
   while the other nodes carry on.
10. **Regressions.** The Windows and Linux regression suites pass against one
    node of a set of three.

The warm-standby rehearsal of 9 September 2026 ([WarmStandby.md](WarmStandby.md),
"The rehearsal") is where the bench starts. It showed the problem this design
solves: a second service's start set a message's lock back to 0 while the first
was delivering it.

The order of the work
---------------------

1. **A guard, first and alone.** Nodes register in `hm_nodes`, and a node that
   finds another live node refuses to start its delivery queue and external
   fetch, and says so. This turns the rule of [WarmStandby.md](WarmStandby.md)
   from a sentence into a refusal. Its cost is one schema step. It was offered
   for 6.3.6 as the owner's option, and was left out by the maintainers'
   decision of 28 September 2026 (design only).
2. The database layer's row counts, and the leases on the database's clock.
3. The queue and fetch leases, with fencing, and `UnlockAll` for a node's own
   leases only.
4. UIDs and mod-sequences in one transaction.
5. The change log, the settings generation and the folder-tree generation.
6. IMAP's watch on other nodes, POP3's lock, browser sessions and the lockout in
   the database.
7. The scheduler's leadership and the record of work done.
8. The DPAPI-sealed passwords moved under the installation's key.
9. The bench, and the tests above, before any of it ships.

What the warm-standby guidance keeps saying
-------------------------------------------

Until every step above has shipped and passed that bench, [WarmStandby.md](WarmStandby.md)
and [HighAvailabilityRunbook.md](HighAvailabilityRunbook.md) keep saying these,
and nothing here softens them:

- **One service per database.** A standby's service is set to Manual and left
  stopped.
- **Two active services on one database deliver twice.** The second one's start
  releases every message the first is delivering. The two select the queue
  without coordinating, and the lock is taken after the selection and without a
  condition. External fetch does the same, and IMAP UIDs can collide.
- **A failover starts the standby only once the primary is confirmed stopped**:
  powered off, its service stopped, or cut off from the network.
- **The message store is replicated, not shared** over SMB or a Cluster Shared
  Volume. The object store is the one store that nodes can share.
- **DPAPI-sealed passwords do not travel**, and the stored-secrets key is
  imported by hand.
