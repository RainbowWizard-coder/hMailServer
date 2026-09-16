# What this server has changed from stock

A server holds 391 settings. An administrator has usually changed twenty. Until
now there was no way to be told which twenty — so the first question anybody asks
when a server behaves oddly, and the first thing worth putting in a support
message, had no answer short of reading the whole configuration and remembering
what each value is supposed to be.

This is that answer: **every setting whose value is not the one this version of
the server ships with, with the shipped value beside it.** One list, one route,
one page, one line of `hmctl`.

```text
STORE     NAME                       VALUE           DEFAULT
ini       LogLevel                   5               9
ini       MaxLogLineLen              2000            500
ini       SMTPDMaxTimeout            600             1800
ini       OAuth2HmacSecret           (set)
ini       DnsServer                  10.0.0.1 (forced from [SettingsOverride])
database  smtpnoofretries            6               4
database  maxsmtpconnections         200             0

7 of 391 settings are not what this version ships with.
```

## Where to get it

| Where | What to ask |
|---|---|
| Control Deck | **Changed from stock**, in the navigation beside Settings |
| REST | `GET /api/v1/settings/changed` |
| Command line | `hmctl config changed` |
| PowerShell | `Get-HmChangedSetting` |
| COM | `Settings.GetChangedFromDefault` |

The REST route and the COM call answer **the same document, byte for byte** —
they are one function, `SettingsDiff::Json`, and a regression test compares the
two whole answers rather than a field of each. That is deliberate: the defect
this feature exists to remove is two interfaces describing the same server
differently, and it would be a poor answer to it if the Deck and the Control
Panel could disagree.

## What the document says

```json
{
  "changed": 7,
  "considered": 391,
  "settings": [
    { "store": "ini", "name": "LogLevel", "value": "5", "default": "9",
      "secret": false, "known": true, "overridden": false }
  ]
}
```

* **`changed`** is how many are listed, **`considered`** how many settings the two
  stores hold altogether — so that "7" reads as "7 of 391" rather than as a
  number with no scale.
* **`store`** is `ini` or `database`, which are the server's two settings stores.
  `ini` is `hm_inisettings`, the keys that were the `[Settings]` section of
  `hMailServer.ini` before schema 6042 made the database their store; `database`
  is `hm_settings`, the store the COM `Settings` object and the Control Panel's
  classic pages have always used.
* **`value`** is the value **stored** for the setting — or forced for it, see
  `overridden` — and **`default`** what this version ships with. For all but a
  handful the stored value is also the one in force; the exceptions are the few
  the loader clamps, below.
* **`secret`**, **`known`** and **`overridden`** are below.

## What "default" means

**The value the server behaves as though the setting has when nothing has been
stored for it.** Not "what a fresh installation's files contain", which is a
different thing and a worse one.

The two part company for the keys the installer writes. `hMailServer.ini` ships
with lines the packaging put there — `RestApiBindAddress=127.0.0.1`,
`WindowsEventLogLevel=2`, `MinimumFreeDiskSpaceMB=100` and a dozen more — and
every one of them is adopted into the settings store at the first start. If a
line holding the shipped value counted as a change, a server nobody had touched
would list a dozen settings and the list would be worthless. So it does not: a
line the installer wrote with the value the server would have used anyway is not
a change to anything.

A line the installer wrote with a value that is **not** the shipped one *is*
listed, and should be — that installation is different from a stock one, whoever
made it so, and somebody comparing two servers needs to know.

Where each default comes from, and what stops it drifting:

| Store | The shipped value is | Held to it by |
|---|---|---|
| `ini` | the third argument of the read in `IniFileSettings.cpp` — it has always been there, and until now nothing but the reader saw it | `build/check-ini-coverage.py`, which extracts it into `build/ini-settings-baseline.txt` and generates `Common/Application/IniSettingsBaseline.h` from that, and fails on every pull request when a default is edited in the source and not regenerated |
| `database` | what this version's create script puts in a new database (`insert into hm_settings ...`), plus `Configuration::EnsureSettingDefaults_` for a property added after the create script | `build/generate-settings-defaults.py`, which writes `Common/Application/SettingsDefaults.h` from both and whose `--check` runs in CI |

Neither header is written by hand, and neither check is advisory. **A list of what
has been changed that compares against last month's defaults is worse than no
list: it is confidently wrong.**

One consequence worth stating: `database` defaults are what a database *created
by this version* holds. An installation upgraded from an old schema got its
values from the upgrade scripts of the time. Usually those are the same values;
where they are not — `maxmessagesize`, `usecache`, `protocolimap`,
`smtpallowincorrectlineendings` and half a dozen more were seeded differently by
an old upgrade script — the older installation genuinely is different from a new
one, and saying so is the point.

**A row nothing reads is not reported at all.** The same upgrade scripts inserted
a dozen rows for features that have since gone — `useordb`, `usespamhaus`,
`tarpitcount`, `filters`, `imapport` and the rest — and `relaymode` is still
seeded by the create scripts with nothing left to read it. They differ from a
database made today and not one of them can explain anything a server is doing,
so they are left out: a list that carried a dozen fossils on every upgraded
installation is a list nobody finishes reading. What decides is
`SettingsDefaults::PropertyNames`, generated from the server’s own constants, so
a property that is deleted from the code stops being reported at the next build
rather than when somebody remembers.

## A secret is named and never shown

