# Event scripts

**The server running your code when something happens to mail: VBScript and JScript on Windows, JavaScript on Linux.**

An event script is one file, `EventHandlers.vbs` or `EventHandlers.js` in the server's
Events directory, holding functions named after the events they handle -
`OnAcceptMessage`, `OnClientLogon`, `OnDeliveryStart` and a dozen more. When scripting
is switched on, the server calls the function for each event as it happens, hands it
the objects the event is about - the client, the message, the account - and reads
back what the function decided through the `Result` object: refuse this message,
deny this logon, keep that download on the remote server. A rule's "Run function"
action calls a function of the same file by name.

No handler is called until you switch scripting on: the switch is off as shipped,
and with it off no handler is called for any event. With it off the file is read -
at start, at every reload, at `Reload()` and at `POST /api/v1/settings/scripting/reload` -
but no engine runs it and nothing in it is checked. Switching scripting on - the
Control Panel, COM's `Scripting.Enabled`, `PUT /api/v1/settings/scripting` or the
environment - loads the file before the switch takes effect, and so does
every load while it is on: on every engine loading a file runs its code outside
functions, once to compile it and once more for each of the fifteen handlers looked
for. Keep that code to declarations. A top level that throws, or runs past the
timeout, is reported then as a load that failed (HM5016, HM5710 or HM5021).

---

## Which language, where

There are three languages, and each build runs the ones it has an engine for.

| Language | Windows | Linux | The engine | The file |
|---|---|---|---|---|
| `VBScript` | runs | refused at load | Windows Active Scripting (vbscript.dll) | `EventHandlers.vbs` |
| `JScript` | runs | refused at load | Windows Active Scripting (jscript.dll), ECMAScript 3 | `EventHandlers.js` |
| `JavaScript` | refused at load | runs | QuickJS 2026-06-04, embedded in the server, ES2025 | `EventHandlers.js` |

The language is the database setting `scriptlanguage`: `Settings.Scripting.Language`
over COM, `"language"` in `PUT /api/v1/settings/scripting` over REST, and the
**Language (VBScript, JScript or JavaScript)** field on the Control Panel's
Advanced settings page. It accepts all three words on both platforms, in any case
and with spaces around them trimmed, and stores the spelling above; any other word
is refused and nothing is stored. Both platforms accept all three because a backup,
an environment configuration and a configuration document (`hmctl config export`)
carry settings from one machine to another, and a configuration made on a Linux
server has to apply on a Windows one.

Storing the language loads nothing. The script in force goes on running, in the
language it was loaded in, until the next load; every event is called in that
language, never in the one the setting names meanwhile.

A language the build does not run is refused **when the script is loaded with
scripting on** - when scripting is switched on, and while it is on at start, at every
reload, at `Reload()` and `POST /api/v1/settings/scripting/reload` - with one line in
the ERROR log (HM5016, or HM5710 when an earlier script stays in force, as it then
does), naming the language to use instead, whether the file for it exists or not. Not
once per event: the load refuses, nothing of the new file is registered, and no event
tries it. While scripting is switched off the file is read but not run and nothing is
reported; a script loaded then registers no handler, and if the load that switches
scripting on refuses the file, that script stays in force, never compiled: the first
event that reaches it - a rule's "Run function" action, the one call no handler gates
- is reported (HM5018) and the rest are skipped without a line until the next load.

`GET /api/v1/status` says which languages this build runs, under `build`:

```json
"build": { "script_engine": true, "script_languages": ["JavaScript"], ... }
```

`["VBScript","JScript"]` on Windows. The setting is seeded `VBScript` on every
platform - the database scripts are shared - so a Linux server switching scripting
on sets `JavaScript` first.

