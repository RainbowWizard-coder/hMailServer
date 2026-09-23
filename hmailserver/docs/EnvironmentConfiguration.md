# Every setting from the environment

Until 23 September 2026 the container image took sixteen `HM_*` variables and
wrote what they said into `hMailServer.ini` at every start: the database
connection (six, the password also as `_FILE`), the administrator password (also
as `_FILE`), four for the REST listener, the ini path, and whether to create and
upgrade the database. Only the four REST ones were settings, and since schema
6042 - when the database became the settings store - a line written into the
file is taken only at the first start. A Kubernetes user expects every setting to
come from the environment, and a Secret to be able to supply any of them as a
file.

Now **every `[Settings]` key and every `hm_settings` row this build reads has a
name in the environment**, the entrypoint **stores the environment into the
database** with the server's own verb, and **the environment wins**: a setting it
names is refused to an administrator's change over REST, COM, the Deck and the
Control Panel, because the next start would undo the change. The two
`[SendingLimits]` keys, which are neither, stay in `hMailServer.ini`.

## The naming rule

One rule, and no table to keep:

| Variable | Sets |
|---|---|
| `HM_SETTING_<key>` | the `[Settings]` key `<key>` - one of the 241 keys `build/ini-settings-baseline.txt` lists, matched without case and stored in its own spelling. `HM_SETTING_MaxSubmissionsPerIPPerMinute=30` |
| `HM_PROPERTY_<name>` | the `hm_settings` row `<name>` - one `SettingsDefaults.h` lists with the column this build reads it by, 250 of them, and `backupencryptionpassphrase`, which is read both ways (the text is the passphrase, the number whether one is set) and is written through the setter that keeps the two together. `HM_PROPERTY_maxmessagesize=20480` |
| `HM_SETTING_<key>_FILE`, `HM_PROPERTY_<name>_FILE` | the same, the value read from the file the variable names - a Kubernetes Secret or a Compose secret mounted as a file. The file's trailing line breaks are stripped (a file made with `echo` ends in one); a byte order mark is dropped; the text must be UTF-8 without a NUL, and at most 64 KB. |

The prefixes and `_FILE` are written in capitals; the name after the prefix is
matched without case. No key and no property ends in `_FILE`
(`build/check-environment-names.py` holds that), so the suffix is never part of a
name.

**What is refused**, before anything is stored, with a sentence:

* a name that is not a setting of the store its prefix names - with the nearest
  name of either store, and the variable that sets it. `MaxMessageSize` after
  `HM_SETTING_` is refused naming `HM_PROPERTY_maxmessagesize`, because the
  maximum message size is an `hm_settings` row and not a `[Settings]` key; a
  misspelled key is refused naming the key it was meant for;
* a prefix that looks like one of the two and is not - `HM_SETTINGS_`,
  `HM_PROPERTIES_`, `HM_SETING_`, `hm_setting_` - naming the right one. Any other
  `HM_` variable the entrypoint does not read itself is left alone;
* one of the five `hm_settings` rows a create script still seeds for a feature
  that has gone, which nothing reads (`antispamenabled`, `groupcachettl`,
  `relaymode`, `sendstatistics`, `smtpnooftries`), and the two rows the server keeps
  its records in (below);
* a property REST answers read-only, which the server derives or keeps itself -
  `WebPushVapidPublicKey` above all, which is derived from the private key and is
  refused saying so;
* the plain form and the `_FILE` form of one setting set together, an alias and
  the name it stands for set together, or two spellings of one key (a POSIX
  environment tells them apart) - a mistake, not a precedence question;
* a `_FILE` naming a path that does not exist, is not a regular file (a
  directory: the folder a Secret is mounted in rather than the file inside it, or
  a bind mount whose source is missing, which Docker makes an empty directory),
  cannot be read to its end, is larger than 64 KB, or is not UTF-8 text. The path
  is named; the content never is;
* a `_FILE` form of one of the settings a domain or an account may hold its own
  value of ([SettingLevels.md](SettingLevels.md)): what is in effect is shown to
  their administrators, so a file cannot keep it secret, and the plain variable
  is the one to use;
