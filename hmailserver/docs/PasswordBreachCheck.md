Checking new passwords against published breaches
=================================================

When a password is chosen - by the person it belongs to or by an administrator -
the server can check it against the published corpus of passwords that have
leaked from breaches elsewhere, and refuse one that is in it. Those are the
passwords an attacker tries first, whatever the password policy says about their
length or their digits.

It is **off by default**, because it sends something off the server. What it
sends is five characters of the password's SHA-1 hash and nothing else; the
section below says exactly what, and what the endpoint can learn from it.

It is checked **when a password is chosen, never at logon**. A password that
works today keeps working after the check is turned on, exactly as with the
password policy: refusing a password somebody can open their mailbox with today
would lock them out in the name of protecting them.

Turning it on
-------------

Control Panel → **Administrative access** → **Password** → the *Passwords found
in published breaches* card. The same four settings are in the Control Deck's
Settings view (the Server group), in `GET`/`PUT /api/v1/settings`, on the COM
`Settings` object, and in `hmctl settings set`:

```bash
hmctl settings set --set breach_check_enabled=true
hmctl settings set --set breach_check_minimum_count=1 --set-string breach_check_unreachable_action=allow
```

The values live in the `hm_settings` table, not in `hMailServer.ini`, so a Control
Panel or a Deck on another machine reads and writes them and a configuration
backup contains them. They are read at every password change: a change applies to
the next one, with no restart.

| Setting | `hm_settings` name | COM property on `Settings` | REST key under `/api/v1/settings` | Default | What it means |
|---|---|---|---|---|---|
| Refuse a new password found in published data breaches | `BreachCheckEnabled` | `BreachCheckEnabled` | `breach_check_enabled` | off | The whole feature. Off, nothing is sent and nothing below has any effect. |
| Range endpoint | `BreachCheckUrl` | `BreachCheckUrl` | `breach_check_url` | `https://api.pwnedpasswords.com/range/` | Where the five characters are sent. See *Running your own mirror*. |
| Refuse when it has appeared at least this many times | `BreachCheckMinimumCount` | `BreachCheckMinimumCount` | `breach_check_minimum_count` | `1` | A password seen in the corpus this many times or more is refused. 1 refuses anything that has appeared at all; below 1 is refused as a value. |
| When the endpoint cannot be reached | `BreachCheckUnreachableAction` | `BreachCheckUnreachableAction` | `breach_check_unreachable_action` (`allow`, `refuse`) | `0` (`allow`) | `0` lets the change through without the check; `1` refuses it. Both are recorded in the error log. |

The names say *BreachCheck* and not *Password* on purpose: the audit trail records
any setting whose name contains "password" as `(changed)` and never as a value,
and an auditor needs to read that this one went from `false` to `true`.

What leaves the server
----------------------

The password is converted to UTF-8 and hashed with SHA-1, giving forty
hexadecimal characters. The first five are the **prefix**; the other thirty-five
are the **suffix**. For the password `password`:

```
SHA-1   5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8
prefix  5BAA6
suffix       1E4C9B93F3F0682250B6CF8331B7EE68FD8
```

The server sends one request, and this is all of it:

```
GET /range/5BAA6 HTTP/1.0
Host: api.pwnedpasswords.com
User-Agent: hMailServer
Accept: application/json
Add-Padding: true
Connection: close
```

The endpoint answers with every suffix in the corpus that follows that prefix -
several hundred of them - each with how many times it has been seen. An extract,
with the counts made up and the second line a padding entry:

```
1E4C9B93F3F0682250B6CF8331B7EE68FD8:9545824
1E4E1D3DAA8E4E5B5B7ECAF5F1C2B7D3C1D:0
...
```

and the server looks for the suffix in that list **itself**. `Add-Padding: true`
asks the endpoint to add entries with a count of zero, so that the number of lines
in the answer - and so its size on the wire - says nothing about which prefix was
asked for either. A padding entry can never match, because its count is below any
minimum.

**What the endpoint learns:** a five-character prefix - one of 1,048,576, and the
same for every password whose hash begins with it - the address this server
connected from, and the time, which says that somebody chose a password on this
server at that moment. It does not learn the password, the hash, the suffix, which
account it was for, or whether the password was found. Over https, whoever is
between the two learns only that this server contacted that host. If `HttpProxy`
is set in `hMailServer.ini`, the request goes through the proxy, which then learns
the host and the time as well. A mirror on this machine - `localhost`, an address
in `127.0.0.0/8`, or `::1` - is asked directly, never through a proxy elsewhere,
which would otherwise see a plain-http request whole, prefix included, and connect
to its own loopback address rather than this server's; only a proxy that is itself
on this machine is still used for it.

**What is never written to any log:** the password, its hash, the prefix, the
suffix and the request path. A refusal is logged naming whose password it was - the
account's address, the account an app password belongs to, or the administrator -
and nothing about the password; an endpoint that could not be used is logged with
the reason, which names the host at most.

