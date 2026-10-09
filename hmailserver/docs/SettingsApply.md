# When a saved setting takes effect

Until 22 September 2026 every key of the settings store that was the `[Settings]`
section of `hMailServer.ini` was latched into a typed member when the service
started. A change was stored at once and applied at the next start, which is what
the Control Panel meant by "applies after a service restart" on page after page,
and what an administrator reads as "this product cannot be configured while it is
running".

It can now. **A save publishes**: the store takes the value, the server rebuilds
its settings from the store, and every key whose readers can take a new value is
in effect when the save returns. The keys that genuinely cannot - a listener's
port under a live socket, a TLS context the listeners were built with, the
service account - say so, wait for what applies them, and are listed as
**pending** until it has happened.

## The three marks

Every key carries one, in `build/ini-settings-baseline.txt` (the fifth field of
each line), decided key by key from the code that reads it:

| Mark | What it means | Keys |
|---|---|---|
| `now` | A save applies it. Every reader asks for the value when it uses it, or a subscriber takes the new one when the store changes. | 211 |
| `reinitialize` | It is bound into something the servers build when they start - a listener, a TLS context, a work queue's size, the database pool's connections - so it waits for a reinitialise: the Control Panel's **Apply now**, the Deck's **Apply now**, `POST /api/v1/server/reinitialize`, `Application.Reinitialize` over COM. | 28 |
| `restart` | It is read once per process, before the database is open or where nothing can rebuild it in place, so it waits for the service to be restarted. | 31 (2 `[Settings]` keys and the 29 bootstrap keys: 20 `[Database]`, 5 `[Directories]`, 3 `[Security]`, 1 `[GUILanguages]`) |

`build/check-ini-coverage.py` requires the mark on every line - a line without one
fails with a sentence saying what the three mean - and writes it into
`IniSettingsBaseline.h` as each row's `Applies`, which is where the server reads
it. A bootstrap key (`[Database]`, `[Directories]`, `[Security]`,
`[GUILanguages]`) can only be `restart`. None of them is in the settings store, so
no save reaches one and none is ever pending; a save's reload never re-reads those
sections. A reinitialise does re-read them - it reconnects the database from
`[Database]` - so some would apply at one, but `restart` is the mark that is true
of every one of them, and it is the one they carry. The check proves its own
negative control on every run.

## Where the mark is shown