* a value the setting does not take. A `[Settings]` key is held to what the store
  holds (at most 4000 characters, no line break - the sentence the REST ini route
  and `--set-setting` refuse with), to a whole number where the server reads it as
  one, and, for the six listener ports and `RestApiBindAddress`, to a port and an
  address the listener takes. A property is held to its kind - a whole number, or
  text of at most 4000 characters - and every setting a REST settings row keeps,
  property or `[Settings]` key, to **that row's type and check, in that row's
  sentence**: a value the server would refuse over `PUT /api/v1/settings/...` is
  refused here in the same words. The words true, false, yes, no, on and off are
  taken for a setting whose row is a switch, and only there - `maxmessagesize=off`
  is refused rather than stored as 0, which is "no limit"; a setting whose row is
  one of words (a connection security, a log device) takes the row's words as well
  as the number each stands for;
* a run that names so many settings that the record of them would not fit the
  4000 characters its row holds - refused before anything is stored.

A row's check can quote the value it refuses. For a value from a file, or of a
setting whose name says it is a secret, or one a file supplied before (below), the
verb prints a fixed sentence in its place - the variable, the setting and the route
whose check refused it - and never the row's.

Which REST row holds which setting is not written down anywhere. A `[Settings]` key
is the row's `ini_key`; a write-only row (a password, a key) names the property it
writes; and every other row's value is read with a recorder on the property set,
so that the one name its getter reads is the name the row holds - the getter is
the code COM's getter runs. A row lends itself only when it is the one row that
reads the property, reads nothing else, and answers the property as stored; a row
that inverts a switch or shows a size in other units lends nothing, and the
property is held to its kind.

## The verb

```sh
hmailserver --config /etc/hmailserver/hMailServer.ini --apply-environment
```

```text
HM_PROPERTY_maxmessagesize: set (maxmessagesize was 20480)
HM_PROPERTY_smtprelayerpassword_FILE: set (smtprelayerpassword, from a file; the value is not shown)
HM_SETTING_MaxSubmissionsPerIPPerMinute: unchanged
2 set, 1 unchanged. The server takes them when it starts; one already running takes them at its next reinitialise, or restart for a key marked restart.
```

`hMailServer.exe --apply-environment` on Windows is the same verb, from the same
code (`Common/Application/EnvironmentSettings`); it writes its lines to the
handles it was given, or to the console of whoever ran it. It:

1. reads the process's environment and resolves every `HM_SETTING_*`,
   `HM_PROPERTY_*` and alias, reading the files the `_FILE` forms name;
2. opens the configuration and the database as the service does - the whole of a
   start short of the listeners, as `--sample-domain` does - so run it as the user
   the service runs as;
3. **checks every value first**, and when any is refused prints every variable -
   `refused: <why>` for the ones refused, `not stored, because another variable
   was refused` for the rest - and **exits 1 having changed nothing**: a container
   does not start on settings it was not given;
4. writes each, and prints its line as soon as it is written: **through the REST
   settings row's setter wherever a row lends one**, so that a value is stored as
   `PUT /api/v1/settings/...` stores it - the script language as `JScript` or
   `VBScript` whatever the case it was given in, a URL trimmed, the VAPID private
   key with the public half derived from it, the backup passphrase with its is-set
   flag - and otherwise a `[Settings]` key through
   `IniFileSettings::WriteSettingsValue`, the locked write path every save takes,
   and a property through the property set. `set` says the value it replaced -
   never a secret's, a file's, or one a file supplied before - or `unchanged`;
5. rewrites the two records (below), reads every property it wrote and both
   records back from the table, and says which settings are no longer managed.

A failure after the checks - a write the table did not take, a record that did not
read back as written - is a `failed` line, and the verb exits 1. Anything that stops
the run part-way says where, and that the records were not updated; the lines above
it say what was stored.

With nothing set it changes nothing, exits 0, and says so - unless the last run
recorded names, which it then gives back. Exit 2 is a configuration file that
names no database. The `[Settings]` values are written to the file's copy as well
as the database, as every save is, so the file must be writable (the verb refuses
first, as `--set-setting` does, and `--set-setting` holds a value to the same
checks, in the same words).