A setting whose name says its value is a secret — a password, a shared secret, a
token, the pepper — is listed **by name**, with `(set)` for a value it has and
empty for one it has not, and `secret` true.

The rule is `IniSettingStore::IsSecretName`, the same one `GET
/api/v1/settings/ini` masks its list with; there is deliberately not a second
one, because two rules for the same question are two rules that will one day
disagree. A key *file* is a path and not a secret, and is shown:
`RestApiPrivateKeyFile` says where the key is, not what it is.

This matters because of what the list is for. It is what somebody pastes into a
support message or an issue. That it has been changed is the fact worth having;
the value is a separate question, and `GET /api/v1/settings/ini/{name}`, `hmctl
ini get <name>` and COM's `Settings.GetIniSetting` are where it is asked.

## A setting the server ships no default for

`known` is false for a name the server holds a value for and ships no default
for. It is still listed — stock has no such value at all, so it is a difference
from stock, and leaving it out would hide it.

Two shapes reach it:

* a key an administrator stored for a script of their own. The REST API and COM
  store any name on purpose (`hmailserver --set-setting` is the one that refuses
  a name the server does not read, because storing it there would change
  nothing);
* the handful the server reads by name where they are used rather than through
  the settings loader — the webmail's branding and file-link keys, with their
  `.<domain>` twins — which have REST pages of their own.

`hmctl` prints `(none: this server does not read it)` in the default column for
these, and the Deck says the same in words, rather than leaving the column empty,
which would read as "the default is empty".

## A value the file is forcing

`overridden` is true where the `[SettingsOverride]` section of `hMailServer.ini`
is forcing the value over whatever is stored. That section is the deliberate door
for a database that is unreachable, or that holds a value the server will not
start on; the server announces it by name in the error log at every start
(HM5807) precisely because **no page and no configuration backup shows it**.

A key forced that way is listed **even when neither store holds a row or a line
for it** — which is the case the whole feature exists for, since a server can be
running on a setting nothing has ever stored. `hmctl` prints
`(forced from [SettingsOverride])` beside the value and the Deck says the same in
words.

A key the section forces to the *shipped* value is not listed, for the same
reason a stored shipped value is not: the server is behaving as stock for it. The
error log still names it at every start.

## Which values these are

This server process's. A setting stored since it started is listed as stored,
though **most settings take effect only when the server next starts** — that is
true of the whole `[Settings]` half and is not a property of this list. A setting
stored by another process (`hmailserver --set-setting`, another node on the same
database) is listed as this server last loaded it, until it restarts or is
reinitialised.

It is the value **stored**, not the value the server settled on where the two
differ. The loader clamps a handful and says so: `PasswordHashIterations` outside
the range this server will derive a hash with is reported as HM5561 and the
built-in work factor used, and a `PreferredHashAlgorithm` that is not a
password-hashing scheme is reported as HM5528 (or HM5607, for Argon2id on a build
whose OpenSSL has no Argon2id) and another used. What is listed there is what
somebody set — which is what this list is for, since a value that did not take is
exactly the kind of thing being looked for — and the error log says what the
server did with it.

So this answers *what this server is running on*, which is the question being
asked when something behaves oddly. It is not a report on the database.

## Who may see it

The **administrator password**, and nothing else. An API key of any scope is
refused with a 401, exactly as it is for `GET /api/v1/settings/ini`, and for the
same reason: the settings include the server's secrets and the commands it runs,
and a value that is not a secret *by name* can still be one that matters —
`AutoBanCommand` is a command this server runs as the service account, and it is
precisely the kind of setting that appears in this list.

**An account holding an administrative role is refused too**, with a 403 and the
reason, however many permissions the role holds — an `auditor` reads a great deal
and does not read this. That is the same line `GET /api/v1/settings/ini` draws,
and it is drawn in `RestApiServer::Authorize_` before any permission is
consulted, so the `view_configuration` the route is classified with in
`PermissionFor_` never decides anything here. (Every route must be classified,
which is what `build/check-role-choke-point.py` holds; for the routes the
administrator password keeps to itself, the classification is about that password
and not about what a role may read.)

The Control Deck leaves the **Changed from stock** page out for every credential
but the administrator password — its button carries `data-admin` rather than
naming a permission — because a page offered to a role and refused on the first
click is worse than a page that is not offered.

It reads and changes nothing: there is no write anywhere in this feature. A
setting is changed where it is set — the Settings page, and the pages of the
thing it belongs to — and a second place to change one would be a second set of
rules about who may.

## What it does not do

* **It does not show a setting that has never been changed.** That is the whole
  design; a list of all 391 exists already, as `GET /api/v1/settings/ini` and the
  settings pages.
* **It does not change anything.** There is no write in it anywhere.
* **It does not compare two servers.** Two `hmctl config changed --json` answers
  and `diff` do that today. `hmctl config export` and `hmctl config diff` are the
  document-shaped tools for holding a server to a file.
* **It does not say when or by whom a setting was changed.** The audit trail does
  (`GET /api/v1/audit?object_type=setting`, and the Deck's Audit trail page), for
  every change made through an interface since the trail was switched on.

## See also

* [CommandLine.md](CommandLine.md) — `hmctl config changed` and
  `Get-HmChangedSetting`, and the configuration as one file
* [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) — who changed a setting, and
  when
* `build/ini-settings-baseline.txt` — every `[Settings]` key the server reads,
  with its kind and its shipped default