The default endpoint, `api.pwnedpasswords.com`, is a **third-party service**,
Have I Been Pwned's Pwned Passwords. Pointing the setting at it is a decision to
send those five characters, this server's address and the time to that operator
at every password change. An organisation that cannot make that decision runs its
own mirror.

Where it applies, and where it does not
---------------------------------------

The check runs at every place a password is **chosen**:

| Where | How a refusal arrives |
|---|---|
| The Control Panel's account dialog and new-account box, the Import Tool, `build/hmconfig.ps1` and event scripts - all through COM `Account.Password` | A COM error carrying the sentence, shown translated in the Control Panel |
| `PUT /api/v1/accounts/{address}` - the Deck's account editor, `hmctl account update`, `Set-HmAccount` | `400` with `"reason": "breached"`, or `503` with `"reason": "breach_check_unavailable"` |
| `POST /api/v1/domains/{domain}/accounts` - the Deck's new account, `hmctl account create`, `hmctl accounts import` and `Import-HmAccount`, `hmctl config apply` | The same |
| `POST /api/v1/me/password` - the webmail's password form | The same, shown translated in the webmail |
| A chosen app password: COM `AppPassword.SetPassword`, `POST /api/v1/accounts/{address}/app-passwords` with a `password` | The same |
| The administrator password: COM `Settings.SetAdministratorPassword`, the Control Panel's *Administrator password* card | A COM error carrying the sentence |

In each, the password policy and the reuse history are asked **first**, so a
password they refuse costs no request, and the breach check comes **before** the
old password is written to the reuse history, so a refused change leaves no trace.
`build/check-password-choice-sites.py` fails the build when a place that chooses a
password stops asking, or asks in the wrong order.

It deliberately does **not** run:

- **At logon**, over IMAP, POP3, SMTP, the webmail, the Deck or the REST API.
- On a password that already exists. Turning the check on does not re-examine
  anybody's current password; it applies to the next one each person chooses.