**The entrypoint** runs it after `--create-database` and `--upgrade-database` and
before the server, whenever there is anything for it to do: an `HM_` variable other
than the ones it reads itself (the database, the administrator password, the ini
path, the two switches, and `HM_PROFILE` and `HM_PROFILE_CONFIRM`, the setting
profile it applies once after this verb, [Profiles.md](Profiles.md)) - which
includes a misspelt prefix, so that the verb can
refuse it - or the file `.hmailserver-environment` in the data directory, which the
verb keeps there while the environment manages anything, so that the first start
without the variables gives those settings back. Otherwise the database is not
opened before the server starts, as it was not before settings came from the
environment. Run against a server that is already running, the verb's values reach
that server at the server's next reinitialise (the Deck's and the Control Panel's
**Apply now**) - or its next start for a key marked `restart`
([SettingsApply.md](SettingsApply.md)) - because nothing watches the table; the
last line says so.

## The environment wins, every start

A value the environment sets is written at every start. So an edit made in the
Deck to a setting the environment manages would last until the next restart and
then be gone - which is worse than a refusal. The server therefore records which
names the environment manages - the `hm_settings` row `EnvironmentManagedSettings`,
the comma-separated variables of the last run, rewritten by every run and so
cleared of a variable no longer set - and **refuses an administrator's change to
one of them**:

| Where | What |
|---|---|
| REST | The settings groups (`GET /api/v1/settings` and the rest) answer `<key>_environment`, `variable` or `file`, beside a key the environment sets - a write-only key (a password) with its mark and no value; answered, never taken. A `PUT` that would change such a key, or that names a write-only one at all, is **409** with the sentence, before anything in the body is applied; a key sent with the value it already has is taken. The `<key>_environment` and `<key>_levels` marks and a read-only key are refused with 400, as a read-only key always is, and a value a file supplied - answered as `(set)` or `null` - is not one to send back: a client sends the keys it changed, as the Deck and `hmctl` do. `GET`, `PUT` and `DELETE /api/v1/settings/ini/{name}` answer `managed` and `environment`, and the `PUT` of another value and the `DELETE` are 409; the list answers an `environment` map. `GET /api/v1/settings/environment` lists every managed setting as `{variable, store, name, source}` - a read-only key may read it, a key confined to named domains may not. |
| COM | Every setter refuses, with the same sentence - the refusal is made where every setter writes (`PropertySet`'s setters and `IniSettingStore`'s writes), whenever an administrator's session is on the thread, and COM's catch reports it. `Settings.GetChangedFromDefault` names the managed settings. No new member. |
| Control Deck | The Settings view shows **Set by the environment** (or **... from a file**) beside such a key, with its value and no control, so there is nothing to change and nothing a save could send; a note explains the mark; the Changed from stock page says it beside the value. |
| Control Panel | The feature-settings and server-settings pages show the same mark in place of the applies mark, with the field disabled; the server-settings pages name every managed setting in the notice at their top. The Control Panel reads the names from `Settings.GetChangedFromDefault`, whose document carries them. |
| `hmctl`, PowerShell | `hmctl config changed` prints `(set by the environment)` beside such a value; `hmctl config export` and `Export-HmConfiguration` leave every managed setting out of the document, whatever it is set from - a group's key and its mark, and a `[Settings]` key the ini list's `environment` map names - because the value is the deployment's, not the configuration's, and applied to a server whose environment sets it the write would be refused. |
| Application log | One line at every start and reinitialise naming the managed settings. |
| `--set-setting` | Stores the value, as the operator's own command line, and says the next `--apply-environment` writes the environment's back. |

The sentence, which names the variable and never the value:

> max_message_size_kb is set by this server's environment (HM_PROPERTY_maxmessagesize): hmailserver --apply-environment writes it at every start, so a change made here would be undone at the next one, and it is refused. Change the variable and restart, or unset it and restart to manage the setting here.

**Nothing but an administrator is refused.** The refusal is made only while an
administrator's session is on the thread - which every COM call and every REST
request carries, and the verb, `--set-setting`, a restore and the server's own
bookkeeping do not. A restore writes the archive's values as it always did; the
next start writes the environment's back. Neither record is in the XML backup, and
a restore ignores one an archive carries: they describe the deployment a database
runs under, not the configuration a restore should carry, and a server that never
runs the verb would otherwise refuse those settings for ever.

## A secret stays one

