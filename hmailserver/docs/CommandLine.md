Administering the server from a command line
============================================

Two clients, one vocabulary, one API: `hmctl` for any shell, and the
`HmailServer` PowerShell module for PowerShell. Both speak the REST API, so both
work against a Windows server, a Linux server, or a server on another machine,
and neither needs COM or the Control Panel.

This is not a replacement for either administration program. The Windows Control
Panel is the place to configure a server, and the browser Control Deck is that
place on Linux. A command line is for the things a command line is better at:
making fifty accounts from a spreadsheet, reading one setting in a script,
watching the delivery queue, asking what a rule criterion would decide, or doing
the same thing to twenty servers.

| | |
|---|---|
| `hmailserver/source/Tools/Cli/hmctl` | One Python 3 file, no dependencies beyond the standard library. The Linux packages install it as `/usr/bin/hmctl`. |
| `hmailserver/source/Tools/Cli/HmailServer.psm1` | The PowerShell module: `Get-HmDomain`, `New-HmAccount`, `Set-HmSetting` and the rest, answering objects rather than text. |
| `build/check-cli.py` | Runs both against a recorded server and asserts what each one sent, so the two cannot drift apart in what they ask for. In CI. |

`build/hmconfig.ps1` is a different tool and stays: it exports the whole
configuration as one reviewable JSON document and applies it to another machine,
over COM, on Windows. These two are per-object verbs over HTTP.

Credentials
-----------

Either the administrator password, sent as HTTP Basic with the user name
`Administrator`, or an API key, sent as a bearer token. An API key is the better
answer for anything unattended: it can be read-only, it can be restricted to one
domain, and it can be revoked without changing the administrator password.

```bash
export HMAIL_URL=https://mail.example.com:8045
export HMAIL_API_KEY="$(cat /etc/hmailserver/api.key)"
hmctl domain list
```

```powershell
Connect-HmServer -Url https://mail.example.com:8045 -ApiKey (Get-Content .\api.key)
Get-HmDomain
```

`hmctl` reads `HMAIL_URL`, `HMAIL_API_KEY`, `HMAIL_PASSWORD` and
`HMAIL_PASSWORD_FILE`, and takes `--url`, `--api-key`, `--password` and
`--password-file` wherever you type them - before the verb or after it. With no
credential at all it asks for one, but only when there is a console to answer:
in a scheduled task or a pipeline it says what is missing and stops, rather than
waiting at a prompt nobody can see.

**`--insecure` is refused for anything but the loopback.** A self-signed
certificate on `localhost` is the ordinary case and nothing is on the wire; an
administrator password sent to a remote host over a connection nobody verified
is not a convenience, and this will not make it one. Give the server a
certificate the client trusts.

hmctl
-----

```
hmctl [--url URL] [--api-key KEY | --password PASSWORD | --password-file FILE]
      [--insecure] [--json] <resource> <verb> [arguments]
```

