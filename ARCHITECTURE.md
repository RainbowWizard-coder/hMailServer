Architecture
============

A map of the codebase, for someone about to change it. It answers "where does this
change go" rather than "how does mail work", and it records the constraints that are
not obvious from reading any single file — the ones that have actually caused bugs
here.

Pair it with [CONTRIBUTING.md](CONTRIBUTING.md) for process and
[RELEASE.md](RELEASE.md) for the release gates.

Shape of the repository
-----------------------

```
hmailserver/
  source/
    Server/          C++ mail server - almost all feature work happens here
    Tools/           C# administration tools and the Control Panel (.NET 10)
    Addons/          Standalone sample add-ons
    DBScripts/       Schema creation and the upgrade chain, per backend
  test/
    RegressionTests/ NUnit suite driving a real running server
  installation/      Inno Setup installer
  docs/              Operator documentation
libraries/           Vendored third-party code
build/               Build, publish, preflight and test scripts
```

The server
----------

Entry point: `source/Server/hMailServer/hMailServer.sln`.

```
Server/
  COM/               COM/IDispatch public API - the management seam
  Common/            Shared infrastructure used by every protocol
  ExternalFetcher/   POP3 and IMAP *clients*: fetch mail from remote accounts
                     (`ExternalFetchClientBase` turns a downloaded message into a
                     delivered one; `IMAPClientConnection` also mirrors whole
                     folders when `FetchAccount.MirrorFolders` is on - 6.2.25/6.2.27)
  hMailServer/       Windows service shell (WinMain, service control)
  hMailServer.Minidump/  Crash-dump helper
  hMailServer.Updater/   The live-update apply helper: runs a verified installer,
                     waits for the service, rolls back (new in 6.2.28). Its Linux
                     counterpart is `platform/packaging/hmailserver-update`, a
                     shell script run as root by `hmailserver-update.path` and
                     `.service` when the server writes its request
  zlib/              Vendored zlib 1.3.2 for IMAP COMPRESS=DEFLATE (new in 6.2.28)
  IMAP/              IMAP
  POP3/              POP3
  SMTP/              SMTP, delivery queue, outbound transport security
```

`Server/hMailServer/` is the service shell only — no protocol or business logic.

### `Server/Common/`