- On a **generated** app password (`Generate`, the webmail's app passwords, an
  administrator's app password made without a `password`): it is random and
  nobody's.
- On the paths that store a password without anybody choosing one: a restore from
  backup, directory provisioning, and the re-hash of a password to a stronger scheme
  at logon.
- On `hmailserver --set-admin-password` on Linux, which runs with no database open
  and so has no settings to read, and on the installer, which writes the
  administrator password straight to `hMailServer.ini`.
- On secrets that are other systems' passwords: an external account's, a route's
  relay password, the LDAP bind, the database connection, a certificate's
  passphrase, a file link's password.

The two sentences a refusal carries are:

> This password appears in a published data breach, so it would be among the first
> tried against this account. Choose a different one.

> The password could not be checked against published data breaches just now, and
> this server is set to refuse a change it cannot check. Try again later.

The Control Panel, the Deck and the webmail show them in the reader's language;
the REST body carries the English sentence as `error` and the `reason` word beside
it, which is what a page of your own should translate by.

When the endpoint cannot be used
--------------------------------

"Cannot be used" is any of: a host name that does not resolve, a TLS certificate
that does not verify for the host, no complete answer within **eight seconds**, an
HTTP status other than 200, an answer larger than 256 KB, an answer with no entries
in it at all, or an answer that is not a list of suffixes and counts - a captive
portal's sign-in page is not a verdict, and neither is a JSON error or an empty
page.

The eight seconds are for the whole request, on Windows and on Linux alike: looking
the name up, connecting, the TLS handshake and reading the answer to its end share
them. An endpoint that accepts the connection and then says nothing, or sends its
answer a byte at a time, is given up on at eight seconds exactly like one that never
answers. That matters because the check runs on the thread that is changing the
password - one of the REST API's worker threads, or the one a Control Panel is
waiting on - and a hung mirror must not be able to hold it for longer than that. A
name lookup the operating system is still working on when the eight seconds are up is
abandoned rather than waited for.

What happens then is `BreachCheckUnreachableAction`. **Allow** (the default) lets
the change through without the check. **Refuse** refuses it with the second
sentence above - which, while the endpoint is down, refuses every password change
on the server, the administrator password included. Either way the error log
records it:

| Code | When | The change |
|---|---|---|
| `HM6600` | The endpoint could not be used | Let through, as `allow` says |
| `HM6601` | The endpoint could not be used | Refused, as `refuse` says |
| `HM6602` | The stored endpoint is not one the check accepts (a row edited by hand) | As the action says |
| `HM6603` | The password could not be converted to UTF-8 to be hashed | As the action says |
| `HM6604` | Something unexpected ended the check | As the action says |

A broken mirror under `allow` lets every change through silently to the person
and loudly in the error log; watch for `HM6600`. The endpoint test below is how to
find out before anybody changes a password.

**Bulk operations make one request per account.** An import of five thousand
accounts through the Import Tool, `hmctl accounts import`, `Import-HmAccount`, `hmctl config apply` or
`build/hmconfig.ps1` asks the endpoint five thousand times. A public endpoint may
rate-limit that, which shows up as a run of `HM6600` (or refused imports under
`refuse`). Run bulk imports against a mirror, or with the check off.

### Getting out of `refuse` while the endpoint is down

The administrator can still sign in - the check is never applied at logon - so
switch the action back, from any of:

```bash
hmctl settings set --set-string breach_check_unreachable_action=allow
```

```powershell
Set-HmSetting -Property @{ breach_check_unreachable_action = 'allow' }
```

or the Control Panel card, or the Deck's Settings view, or `Settings.BreachCheckUnreachableAction = 0`
over COM. On Linux, a lost administrator password is still reset with
`hmailserver --set-admin-password`, which is not checked.

Testing the endpoint
--------------------

Every surface has a test that asks the endpoint for the prefix `00000` - which
belongs to no password in particular, so the test tells the endpoint nothing - with
the same header, timeout and size limit a real check uses, and reports what came
back. It changes nothing and does not need the check to be on.

| Where | What |
|---|---|
| Control Panel | *Test the endpoint…* on the card, against whatever is typed in the endpoint box, saved or not |
| Control Deck | *Test the breach check endpoint* beside the Server group, against the saved endpoint |
| REST | `POST /api/v1/settings/breach-check/test`, optional body `{"url": "..."}`; answers `{"reachable": true, "status": 200, "entries": 812, "padded": true, "milliseconds": 41}` or `{"reachable": false, "status": 0, "error": "...", "milliseconds": 8004}`. Refused to domain-restricted and read-only keys. |
| `hmctl` | `hmctl settings breach-test [--endpoint URL]` - exits 1 when the endpoint could not be used |
| PowerShell | `Test-HmBreachCheckEndpoint [-Url URL]` |
| COM | `Settings.TestBreachCheckEndpoint(url)` - the number of entries, or an error with the reason; an empty string tests the saved endpoint |

`padded: false` from an endpoint you expected padding from means the answer's size
does say something about the prefix - worth knowing about a mirror.

The endpoint's answer itself is never returned by any of these: a route that makes
the server fetch a URL of the caller's choosing must not also be a way to read that
URL through the server.

Running your own mirror
-----------------------

A mirror is any server that answers `GET <endpoint><prefix>` with lines of
`SUFFIX:COUNT` - thirty-five hexadecimal characters in either case, a colon, a
decimal count, lines ended by CRLF or LF - for the corpus you hold. Ideally it also
honours `Add-Padding: true`; the server does not require it.

It must answer **every** prefix with at least one line. An empty answer is read as
an endpoint that could not be used - a mirror serving empty files, or a proxy
answering for an endpoint it could not reach, would otherwise switch the check off
without a word in any log - so a mirror of a corpus smaller than the public one
answers a prefix it holds nothing under with a padding entry: any thirty-five
hexadecimal characters with a count of `0`, which can never match.

The endpoint setting takes one of two forms:

| Form | Example | The request for `5BAA6` |
|---|---|---|
| A path the prefix is appended to (a slash is added if the path lacks one) | `https://pwned.corp.example/range/` | `https://pwned.corp.example/range/5BAA6` |
| A URL with `{prefix}` where the prefix goes | `https://pwned.corp.example/ranges/{prefix}.txt` | `https://pwned.corp.example/ranges/5BAA6.txt` |

The server refuses, as the setting's value, anything it could not use safely:

- plain `http` to anything but this machine - `localhost`, an address in
  `127.0.0.0/8`, or `::1` (written `http://[::1]:port/`), each parsed as an address
  (`127.example.com` is somebody else's machine and is refused);
- a user name or password in the URL, a fragment (`#`), a query string without
  `{prefix}`, `{prefix}` more than once or any other brace, a character outside
  ASCII, a port outside 1 to 65535.

Its refusal is a sentence naming the rule, and never repeats the URL's path.

https is verified as every outbound connection of this server's is: against the
machine's root certificate store, with the host name checked and TLS 1.2 at the
least. A mirror with a private CA needs that CA in the machine's trust store.

What this deliberately does not do
----------------------------------

- **It does not cache.** Every password change asks. A cache of answers would be a
  list of the prefixes of this server's recent passwords.
- **It does not hash with anything but SHA-1.** The corpus is published as SHA-1;
  SHA-1 here is an index into it, not a way of storing anything. The password is
  stored with the configured password hash, as it always was.
- **It does not tell the person how many times their password was seen.** The count
  narrows the prefix to a handful of candidates, and the refusal sentence is shown on
  pages other people can look over a shoulder at.
- **It does not refuse at logon, or expire passwords that are found later.** A
  password that enters the corpus after it was chosen keeps working. Pair the check
  with the password policy's maximum age, or ask people to change their passwords,
  if that matters where you are.