| Resource | Verbs | Notes |
|---|---|---|
| `domain` | list, get, create, update, delete | |
| `account` | list, get, create, update, delete | Listed and created under `--domain`, read and written at the address: which is how the API is shaped. |
| `alias` | list, create, delete | Under `--domain`. |
| `list` | list, create, update, delete | Distribution lists, under `--domain`. |
| `group` | list, get, create, update, delete | Account groups; `member` handles the accounts in one. |
| `rule`, `route`, `port`, `iprange`, `certificate` | list, create, update, delete | Server-wide. |
| `fetchaccount` | list, get, create, update, delete | Under `--address`. |
| `apikey` | list, create, delete | Administrator password only, never an API key. |
| `suppression` | list, create, delete | A domain's suppression list, under `--domain`; `list` takes `--search`, and an entry is deleted by its address. |
| `bounce` | list | The bounce counts behind a domain's suppression list, under `--domain`, with `--search`. |
| `bounces` | classify, expire | How the server would read a status or a reply, and the list's hourly sweep run now. |
| `complaint` | list | The abuse reports a domain's mail has drawn, under `--domain`, with `--search` over the complained-of address, the sender, the subject and the provider. |
| `complaints` | rates, expire | Every domain's complaint rate over the last seven days, and the feedback loops' hourly sweep run now. |
| `openpgp` | list, show, store, remove, publish | The OpenPGP PUBLIC keys this server publishes ([OpenPGP.md](OpenPGP.md)): a domain's whole listing, one account's document, a key stored from a file or from standard input, one removed by fingerprint, and an account's own publishing switched on or off. |
| `queue` | list, retry, delete | |
| `webhook` | list, get, create, update, delete | The outbound webhook subscriptions: where this server's mail events go, signed with what, and confined to which domains and addresses. The secret is write-only and never read back ([Webhooks.md](Webhooks.md)). |
| `webhooks` | deliveries, run, test | The delivery queue with `--webhook`, `--state` and `--limit`; one delivery pass now; and one event raised through the path a real one takes, to see whether it arrives. |
| `quarantine` | list, release, delete | |
| `settings` | get, set, breach-test | A group whole, or one setting: `settings`, `antispam`, `antivirus`, `logging`, `backup`, `cache`, `indexing`, `scripting`, `bounces`, `complaints`, `directories`, `openpgp`. `breach-test [--endpoint URL]` asks the password breach check's endpoint for a range and exits 1 when it could not be used ([PasswordBreachCheck.md](PasswordBreachCheck.md)). |
| `ini` | list, get, set, delete | The server settings that were the `[Settings]` section of `hMailServer.ini` - stored in the database since schema 6042, under the names they have always had. `list` shows a secret as `(set)`; `get` reads it. |
| `log` | list, tail | |
| `backup` | status, start | The server's own backup. |
| `dkim` | status, rotate, check, switch, retire, cancel, run | A domain's DKIM key rotation and the records it needs published. The schedule is five keys of the domain: `domain update`. |
| `mailbox` | status, move | Renaming a mailbox, or moving it to another domain on this server. `status` says whether anything is holding it and what it has been through; `move` takes the address it is to have. [MailboxMove.md](MailboxMove.md). |
| `app-password` | list, create, delete | Under an account's address. |
| `device` | list, revoke | The clients that have signed in to an account - protocol, address, program, which credential, first and last seen - under its address. `revoke ID` ends what one holds now (its browser session, its live IMAP, POP3 and SMTP connections); `--credential` also turns off the app password it signed in with, `--forget` removes the entry ([DevicesAndSessions.md](DevicesAndSessions.md)). |
| `match` | *(a criterion and a value)* | What a rule criterion would decide, without a rule. |
| `accounts` | import, export | A CSV file, with a dry run. |
| `report` | *(a section)* | What happened on this server, per domain and per day. `sections` first: it says what can be reported and what cannot. |
| `api` | *(method and path)* | The escape hatch, for a route newer than this file. |

A few whole commands:

```bash
hmctl domain create --set name=example.com --set active=true
hmctl account create --domain example.com --set address=ann@example.com --set password=...
hmctl account list --domain example.com
hmctl settings get antispam use_spf
hmctl settings set antispam --set use_spf=true --set spam_mark_threshold=5
hmctl settings set --set breach_check_enabled=true
hmctl settings breach-test --endpoint https://mirror.example/range/
hmctl settings set --set session_idle_seconds=900 --set session_bind_to_address=true
hmctl domain update example.com --set session_lifetime_seconds=28800 --set session_one_per_device=1
hmctl device ann@example.com list
hmctl device ann@example.com revoke 0123456789abcdef0123456789abcdef --credential
hmctl queue list --json | jq '.[] | select(.retry_count > 3)'
hmctl match contains viagra "cheap viagra here"
hmctl dkim status example.com
hmctl report sections
hmctl report traffic --from 2026-09-01 --to 2026-09-15 --domain example.com
hmctl report mailboxes --top 25 --csv mailboxes.csv
hmctl api POST /api/v1/server/reinitialize
```

