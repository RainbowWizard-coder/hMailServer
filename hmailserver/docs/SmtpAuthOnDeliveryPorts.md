# AUTH on a delivery port: On, Off or Honeypot

Port 25 is where other servers deliver mail (RFC 5321); mail clients submit on
587 (RFC 6409). Up to 6.3.5 every SMTP listener offered `AUTH` in its `EHLO`
reply and checked the password it was given, port 25 included - the port every
scanner on the internet reaches first, and so the one a password spray goes to.
From 6.3.6 (GitLab work item #54) each SMTP listener says what it does with
`AUTH`:

| SMTP authentication | `EHLO` | What `AUTH` does |
|---|---|---|
| **On** | Offers `AUTH` | What it always did: the password is checked, and failed logons count towards the logon auto-ban |
| **Off** | Does not offer `AUTH` | Answered `502 5.5.1 AUTH not permitted on this port.` No account is looked up and nothing the client sent is evaluated. The first `AUTH` of a session bans the address |
| **Honeypot** | Offers `AUTH` exactly as On does - the same mechanisms on the same conditions | As Off: `502 5.5.1`, nothing evaluated, the first `AUTH` of a session bans the address. The offer is the bait |

`HELO` has no capability list, so a client that only says `HELO` never sees the
offer either way (and its `502` comes without the enhanced code `5.5.1`, which,
as on every reply, is sent only after `EHLO`). Off and Honeypot are not a password check that fails: the
answer, and the time it takes, are the same for every mechanism, every user name
and every password, so there is no "invalid credentials" to tell a real account
from a made-up one. A correct password is refused like any other.

POP3 and IMAP listeners are always On: the choice is refused on them.

## Defaults

* **A new installation**: port 25 is **Honeypot**, port 587 **On**. It is made so
  by the step that gives a new database its certificate and STARTTLS
  ([SecureDefaults.md](SecureDefaults.md)), in the same save as port 25's
  certificate. The create scripts themselves, and so the regression benches, make
  every listener On.
* **An upgrade**: every existing listener stays **On**, and the database step
  (6107 to 6108) writes On into every row it finds. Old clients that still sign in
  on 25 go on doing so. The setup checklist says so - *No password on port 25*,
  `delivery_port_auth_on` ([SetupChecklist.md](SetupChecklist.md)) - until the
  listener is changed. Changing it is what arms the ban, so it is an
  administrator's step and the checklist offers no one-click fix: move the
  clients to 587 first, then set port 25 to Honeypot or Off.
* **Restoring the default ports** (`TCPIPPorts.SetDefault`, the Control Panel's
  *Restore defaults*) writes the listeners a create script makes, every one On; it
  never arms a ban.

## The ban

The first `AUTH` a session sends to an Off or Honeypot listener writes the same
kind of IP range every other auto-ban writes ([AutoBan.md](AutoBan.md)): the one
address, priority 100, allowing nothing, expiring after `AutoBanMinutes`
(*Minutes to ban* in the Control Panel; 60 as shipped), named
`Auto-ban: AUTH on a delivery port <address>`. It is mirrored below the server
by `AutoBanFirewall` and `AutoBanCommand` when those are set (they are off unless
already on), widened to its reverse-name neighbours when
`AutoBanWidenByReverseName` is on, and lifted by the expiry sweep. It does not
depend on `AutoBanOnLogonFailure`: the listener's setting is what arms it.

**One ban per address while it lasts**, not one per `AUTH` line or per session.
A session's later `AUTH` lines are answered `502` and written to the debug log
only. A second session from the same address - one that connected before the ban
and so was not refused at the door - finds the range in force and writes nothing.
A range of that name whose time has run out but that the minute's sweep has not
lifted yet is replaced at once. A banned address is refused at its next
connection, as every banned address is; the session that earned the ban is not
cut off, and goes on as inbound delivery.

**Never banned**, though still answered `502`:

* this computer: `127.0.0.0/8`, `::1` and `::ffff:127.0.0.0/104`;
* an address `AutoBanNeverBan` lists, alone or inside a block - this is where an
  incoming relay that sends `AUTH` to port 25 is named;
* an address the never-ban list by reverse name spares
  ([AutoBan.md](AutoBan.md)), as far as the session's own reverse lookup already
  knows - nothing is looked up inside the command;
* a session that has authenticated, which on a delivery port only a trusted
  `XCLIENT` upstream can have vouched for;
* every address while `AutoBanMinutes` is 0.

The address judged is the client's: behind a trusted `XCLIENT` or PROXY
upstream, the one it names.

What does not change: `MAIL FROM` from an unauthenticated client is inbound
delivery before the refused `AUTH` and after it, and `RequireSMTPAuthLocalToLocal`
is decided where it always was. `STARTTLS` is not required on 25 by any of this -
that would drop servers that cannot start TLS. And `AUTH` on a delivery port is
not a spam score: a delivery port is not a place to try a password.

## What is written in the log

One line per event, in the application log, never with what followed the
mechanism - a user name, a password or an initial response:

| Line | When |
|---|---|
| `Auto-ban: <address> is banned for <n> minutes after AUTH on a delivery port (port <port>, mechanism <mechanism>, listener off\|honeypot)` | A ban was written. The line the packaged fail2ban filter matches, its fifth expression. Written after the never-ban list by reverse name has had its answer when that list has rows, and not at all for an address it spares |
| `AUTH on a delivery port: <address> sent AUTH to port <port> (mechanism <mechanism>, listener off\|honeypot); refused, <why>.` | The session's first `AUTH` wrote no ban: `banned already`, or `not banned:` and the reason - this computer's loopback address, `AutoBanNeverBan` lists it, the never-ban list's reason, the session had authenticated, `AutoBanMinutes is 0`, or the ban could not be saved. No fail2ban expression matches it |

The mechanism is written only as a SASL mechanism name (RFC 4422: one to twenty
of `A-Z`, `0-9`, `-` and `_`, in upper case); anything else is
`(not a mechanism name)`, and an `AUTH` with none is `(none)`. The SMTP protocol
log, when it is on, writes the command as it writes every `AUTH`, with the
password masked.

## Error codes

| Code | Severity | Said when |
|---|---|---|
| HM6099 | High | The ban's range could not be saved, so the address is not banned (the error every auto-ban writer reports) |
| HM7642 | High | A range of the ban's name whose time had run out could not be deleted to be written again, so the address is not banned |

## DisableAUTHList

`DisableAUTHList` - *Do not offer AUTH on these local TCP ports* on the
Authentication page - is older and stays as it was, for an On listener only: a
port it names does not offer `AUTH` and answers `504 Authentication not
enabled.`, with no ban. A listener that is Off or Honeypot is that, whatever the
list says. The checklist calls an On port 25 that the list names done, without a
ban (`delivery_port_auth_disabled`).

## Where to set it

| Where | How |
|---|---|
| Control Panel | *TCP/IP ports*, a port's dialog: *SMTP authentication* - On, Off or Honeypot, fixed at On for POP3 and IMAP. The ports list has a column for it |
| Control Deck | *Ports*: the port's editor (`smtp_authentication`) and a column in the list |
| REST | `GET /api/v1/ports`, `POST /api/v1/ports`, `PUT /api/v1/ports/{id}`: `smtp_authentication`, `on` (the default when a body leaves it out), `off` or `honeypot`. `PUT` takes the whole port, so send back what `GET` returned with the one field changed. Refused (400): any other word, and `off` or `honeypot` on a port that is not SMTP |
| COM | `TCPIPPort.SMTPAuthentication`: 0 On, 1 Off, 2 Honeypot. Another number is refused by the property, and Off or Honeypot on a POP3 or IMAP port by `Save` |
| Database | `hm_tcpipports.portsmtpauth`, `tinyint`/`smallint`, 0 by default (schema 6108) |
| Backup | The port's `SMTPAuthentication` attribute; a backup taken before 6.3.6 restores every port On |

Like every property of a port, it takes effect when the server is reinitialized.

## Where the code is

`SMTPAuthenticationMode` in `Common/TCPIP/SocketConstants.h`; the listener hands
it to each connection (`TCPServer`, `TCPConnection::SetListenerSMTPAuthentication`);
`SMTPConnection::RefuseAuthOnDeliveryPort_`, `GetAuthIsAdvertised_` and
`AuthMechanismForLog_` decide and answer; `AccountLogon::BanForAuthOnDeliveryPort`
writes the range and `AutoBanFirewall::BannedForAuthOnDeliveryPort` the line.
`PersistentTCPIPPort` stores and refuses it, `NewInstallDefaults` makes port 25 a
honeypot, `SetupChecklist.cpp` judges port 25.

The regression fixtures are `test/RegressionTests/Security/SmtpAuthOnDeliveryPorts.cs`
(REST and raw sockets, both suites: a honeypot's offer equal to the On
listener's, the account's own password refused there and taken on the On
listener, Off offering nothing, the session going on as delivery, the ban in force
at the next `XCLIENT`, one range for several `AUTH` lines and two sessions, the
loopback, `AutoBanNeverBan` and `AutoBanMinutes` 0 refused and not banned, the
mechanism logged only as a name, the route's refusals),
`SmtpAuthOnDeliveryPortsCom.cs` (COM, Windows: the property, its refusals and
`SetDefault`), `Security/NewInstallDefaults.cs` (port 25 a honeypot after
`--apply-new-install-defaults`) and `API/RestApiSetupChecklist.cs` (the item's
findings).
