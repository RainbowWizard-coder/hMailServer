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

Now **every setting has a name in the environment**, the entrypoint **stores the
environment into the database** at every start with the server's own verb, and
**the environment wins**: a setting it names is refused to every other interface,
because the next start would undo the change.

## The naming rule

One rule, and no table to keep:

| Variable | Sets |
|---|---|
| `HM_SETTING_<key>` | the `[Settings]` key `<key>` - one of the 241 keys `build/ini-settings-baseline.txt` lists, matched without case and stored in its own spelling. `HM_SETTING_MaxSubmissionsPerIPPerMinute=30` |
| `HM_PROPERTY_<name>` | the `hm_settings` row `<name>` - one `SettingsDefaults.h` lists and this build reads. `HM_PROPERTY_maxmessagesize=20480` |
| `HM_SETTING_<key>_FILE`, `HM_PROPERTY_<name>_FILE` | the same, the value read from the file the variable names - a Kubernetes Secret or a Compose secret mounted as a file. The file's trailing line breaks are stripped (a file made with `echo` ends in one); a byte order mark is dropped; the text must be UTF-8 and at most 64 KB. |

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
* an `hm_settings` row nothing in this build reads, which would change nothing;
* the plain form and the `_FILE` form of one setting set together, an alias and
  the name it stands for set together, or two spellings of one key (a POSIX
  environment tells them apart) - a mistake, not a precedence question;
* a `_FILE` naming a file that cannot be read, is larger than 64 KB, or is not
  UTF-8 text (the path is named; the content never is);
* a `_FILE` form of one of the settings a domain or an account may hold its own
  value of ([SettingLevels.md](SettingLevels.md)): what is in effect is shown to
  their administrators, so a file cannot keep it secret, and the plain variable
  is the one to use;
* a value the setting does not take. A `[Settings]` key is held to what the store
  holds (at most 4000 characters, no line break - the sentence the REST ini route
  refuses with), to a whole number where the server reads it as one, and, for the
  six listener ports and `RestApiBindAddress`, to what `--set-setting` holds them
  to. A property is held to its kind - a whole number (true, false, yes, no, on
  and off say 1 and 0 for a switch), or text of at most 4000 characters - and to
  **the check its REST settings row makes, in that row's sentence**: a value the
  server would refuse over `PUT /api/v1/settings/...` is refused here in the same
  words. A property whose row is one of words (a connection security, a log
  device) takes the row's words as well as the number each stands for.

Which REST row holds which property is not written down anywhere: every row's
value is read with a recorder on the property set, and the one name a row's getter
reads is the name the row holds - the getter is the code COM's getter runs. A row
lends its check only when it is the one row that reads the property, reads
nothing else, and answers the property as stored; a row that inverts a switch or
shows a size in other units lends none.

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
4. writes each: a `[Settings]` key through `IniFileSettings::WriteSettingsValue`,
   the locked write path every save takes, and a property through the property
   set, read back afterwards; prints `set` with the value it replaced - never a
   secret's, a file's, or one a file supplied before - or `unchanged`;
5. rewrites the record of what the environment manages (below), and says which
   settings are no longer in it.

With nothing set it changes nothing, exits 0, and says so - unless the last run
recorded names, which it then gives back. Exit 2 is a configuration file that
names no database. The `[Settings]` values are written to the file's copy as well
as the database, as every save is, so the file must be writable (the verb refuses
first, as `--set-setting` does).