`--set name=value` reads `true`, `false`, `null`, numbers and anything starting
with `{` or `[` as JSON and everything else as a string; `--set-string` keeps a
value that looks like one of those a string. `--from-json FILE` takes a whole
object, and `-` reads it from standard input.

Output is a table by default and the server's own JSON with `--json`. The exit
code is 0 for success, 1 for an error, 2 for a usage mistake, 3 when the server
refused the credential and 4 when what you named does not exist - so a script
can tell "no such account" from "wrong password" without reading the text.

Reports
-------

```bash
hmctl report sections
hmctl report summary --from 2026-09-01 --to 2026-09-15
hmctl report senders --domain example.com --top 25
hmctl report traffic --csv traffic.csv
```

The sections are `traffic`, `failures`, `senders`, `recipients`, `mailboxes`,
`volume`, `storage` and `summary`. The window is `--from` and `--to` as
`YYYY-MM-DD`, both inclusive, defaulting to the last thirty days ending today on
the server; `--domain` narrows it to one domain the server hosts; `--top` is how
many rows the sender, recipient and mailbox sections return.

`--csv FILE` writes the table, and `--csv -` writes it to standard output. It is
the **server's** CSV, fetched with `format=csv`, rather than one built here out of
the JSON: two writers of the same table drift, and the file that ends up in a
spreadsheet should be the one the route promises.

Without `--csv`, the section's own note is printed above the table. Read it. The
four sections counted from the message trace are empty when the trace is switched
off - which is the default - and "the message trace is switched off" is not the
same statement as "nothing happened". `hmctl report sections` says which sources
are recording, how long each is kept, and the two questions this server cannot
answer at all: spam and virus counts per domain, and storage growth per domain.
[Reports.md](Reports.md) is why.

DKIM keys that rotate
---------------------

```bash
hmctl domain update example.com --set dkim_rotation_enabled=true    # the schedule, with its defaults
hmctl dkim rotate example.com                     # or start one now; --selector names it
hmctl dkim status example.com                     # what to publish, keep and remove
hmctl dkim status example.com | grep '^  publish' # just the records to add
hmctl dkim check example.com                      # look them up from the server's resolver now
hmctl dkim switch example.com --force             # before the propagation period is over
hmctl dkim retire example.com                     # before the retention period is over
hmctl dkim cancel example.com                     # drop the next key; signing is unchanged
```

`status` prints the phase, then one block per key - current, next, retiring and
retired - with its times and its last check, and under each the records as
`ACTION HOST TXT VALUE` lines: `publish` for the next key, `keep` for the current
and retiring ones, `remove` for a retired selector. Then what is waiting, and until
when, in the server's words. `--json` is the status document itself.

The server does the work on its own schedule; these verbs only make it happen now.
`switch` is refused - exit 1, with the status and the reason word - until the next
key's record resolves with that key in a check made for the request, and without
`--force` until it has resolved for the whole propagation period. The other
refusals are the same shape: a second `rotate` while a next key is staged, a
`retire` with nothing retiring. `run` makes one pass over every domain, and
`--seconds-ahead` moves the clock for that pass, which is how a test waits out three
days; it needs the administrator password or a server-wide key. **It is a test's
tool, not for a production server**: a switch or retire the pass makes happens at
once, early, and the times it records are the moved clock's and stay recorded, so
the server's own passes afterwards wait that much longer.
[DkimKeyRotation.md](DkimKeyRotation.md) is the whole walk.

In the PowerShell module the same verbs are `Get-HmDkimRotation` (with
`-Records`, one object per record), `Start-HmDkimRotation`, `Test-HmDkimRotation`,
`Complete-HmDkimRotation`, `Remove-HmDkimRetiringKey`, `Stop-HmDkimRotation` and
`Invoke-HmDkimRotation`.

