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
| `restart` | It is read once per process, before the database is open or where nothing can rebuild it in place, so it waits for the service to be restarted. | 28 (2 `[Settings]` keys and the 26 bootstrap keys) |

`build/check-ini-coverage.py` requires the mark on every line - a line without one
fails with a sentence saying what the three mean - and writes it into
`IniSettingsBaseline.h` as each row's `Applies`, which is where the server reads
it. A bootstrap key (`[Database]`, `[Directories]`, `[Security]`,
`[GUILanguages]`) can only be `restart`: a reload never re-reads those sections.
The check proves its own negative control on every run.

## Where the mark is shown

| Where | What |
|---|---|
| Control Panel | A small line under each field kept in the settings store - **Applies on save**, **After reinitialize** or **After a restart** - on the feature-settings pages, the server settings pages and the Backup page. Read over COM once per key per session. The blanket "hMailServer.ini settings apply after a service restart" footers and the "Applies after a service restart" sentence under thirty-odd fields are gone; the mark says it, from one source. |
| Control Deck | The same three words as a badge beside a settings field kept in the store (the document's `x-applies`), in the Settings view. |
| REST | `applies` on every entry of `GET /api/v1/settings/ini`, on `GET`, `PUT` and `DELETE /api/v1/settings/ini/{name}`, and an `applies` object in the list mapping every key this build reads - stored or not - to its mark. The OpenAPI document gives the ini-backed rows of the settings groups the mark's sentence and `x-applies`. |
| COM | `Settings.IniSettingApplies(name)` answers `now`, `reinitialize` or `restart`, and refuses a name the loader does not read. |

A name the loader does not read - `UseLanguage`, the webmail's branding keys, a
key a script of your own reads - is answered `now` over REST: nothing in the
server holds a copy of it for a save to miss. COM refuses it instead, because a
misspelled key answered as though it applied would be the misspelling looking
like a working setting.

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
| REST | `GET /api/v1/server/settings/pending` - `[{name, stored, running, applies, secret}]`; a secret's two values are `(set)` or empty. `pending`, true or false, on every ini answer. The administrator password's route, as the ini list is. |
| COM | `Settings.PendingIniSettings` - the names, one per line, as `IniSettingNames` lists names. |
| Control Panel | The **pending bar** at the foot of the window: "N saved setting(s) wait for a reinitialize", with **Apply now**, refreshed after every save, every page and every connection. A page's own save says how many of its keys wait. |
| Control Deck | The same sentence above the Settings view's groups, with the pending keys and their two values, and **Apply now**. |

**Apply now** is a reinitialise: every service stops and starts again in place,
which takes every `reinitialize` key. It asks first. A reinitialise ends
maintenance mode (see [Upgrading.md](Upgrading.md#an-upgrade-without-a-window)),
so while the server is draining the question says that the listeners open again
and delivery resumes, and `POST /api/v1/server/reinitialize` answers
`{"reinitializing": true, "drained": true}` when it ended a drain (`false`
otherwise). The Deck's session ends with the REST listener it was using, as it
always has after a reinitialise; the Control Panel's COM session survives it.

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
| `ServiceAccountName`, `ServiceAccountPassword` | Read by `hMailServer.exe /Register`, which runs with no database open; the service logs on as the account it was registered with. A change takes a registration (`/Register`, or the installer) and then a restart. The pending list clears at the next start whether or not the service was re-registered - the **Service account** card on the Hardening page says which account the service actually runs as. |
| every `[Database]`, `[Directories]`, `[Security]` and `[GUILanguages]` key | The bootstrap: read from the file before the database is open, and never by a reload. They are not in the settings store, so they are never listed as pending; the Control Panel marks its `[Database]` rows **After a restart**. |

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

A subscriber runs on the thread that saved, after the new values are published,
and must be quick; it reaches only a server that is running or draining (a
stopped or starting one reads the new value when the servers start), and the two
that reach the scheduler do nothing while a reinitialise is rebuilding it. A latch
that could not be redone safely is marked `reinitialize` instead.

## How it works

**One immutable block, swapped.** The ~240 values the `[Settings]` section is read
into are one `IniFileSettings::Values`. A load builds a *new* block and publishes it
with one atomic store; every getter reads through `Current()`, one atomic load, and
holds the block for as long as its expression lasts. A reader sees one block or the
other, never half of each. That is also why **every String getter returns a copy**:
a reference into a block that may be replaced - and freed, once its last reader lets
go - the moment after the getter returns would be a use-after-free waiting for the
first save. The internal `IniFileSettingsTester` reads a String getter ten thousand
times and more while a thousand blocks are published under it, alternating between
two values of different lengths, and fails on any read that is neither.

**What a save does.** `WriteSettingsValue` and `RemoveSettingsValue` - every door to
the store: COM `SetIniSetting` and `DeleteIniSetting`, the REST ini route, the
settings-group fields kept in the store, the branding and file-link routes,
`hmailserver --set-setting`, the audit trail's revert - store the value and call
`Reload()`:

1. the store is read back: the file reconciled against the table, the table
   winning, as a start does it (`IniSettingStore::Synchronize`) - except that a
   line the service cannot write into the file is not reported again as
   `HM5803`: the save has said so already (`HM5806`), and on the Linux package's
   layout, where the service cannot write the file at all, every save would
   otherwise repeat it for every line stored since the start;
2. the `[SettingsOverride]` values the last start loaded are applied over it again;
3. a new block is built in which every `now` key takes the stored value and every
   `reinitialize` and `restart` key keeps the value it is running on;
4. the block is published, and the subscribers of every key whose value moved are
   called.

A value a load has to correct - an unusable `PreferredHashAlgorithm`, a control
character in `SpamAssassinUser` - is reported at every start and reinitialise as it
always was, and on a save only when that key's value moved.

**What a reload does not re-read.** The bootstrap sections, which are read once
before the database is open. And `[SettingsOverride]`: a reload applies the
overrides the last start loaded, but a change to that section still takes a start,
and still announces itself there (`HM5807`).

**What a reinitialise and a start take.** A reinitialise (`Application::InitInstance`
in `ScopeReinitialize`) takes every `now` and `reinitialize` key from the store; a
`restart` key keeps what the process started with. A start takes everything.

## What is thinner than the row asked

* **Twenty-eight keys still need a reinitialise, and two `[Settings]` keys a
  restart** - the listeners, their TLS contexts, the pool's connections and the
  fetch queue are not rebuilt in place. Apply now makes that one click, not a
  service restart.
* **`[SettingsOverride]` still needs a start.**
* **A value stored by another process** - another node on the same database,
  `hmailserver --set-setting` while the service runs - reaches this server at the
  next save here, a reinitialise or a restart. Nothing watches the table.
* **The pending list covers the `[Settings]` keys only.** A `[Database]` row's
  restart is said by its mark, not listed.
* **The Deck marks the ini-backed rows of its settings groups** (the two tarpit
  values and the forwarding rewrite); it has no page for the other ini keys, which
  are the Control Panel's.
* **Apply now ends the Deck's session**, because the REST listener restarts with
  the other services.
* **A subscriber that has to wait** - an OpenTelemetry exporter restart behind a
  collector that is not answering - holds the save that triggered it until the
  exporter's post times out.
* `hmctl` has no pending verb; `GET /api/v1/server/settings/pending` is one request.

## Tests

`RegressionTests/Infrastructure/SettingsApplyOnSave.cs` (REST 9876, web services
9877, ManageSieve 9878-9879), each with its negative control: a `now` key applying
with no reinitialise (`MaxSubmissionsPerIPPerMinute`, the third MAIL FROM a 421 at
once, back to unlimited at once); a subscribed key re-latched (`CalDavRedirectUrl`
after the redirect cache was primed with the old target); a `reinitialize` key
stored and not applied (`ManageSieveServerPort`: the old port answers, the new does
not, pending with both values, empty when put back, taken by the reinitialise);
the reinitialise's `drained` answer; every listed key's mark and the `applies` map
held to the baseline file; COM's `IniSettingApplies` and `PendingIniSettings`.
`IniFileSettingsTester` in the internal suite (`MainOperations.RunTestSuite`).
`check-ini-coverage.py`'s own negative control. The Deck harness checks the badge
and the banner; `ControlPanel.Tests` checks the words for each mark.