| Where | What |
|---|---|
| Control Panel | A small line under each field kept in the settings store - **Applies on save**, **After reinitialize** or **After a restart** - on the feature-settings pages, the server settings pages and the Backup page. Every key's mark comes in ONE COM call when such a page is opened (`Settings.IniSettingAppliesAll`); a call that fails is not remembered, and the next page asks again. The blanket "hMailServer.ini settings apply after a service restart" footers and the "Applies after a service restart" sentence under thirty-odd fields are gone; the mark says it, from one source. |
| Control Deck | The same three words as a badge beside a settings field kept in the store (the document's `x-applies`), in the Settings view. |
| REST | `applies` on `GET`, `PUT` and `DELETE /api/v1/settings/ini/{name}`, and in the list `GET /api/v1/settings/ini` an `applies` object mapping every key this build reads - stored or not - to its mark. The list's entries stay `{name, value, secret}`, the shape the export clients read, and every stored name is in them, whether this build reads it or not. The OpenAPI document gives the ini-backed rows of the settings groups the mark's sentence and `x-applies`. |
| COM | `Settings.IniSettingApplies(name)` answers `now`, `reinitialize` or `restart`, and refuses a name the loader does not read; `Settings.IniSettingAppliesAll` answers every key's mark as one JSON object, the REST list's `applies` map. |

A name the loader does not read - `UseLanguage`, the webmail's branding keys, a
key a script of your own reads, a misspelling - has **no mark on either surface**.
COM's `IniSettingApplies` refuses it, the single-key REST answers carry `applies:
null` with the same sentence as `applies_refused`, and the list's `applies` map does
not name it: a misspelled key answered `now` would be the misspelling looking like a
working setting. The REST route and COM still store, read and list such a name - on
purpose, for a setting a script of your own reads - and COM takes the name as
spelled, as `GetIniSetting` does, so a name with a space round it is no key on
either.

## The pending list

A key marked `reinitialize` or `restart` whose stored value is not the value the
server is running on is **pending**. The list is **recomputed** from the store and
the running values at every request, never remembered, so:

* a value put back before anything applied it is not pending;
* whatever applies a key clears it by making the two agree - a reinitialise for a
  `reinitialize` key, the next start for a `restart` key;
* another administrator's save shows up at the next refresh.

| Where | What |
|---|---|
| REST | `GET /api/v1/server/settings/pending` - `[{name, stored, running, applies, secret}]`; a secret's two values are `(set)` or empty. `pending`, true or false, on every single-key ini answer, and a `pending` array of names in the list. The administrator password's route, as the ini list is. |
| COM | `Settings.PendingIniSettings` - the names, one per line, as `IniSettingNames` lists names. |
| Control Panel | The **pending bar** at the foot of the window: "2 saved settings wait for a reinitialise - Apply now takes them. 1 waits for a service restart.", with **Apply now** for the first and **Restart the service…** for the second - the restart the feature settings pages offer after a save, which asks first and is offered only while the Control Panel is signed in to the server on its own machine - and the settings themselves, by caption and page, in the notice's tool tip and help text. It is refreshed once after every save - however many fields it wrote, reusing the list the page read after its save - and afresh on every page and every connection. A page's own save says how many of its keys wait, and its pill says *Reinitialise to apply* or *Restart to apply* until the next edit. A page's save writes only the fields whose value changed: every write publishes, and a page of a hundred fields saved with one changed was a hundred reloads of the store. |
| Control Deck | A sentence of the same counts above the Settings view's groups, with the pending keys and their two values, and **Apply now**. |

**Apply now** is a reinitialise: every service stops and starts again in place,
which takes every `reinitialize` key - and not a `restart` key, so when one of
those waits too, what Apply now says afterwards is how many still wait for a
service restart rather than that the saved settings are in effect. It asks first.
A reinitialise ends maintenance mode (see
[Upgrading.md](Upgrading.md#an-upgrade-without-a-window)), so while the server is
draining the question says that the listeners open again and delivery resumes, and
`POST /api/v1/server/reinitialize` answers `{"reinitializing": true, "drained":
true}` when it ended a drain (`false` otherwise). The Deck's session ends with the
REST listener it was using, as it always has after a reinitialise; the Control
Panel's COM session survives it.

**Against a server from before this** - one without `IniSettingAppliesAll` and
`PendingIniSettings`, whose `[Settings]` keys all apply at its next start - the
Control Panel says what it said then: every field kept in the store is marked
**After a restart**, a save says the service must be restarted, and no pending
bar is shown. It never says a save took effect. It tells such a server by the
member being unknown (`DISP_E_UNKNOWNNAME`), not by any failure: a call that fails
for a moment is answered as "cannot say" and is asked again.

## The inventory

The `now` keys are the 211 not listed here. Most of them are read by their
readers at the moment of use - limits, timeouts, rate limits, feature switches,
the drain seconds, log options, anti-spam and anti-virus knobs, DNS options, the
OAuth2 configuration (built per validation), the update check's hours and window,
the retention days the sweeps read on every pass. A cache of *data* - a DNS
answer, a validated DNSSEC zone, a JWKS document (kept per URL) - is not a
setting; a changed `DNSServer` is used for the next lookup, and an answer
already cached stays until its TTL runs out.

### Keys that wait for a reinitialize

| Key | Why |
|---|---|
| `AcmeCertificateDirectory` | The REST and web-services listeners fall back to the certificate in it when they start; the renewal reads it too, and one value until the reinitialise keeps the two agreeing. |
| `AcmeEnabled` | The renewal task is registered only when the servers start with it on. |
| `DatabaseStatementTimeout` | Set on each pooled connection when it connects (`DALConnection::ApplyDefaultTimeout`, a statement on MySQL and PostgreSQL); a connection another thread is using cannot be given a new one safely. |
| `ManageSieveServerBindAddress`, `ManageSieveServerPort` | The ManageSieve listener binds when it starts. |
| `MaxNumberOfExternalFetchThreads` | The external-fetch work queue's size, set when the fetch manager is built. |
| `MetricsServerAuthPassword`, `MetricsServerAuthToken`, `MetricsServerAuthUsername` | Latched by `MetricsServer::Start` with the listener, and written to it only once its socket listens. |
| `MetricsServerBindAddress`, `MetricsServerPort` | The metrics listener binds when it starts. |
| `MetricsServerCertificateFile`, `MetricsServerPrivateKeyFile` | The metrics listener's TLS context is built when it starts. |
| `RestApiBindAddress`, `RestApiPort` | The REST listener binds when it starts. |
| `RestApiCertificateFile`, `RestApiPrivateKeyFile` | The REST listener's TLS context is built when it starts. |
| `TlsCipherSuites13`, `TlsKeyExchangeGroups`, `TlsSessionCacheSize`, `TlsSessionTicketsEnabled`, `TlsSessionTimeoutSeconds`, `TlsTicketKeyRotationSeconds` | Written into the SSL contexts `SslContextInitializer` builds for the SMTP, IMAP, POP3 and ManageSieve listeners and the outbound client, all built when the servers start. |
| `WebServicesBindAddress`, `WebServicesHttpPort`, `WebServicesHttpsPort` | The web-services listener binds when it starts. |
| `WebServicesCertificateFile`, `WebServicesPrivateKeyFile` | Its TLS context is built when it starts. |

### Keys that wait for a restart

| Key | Why |
|---|---|
| `ServiceAccountName`, `ServiceAccountPassword` | Retired: no longer read by anything - `hMailServer.exe /Register` takes the account from its command line alone ([ServiceAccount.md](ServiceAccount.md)). Still marked restart, so a value stored is shown as pending until the next start, which is all it ever does now; the **Windows service account** card on the Hardening page says which account the service actually runs as, and changes it. |
| every `[Database]`, `[Directories]`, `[Security]` and `[GUILanguages]` key | The bootstrap: read from the file before the database is open - at a start, at a reinitialise (which reconnects the database from `[Database]`), and when COM's `Settings.Directories` asks - and never by a save's reload. They are not in the settings store, so they are never listed as pending; the Control Panel marks its `[Database]` rows **After a restart**, and a page's save counts a changed one as waiting. |

### Keys marked now because a subscriber re-latches them

A subsystem that copied a value into something of its own when the servers
started, and can take a new one in place, subscribes
(`IniFileSettings::Subscribe`) and takes it when a save changes it
(`Application::SubscribeToSettings_`):

| Keys | What the subscriber does |
|---|---|
| `GreylistingRecordExpirationInterval` | Moves the greylist cleaner's period, from now (`Scheduler::SetTaskInterval`; the scheduler now writes a task's next run time only under its own lock). |
| `ScheduledBackupTime`, `ScheduledBackupIntervalMinutes` | Registers the backup schedule's task when a schedule is saved on a server that started without one. A registered task reads the time, the interval, the keep count and the age on every tick, so it needs nothing else. |
| `OtelEndpoint`, `OtelServiceName`, `OtelMetricsEndpoint`, `OtelMetricsInterval`, `OtelLogsEndpoint` | Restarts the exporter in place, under a lock of its own that a reinitialise's restart shares. |
| `CalDavRedirectUrl`, `CardDavRedirectUrl` | Drops the web-services listener's one-minute cache of the redirect targets. |
| `SimulateDatabaseFailureFor` | The injector reads the value per statement already; the subscriber says `HM6119` the moment it is set, as a start does. |
| `MaxSubmissionsPerIPPerMinute`, `MaxOutboundPerDestinationPerMinute` | Read per transaction already; the subscriber starts each limit's one-minute window again (`RateLimiter::ClearBuckets`), so a saved limit counts from when it was saved, as a restart always made it. Kept, the events counted under the old limit refused the first transaction a new limit saw - found by `SMTP.RateShaping` run within a minute of a test that had used the limit. |

A subscriber runs on the thread that saved, after the new values are published
**and the reload lock released**, so an exporter restart waiting on a collector
holds up the save that triggered it and no other save or load. Subscribers are
called one at a time, and each reads the value it takes when it runs, rather than
being handed one: when two saves of one key are told out of order, the call that
runs last runs after the last publish and takes the latest value. A subscriber
reaches only a server that is running or draining (a stopped or starting one reads
the new value when the servers start); the two that reach the scheduler do nothing
while a reinitialise is rebuilding it, and copy the scheduler and their task under
a lock of their own that a service stop's reset takes too. A period changed while
the greylist cleaner is running is not moved on again when that run ends, so the
first run after the save is one period away, not two. A latch that could not be
redone safely is marked `reinitialize` instead.

## How it works

**One immutable block, swapped.** The ~240 values the `[Settings]` section is read
into are one `IniFileSettings::Values`. A load builds a *new* block and publishes it
with one atomic store and advances a generation counter; every getter reads through
`Current()` and holds the block for as long as its expression lasts. A reader sees
one block or the other, never half of each. `Current()` keeps a copy per thread of
the block it last loaded and goes to the shared pointer only when the generation
has moved: the server is built as C++14 on Windows, where `std::atomic_load` of a
`shared_ptr` takes one spin lock every such atomic in the process shares, and every
getter on every thread - four per log line - would otherwise take it. The cost is
one block kept alive per idle thread until it next reads a setting. That is also why **every String getter returns a copy**:
a reference into a block that may be replaced - and freed, once its last reader lets
go - the moment after the getter returns would be a use-after-free waiting for the
first save. The internal `IniFileSettingsTester` reads a String getter ten thousand
times and more while a thousand blocks are published under it, alternating between
two values of different lengths, and fails on any read that is neither.

**What a save does.** `WriteSettingsValue` and `RemoveSettingsValue` - every door to
the store: COM `SetIniSetting` and `DeleteIniSetting`, the REST ini route, the
settings-group fields kept in the store, the branding and file-link routes,
`hmailserver --set-setting`, the audit trail's revert - take the reload lock, store
the value, and under the same hold:

1. read the store back - **the rows alone, read-only**
   (`IniSettingStore::ReadStoredValues`): nothing is written into the file or the
   table, no line the table has never seen is adopted, and nothing is reported;
2. apply over them again the `[SettingsOverride]` values the last start or
   reinitialise loaded, without re-reading the section;
3. build a new block in which every `now` key takes the stored value and every
   `reinitialize` and `restart` key keeps the value it is running on;
4. publish it, release the lock, and call the subscribers of every key whose value
   moved.

The store write and the reload are under **one** hold because a save's reload used
to run the whole file-and-table reconcile a start runs, while another thread's save
was half way through its own store write: a reconcile that read a row before a
DELETE dropped it and the file after the line had gone wrote the line back, and the
DELETE's own reload then adopted it as a new setting - the DELETE answered 200 and
the setting was stored and in effect; one that read a row before a PUT and the file
after put the old value back into both, under a PUT that had answered 200. The
reconcile - a hand edit of the file named as `HM5804` and put back, a line the
service cannot write named as `HM5803`, a line the table has never seen adopted -
is now a start's and a reinitialise's only, as it was before saves published.

A value a load has to correct - an unusable `PreferredHashAlgorithm`, a control
character in `SpamAssassinUser` - is reported at every start and reinitialise as it
always was, and on a save only when that key's value moved.

**What a save's reload does not re-read.** The bootstrap sections. And
`[SettingsOverride]`: a save re-applies the overrides the last start or
reinitialise loaded and does not re-read the section; a reinitialise (**Apply
now**) or a start re-reads it and announces it (`HM5807`). An override of a key
marked `restart` still waits for a start, as the key itself does.

**What a reinitialise and a start take.** A reinitialise (`Application::InitInstance`
in `ScopeReinitialize`) reconciles the file against the table, re-reads
`[SettingsOverride]`, and takes every `now` and `reinitialize` key from the store and
the overrides; a `restart` key keeps what the process started with. A start takes
everything.

**A value with levels applies on save at every level.** From schema 6088 some
settings may be set for a domain and an account as well as for the server
([SettingLevels.md](SettingLevels.md)): the account's own value, else its domain's,
else the server's. The resolver that answers that is asked when the value is
used - at `MAIL FROM`, when a digest pass runs, when a session is checked - and
never latched, so a domain's or an account's own value is in effect for the next
message, pass or request the moment its save returns, whichever door saved it,
with no reinitialise; the server level keeps the mark this page gives it
(`QuotaWarningPercent`, the one key of the store with levels, is `now`). The one
cache in the way, the overlay's rows, is dropped by every write through the
interfaces and by emptying the caches; a row written behind the server's back is
seen within sixty seconds, or at once after the caches are emptied.

## What is thinner than the row asked

* **Twenty-eight keys still need a reinitialise, and two `[Settings]` keys a
  restart** - the listeners, their TLS contexts, the pool's connections and the
  fetch queue are not rebuilt in place. Apply now makes that one click, not a
  service restart.
* **A change to `[SettingsOverride]` is not taken by a save.** A reinitialise
  (Apply now) or a start takes it; an override of a `restart` key waits for a
  start.
* **A value stored by another process** - another node on the same database,
  `hmailserver --set-setting` or `--apply-environment` while the service runs -
  reaches this server at the next save here, a reinitialise or a restart. Nothing
  watches the table. (A container runs `--apply-environment` before its server
  starts, so there every value is in effect from the start -
  [EnvironmentConfiguration.md](EnvironmentConfiguration.md).)
* **The pending list covers the `[Settings]` keys only.** A `[Database]` row's
  restart is said by its mark, not listed.
* **The Deck marks the ini-backed rows of its settings groups** (the two tarpit
  values and the forwarding rewrite); it has no page for the other ini keys, which
  are the Control Panel's.
* **Apply now ends the Deck's session**, because the REST listener restarts with
  the other services.
* **A subscriber that has to wait** - an OpenTelemetry exporter restart behind a
  collector that is not answering - holds the answer to the save that triggered it
  until the exporter's post times out. No other save or load waits for it.
* **The branding and file-link routes** reload once per field they write (four at
  most); the Control Panel's pages write only what changed.
* **The bootstrap strings are rewritten in place** by a start, a reinitialise and
  COM's `Settings.Directories`, under the reload lock their readers do not take - a
  race older than the block, which it does not close.
* `hmctl` has no pending verb; `GET /api/v1/server/settings/pending` is one request.

## Tests

`RegressionTests/Infrastructure/SettingsApplyOnSave.cs` (REST 9876, web services
9877, ManageSieve 9878-9879), each with its negative control where one can be
written:

* a `now` key applying with no reinitialise (`MaxSubmissionsPerIPPerMinute`, the
  third MAIL FROM a 421 at once) and a DELETE returning it to its default at once,
  answering `applies` now and `pending` false;
* a saved rate limit counting from when it was saved: two MAIL FROMs at 2 and a
  refusal, then three at a newly saved 3 before the fourth is refused;
* a subscribed key re-latched (`CalDavRedirectUrl` after the redirect cache was
  primed with the old target);
* a `reinitialize` key stored and not applied (`ManageSieveServerPort`: the old port
  answers, the new does not, pending with both values, empty when put back, taken by
  the reinitialise);
* a `restart` key (`ServiceAccountName`) still pending, its running value unmoved,
  after a reinitialise;
* a secret (`MetricsServerAuthPassword`) named in the pending list as `(set)` and
  its text nowhere in the answer;
* the fault injector's `HM6119` from the save's own subscriber;
* two clients each making twenty PUT/DELETE pairs on a key of their own at once,
  each key ending as its own client's last acknowledged request left it;
* a name the loader does not read: refused over COM, `applies` null with the same
  sentence over REST;
* the reinitialise's `drained` answer; the list's entries kept as `{name, value,
  secret}` with a script's own key among them, its `pending` array against the
  pending route and its `applies` map against the baseline file (a check that the
  route answers the header, not a proof of any mark); COM's `IniSettingApplies`,
  `IniSettingAppliesAll` and `PendingIniSettings`.

`IniFileSettingsTester` in the internal suite (`MainOperations.RunTestSuite`): the
swap under load, a save of a `now` key in effect and told to its subscriber once, a
removal publishing the default with no reload of the tester's own, a `reinitialize`
key held and pending, and eight threads each alternating two hundred writes and
deletes of a key of their own while a ninth reloads - every key's row and running
value what its own thread did last. It logs how long the swap and the storm took.
`Roles.cs` holds the pending route to the administrator password. `IniSettingsMirror`
holds a save of another key to leaving an override in force without re-reading the
section, and a reinitialise to re-reading it. `check-ini-coverage.py`'s own
negative control. The Deck harness checks the badge, the banner, the note that
names the three marks, and Apply now's yes path with a `restart` key still waiting;
`ControlPanel.Tests` checks the words for each mark, the one read of the marks and
of the pending list, a failed read not remembered, and an older server told apart
and never told its settings applied.

**On Linux** the fixture is linked into `LinuxRegressionTests`, with the three COM
members answered over REST; `IniFileSettingsTester` does not run there, because the
Linux suite's `RunTestSuite` is skipped, so the Linux build's
`std::atomic<std::shared_ptr>` path and the thread-local build context are covered
only by the fixture. They compile: every file this change touches was built with
clang (C++20) in an Ubuntu 26.04 container on 23 September 2026, and with the two
errors that build found elsewhere fixed - `CardDavServer.cpp` and `Compression.cpp` -
the server links.