Bounces and the suppression list
--------------------------------

```bash
hmctl settings set bounces --set processing_enabled=true --set suppression_action=observe
hmctl suppression list --domain example.com --search elsewhere.test
hmctl suppression create --domain example.com --set address=gone@elsewhere.test --set-string note="old export" --set expires_days=90
hmctl suppression delete gone@elsewhere.test --domain example.com
hmctl bounce list --domain example.com
hmctl bounces classify "550 5.1.1 <gone@elsewhere.test>: User unknown"
hmctl bounces expire
```

`bounces classify` asks the server rather than a copy of its table, because the
rule that matters is the server's: a code a remote did not send itself - a bare
`550` - is never read as a dead mailbox, and the answer says so when it is not
trusted. Deleting an entry sends the next message to the address and starts its
run of hard bounces again. [BounceSuppression.md](BounceSuppression.md) is the
whole of it.

OpenPGP keys
------------

```bash
hmctl settings set openpgp --set web_key_directory_enabled=true
gpg --armor --export ann@example.com | hmctl openpgp store ann@example.com - --publish
hmctl openpgp list example.com
hmctl openpgp show ann@example.com
hmctl openpgp publish ann@example.com off
hmctl openpgp remove ann@example.com 866ac3a0785f51afd5eada4635efb5d13db90a11
```

```powershell
Set-HmOpenPgpKey -Account ann@example.com -Path .\ann.asc -Publish
Get-HmOpenPgpKey -Domain example.com | Format-Table address, fingerprint, algorithm_name, published
Set-HmOpenPgpPublish -Account ann@example.com -Enabled $false
```

`store` reads the key from a file, or from standard input with `-`, because a key
is thousands of characters and a command line is not where it belongs. Only the
PUBLIC half is ever accepted: a secret key is refused with a sentence saying so,
here and at every other door. A domain listing that is not being served says
which of the three switches is in the way before it prints the table.
[OpenPGP.md](OpenPGP.md) is the whole of it.

Outbound webhooks
-----------------

```bash
hmctl webhook create --set name=ticketing --set url=https://tickets.example/hook \
                     --set-string secret="$SECRET" --set events=message.bounced --set domains=example.com
hmctl webhook list
hmctl webhook update ticketing --set enabled=false
hmctl webhooks test --event message.bounced --domain example.com
hmctl webhooks run
hmctl webhooks deliveries --state dead-lettered
```

`webhooks test` raises a real event through exactly the path a real one takes -
the same matching, the same per-domain switches, the same rate ceiling - and
says how many subscriptions took it rather than pretending one did, so a `0` is
an answer rather than a failure. `webhooks run` makes the delivery pass happen
now rather than within the minute, which is what an administrator wants
immediately after configuring an endpoint. A subscription's shared secret is
never returned by any read, so it is not in `webhook list` and not in a
configuration export; keep it wherever you keep the rest of your secrets.
[Webhooks.md](Webhooks.md) is the whole of it.

Complaints and the feedback loops
---------------------------------

```bash
hmctl settings set complaints --set processing_enabled=true --set-string mailbox=fbl@example.com
hmctl complaint list --domain example.com
hmctl complaint list --domain example.com --search anna@example.com
hmctl complaints rates
hmctl complaints expire
```

```powershell
Get-HmComplaint -Domain example.com | Format-Table address, feedback_type, sender, arrival
Get-HmComplaintRate | Format-Table domain, sent, complaints, rate_ppm, judged
Invoke-HmComplaintMaintenance
```

`complaints rates` asks the server rather than working the rate out from the
listing, because the window, the volume below which a rate is not judged and the
denominator are the server's, and a second implementation of them would drift
from the one the `complaints.rate.high` alert fires on. A domain that has sent too
little is said to be so rather than given a rate.
[Complaints.md](Complaints.md) is the whole of it.

Renaming a mailbox, and moving one to another domain
----------------------------------------------------