The container's entrypoint runs it at every start, after `--create-database` and
`--upgrade-database` and before the server. Run against a server that is already
running, its values reach that server at the server's next reinitialise (the
Deck's and the Control Panel's **Apply now**) - or its next start for a key marked
`restart` ([SettingsApply.md](SettingsApply.md)) - because nothing watches the
table; the last line says so.

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
| REST | The settings groups (`GET /api/v1/settings` and the rest) answer `<key>_environment`, `variable` or `file`, beside a key the environment sets; answered, never taken. A `PUT` that would change such a key is **409** with the sentence, before anything in the body is applied; a `PUT` sending the value it already has is taken, so a group read and sent back whole is saved. `GET`, `PUT` and `DELETE /api/v1/settings/ini/{name}` answer `managed` and `environment`, and the `PUT` of another value and the `DELETE` are 409; the list answers an `environment` map. `GET /api/v1/settings/environment` lists every managed setting as `{variable, store, name, source}` - a read-only key may read it, a key confined to named domains may not. |
| COM | Every setter refuses, with the same sentence - the refusal is made where every setter writes (`PropertySet`'s setters and `IniSettingStore`'s writes), whenever an administrator's session is on the thread, and COM's catch reports it. No new member. |
| Control Deck | The Settings view shows **Set by the environment** (or **... from a file**) beside such a key, with its value and no control, so a save never sends it; a note explains the mark; the Changed from stock page says it beside the value. |
| Control Panel | The feature-settings and server-settings pages show the same mark in place of the applies mark, with the field disabled; the server-settings pages name every managed setting in the notice at their top. The Control Panel reads the names from `Settings.GetChangedFromDefault`, whose document now carries them. |
| `hmctl`, PowerShell | `hmctl config changed` prints `(set by the environment)` beside such a value; `hmctl config export` and `Export-HmConfiguration` leave a managed key and its mark out of the document - the value is the deployment's, not the configuration's. |
| Application log | One line at every start and reinitialise naming the managed settings. |
| `--set-setting` | Stores the value, as the operator's own command line, and says the next `--apply-environment` writes the environment's back. |

The sentence, which names the variable and never the value:

> max_message_size_kb is set by this server's environment (HM_PROPERTY_maxmessagesize): hmailserver --apply-environment writes it at every start, so a change made here would be undone at the next one, and it is refused. Change the variable and restart, or unset it and restart to manage the setting here.

**Nothing but an administrator is refused.** The refusal is made only while an
administrator's session is on the thread - which every COM call and every REST
request carries, and the verb, a restore and the server's own bookkeeping do not.
A restore writes the archive's values as it always did; the next start writes the
environment's back. The record itself is not in the XML backup - it describes the
deployment a database runs under, not the configuration a restore should carry, and
a server that never runs the verb would otherwise refuse those settings for ever.

## A secret stays one

A value a `_FILE` supplied, and a value of a setting whose name says it is a secret,
is never printed by the verb - its line says `from a file; the value is not shown` -
never answered by the settings routes (a settings group answers `(set)` for text
and `null` otherwise, the ini route `(set)` with `secret` true in its list, the
pending list and the changed document `(set)`), never put on a command line (the
entrypoint runs the verb with none), and never written to a log.
`GET /api/v1/settings/environment` answers names and sources and no value at all.
The value is used for what the setting is for - a greeting a file supplies is still
the greeting the SMTP server sends - which is why the `_FILE` form is refused for a
setting with levels, whose value in effect is shown to a domain's administrators.
The verb's writes are not rows in the audit trail: it runs with no administrator,
before the server, as a restore does - its output, which is the container's log,
says what it changed.

## Levels, and limits

What the environment sets is the **server** level. Twenty-seven settings may also
be set per domain and per account ([SettingLevels.md](SettingLevels.md)), and the
environment sets none of those levels. For the fifteen that are **limits** - the
maximum message size (`HM_PROPERTY_maxmessagesize`), the recall and recover-deleted
windows (`HM_PROPERTY_RecallDays`, `HM_PROPERTY_RecoverDeletedDays`), the session
lifetimes, the quota grace and send-stop shares, the reader maximums - the server
level is the ceiling a domain and an account may tighten and never loosen: lowering
it from the environment lowers what every tenant may do, and a tenant's own value
above the new ceiling is capped when it is read.

## Kubernetes

The chart's values take three maps, rendered into the container's environment;
nothing is in the chart that the entrypoint does not read:

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
no log line. A value is text, as the environment has no other kind: quote a number
or a switch. A wrong one keeps the pod from starting, and `kubectl logs` says which
and why. The database, the administrator password and the REST listener keep their
own values, as before.

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