| Sub-folder | Purpose |
|---|---|
| `AntiSpam/` | SPF, SURBL, DNS blacklists, greylisting, score-based filtering, `DMARC/`, and `DKIM/` (RSA and Ed25519 signing/verification, ARC sealing in `Arc.{h,cpp}`) |
| `AntiVirus/` | ClamAV (clamd and clamscan) and arbitrary command-line scanners |
| `Application/` | Startup, configuration, scheduling, logging. `IniFileSettings` holds every `hMailServer.ini` setting; `Application::StartServers` starts the optional listeners |
| `BO/` | Business objects — the domain model: domains, accounts, aliases, distribution lists, rules |
| `Cache/` | In-memory caches in front of the BO layer, to keep hot paths off the database |
| `Diagnostics/` | The checks behind the Control Panel's "Run diagnostics" |
| `Mime/` | MIME parsing and construction |
| `Persistence/` | One class per business object, mapping it to database columns. `MessageStore` is the one door to a message's file - where it is, and every way it is read, sized, copied, moved and deleted; `build/check-message-store-seam.py` refuses a path to a stored message taken anywhere else |
| `Scripting/` | Event hooks: VBScript/JScript through Windows Active Scripting (`ScriptSite`), JavaScript through the QuickJS engine the Linux builds embed (`ScriptBinding`, the engine under `Server/quickjs`) |
| `Sieve/` | `SieveLexer`/`SieveParser` (AST), `SieveEvaluator`, `SieveStorage` (per-account scripts under `{DataDir}\Sieve\`), and the optional RFC 5804 `ManageSieveServer`. Evaluated during local delivery |
| `SQL/` | Database abstraction: connections, pooling, parameterised queries |
| `TCPIP/` | Boost.Asio networking, TLS, DNS. Also `DnssecResolver` (validating stub resolver) and `DaneVerifier` (TLSA matching) |
| `Threading/` | Thread pools and task queues |
| `Tracking/` | Publish/subscribe bus between components |
| `Util/` | Utilities, plus the optional listeners: `MetricsServer`, `RestApiServer`, `WebServicesServer` (the last two on `HttpServer`, the shared Boost.Asio HTTP/1.1 listener), `AcmeClient`, `TlsRptStore`, `HttpsClient` and the OTLP exporters (`Otel*`), and the live update (`UpdateChecker`, `SigstoreVerifier`, `UpdateDownloader`, `UpdateInstaller`, `UpdateWindow`, `UpdateApplyToken`). `HttpServer` and the `Update*` files are new in 6.2.28; `Application/` holds their tasks `UpdateCheckTask` and `MetricsHistoryTask` |

### `Server/SMTP/`

The most complex module: reception, relay decisions, the disk-backed delivery queue,
bounces, DKIM signing, and outbound delivery. Outbound transport security lives here
too — `TlsPolicy` implements MTA-STS discovery/caching and DANE TLSA lookups,
`ExternalDelivery` applies the per-host requirements, `TlsRptReporterTask` sends the
daily RFC 8460 reports. SPF is `SPF/`, an implementation of RFC 7208 with a README of
its own; it reaches DNS only through `SPFDnsLookup`, which is what lets the conformance
suite beside it answer 203 cases without a network.

### `Server/IMAP/` and `Server/POP3/`

One command-handler class per IMAP command. Folder hierarchy and message flags are in
the database via `Persistence/`; message bodies are on disk. POP3 is much simpler and
reads the same storage.

Patterns that matter
--------------------

**The COM API is the seam.** All configuration and management goes through
`Server/COM/`. The Control Panel, the regression suite and every third-party script
use it. A new configurable feature normally needs a COM property or method — and
because the test suite drives COM, that is also how the feature becomes testable.

**Persistence is layered.** `BO/` → `Persistence/` → `SQL/`, with `Cache/` in front
for frequently-read objects. A feature that persists new data touches all of the
first three, and `Cache/` if it is on a hot path.

**Four database backends, one abstraction.** MySQL/MariaDB, MS SQL Server,
PostgreSQL and the embedded SQL CE. **Use parameterised queries exclusively** — never
build SQL by string concatenation. Backend-specific DDL goes through the macro
expanders in `SQL/Macros/`, which recognise a deliberately small vocabulary; if you
need something they do not express, that is a design conversation, not a place to
special-case.

**The schema is pinned, one way.** `REQUIRED_DB_VERSION` in
`Common/Application/Constants.h` (6037 on 12 September 2026) must equal
`hm_dbversion.value`; the server refuses to start on an older *or* newer database
(error 5011; 5010 when the version cannot be read). The refused database stays open
for `hMailServer.Database`, which is how DBUpdater brings it up to date; every other
object `hMailServer.Application` hands out - `Settings`, `Domains`, `Rules`, `Links`,
`BackupManager`, `GlobalObjects`, `Diagnostics` - is refused with the 5011 text until
a `Reinitialize` or a restart succeeds, because the configuration they read was never
loaded (`Application::IsInitialized`; before 12 September 2026 a call into `Settings`
on such a server was an access violation inside the COM call). A schema change is four
`DBScripts/Upgrade<from>to<to><backend>.sql` files, a `new UpgradeScript(from, to)` row
in `Tools/DBUpdater/formMain.cs`, a probe statement DBUpdater runs after the step, and
the bump in `Constants.h`; `build/check-schema-versions.ps1` reconciles them and
`build/check-db-scripts.ps1` builds a database from the create script and executes
every probe through the SQL Server Compact provider, with a negative control — a probe
the provider could not run took the service down with it once (#114, 6.2.26). One rule
of the scripts' text: `SQLScriptParser` splits a script into commands on a *blank line*
and sends each command as one query, and MySQL refuses a command holding two
statements, so every statement in a MySQL script is followed by a blank line -
`build/check-sql-separators.py` enforces it on every push, after two `CREATE INDEX`
lines written together left the MySQL create script unable to run from 12 September
2026's tenth webmail wave until the Linux build's database gate said so the same day.

**The database is the settings store; hMailServer.ini is the bootstrap.** Since
schema 6042 the 241 keys the `[Settings]` section used to hold are read from
`hm_inisettings` and written to it, and the section in the file is a CACHE the server
keeps current - because a handful of readers cannot go to the database, and one of
them never can: `hMailServer.exe /Register` reads `ServiceAccountName` before there
is a database to read. A value edited into the file is named in the error log and put
back at the next start; a key in a `[SettingsOverride]` section IS honoured, over
everything, and announced at every start, which is the door for a database that is
unreachable or holds a value the server will not start on. `IniSettingStore` is where
all of that lives and its header is the reference. The file keeps `[Directories]`,
`[Database]`, `[Security]` and `[GUILanguages]`: how the database is reached, how an
administrator is let in without one, and the language list that is read before it is
open.

**A save of a setting publishes it.** Since 22 September 2026 the values those keys
are read into are one immutable block (`IniFileSettings::Values`) swapped atomically,
and a write to the store reloads and publishes it: a key marked `now` in
`build/ini-settings-baseline.txt` is in effect when the save returns, one marked
`reinitialize` (a listener, a TLS context, a work queue's size) waits for a
reinitialise, one marked `restart` for the service to start again, and the server
lists what waits (`GET /api/v1/server/settings/pending`). A subsystem that copies a
value when the servers start subscribes and re-latches it
(`IniFileSettings::Subscribe`). A new getter returns a copy, never a reference into
the block. `docs/SettingsApply.md` has the marks, the inventory and the reasons.

**A new server-wide setting goes in `hm_settings`, not in the file.** That is the
rule from 15 September 2026, written in [CONTRIBUTING.md](CONTRIBUTING.md) and held by
`build/check-ini-coverage.py` against the committed baseline
`build/ini-settings-baseline.txt`: a key that appears in `[Settings]` and is not in
the baseline fails the check. The existing 241 keep the shape they have - a getter in
`IniFileSettings.h`, the default on the member declaration, a `ReadIniSetting*_` call
in `IniFileSettings.cpp` and a control in the Control Panel's `FeatureSettingsView` -
they are simply read from the database now. Per-account and per-domain settings go in
the database as they always did.

**Two of the optional listeners are now Boost.Asio, two are not.** `RestApiServer` and
`WebServicesServer` are hosted on `Common/Util/HttpServer` — an HTTP/1.1 server on
Boost.Asio with its own `io_context` (separate from the mail listeners', so a request
storm cannot starve SMTP accept), a bounded worker pool (4 threads), a connection cap
(64) and absolute per-request (30 s) and per-connection (300 s) deadlines; a handler
runs on a worker and may block. This is new in 6.2.28; before it, both were raw
sockets and `std::thread` like the other two. `MetricsServer` and `ManageSieveServer` remain raw sockets and `std::thread`,
outside the `TCPIP/` stack. All four are started from `Application::StartServers`
only when their port is non-zero. Two things to know, both of which have bitten the
raw-socket pair:

* **A thread of their own has no exception barrier by default.** An exception
  escaping the top of a raw listener thread is `std::terminate` — the whole mail
  server dies. Anything that can throw, including any database call, needs a
  `try`/`catch` inside the thread. (`HttpServer` catches around each handler and
  answers 500, and again at the top of each worker.)
* **They share the mail listeners' TLS configuration.** Each builds a
  `boost::asio::ssl::context` and hands it to `SslContextInitializer::InitServer`
  (MetricsServer.cpp:730, RestApiServer.cpp:618, WebServicesServer.cpp:472,
  ManageSieveServer.cpp:497), so cipher lists, key-exchange groups and protocol floors
  applied there reach all four; there is no second `SSL_CTX` configuration to keep in
  step. The REST listener alone adds a TLS 1.2 floor of its own after `InitServer`,
  which can only tighten what the shared configuration allows.

**Scheduled work** uses `BO/ScheduledTask` and the `Scheduler`. `RunOnce` tasks go
through the maintenance work queue immediately; recurring tasks are polled once a
minute. `Application::CreateScheduledTasks_` registers, in order: `GreyListCleanerTask`
(every `GreylistingExpirationInterval` minutes), `RemoveExpiredRecords` (expired IP
ranges, every minute), `TlsRptReporterTask` and `DmarcRptReporterTask` (hourly; each
sends only when its `*RptFromAddress` is set), `LogRetentionTask` (at start, then every
6 h), `ArchiveRetentionTask` (start + 12 h), `MailboxRetentionTask` (start + 6 h;
message retention, 6.2.25), `MetricsHistoryTask` (start + every minute; 6.2.25),
`UpdateCheckTask` (start + every 15 min; a no-op until `UpdateCheckEnabled=1` — new in 6.2.28),
`IMAPExpungeRetentionTask` (start + 12 h), `MessageStoreConsistencyTask` (start + hourly), `DiskSpaceMonitorTask` (start +
hourly), `WorkQueueHealthTask` (every minute), `BackupScheduleTask` (a one-minute tick
against the wall clock, only when a schedule is set), `DirectorySyncScheduleTask` (only
when `[LDAP] SyncScheduleMinutes` is set) and `AcmeRenewalTask` (start + hourly, only
when `AcmeEnabled=1`). A startup-plus-periodic pair is the shape for a sweep that must
also run on a server that is only ever restarted (source:
`Common/Application/Application.cpp:600-860`).

Constraints learned the hard way
--------------------------------

These are the ones that cost real releases. They are not stylistic.

**Every wait on a pooled thread needs a ceiling.** The server runs work on bounded
pools — a 15-thread async queue that also sends the SMTP `250`, a 10-thread delivery
queue. A dependency that stops responding consumes threads until none are left, and
then the server accepts mail and never replies. That was
GitHub discussion 18, on the project's former GitHub home, and
once found the same shape turned up in DNS, virus scanning, event scripts, external
processes and outbound delivery. Every one of those now has a bound, and a new one
must arrive with one.

**An idle timeout is not a ceiling.** Idle timeouts here re-arm on every byte
received, so a peer that dribbles one byte at a time is never *idle* and holds the
connection — and anything waiting on it — indefinitely. Absolute session ceilings are
a separate mechanism (`ClientSessionCeiling`) for exactly this reason.

**Distinguish "the answer is no" from "there was no answer."** A recipient lookup
that fails because the database did not respond used to be indistinguishable from one
that found nothing, so a database locked by a backup told the sender a valid mailbox
did not exist and the mail was *bounced*. The fix is the thread-local
`DatabaseUnavailableMarker` with its RAII `Scope`, read at both `RCPT TO` decision
points to answer `451` instead of `550`. Two non-obvious constraints if you extend
it: set the marker **after** releasing the pool lock and **before** `ReportError`,
because the error path can run an `OnError` script that re-enters the pool on the
same thread.

**`shared_from_this()` is invalid in a constructor.** It throws `bad_weak_ptr`. Timers
and anything else needing a `shared_ptr` to the object must be armed from `Start()`,
not the constructor.

**A new diagnostic must not fire on the shipped default configuration.** Reporting a
default as an `ErrorManager` error puts a Medium entry in every stock install's ERROR
log — and fails the regression fixtures, which assert a clean log. If it describes a
default, it is `LOG_APPLICATION`.

**Prefer deferral to bouncing, always.** A temporary failure costs a retry. A
permanent one costs someone their mail.

**A collection another thread can change is iterated as a locked copy.**
`Collection<T,P>` (`Common/BO/Collection.h`) keeps its items in one vector behind a
mutex that `GetCount`, `GetItem`, `AddItem`, the deletes and the refresh all take.
`GetVector()` and `GetConstVector()` hand out the vector itself and take nothing, so a
range-for over them - or a copy of them - while another thread refreshes the
collection (a swap, and the old buffer freed), adds to it (a reallocating
`push_back`) or erases from it is a read of freed memory. That was the
heap-corruption abort of 15, 22 and 23 September 2026: the IMAP special-use pass
walking an account's shared folder tree while other sessions created and deleted
folders. An index walk - `GetCount()` and `GetItem(i)` - over such a collection is
memory-safe, since each call locks, but a refresh between two `GetItem` calls
re-indexes the collection under it and an item is skipped or visited twice, so it
counts too. Anything another thread can reach - a cache, a singleton, a
configuration object, a folder tree - is iterated through `GetSnapshot()`, the locked
copy, taken once, or by a member of the collection that holds the mutex. The one
deliberate exception is `DomainAliases::ApplyAliasesOnAddress`, which walks
ObjectCache's domain aliases unlocked: that object is built and refreshed under
ObjectCache's mutex before it is published, and a reload replaces it rather than
changing it, so its readers read a vector nobody writes - on the path every `RCPT TO`
and every logon takes, where a copy per call is a cost for nothing. A collection
loaded by the function, a message's own recipients, and a collection a getter builds
new for every caller may keep `GetVector()`, with `// owned by this call` and the
reason as the comment on the call's line or as the whole of the line before: one
marker to one call, none inside a `#define`, and a marker on a forwarding accessor -
a function that hands `GetVector()` on to its callers - is refused in review, since
it would exempt every caller. `build/check-collection-iteration.py` fails on any call
of either that breaks this; it does not see index walks.

A locked copy protects the vector, not its elements. A session holding a route or a
relay out of a snapshot reads its fields while whoever changes it writes them, and a
string assigned under a reader frees the buffer the reader is copying - the same
use-after-free, one level down. So a change to an element of a shared configuration
collection is written onto a new object, which is saved, and the collection is then
refreshed, swapping in new elements under its lock while readers keep the ones they
hold. REST does that for a route (`PUT /api/v1/routes/{id}`), a DNS black list or
SURBL server, an incoming relay, a remote domain policy, a server message, a blocked
attachment and a group, and COM does it for every item of such a collection (below).
An element's own
children are loaded before it is published - `PersistentRule::ReadObject` loads a
rule's criteria and actions, and `PersistentRoute::ReadObject` a route's addresses,
inside the refresh that builds the collection - and `Route::GetAddresses`' lazy path,
left for a route built in memory, is locked. A reader that takes the mutex waits for
a concurrent refresh to finish its database read, since `DBLoad_` holds the mutex
across it; the relay check and the route lookup now do, as `GetItemByName` always has.

`Stress/SharedCollections.cs` races two of these from outside for thirty seconds
each, in the Windows suite (the Linux project lists its stress sources by name and
does not carry it): ObjectCache's domain and account rules, created and deleted over
REST while three accounts submit through the webmail's send route and delivery
applies them, and SMTPConfiguration's routes, appended to, refreshed and erased from
over REST while the send route resolves an unhosted address past forty standing
routes. Both are survival smoke tests, not seen to catch the old code: against a build
with `HasRejectAction` and `WouldReject` put back to `GetVector()` and `ApplyRules` to
its index walk, the rules race passed three times in three, and the routes race has
not been run against it. The relay lock, the route-address lock and the alias snapshot
have no test and are proven by reading.

Swept on 24 September 2026: every call of `GetVector()` and `GetConstVector()` - on
master there were 87, and two classes of their own that declare an accessor of the
name, and three comments that mention it - and, after the review, the two index walks
over ObjectCache's rules. Index walks elsewhere were not swept; among those seen are
IMAP `SELECT`/`EXAMINE` collecting the keywords of an account's cached messages and
the REST listing and the alert check walking the shared SSL certificates.

| File | Sites | Verdict | Why |
|---|---|---|---|
| `SMTP/RuleApplier.cpp` | `HasRejectAction`, `WouldReject` | shared: `GetSnapshot()` | ObjectCache's global and domain rules, which `RejectedBySubmissionRules` hands them for every message accepted over SMTP and every webmail send; a domain's are refreshed in place by `GetDomainRules` on whichever thread asks first after a save, the global ones by a restore |
| | `ApplyRules` (an index walk) | shared: `GetSnapshot()`, taken once | the account, domain and global rules delivery applies; an account's and a domain's are refreshed in place by `GetAccountRules`/`GetDomainRules` |
| | `SendPolicyNotice_`, `RejectedBySubmissionRules`, `ApplyAction_Copy`, the recipient-list criterion | owned | the recipients of the message the delivery or the submission holds |
| `Common/Util/RestApiServer.cpp` | `HandleListRules_` (an index walk) | shared: `GetSnapshot()` | the same cached rules, listed |
| `Common/Util/ImpersonationCheck.cpp` | `HostedDomains_` | cached, to a snapshot defensively | ObjectCache's domain aliases, replaced on reload and never changed, so nothing writes them today; off the hot path, unlike `ApplyAliasesOnAddress` |
| `Common/BO/IncomingRelays.cpp` | `IsIncomingRelay` (a member, on `vecObjects`) | shared: now locked | SMTPConfiguration's relays, asked by every session and changed in place by COM and REST |
| `Common/BO/Routes.cpp` | `GetItemByNameWithWildcardMatch` (member) | shared: now locked | SMTPConfiguration's routes; `InterfaceRoutes` holds this very object |
| `Common/BO/RouteAddresses.cpp` | `DeleteByAddress` (member) | shared: now locked | a shared route's addresses, erased by COM while sessions ask `GetItemByName` |
| `SMTP/SMTPConnection.cpp` | 12 | owned | the session's own message's recipients (two take the count before and after the parser appends to them) |
| `SMTP/SMTPDeliverer.cpp`, `LocalDelivery.cpp`, `BounceTracker.cpp`, `ComplaintTracker.cpp`, `DistributionListSender.cpp`, `ServerTargetResolver.cpp`, `MirrorMessage.cpp` | 4, 2, 3, 2, 2, 2, 3 | owned | the recipients of the message the delivery holds (`LocalDelivery::Perform` and `BounceTracker::ApplyAtDelivery` erase through the reference on purpose; one `MirrorMessage` site is the copy it made) |
| `SMTP/RecipientParser.cpp` | 5 | owned | three the recipient list the parser is building (two read after a recursion appended to it), two `DistributionList::GetMembers()`, which loads a new collection for every caller |
| `SMTP/SMTPMessageHeaderCreator.cpp` | 1 | owned | the session's message |
| `SMTP/SMTPVacationMessageCreator.cpp`, `DeliveryQueue.cpp` | 1, 1 | owned | a local collection loaded above |
| `COM/InterfaceRecipients.cpp` | 2 | owned | the COM message's recipients; the object is apartment-threaded |
| `Common/AntiSpam/BlockedSenderCache.cpp`, `GreyListingBypass.cpp`, `SenderListCache.cpp`, `WhiteListCache.cpp` | 1 each | owned | copied FROM the local collection just loaded; the cache's own vector is guarded by the cache's `shared_mutex` |
| `Common/AntiSpam/QuarantineStore.cpp`, `SpamTestFilterHook.cpp`, `SpamTestSpamAssassin.cpp`, `DKIM/DKIMSigner.cpp` | 1, 1, 1, 2 | owned | the recipients of the message under test or being signed |
| `Common/AntiSpam/DKIM/DkimRotation.cpp` | 2 | owned | local collections |
| `Common/AntiVirus/VirusScanner.cpp` | 1 | owned | the attachments this scan parsed; `Attachments::GetVector` returns a copy |
| `Common/Application/ACLManager.cpp` | 1 | owned | `GetPermissionsSetOnFolder` loads the permissions new for every question |
| `Common/Application/DomainSamples.cpp`, `SampleDomain.cpp`, `ScheduledMailTask.cpp` | 1 each | owned | the caller's message; a message the loop read; the message the task built |
| `Common/Diagnostics/TestIPRanges.cpp` | 1 | owned | a local collection |
| `Common/Persistence/PersistentDomain.cpp` | 1 | owned | the caller's own `Domain` (a REST local, or a COM object on its apartment), never the cache's |
| `Common/Persistence/PersistentMessage.cpp` | 1 | owned | the message its owner is saving |
| `Common/Sieve/ManageSieveServer.cpp`, `Common/Util/AutoBanFirewall.cpp` (`ListeningPorts`), `RestApiServer.cpp` (`HandleSrv_`), `WebServicesServer.cpp` | 1 each | owned | `Configuration::GetTCPIPPorts()` loads a new collection for every caller - the configuration keeps none |
| `Common/Util/AutoBanFirewall.cpp` (`ActiveBans_`), `SetupChecklist.cpp`, `EmailAllUsers.cpp`, `PasswordValidator.cpp`, `RestApiAppPasswords.cpp` | 1, 2, 1, 1, 1 | owned | local collections |
| `Common/Util/DistributionListMembership.cpp` | 2 | owned | `GetMembers()` loads new |
| `Common/Util/CompromiseDetector.cpp`, `ContentPolicy.cpp`, `DisclaimerAdder.cpp`, `DomainReports.cpp`, `Journaling.cpp`, `MailerDaemonAddressDeterminer.cpp`, `MessageRecall.cpp`, `SignatureAdder.cpp` | 1 each | owned | a message's own recipients |
| `ExternalFetcher/ExternalFetchManager.cpp` | 1 | owned | `fetch_accounts_` is made, refreshed and walked by the manager's thread alone; tasks get accounts, not the list |
| `Common/BO/MessageRecipients.h`, `Attachments.h` | declarations | not calls | classes of their own with no lock, each held by one message or one `MessageData` |
| `Common/BO/Messages.h`, `IMAP/IMAPSpecialUse.cpp`, `IMAP/MessagesContainer.cpp` | comments | not calls | |

The members of the `Collection` subclasses that touch `vecObjects` themselves:
`IMAPFolders` and `Messages` (the shared folder tree and message cache) take the
mutex in every one; `IncomingRelays`, `Routes` and `RouteAddresses` did not and now
do. `ACLPermissions`, `AppPasswords`, `DistributionLists`, `Domains`,
`FetchAccountUIDs`, `GroupMembers`, `RuleActions`, `Rules` (the move and sort-order
members, which COM calls on its own collections, never ObjectCache's) and
`TCPIPPorts` are only ever used by the call or the COM object that loaded them, and
`DomainAliases::ApplyAliasesOnAddress` is the exception above. Nothing outside a
collection reaches into its `vecObjects`.

**A COM change to an item of a shared collection is written onto a copy.** COM hands
out the running server's own collections for the SMTP configuration's routes,
incoming relays and remote domain policies, the anti-spam configuration's DNS black
lists and SURBL servers, the server messages, the blocked attachments, the SSL
certificates, the IMAP groups and a running route's addresses, and each item it
hands out of one is the object every session reads. `COMCollectionItem`
(`COM/COMCollection.h`) keeps the rule for all of them: a collection that is one of
these attaches its items with `AttachSharedParent`, and the first `put_*` on such an
item copies it - from the collection's item with its id as the running server has it
now, so a refresh since the item was handed out is not undone - after which the COM
object holds and writes the copy and its `get_*` read it. `Save()` saves the copy and
reads the collection again (`PublishSaved_`), which swaps in new items under the
collection's lock while a session keeps the one it holds. A `Save()` with nothing put
before it copies first as well, so the object a persister is handed is never the
running item - `PreSaveLimitationsCheck` trims a remote domain policy's key on the
object it saves - and an object handed out before a refresh replaced its item saves
the running item's values rather than writing back the ones it was handed. A new
item - `Add()` - is nobody else's until its `Save()` appends it, and is written directly
until then; after that it is an item of the collection like any other. A route cannot
be copied by its copy constructor (it holds the lock over its lazily loaded
addresses), so `Route::CopyFields` makes the copy for COM and for REST's route update
alike. `InterfaceRoute::get_Addresses` hands out the running route's list whether or
not the route object holds a copy, so an address added or deleted is in effect at once,
as it always was; an address changed in place is copied like any other item. A route's
`Addresses` read before a refresh of the routes replaced the route - the route's own
COM `Save()`, `Routes.Refresh()`, a REST update - is the retired route's list; an
address saved or deleted through it is saved or deleted in the database and then
`InterfaceRouteAddresses::ReachRunningRoute` reads the running route's list again, so
it is in effect as it would have been through the running list. The retired list itself
goes on reading as it was, with its own change; `Addresses` read again is the running
list.

A read of the collection that fails after a successful save leaves the change in the
database and not yet in effect, and COM reports it as error 7390; its `Save()` still
answers success, since the save did. Until the list is next read, a later change to the
same item starts from the running item, which still has the old values, and its save
writes them back over the first change - the error text says so. REST's updates log a
failed re-read in the application log and answer as the save did; they do not report
7390.

What a COM caller sees, which is why this was a row of its own: a `put_*` is not in
effect until `Save()` - the running server, and any other COM object read from the
collection before the save, keep the values they had - and a `put_*` never followed
by a `Save()` changes nothing at all. After the save, an object read from the
collection again has the saved values, and one read before it keeps what it read.
A `Save()` the database refuses leaves the running item as it was; the object keeps
the refused values to be corrected and saved again, except a remote domain policy,
which is read back as the database holds it, as it was before the copy existed. Two
places in the suite relied on the old behaviour and now save: the basic setup's
switching off of `multi.surbl.org` (a `put_*` with no `Save`, which had switched it
off in memory while the database kept it on) and `SMTPClientTests`'s relay
credentials. REST's server message update wrote the running item before saving it,
and its blocked attachment and group updates wrote the saved values back onto the
running item; all three now save a copy and read their collection again.
`build/check-com-shared-copies.py` holds the ten item classes to this: no `put_*`
writes the item, `Save()` copies before it saves and publishes after, and every place
that hands one of them out attaches the shared collection; its `--self-test` proves
each is reported, and against the COM sources before the row it reports all 59 puts
and all 38 hand-outs.

Swept on 24 September 2026, every COM class that wraps an item of a collection:

| Class | Verdict | Why |
|---|---|---|
| `InterfaceRoute`, `InterfaceIncomingRelay`, `InterfaceDNSBlackList`, `InterfaceSURBLServer` | fixed: onto a copy | the row's four; SMTPConfiguration's and the anti-spam configuration's shared collections, read by every session |
| `InterfaceRouteAddress` | fixed | a running route's addresses, asked by every session through `GetItemByName` |
| `InterfaceRemoteDomainPolicy` | fixed | SMTPConfiguration's policies, read by every delivery; its refused save used to put the running record back by reading it in place |
| `InterfaceServerMessage` | fixed | `Configuration`'s messages, copied by every bounce and virus notice; it was attached with no parent at all |
| `InterfaceBlockedAttachment` | fixed | `Configuration`'s list, compared with every attachment's name by the virus scanner |
| `InterfaceSSLCertificate` | fixed | `Configuration`'s certificates, read by the listeners at start, the alert check, the metrics scrape and REST |
| `InterfaceGroup` | fixed | IMAPConfiguration's groups, looked up by name by IMAP sessions (GETACL, SETACL) - and handed out by `InterfaceIMAPFolderPermission::get_Group` as well, with no parent before |
| `InterfaceTCPIPPort`, `InterfaceSecurityRange`, `InterfaceWhiteListAddress`, `InterfaceGreyListingWhiteAddress`, `InterfaceGreyListingBypassEntry`, `InterfaceBlockedSender`, `InterfaceSenderListEntry` | safe | the collection is loaded new for the COM object (`GetTCPIPPorts`, `SecurityRanges::Refresh`, the anti-spam configuration's getters); sessions read caches that copy under a lock of their own |
| `InterfaceDomain`, `InterfaceAccount`, `InterfaceAlias`, `InterfaceDistributionList`, `InterfaceDistributionListRecipient`, `InterfaceDomainAlias`, `InterfaceRule`, `InterfaceRuleCriteria`, `InterfaceRuleAction`, `InterfaceFetchAccount`, `InterfaceAppPassword`, `InterfaceGroupMember`, `InterfaceIMAPFolderPermission` | safe | loaded by COM for itself; the caches hold objects of their own (`const` domains and accounts, ObjectCache's rules and aliases), and group members and permissions are loaded new for every question |
| `InterfaceIMAPFolder`, `InterfaceMessage` | left | an account's or the public folders' cached tree and messages, which the IMAP server itself writes in place (a RENAME sets the folder's name, a STORE its flags) and REST's mailbox rename too: a copy on the COM side would not make the folder safe, and the folder's own fields want a lock of their own, which is another row |

`API/ComWritesOntoCopies.cs` proves the caller's side for each of the ten classes: a
`put_*` read back from its own object, the running item unchanged before `Save()`, the
saved values after it, and an object read before the save keeping what it read - each
of those tests fails against the code before the row, which is traced in the fixture's
summary (the group's reads go through one `Groups` object, since `Settings.Groups`
reads the groups from the database every time and would show the old name either
way). Beside them: a refused route save leaving the running route alone, an address
added through a route object that holds a copy going into the running route's list, an
address added and deleted through a list held across the route's save reaching the
running route, a `Save()` with nothing put not writing back what the object was handed,
and a save whose list cannot be read again (the database fault injector failing the
DNS black lists' read alone) succeeding, reporting HM7390 and not in effect until the
next read. REST's blocked attachment and server message updates are held to it by
`RestApiParityGaps.BlockedAttachmentsRoundTrip` and
`RestApiSettings.ServerMessagesRoundTripThroughCom`, each reading a COM object before the
PUT and finding it unchanged after; the group update is proven by reading only, since
the REST groups route reads the groups again before every request and no object from
outside can hold a group across it.

`Stress/SharedCollectionsOverCom.cs` (REST port 10170, in the Windows suite) flips a
route, a DNS black list, a SURBL server, an incoming relay, a remote domain policy, a
group, a blocked attachment and a server message between two whole states over COM for
thirty seconds a race, pausing after each put, while the send route resolves past the
routes and the REST listings read every field - the listings are the only readers in
the two races without routes - and asserts every listing shows each item whole in one
state or the other, and the process, the crash oracle and the error log clean. Against
the old code, where each put wrote the running item, the pauses make a mix visible for
most of every write of the six multi-field items; the group and the message have one
field each and are survival tests only. Not run against the old code.

Tests
-----

`test/RegressionTests/` is NUnit driving a **real running server** over real sockets,
with live SpamAssassin and ClamAV, SPF, DKIM and DMARC evaluated against the suite's
own DNS zone (`Shared/SuiteDns.cs` — one fake resolver the server is pointed at for
the whole run), and real TLS handshakes. Nothing but DNS is mocked. Two things to
know before adding a test:

* **`RegressionTests.csproj` lists every source file explicitly — there is no glob.**
  A test file nobody adds to it is not merely unrun, it is invisible, and a green
  suite says nothing about it. `build/preflight-tests.ps1` now fails on orphans; that
  check exists because a committed test file went uncompiled for months.
* **A test that deliberately provokes a reported error must clear the ERROR log**, via
  `CustomAsserts.AssertReportedError(...)` which asserts and deletes. `PerformBasicSetup`
  calls `AssertNoReportedError`, so a provoked error left behind fails whichever
  unrelated fixture runs next.

A fix does not ship without a test that **fails against the build before it**. A test
that passes both ways proves nothing — build the pre-fix binary and check.

Building
--------

| Artifact | How |
|---|---|
| Server | `build/build.ps1 -Configuration Release` (VS 2026, toolset v145, x64) |
| Admin tools | `build/build-tools.ps1` (`source/Tools/hMailServer Tools.sln`) |
| Control Panel | `source/Tools/ControlPanel.sln`, published separately into its `publish/` folder |
| Tests | `build/build-tests.ps1` |

**.NET targets.** Everything shipped is `net10.0-windows`, pinned via `global.json`.
The regression suite and its companions are **.NET Framework 4.8.1** and stay there:
4.8.1 has no end-of-support date, and the suite drives the server over COM interop.
Dormant upstream dev tools under `tools/` and the unused performance/stress projects
target 4.8.1 or 3.5 and are built by nothing — do not assume they compile.

The SDK is pinned in `global.json`. There is one build tree and the service holds the
output binary, so **stop the service before linking** — and never build while a
regression run is in progress.

Where to start
--------------

| Change | Start at |
|---|---|
| A new SMTP behaviour | `SMTP/SMTPConnection.cpp` (reception) or `SMTP/ExternalDelivery.cpp` (sending) |
| A new IMAP command | a new `IMAP/IMAPCommand*.{h,cpp}` plus the dispatch table and `IMAPCommandCapability.cpp` |
| A server-wide setting | `hm_settings`, never a new `[Settings]` key: a `PROPERTY_*` name in `Common/Application/Constants.h`, its default row from `Configuration::EnsureSettingDefaults_` or an upgrade script, and a Control Panel editor (CONTRIBUTING.md, "A new setting goes in the database"). The 241 keys already in `Common/Application/IniFileSettings.{h,cpp}` are frozen |
| A per-account or per-domain setting | `Common/BO/`, `Common/Persistence/`, the COM interface, the schema, and the upgrade chain |
| A new anti-spam test | `Common/AntiSpam/` and the score pipeline |
| Something exposed to scripts | `Common/Scripting/` and the COM layer |
| A new REST route or portal page | `Common/Util/RestApiServer.cpp` (dispatch, authorisation through `RoleManager` - `ACLManager` decides folder access only - and the served `openapi.json`); the transport is `Common/Util/HttpServer` — new in 6.2.28 |
| A new periodic sweep | a `ScheduledTask` subclass registered in `Application::CreateScheduledTasks_`, startup-plus-periodic |

If a change spans more than about three of those rows, it is worth discussing in an
issue before writing it.