```bash
hmctl mailbox status ann@example.com
hmctl mailbox move ann@example.com anne@example.com
hmctl mailbox move ann@example.com ann@example.net
hmctl mailbox move ann@example.com anne@example.com --drop-alias
```

The new address carries the domain, so a rename and a move to another domain on
this server are the same command. The account keeps its identity: its messages,
folders, rules, filters, application passwords, second factor, contacts,
calendars, preferences, password history, sender lists, group memberships and
role all follow it, which is what a second account and a copy could never do.

The old address is left behind as an alias unless `--drop-alias` says otherwise,
because a correspondent's address book, a list somebody signed up to and a
printer configured three years ago all still hold it. `status` first: a mailbox a
POP3 or IMAP session is holding cannot be moved, and it says which.

In PowerShell, `Get-HmMailboxMove` and `Move-HmMailbox`. `Move-HmMailbox` asks
before it moves anything and `-WhatIf` sends nothing at all.

[MailboxMove.md](MailboxMove.md) says what moves, what deliberately does not, and
what an interrupted move leaves behind.

Accounts from a spreadsheet
---------------------------

```bash
hmctl accounts import new-people.csv --dry-run
hmctl accounts import new-people.csv
hmctl accounts export example.com accounts.csv
```

```powershell
Import-HmAccount -Path .\new-people.csv -WhatIf
Import-HmAccount -Path .\new-people.csv
Export-HmAccount -Domain example.com -Path .\accounts.csv
```

The file is UTF-8 - in Excel, *CSV UTF-8* - and its header row names the
columns. These are the columns, and there are no others:

| Column | Required | Sent as | Notes |
|---|---|---|---|
| `address` | yes | text | Created under the address's own domain. |
| `password` | yes | text | Digits alone are still a password: `20260915` is sent as the text it is. Never trimmed: white space at either end is refused. Judged by the server's password policy and, when it is switched on, the breach check ([PasswordBreachCheck.md](PasswordBreachCheck.md)), then stored under its configured hash. |
| `first_name` | | text | At most 60 characters, counted as UTF-16 code units, so a character outside the Basic Multilingual Plane (an emoji) counts two. |
| `last_name` | | text | As `first_name`. |
| `active` | | true or false | `true`, `yes`, `on` or `1`; `false`, `no`, `off` or `0`. Default true. |
| `max_size_mb` | | a whole number | Megabytes; `0` is no limit, and is the default. |
| `admin_level` | | `user`, `domain` or `server` | Default `user`. |

```csv
address,password,first_name,last_name,active,max_size_mb,admin_level
ann@example.com,Sup3rSecret!,Ann,Adams,true,500,user
bob@example.com,An0therOne!,Bob,"Brown, Jr",true,,domain
```

A person's name is two columns, not one: a name split at a space is a guess
("Mary Ann Smith"), and the file has to come back out exactly as it went in. The
columns may come in any order, their names in any case, and every cell but the
password is trimmed. An empty cell is not sent, so the server's default applies.
A value with a comma in it goes in quotes, as `"Brown, Jr"` does; a row with more
cells than the header has columns is refused rather than shifted along.

Each row becomes one `POST /api/v1/domains/<domain>/accounts`, and that route
refuses a field it does not know by name rather than ignoring it - so **a column
that is not in the table is refused before anything is sent**, a `name` column
included. The clients also refuse, before anything is sent and in the words the
server would use, a row with no password, an `active` that is not one of the
eight words, a `max_size_mb` that is not a whole number, an `admin_level` that is
not one of the three and a name past its 60 characters - which is why `--dry-run`
and `-WhatIf` refuse the rows the create would refuse for their cells. They
refuse one row the server would take: a password with a space or a tab at its
start or end, which a spreadsheet cell rarely means and which trimming would
silently turn into a different password. What only the server can judge - the
password policy, the domain's account limit, an address it does not accept - is
refused per row when the import runs, and listed with its address and the
server's reason (hmctl adds the line number); a dry run cannot foresee those.