An alias set to the empty string is unset, as the entrypoint always read it; an
`HM_SETTING_` or `HM_PROPERTY_` set to the empty string stores the empty string.
An alias is recorded under its own name, so `GET /api/v1/settings/environment`
answers `HM_REST_PORT` for `RestApiPort`.

## What is not there

* **No live reload from a ConfigMap change.** A restart applies it - the verb runs
  before the server, so every key is in effect when the server has started. A
  reinitialise does not re-run the verb; it takes a value the verb stored while the
  server ran.
* **No environment names for a domain's or an account's value.** The environment
  sets the server level only.
* **No export of the current configuration as an environment.** The changed
  document (`GET /api/v1/settings/changed`) already answers what differs, and now
  says which of it the environment set.
* **The Control Panel ties a COM field to its setting by name.** A field kept in
  the settings store is known by its key, and a field of the COM settings tree when
  its property carries the `hm_settings` row's own name - about half of them. A
  managed setting whose field is named otherwise is not marked beside it; it is
  named in the notice at the top of the page, and the server refuses its change
  with the sentence.
* **A write-only REST key** (a password) has no value the settings route can read
  before a `PUT`, so the route's own check cannot see it: the store refuses it as
  it is written, and the 409 names the keys of the same body that were applied
  before it.
* **The verb's writes are not in the audit trail** (above).
* **The Backup page's schedule rows are not marked**, though the server refuses a
  change to a managed one.
* **Windows services do not have an environment of their own that anything
  sets.** The verb is there for a script, a deployment tool, or an operator on
  Windows as on Linux; the service does not run it.

## Tests

`RegressionTests/Installation/EnvironmentConfiguration.cs` (REST on 10020). The
environment is set for a child `hMailServer.exe --apply-environment` only, never for
the bench's service, and every value is put back in the teardown - the record
first, since while it names a setting the writes that put the others back are
refused. Each with its negative control:

* `HM_PROPERTY_maxmessagesize` stored, read back over REST and COM after a
  reinitialise, the verb printing `set` and then `unchanged`;
* an unknown name refused naming the nearest - `MaxMessageSize` as a `[Settings]`
  key named as the property, a misspelled key named as the key - with exit 1 and
  nothing stored, not even the valid variable of the same run;
* a key read as a number refusing a word, and `DomainReportRetentionDays` refused
  with the sentence `PUT /api/v1/settings` refuses the same value with, compared;
* a `_FILE` form read with its line break stripped, never in the verb's output, the
  settings group, the changed document or the environment route, both forms
  together refused, and the `_FILE` form of a setting with levels refused;
* the record: `managed` and `<key>_environment` answered, a `PUT` of another value
  409 and of the same value 200, the `DELETE` 409, COM's `SetIniSetting` and
  `MaxMessageSize` refused with the sentence; after a run without the variables,
  every one of those writes taken;
* `GET /api/v1/settings/environment` with names and sources and no value, read by a
  read-only key and refused to a domain-confined one;
* the four aliases, each refused beside the name it stands for, and two recorded
  under their own names;
* a run with nothing set changing nothing;
* `build/check-environment-names.py`, and its own negative control.

The Deck harness checks the mark, the missing control and a save that does not send
the key; `ControlPanel.Tests` (`EnvironmentSettingsTests`) the parse, the words, the
field a managed setting is and a failed read; `check-cli.py` an export without a
managed key or its mark.

`build/check-environment-names.py` holds every `HM_SETTING_` and `HM_PROPERTY_`
name the entrypoint, the chart's values (the examples commented out included), the
Compose file, the Dockerfile, the packaging README and this document mention to a
setting; that no key or property ends in `_FILE`; that the four aliases name
`[Settings]` keys the entrypoint names; and that the record's row is no property. It
proves its own negative controls on every run - a made-up name in a copy of this
document, and under `settings:` in a copy of the values - are refused.