JScript and JavaScript share the file name. A JScript file carried from Windows to
Linux is the file the JavaScript engine is given, which is how a port starts; what
has to change in it is under [How JavaScript differs](#how-javascript-differs-from-vbscript-and-jscript).

### Switching it on, on Linux

```sh
hmctl settings set scripting --set-string language=JavaScript
sudo -u hmailserver tee /var/lib/hmailserver/events/EventHandlers.js <<'EOF'
function OnAcceptMessage(client, message) {
   if (message.Subject.includes('[URGENT]') && !client.Authenticated) {
      Result.Value = 2;
      Result.Message = 'Urgent mail comes from people who have logged on.';
   }
}
EOF
hmctl settings set scripting --set enabled=true
hmctl settings get scripting current_script_file
```

Switching scripting on loads the file, `POST /api/v1/settings/scripting/reload`
loads it again after an edit, and
`POST /api/v1/settings/scripting/check` answers `{"result":""}` when it compiles
and the error, with its line, when it does not. Over REST alone the first and
fourth steps are `PUT /api/v1/settings/scripting` with `{"language":"JavaScript"}`
and `{"enabled":true}`.

---

## The events and what a handler is given

Each handler is called with the objects the event is about and, for some, values
after them. The objects an event has are also globals of the same names -
`HMAILSERVER_CLIENT`, `HMAILSERVER_MESSAGE`, `HMAILSERVER_ACCOUNT`,
`HMAILSERVER_FETCHACCOUNT`, `Result` and `EventLog` - and the parameter and the
global are the same object. An object an event does not have is `null` as a
parameter. As a global it is `null` where the event names the object and has none -
the message of an SMTP event fired before `MAIL FROM` - and **not defined at all**
where the event does not name it: the message of an external download that failed,
and `Result` in `OnClientLogon`, `OnRecipientUnknown`, `OnTooManyInvalidCommands`,
`OnDeliveryFailed` and `OnError`. Reading an undefined global throws a
`ReferenceError`, so a handler reads its parameter, or asks `typeof`.

| Handler | Called with | What `Result.Value` does |
|---|---|---|
| `OnClientConnect(client)` | the client | 1 closes the connection at once |
| `OnHELO(client)` | the client | 1 answers `554 5.7.1 Rejected`; 2 `554 5.7.1` with `Result.Message`; 3 `453 4.7.0` with `Result.Message` |
| `OnClientLogon(client)` | the client, `client.Authenticated` saying how the logon went | nothing: the logon has been decided; the event has no `Result` on any platform |
| `OnClientValidatePassword(account, password)` | the account and the password typed | 0 lets the client in, 1 turns it away, 2 (as given) leaves it to the stored password |
| `OnAcceptMessage(client, message)` | the client and the message | as `OnHELO` |
| `OnSMTPData(client, message)` | the client and the message | as `OnHELO` |
| `OnRecipientUnknown(client, message)` | the client and the message | nothing |
| `OnTooManyInvalidCommands(client, message)` | the client and the message | nothing |
| `OnDeliveryStart(message)` | the message | 1 deletes the message |
| `OnDeliverMessage(message)` | the message | 1 deletes the message |
| `OnDeliveryFailed(message, recipient, error)` | the message, the recipient, the error text | nothing |
| `OnExternalAccountDownload(fetchAccount, message, uid)` | the fetch account, the message or `null`, its UID on the remote server | 1 deletes it there now; 2 after `Result.Parameter` days; 3 never |
| `OnBackupCompleted()` | nothing | nothing |
| `OnBackupFailed(reason)` | the reason | nothing |
| `OnError(severity, code, source, description)` | the error, as the ERROR log has it | nothing |
| a rule's "Run function" | the message | nothing; a function the file does not define is a Script Error |

A `554` from `OnHELO`, `OnAcceptMessage` or `OnSMTPData` counts towards the session's
limit of invalid commands (`MaxNumberOfInvalidCommands`), as the server's own refusal
of a message for its size, its bare line feeds or its spam score does; until 6.3.7
none of them did (upstream PR 664). With
`DisconnectInvalidClients` on, the refusal that takes the count past the limit is
answered `421 4.7.0 Too many invalid commands. Bye!` in its place, the session is
closed and `OnTooManyInvalidCommands` fires. A `453` is not counted. The count is the
connection's: RSET, a command that succeeds and a message accepted leave it as it is.
`OnRecipientUnknown` fires when the server refuses a recipient it does not have -
unless that refusal is the one past the limit, which fires `OnTooManyInvalidCommands`
instead - and for nothing else: a script's `554` whose `Result.Message` reads
`Unknown user` refuses no recipient and does not fire it.

`OnBackupCompleted` and `OnBackupFailed` are called once the backup has ended and
given up the run slot that lets one backup or restore run at a time, after the
backup log's line, so a handler - or anything that reads the log - may start the
next backup at once. Before 12 October 2026 they were called from inside the run,
and a backup started from one was refused as already started for as long as the
handler took.

On JavaScript the values - a password, a recipient, an error's description - are
handed to the handler as values, with `JS_Call`. VBScript and JScript are handed
their call as source text built and quoted by the server; JavaScript's is not built
at all, so no quote or backslash in a value can end a string early. On Windows the
quoting is one pair of helpers (`ScriptServer::EscapeForJScriptLiteral` and
`EscapeForVBScriptLiteral`): for JScript every backslash, both quotes, CR, LF,
U+2028, U+2029 and NUL are escaped, the backslash first - before 24 September 2026
only the apostrophe was, and a password ending its string with `\'` ran the code
after it; for VBScript the quote is doubled and CR, LF and NUL are joined in from
outside the literal (`" & vbCr & "`). Before 12 October 2026 three VBScript calls were
quoted by hand instead - `OnClientValidatePassword`'s password and
`OnExternalAccountDownload`'s remote unique-id had their quotes doubled and nothing
else, and `OnDeliveryFailed` took the quotes out of the recipient and the error and
joined in only a CR LF pair - so a lone CR or LF in a password or in the unique-id a
remote POP3 server lists (its listing is split at CR LF pairs only), or a lone LF in
a remote server's answer (a CR ends the line the answer is read from), left the
literal open, the call did not compile and the handler was not asked;
`OnDeliveryFailed` is now handed its two values with their quotes. Its error is
trimmed of the white space at either end; on VBScript, as before, the trim does not
reach into a CR LF pair - the hand-quoting joined each pair in before it trimmed - so a
VBScript handler is handed the CR LF that ends the error's last line, which a JScript
or JavaScript one is not.
A rule's "Run function" name is written
into the call as it is, so a name that is not a plain identifier - ASCII letters,
digits, `_` and `$`, not starting with a digit, at most 255 characters - is refused
(HM5018) and nothing is run. On Linux no call text is built, but the handler is
found by evaluating its name - `(typeof name === 'function') ? name : undefined`, so
that one declared with `let`, `const` or `class` is found too - and only a plain
identifier is evaluated (`ScriptBinding.cpp`'s `IsIdentifier_`, which asks
`ScriptServer::IsPlainProcedureName`, the test the Windows call text and the save are
held to). That one test is all that stands between a rule's name and script source
on both platforms, and widening it widens what both put into it. Such a name finds
no function, nothing is run, and the ERROR log has a Script Error line saying it is
not a function defined in the event script.
Since 12 October 2026 such a name, or none, is refused before that, when the rule is
saved, on both platforms and for a rule of every owner: COM's `RuleAction.Save` and
`Rule.Save` and the REST API's rule routes answer with the reason
(`RuleGuard::WhyScriptFunctionMayNotBeStored`, which asks the firing path's own test,
`ScriptServer::IsPlainProcedureName`) - `Rule.Save` where it changes the action, so
that over COM a rule holding one stored before is still changed around it ([MailFlowRules.md](MailFlowRules.md)).
What the firing path does is then for a name stored before that, restored from a
backup or written to the database.

---

## The objects: the contract

What each object has on JavaScript, member by member against the COM interface the
Windows engines are given (`hMailServer.idl`). *read* is a property, *read/write* a
property that may be assigned, *method* a function. An assignment to a read-only
property throws a `TypeError` naming it. Everything COM has and JavaScript does
not is listed, as **not on JavaScript**, with the reason under the table.
`ScriptBinding::Contract()` and `ScriptBinding::NotOnJavaScript()` in
`Common/Scripting/ScriptBinding.cpp` are this list, and `ScriptBindingTester` holds
every object to it.

### Message

| Member | COM | JavaScript |
|---|---|---|
| `ID` | read | read |
| `Filename` | read | read |
| `Subject` | read/write | read/write |
| `From` | read/write | read/write |
| `Date` | read/write | read/write |
| `Body` | read/write | read/write |
| `HTMLBody` | read/write | read/write |
| `Attachments` | read | **not on JavaScript** |
| `Save()` | method | method |
| `To` | read | read |
| `AddRecipient(name, address)` | method | method |
| `FromAddress` | read/write | read/write |
| `State` | read | read |
| `Size` | read (KB) | read (KB) |
| `ClearRecipients()` | method | method |
| `CC` | read | read |
| `Recipients` | read | read: `Count`, and `Item(index)` - see below |
| `HeaderValue(name)` | parameterised read/write | `HeaderValue(name)` reads, `SetHeaderValue(name, value)` writes |
| `HasBodyType(type)` | method | method |
| `EncodeFields` | read/write | read/write |
| `Flag(flag)` | parameterised read/write | `Flag(flag)` reads, `SetFlag(flag, on)` writes |
| `InternalDate` | read (a date) | read (text: `yyyy-mm-dd hh:mm:ss`, which `new Date()` reads) |
| `Headers` | read | **not on JavaScript** - `HeaderValue` and `SetHeaderValue` |
| `RefreshContent()` | method | method |
| `DeliveryAttempt` | read | read |
| `Charset` | read/write | read/write |
| `Copy(folderId)` | method | method |
| `UID` | read | read |

Changes to the subject, the body and the headers are written to the message file by
`Save()`, as on Windows; a handler that does not call it changes nothing on disk.

**`Attachments` and `Headers`** are collections of objects of their own on COM.
The attachment objects read and write files by path (`SaveAs`, `Add`), which is
outside what a JavaScript handler may do (see [Limits](#limits-and-what-a-handler-cannot-do));
the header objects keep their place in the header list across additions and
deletions, which is COM's own bookkeeping. A header is read and written by name
with `HeaderValue` and `SetHeaderValue`.

### Message.Recipients and its items

| Member | COM | JavaScript |
|---|---|---|
| `Count` | read | read |
| `Item(index)` | parameterised read (the default) | method; an index outside the collection throws a `RangeError` |
| `Item(i).Address` | read | read |
| `Item(i).IsLocalUser` | read | read |
| `Item(i).OriginalAddress` | read | read |

### Client

| Member | COM | JavaScript |
|---|---|---|
| `Port` | read | read |
| `IPAddress` | read | read |
| `Username` | read | read |
| `HELO` | read | read |
| `Authenticated` | read | read |
| `EncryptedConnection` | read | read |
| `CipherVersion` | read | read |
| `CipherName` | read | read |
| `CipherBits` | read | read |
| `SessionID` | read | read |

### Result

| Member | COM | JavaScript |
|---|---|---|
| `Value` | read/write | read/write |
| `Message` | read/write | read/write |
| `Parameter` | read/write | read/write |

The server reads `Result` after the handler returns, as on Windows. `Value` and
`Parameter` take a whole Number and nothing else: a string (`'1'` too), a boolean,
`null`, `undefined`, `NaN`, a fraction or an object throws a `TypeError`, the handler
stops there with a Script Error, and the event goes on with the `Result` it had - so
`Result.Value = decisions[user]` for a user the table does not have leaves
`OnClientValidatePassword` at 2, the stored password, and never at the 0 that lets
the client in. On Windows COM refuses a word, `NaN`, `null` or an object the same
way, as a type mismatch, and turns a boolean, `undefined`, a fraction and a string of
digits into numbers - `undefined` into that 0; JavaScript refuses them all.

### EventLog

| Member | COM | JavaScript |
|---|---|---|
| `Write(text)` | method | method |

### FetchAccount (OnExternalAccountDownload)

| Member | COM | JavaScript |
|---|---|---|
| `ID`, `IsLocked`, `NextDownloadTime` | read | read |
| `Name`, `ServerAddress`, `Port`, `ServerType`, `Username`, `MinutesBetweenFetch`, `DaysToKeepMessages`, `AccountID`, `Enabled`, `ProcessMIMERecipients`, `ProcessMIMEDate`, `UseSSL`, `UseAntiSpam`, `UseAntiVirus`, `EnableRouteRecipients`, `ConnectionSecurity`, `MIMERecipientHeaders`, `MirrorFolders` | read/write | read; the assignment is **not on JavaScript** |
| `Password` | read/write | **not on JavaScript** |
| `Save()`, `DownloadNow()`, `Delete()` | method | **not on JavaScript** |

The fetch account is the one the download is running under. A handler decides about
the message through `Result`; the account's own settings are the administrator's,
through COM, REST and the Control Panel, and its password is a secret the handler
has no use for.

### Account (OnClientValidatePassword)

| Member | COM | JavaScript |
|---|---|---|
| `ID` | read | read |
| `Address`, `DomainID`, `Active`, `IsAD`, `ADDomain`, `ADUsername`, `MaxSize`, `AdminLevel`, `PersonFirstName`, `PersonLastName`, `VacationMessageIsOn`, `VacationSubject`, `VacationMessage`, `ForwardEnabled`, `ForwardAddress`, `ForwardKeepOriginal`, `SignatureEnabled`, `AntiSpamEnabled`, `MessageRetentionDays`, `RestrictedDelivery`, `ExternalId` | read/write | read; the assignment is **not on JavaScript** |
| `ZeroAccess` | read | read |
| `Password`, `SignaturePlainText`, `SignatureHTML`, `VacationMessageExpires`, `VacationMessageExpiresDate`, `VacationMessageBeginDate`, `VacationMessageAbortSpamFlagged`, `VacationMessageDays`, `VacationInternalSubject`, `VacationInternalMessage`, `VacationMessageInternalOnly`, `ForwardAbortSpamFlagged`, `SieveScript`, `SpamMarkThreshold`, `SpamDeleteThreshold`, `RoleName`, `RoleDomains`, `OpenPGPPublish`, `DirectoryTitle`, `DirectoryDepartment`, `DirectoryPhone`, `DirectoryMobile`, `DirectoryOffice`, `DirectoryOptOut`, `DirectoryNote`, `DirectoryPhoto`, `SettingOverride(name)` | read/write | **not on JavaScript** |
| `InUseReason`, `LastMove`, `Size`, `Messages`, `FetchAccounts`, `Rules`, `IMAPFolders`, `QuotaUsed`, `PushSubscriptionCount`, `LastLogonTime`, `AppPasswords`, `TOTPEnabled`, `RolePermissions`, `OpenPGPKeys`, `RecoveryAddress`, `RecoveryAddressVerified`, `RecoveryCodesUnused`, `DirectoryPhotoType`, `EffectiveSetting(name)`, `EffectiveSettingSource(name)` | read | **not on JavaScript** |
| `MoveTo`, `Save`, `DeleteMessages`, `ValidatePassword`, `UnlockMailbox`, `RemovePushSubscriptions`, `Delete`, `EnrolTOTP`, `DisableTOTP`, `ExportMessages`, `ExportArchive`, `GetDevices`, `RevokeDevice`, `ClearRecoveryAddress`, `ClearRecoveryCodes` | method | **not on JavaScript** |

This one is thinner than COM on purpose. The account is the one a password is being
checked for, and what the handler decides is `Result.Value`; the properties that
describe who the account is are there to decide it with. The collections, the
secrets, the per-account policy an administrator's tool reads and the operations it
performs - moving a mailbox, exporting it, enrolling a second factor - are COM, REST
and the Control Panel's.

---

## How JavaScript differs from VBScript and JScript

- **Parameterised properties have no assignment.** `message.HeaderValue('X') = 'v'`
  is JScript, and a syntax error in JavaScript: it is `message.SetHeaderValue('X', 'v')`,
  and `message.Flag(n) = true` is `message.SetFlag(n, true)`. The reading forms are
  unchanged: `message.HeaderValue('X')`, `message.Flag(n)`, `message.Recipients.Item(0)`.
- **Names are case-sensitive and spelled as the IDL spells them.** JScript found
  `message.FileName` because COM looked names up without regard to case; JavaScript
  needs `message.Filename`.
- **No `CreateObject`, no `new ActiveXObject`.** They make COM objects, and COM is
  Windows. Both exist on JavaScript only to throw a `TypeError` that says so. The
  objects a handler can use are the server's own, above; `ScriptAllowedObjects`, the
  list of COM classes a Windows script may create, has nothing to apply to.
- **The language is ES2025**, not ECMAScript 3: `let`, `const`, classes with
  private fields, arrow functions, template literals, optional chaining (`?.`),
  nullish coalescing (`??`), `Array.prototype.findLast` and `toReversed`,
  `Object.hasOwn`, `async` and `await`, and since 6.3.6 the iterator helpers
  (`[].values().map(...)`) and the Set methods (`union`, `isSubsetOf`). A handler
  may be declared with `function`, or as a `var`, `let` or `const` holding a
  function.
- **An `async` handler is waited for**, inside the same timeout: the server reads
  `Result` once its promise has settled, and a rejected promise is a Script Error.
- **Values arrive as values.** Nothing is quoted into source text; see the event table.
- **`null`, not `Nothing`,** for an object an event does not have.
- **The objects' members are not globals.** The Windows engines are given the
  objects with `SCRIPTITEM_GLOBALMEMBERS`, so VBScript could call `Write` without
  `EventLog.`; JavaScript has the objects by name only.
- **`InternalDate` is text**, `yyyy-mm-dd hh:mm:ss`, where COM hands over a date.
- **A handler's return value is ignored**, as a VBScript function's is.

### A ported handler

The VBScript a Windows server runs:

```vb
Sub OnAcceptMessage(oClient, oMessage)
   If InStr(oMessage.HeaderValue("X-Spam-Flag"), "YES") > 0 And Not oClient.Authenticated Then
      oMessage.HeaderValue("X-Quarantine") = "yes"
      oMessage.Subject = "[SPAM] " & oMessage.Subject
      oMessage.Save
      EventLog.Write("Tagged spam from " & oClient.IPAddress)
   End If
End Sub
```

The same handler for a Linux server, in `EventHandlers.js`:

```js
function OnAcceptMessage(client, message) {
   if (message.HeaderValue('X-Spam-Flag').includes('YES') && !client.Authenticated) {
      message.SetHeaderValue('X-Quarantine', 'yes');
      message.Subject = `[SPAM] ${message.Subject}`;
      message.Save();
      EventLog.Write(`Tagged spam from ${client.IPAddress}`);
   }
}
```

---

## Limits and what a handler cannot do

The threat model is the Windows one: an event script is code the administrator put
on the server, running inside the server process. What the JavaScript engine adds
is that the script cannot reach past the objects it is given.

- **A fresh engine for every event.** Each event gets a runtime and a context of its
  own, the whole file is parsed and run, the handler is called, and both are freed
  before the event goes on - the shape the Windows host has, where each event
  creates a new script engine. Nothing a script makes outlives the event, so a
  script that keeps an object in a global cannot reach a message that has since been
  delivered, and the protocol and delivery threads that fire events at the same
  time never share an engine. A reload installs new text for the next event; an
  event already running finishes with the text it started with.
- **No file system, no network, no processes.** QuickJS keeps these in its `std` and
  `os` modules, in `quickjs-libc.c`, and that file is not in the tree: it is not
  compiled, not linked, not there. No module loader is installed, so `import()` is
  refused and an `import` statement is a syntax error; `Atomics.wait` may not block a
  server thread; there is no `print` and no `console`. `eval` and `Function` are
  there and are no way out: they compile more JavaScript into the same sandbox.
  What a handler can change is what its objects change - a message's file through
  `Save()`, a copy into a folder through `Copy()`, the event log through
  `EventLog.Write`.
- **The timeout.** `ScriptTimeout` (seconds, default 60, 0 for no limit) bounds the
  JavaScript a run executes. It is a `[Settings]` key, kept in the settings store:
  set it with `PUT /api/v1/settings/ini/ScriptTimeout`, COM `Settings.SetIniSetting`
  or the Control Panel - a value edited into hMailServer.ini is put back at the next
  start. The engine polls it in its interpreter loop, as the script calls functions
  and takes branches, and in the regular expression matcher, so a loop, a deep
  recursion and a runaway backtracking pattern are all stopped, and no
  `try`/`catch` in the script can hold the interruption. A handler stopped at the
  timeout is reported as HM7341; the event goes on with whatever `Result` the
  handler had set. Code outside a function runs when the file is loaded, and a top
  level that runs past the timeout fails the load (HM5021).

  What it does **not** stop is one call into a built-in that loops in the engine's
  C code without polling: `includes`, `indexOf`, `lastIndexOf` and the other array
  methods that read an array-like element by element, called on an object whose
  `length` is huge - `Array.prototype.includes.call({length: 2**53 - 1}, 1)`, or
  `new Array(2**32 - 1).lastIndexOf(0)`. Such a call runs to its end however long
  that takes - for the first, in effect for ever - holding the thread that fired the
  event and the message with it, and it allocates nothing, so the memory ceiling does
  not stop it either (the built-ins that allocate as they go do stop there). A sender
  cannot hand a handler such a length - a message's values, the JSON parsed from them
  and `split()` are all bounded by the message's size - so it takes a handler written
  that way; do not call an array method on an object whose `length` the handler has
  not checked. The Windows engines have a limit of their own: their interrupt cannot
  release a handler blocked inside a COM call.
- **The memory ceiling.** Each run may allocate 64 MB, a constant. A handler stopped
  there is reported as HM7342, with where it was, and the event goes on.
- **The stack.** A run may use 512 KB of the calling thread's stack. A runaway
  recursion throws an `InternalError` ("stack overflow"), not the thread's end - an
  `InternalError` in this release of QuickJS, where browsers and QuickJS-ng throw a
  `RangeError`, so a handler that catches it by type names `InternalError`.

### Errors, and where they are written

| What | Where |
|---|---|
| A script that does not compile, or whose top level throws - loaded with scripting on, or when scripting is switched on | HM5016 at start (nothing loaded), HM5710 at a reload or a switch (the earlier script stays in force) - one line naming the file, the error, `Line: n Column: n` and the stack |
| A script whose handlers could not be looked for - a later run of its top level threw, or reached the memory ceiling | HM5016 or HM5710, as a script that does not compile: the load is refused rather than installed with a handler switched off |
| A language this build does not run | HM5016 or HM5710, once per load, naming the language to use, whether the file for it exists or not |
| A top level that runs past the timeout | HM5021, and the load refused |
| An exception a handler does not catch | one `Script Error: Source: JavaScript - Error: TypeError - Description: ... - Line: n Column: n - Stack: at OnAcceptMessage (EventHandlers.js:3:7) \| ...` line in the ERROR log, as the Windows host writes its own; the event goes on |
| A handler stopped at the timeout | HM7341 |
| A handler stopped at the memory ceiling | HM7342 |
| The engine could not be started (memory) | HM7340 |
| An event that reaches a script loaded while scripting was off and never compiled since - the load that switched scripting on refused it, in a language this build does not run or one that does not compile - in practice a rule's "Run function" | HM5018, once per load |
| On Windows, an event whose call was built for VBScript while a reload installed JScript, or the reverse | HM5018, once per load; that one event is skipped rather than run in the other language |
| A rule's "Run function" saved naming no function, or a name that is not a plain identifier | refused by the save, with the reason - a COM error from `RuleAction.Save` or `Rule.Save`, a 400 from the REST API's rule routes; nothing is stored, and nothing is written to the ERROR log |
| On Windows, a rule's "Run function" whose name is not a plain identifier - stored before the save refused it, restored from a backup or written to the database | HM5018, each time the rule matches; the function is not run |
| On Linux, a rule's "Run function" whose name is not a plain identifier - stored before the save refused it, restored from a backup or written to the database | a `Script Error: ... ReferenceError - Description: <name> is not a function defined in the event script` line in the ERROR log, each time the rule matches; the name is never evaluated and nothing is run |

The load is all or nothing on every engine: a script that does not compile installs
nothing, and whatever was loaded before stays in force - so a typo in a reload never
switches a working `OnClientValidatePassword` off.

---

## The engine

QuickJS, Fabrice Bellard's JavaScript engine, release 2026-06-04, MIT-licensed. It is
vendored as source under `hmailserver/source/Server/quickjs` - the engine, its
regular expression and Unicode libraries, `cutils` and `dtoa`, byte for byte as
released, with its `LICENSE` and `VERSION` - and built by the CMake build into the
Linux server, with the definitions its own Makefile gives it. It came from its
author's site, bellard.org/quickjs/quickjs-2026-06-04.tar.xz (sha256
`b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`), and was checked
against Alpine Linux's package of the same release, whose APKBUILD (aports,
`community/quickjs`, commit `a7a96a01`) records the archive's SHA-512,
`e02722a82de0d2b7d6c95edc57daccecb5680b8d87f3580424a143a04df197c0df5bcd51d138c5e1e8b0d78b423acfa39169e8c0bfc1ace8387e13fc3bca22e2`:
the same file. It replaced release 2025-04-26 in 6.3.6, for the memory-safety fixes
in the engine since then ([CHANGELOG.md](../../CHANGELOG.md)). The Linux packages install its licence as
`/usr/share/hmailserver/licenses/QuickJS.txt`, and the SBOM names it
([ThirdPartyBinaries.md](ThirdPartyBinaries.md)).

The release has no Microsoft C compiler support, so `hMailServer.exe` does not
contain it: Windows runs VBScript and JScript, unchanged.

**Tested by** `ScriptBindingTester` (the contract above member by member, read told
from read/write; values both ways and what `Result` refuses; the timeout, the
ceiling, the stack and the sandbox), which one thing runs: the bench program
`script-binding-tests`, which the CMake build makes beside `spf-tests` and
GitLab CI's `linux-build` job runs, with every other `*-tests` program
(`build/ci/linux-build.sh`). Nothing inside the server runs the tester
and neither regression suite runs the program, so where that job does not run
it is run by hand from the build directory. And by the regression fixture
`API/EventsJavaScript.cs`, which the Linux suite runs: one test for each event
`API/Events.cs` tests on Windows, with a handler mirroring the VBScript or JScript
one there - the two backup events among them, the backup started with
`POST /api/v1/backup`; the limits above; `build.script_languages`; a VBScript file
refused at load with the sentence that says to set `JavaScript`; and JavaScript
counterparts of fifteen of the Linux suite's twenty-four scripting tests, whose
originals still skip there, with the language as the reason. On Windows,
`API/Events.cs` holds the refusals: a JavaScript file refused at load and never run
as JScript, a rule function reaching one reported once, the JScript handler in force
still deciding after JavaScript is stored, and a reload into JavaScript with no file
refused with its line.