**An address that already exists is left alone and counted**, never overwritten:
an import that silently reset a password would lock a hundred people out with
one command. `--dry-run` makes no request that changes anything and prints what
would happen, line by line; the exit code is 1 when any row was refused.

`admin_level` is decided by who is asking, as it is everywhere else: the
administrator password, or a key issued for every domain, may make a server
administrator; a key restricted to named domains may make users and domain
administrators, and a row asking it for `server` is refused with a 403.

The export writes the same columns less `password`, from the domain's account
listing, because the server does not have the passwords to give. Add a password
column and change the addresses, and the exported file imports into another
domain or another server as the same accounts: names, sizes, levels and whether
each is active. `build/check-cli.py` holds both clients to exactly that round
trip, and `RegressionTests/API/RestApiAccountImport.cs` holds the server to it.

The configuration as one file
-----------------------------

```bash
hmctl config export server.json          # the whole configuration, as a document
hmctl config diff server.json            # what would change; exit 1 if anything would
hmctl config apply server.json           # the plan, and nothing else
hmctl config apply server.json --force   # the plan, applied
```

The document holds the settings groups, the `hMailServer.ini` keys the server
exposes, the domains and their accounts, aliases and distribution lists, the
groups, the rules, the routes, the listeners and the IP ranges. It is sorted
throughout and carries nothing the server allocated - no identifier, no count,
no time - so **two exports of an unchanged server are the same bytes**. That is
what makes it usable in a repository: a diff means a change, and nothing churns.

`apply` is a plan unless you add `--force`, and a **deletion needs `--allow-delete`
on top of that**. Without it, an entry the server has and the document does not
is left alone and counted, because the common case is a document written from
one server and applied to another that has things of its own.

What it does not carry, and why:

* **Passwords.** The server does not give them out, so they are not in the
  document. An account in the document that does not exist on the server is
  refused with that reason rather than created with a password nobody chose;
  add a `password` field for the accounts you mean to create.
* **The secrets among the ini keys.** `GET /api/v1/settings/ini` lists a key
  whose name says its value is a secret - `OAuth2HmacSecret`, `BATVSecret`,
  `ServiceAccountPassword`, `MetricsServerAuthToken`, `PasswordPepper` and the
  like - as `(set)` or empty, never as its value, and both clients leave such
  a key out of the document rather than carry a placeholder that `apply` would
  write over the real secret. `hmctl ini get <name>` reads one by name when it
  is wanted. A key *file* is a path, not a secret, and stays in.
* **Certificates and the directories.** A certificate is files on the server's
  own disk, and the directories are that machine's paths; copying either between
  machines would describe a server that does not exist.
* **What lives under an account** - application passwords, fetch accounts,
  folder permissions - which are the account holder's rather than the
  configuration's.
* **The selector and key files of a domain whose DKIM key rotation is on.** The
  rotation moves them on its own schedule, so an export taken before a switch
  would differ from the server after it, and applying it would ask for the old
  key back - which the server refuses. They are left out of the document, out of
  the comparison and out of what `apply` sends while the rotation is on, in the
  document or on the server; the schedule itself (`dkim_rotation_enabled` and the
  four periods) is configuration and travels. A rotation started by hand holds
  them for a while with the schedule off - while its next key is staged or its old
  selector is still being kept - and the server refuses a change to them then
  too. The document keeps them for such a domain, because they are its
  configuration once that rotation is over, but when `diff` or `apply` finds
  them different it asks the server's rotation route, and while the server says
  it holds them they are left out of the comparison and of what is sent.

### A repository that is the configuration

There is no daemon; the loop is two commands and whatever runs them.

```yaml
# On a pull request: does the repository still match the server?
- run: hmctl config diff server.json        # exit 1 fails the job

# On merge: make the server match the repository.
- run: hmctl config apply server.json --force
```