A value a `_FILE` supplied is recorded as such, beside the other record - the
`hm_settings` row `EnvironmentSecretSettings`, names only - and **stays withheld
after the variable is gone**, until an administrator writes another value (or a
later run sets it from a plain variable, whose value is in the environment for
anyone who can read that). Such a value, and a value of a setting whose name says
it is a secret, is:

* **never printed by the verb** - its line says `the value is not shown`, and a
  row's check that would quote it is replaced by a fixed sentence (above);
* **never answered over REST** - a settings group answers `(set)` for text and
  `null` otherwise; the ini route answers `(set)`, the list `(set)` with `secret`
  true, the pending list `(set)`; the changed document lists it as `(set)` with
  `secret` true, and names it in `from_file`;
* **never answered by COM's `Settings.GetIniSetting`**, which answers `(set)`, nor
  by `Settings.GetChangedFromDefault`, which is the changed document;
* **not shown in the Control Panel**: a server-settings field whose value a file
  supplied is left empty with a sentence saying why, and a save writes nothing
  from it unless something was typed; a feature-settings field shows `(set)`,
  which a save does not write back because it has not changed;
* **recorded in the audit trail as `(changed)`**, as a secret's change is, when an
  administrator replaces it - and that write is what makes the new value theirs,
  shown as any other from then on;
* **never put on a command line** (the entrypoint runs the verb with none) and
  **never written to a log**.

`GET /api/v1/settings/environment` answers names and sources and no value at all.
The value is used for what the setting is for - a greeting a file supplies is still
the greeting the SMTP server sends - which is why the `_FILE` form is refused for a
setting with levels, whose value in effect is shown to a domain's administrators.

What still answers it: **the COM property getters** (`Settings.WelcomeSMTP` and the
rest), which answer a server administrator the value as they answer every setting,
and the settings section of an XML backup, which holds every setting's value as it
always has and is what the backup's own passphrase is for. The verb's writes are
not rows in the audit trail: it runs with no administrator, before the server, as a
restore does - its output, which is the container's log, says what it changed.

## Levels, and limits

What the environment sets is the **server** level. Twenty-seven settings may also
be set per domain and per account ([SettingLevels.md](SettingLevels.md)), and the
environment sets none of those levels. Of the fifteen that are **limits**, thirteen
have a server value the environment sets - the maximum message size
(`HM_PROPERTY_maxmessagesize`), the recall and recover-deleted windows
(`HM_PROPERTY_RecallDays`, `HM_PROPERTY_RecoverDeletedDays`), the session
lifetimes, the quota grace and send-stop shares, the reader maximums - and the
server level is the ceiling a domain and an account may tighten and never loosen:
lowering it from the environment lowers what every tenant may do, and a tenant's
own value above the new ceiling is capped when it is read. The other two, the
sending limits, are `[SendingLimits]` keys, which the store does not hold and no
variable names.

## Kubernetes

The chart's values take two maps and a list, rendered into the container's
environment; nothing is in the chart that the entrypoint does not read:

```yaml
settings:                       # HM_SETTING_<key>
  MaxSubmissionsPerIPPerMinute: "30"
properties:                     # HM_PROPERTY_<name>
  maxmessagesize: "20480"
  hostname: mail.example.com
settingsFrom:                   # a Secret's key, mounted as a file, named through _FILE
  - secretName: mail-relay
    key: password
    property: smtprelayerpassword
```

Each `settingsFrom` entry is a key of a Secret you made, projected into
`/run/secrets/hmailserver-settings/<n>` and named to the entrypoint as
`HM_PROPERTY_smtprelayerpassword_FILE` (or `HM_SETTING_<key>_FILE` with
`setting:`), so the value is in no environment variable, no rendered manifest and
no log line. A value is text, as the environment has no other kind; a whole number
may be left unquoted, and is rendered as the whole number it is. A switch is `1` or
`0` for a `[Settings]` key, which the server reads as a number; `true` and `false`
(and yes, no, on, off) are taken as well for a setting whose REST settings row is a
switch. A wrong value keeps the pod from starting, and `kubectl logs` says which and
why. The database, the administrator password and the REST listener keep their own
values, as before.

## Compose

```yaml
    environment:
      HM_SETTING_MaxSubmissionsPerIPPerMinute: "30"
      HM_PROPERTY_maxmessagesize: "20480"
      HM_PROPERTY_smtprelayerpassword_FILE: /run/secrets/relay_password
    secrets:
      - relay_password
```

The Compose file beside the image carries these commented out.

## The sixteen variables the container always took

They keep their names and their meaning. Four were settings, and are now aliases
of the names every setting has:

| Variable | What it is |
|---|---|
| `HM_REST_PORT` | `RestApiPort` - an alias of `HM_SETTING_RestApiPort` |
| `HM_REST_BIND_ADDRESS` | `RestApiBindAddress` - an alias of `HM_SETTING_RestApiBindAddress` |
| `HM_REST_CERTIFICATE_FILE` | `RestApiCertificateFile` - an alias of `HM_SETTING_RestApiCertificateFile`. The key is a path the server reads the certificate from; this is **not** a `_FILE` form. |
| `HM_REST_PRIVATE_KEY_FILE` | `RestApiPrivateKeyFile` - an alias of `HM_SETTING_RestApiPrivateKeyFile`, a path likewise |
| `HM_DB_TYPE`, `HM_DB_HOST`, `HM_DB_PORT`, `HM_DB_NAME`, `HM_DB_USER`, `HM_DB_PASSWORD`, `HM_DB_PASSWORD_FILE` | the `[Database]` section of the file - the bootstrap, which says where the database is and so cannot be in it |
| `HM_ADMIN_PASSWORD`, `HM_ADMIN_PASSWORD_FILE` | `[Security] AdministratorPassword`, written as a hash by `--set-admin-password` |
| `HM_INI` | where the file is |
| `HM_CREATE_DATABASE`, `HM_UPGRADE_DATABASE` | whether the entrypoint runs `--create-database` and `--upgrade-database` |
| `HM_PROFILE`, `HM_PROFILE_CONFIRM` | added with the setting profiles, and not settings either: the profile the entrypoint applies with `--apply-profile NAME --once` after the environment's settings, and whether it confirms an entry that closes a listener, trusts an address range or turns a filter off ([Profiles.md](Profiles.md)) |

An alias set to the empty string is unset, as the entrypoint always read it; an
`HM_SETTING_` or `HM_PROPERTY_` set to the empty string stores the empty string.
An alias is recorded under its own name, so `GET /api/v1/settings/environment`
answers `HM_REST_PORT` for `RestApiPort`.

## What is not there

* **The container path has not been run.** The verb has been run as
  `hMailServer.exe --apply-environment` on Windows only. Its POSIX half (`environ`,
  `stat`, `access`, `fputs`) compiles with clang on Linux, and the entrypoint's
  choice of when to run the verb was run under bash with a made-up data
  directory; the image's entrypoint as a whole, the POSIX verb against a database,
  the chart and the Compose file have not been executed.
* **No live reload from a ConfigMap change.** A restart applies it - the verb runs
  before the server, so every key is in effect when the server has started. A
  reinitialise does not re-run the verb; it takes a value the verb stored while the
  server ran.
* **No environment names for a domain's or an account's value**, nor for the two
  `[SendingLimits]` keys. The environment sets the server level of the two stores.
* **No export of the current configuration as an environment.** The changed
  document (`GET /api/v1/settings/changed`) already answers what differs, and says
  which of it the environment set.
* **The Control Panel ties a COM field to its setting by name.** A field kept in
  the settings store is known by its key, and a field of the COM settings tree when
  its property carries the `hm_settings` row's own name - about half of them. A
  managed setting whose field is named otherwise is not marked beside it; it is
  named in the notice at the top of the page, and the server refuses its change
  with the sentence. A value a file supplied is kept out of a field on the same
  terms.
* **The verb's writes are not in the audit trail** (above).
* **The Backup page's schedule rows are not marked**, though the server refuses a
  change to a managed one.
* **Windows services do not have an environment of their own that anything
  sets.** The verb is there for a script, a deployment tool, or an operator on
  Windows as on Linux; the service does not run it.

## Tests

`RegressionTests/Installation/EnvironmentConfiguration.cs` (REST on 10020, a
stand-in identity provider on 10021). The environment is set for a child
`hMailServer.exe --apply-environment` only, never for the bench's service, and
every value is put back in the teardown - the record of the managed settings first,
since while it names a setting the writes that put the others back are refused, and
the record of the values a file supplied last. A record a killed run left behind is
cleared before any test. Each with its negative control:

* `HM_PROPERTY_maxmessagesize` stored, read back over REST and COM after a
  reinitialise, the verb printing `set` and then `unchanged`;
* an unknown name refused naming the nearest - `MaxMessageSize` as a `[Settings]`
  key named as the property, a misspelled key named as the key - with exit 1 and
  nothing stored, not even the valid variable of the same run;
* `HM_SETTINGS_`, `HM_PROPERTIES_` and `hm_property_` refused naming the right
  prefix, a variable the entrypoint reads left alone;
* the two records and `relaymode` refused, and an empty alias taken as unset;
* a key read as a number refusing a word, `maxmessagesize=off` refused,
  `DomainReportRetentionDays` and `SmtpTarpitDelaySeconds` refused with the
  sentence their `PUT` refuses the same value with, compared, and
  `RewriteEnvelopeFromWhenForwarding=7` refused as the switch it is; the value in
  range taken over REST and by the verb;
* the script language given in small letters stored as `JScript` or `VBScript`;
* a VAPID private key refused as REST refuses it, the public key refused as
  derived, and a good key from a file stored with the public half derived and a new
  pair refused (409);
* a `_FILE` form read with its line break stripped, never in the verb's output, the
  settings group, the changed document (which marks it `file` and lists it in
  `from_file`) or the environment route, both forms together refused, and the
  `_FILE` form of a setting with levels refused;
* a missing path, a directory and a file with a NUL refused;
* a script language from a file refused without the row's sentence, which quotes
  it, and the same text from a plain variable refused with it;
* a value from a file withheld after the release, not printed by a later plain run,
  answered once a variable sets it plainly, and, after an administrator writes one,
  answered as theirs and recorded in the audit trail as `(changed)`; the marker file
  there while something is managed and gone after;
* `HM_PROPERTY_smtprelayerpassword` never printed, `smtp_relayer_password_environment`
  answered, and a group `PUT` naming it refused with nothing in it applied;
* `ServiceAccountName` from a file answered as `(set)` by its own route, the list
  (with the `environment` map), the pending list and COM's `GetIniSetting`;
* `HM_PROPERTY_OidcClientSecret_FILE` stored, answered as set, and sent by the
  single sign-on's token request;
* the record: `managed` and `<key>_environment` answered, a `PUT` of another value
  409 and of the same value 200, the `DELETE` 409, COM's `SetIniSetting` and
  `MaxMessageSize` refused with the sentence and the values unchanged; after a run
  without the variables, the `PUT`, the `DELETE`, the group `PUT`, `SetIniSetting`
  and `MaxMessageSize` all taken;
* `GET /api/v1/settings/environment` with names and sources and no value, read by a
  read-only key and refused to a domain-confined one; the changed document's
  `environment` list; the application log's line;
* a settings backup taken while a setting is managed, restored after a run without
  it, bringing no refusal back;
* the four aliases, each refused beside the name it stands for, and two recorded
  under their own names;
* a run with nothing set changing nothing;
* `build/check-environment-names.py`, and its own negative controls.

The Deck harness checks the mark, the missing control, a save that sends only the
edited key, and the Changed from stock page's mark; `ControlPanel.Tests`
(`EnvironmentSettingsTests`) the parse of both lists, the words, the field a managed
or withheld setting is and a failed read; `check-cli.py` both exports without a
managed key - a group's, and a `[Settings]` key the ini list's `environment` map
names - and `config changed` saying `(set by the environment)`.

`build/check-environment-names.py` holds every `HM_SETTING_` and `HM_PROPERTY_`
name the entrypoint, the chart's values (the examples commented out included, and
each `settingsFrom` entry's `setting:` or `property:`), the Compose file, the
Dockerfile, the packaging README and this document mention to a setting; that no
key or property ends in `_FILE`; that the four aliases name `[Settings]` keys the
entrypoint names; that neither record's row is a property; and that every
write-only REST settings row names the property it writes. It proves its own
negative controls on every run - a made-up name in a copy of this document, and
under `settings:` and in a `settingsFrom` entry in a copy of the values - are
refused.