Give the job an API key rather than the administrator password, keep the key in
the runner's secret store, and let the diff run on a schedule as well: a server
that has drifted from its repository is worth knowing about before somebody
needs the repository to be true.

The PowerShell module
---------------------

```powershell
Import-Module .\hmailserver\source\Tools\Cli\HmailServer.psm1
Connect-HmServer -Url https://localhost:8045 -Insecure

Get-HmDomain | Where-Object active -eq $false
Get-HmAccount -Domain example.com | Sort-Object max_size_mb -Descending | Select-Object -First 10
New-HmAccount -Domain example.com -Address ann@example.com -Password (Read-Host -AsSecureString)
Set-HmAccount -Address ann@example.com -Property @{ max_size_mb = 500 }
Set-HmSetting -Group antispam -Property @{ use_spf = $true }
Test-HmBreachCheckEndpoint -Url https://mirror.example/range/
Get-HmQueue | Where-Object retry_count -gt 3 | Start-HmQueueDelivery
Get-HmDkimRotation -Domain example.com -Records | Where-Object Action -eq publish
Get-HmSuppression -Domain example.com | Where-Object category -eq 'hard' | Format-Table address, status, created
Add-HmSuppression -Domain example.com -Address gone@elsewhere.test -Note 'old export' -ExpiresDays 90
Get-HmBounceCount -Domain example.com | Sort-Object consecutive_hard -Descending | Select-Object -First 10
Test-HmBounceStatus -Status '550 5.2.2 Mailbox full'
Get-HmWebhook | Format-Table name, url, events, enabled, pending, dead_lettered
New-HmWebhook -Name ticketing -Url https://tickets.example/hook -Secret $s -Events message.bounced
Get-HmWebhookDelivery -State dead-lettered | Format-Table id, webhook, event, attempts, error
Test-HmWebhook -Event message.bounced -Domain example.com
Get-HmOpenPgpKey -Domain example.com | Where-Object published -eq $false
Import-HmAccount -Path .\new-people.csv -WhatIf
```

Every function answers objects, so `Where-Object`, `Sort-Object`, `Export-Csv`
and the rest work on them; nothing in the module formats anything. Everything
that changes the server supports `-WhatIf` and `-Confirm`, and deleting a domain
or an account asks before it acts unless told not to. A domain name or an
address completes from the server itself once `Connect-HmServer` has run.

`Invoke-HmApi -Method GET -Path /api/v1/...` is the escape hatch, and is what
every other function is built on.

What neither client does
------------------------

* **It does not read or write the message store.** Mail is IMAP's business.
* **It is not a backup tool.** `hmctl backup start` asks the server to run its
  own backup, and that is all.
* **It sets no administrator password.** `hmailserver --set-admin-password` on
  the server does that, and the Control Panel does it on Windows.
* **It cannot turn the REST listener on**, since both clients need it. On a
  Linux server `sudo hmailserver --set-setting RestApiPort=8080` stores the
  setting - the database is the settings store since schema 6042, so editing
  `hMailServer.ini` does not - and `sudo systemctl restart hmailserver` starts
  the listener. The same verb stores any other key the server's settings
  loader reads (`build/ini-settings-baseline.txt`), for the ones needed before
  the API can be reached; the webmail's branding and file-link keys, read by
  name where they are used, are stored over the API. A key whose value is a
  secret is refused with its value on the command line, where the shell's
  history, the process list and sudo's log would keep it: give the name alone,
  `sudo hmailserver --set-setting SRSSecret`, and the value on standard input,
  typed at a prompt that does not echo it or piped in. A port must be a number
  from 0 to 65535 and `RestApiBindAddress` an IP address or `localhost`. On
  Windows the Control Panel does this over COM.
* **Neither invents a field.** A field you did not give is one the server
  decides; the two clients were made to send the same request for the same
  command, and `build/check-cli.py` fails if they stop doing so.
