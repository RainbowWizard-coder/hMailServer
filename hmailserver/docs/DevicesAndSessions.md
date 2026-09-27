Devices, and the session policies
=================================

A session the webmail holds for a second account added beside the one it signed in as is a session like any other of that account - the same policies, the same list, the same End button - and is listed with `bearer: true` and the badge **A second account in a webmail page**; what that form is and why it exists is in [SecondAccount.md](SecondAccount.md).

Two questions a person and an administrator both ask, and the answers this
server gives from schema 6050 on:

* **What has signed in as me?** Every client that has authenticated to an account
  is one entry of a list - the protocol it spoke, the address it came from, the
  program where the protocol names one, which of the account's credentials it
  used, when it was first and last seen and how many times. The account holder
  reads it on the webmail's Security page; an administrator reads it under the
  address over REST, COM, the Control Deck, `hmctl` and the PowerShell module.
* **Make that one stop.** Revoking an entry ends what the client holds *now* -
  its browser session, and its live IMAP, POP3 and SMTP connections - and, when
  asked, turns off the app password it signed in with, which is what to do
  about a phone that has been stolen.

And four **session policies** an administrator can set, server-wide and per
domain, for every signed-in browser - the webmail, the self-service portal and
the Control Deck: an idle timeout, an absolute lifetime, one session per device,
and a session bound to the address it began on.

A browser session is a browser session whatever proved the person: a password, a
passkey, or the organisation's identity provider. All three are minted by the
same route and governed by the four policies below, and the list says which it
was - `passkey`, `single_sign_on`, `app_password` or `password`. See
[PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md) for the first two.

**Nothing changes on the day you upgrade.** Every one of the eight policy
values reads 0 after the upgrade, and 0 means *not set* rather than *no limit*:
the ceilings this server has always enforced - thirty minutes idle, twelve hours
in all, neither switch on - apply wherever nothing has been set. Nobody is
locked out, and nobody is given a session that outlives the one they had. The
device list starts empty and fills as clients sign in.

What a device is
----------------

No mail protocol carries a device identifier, so this server does not invent
one. A *device* here is a **client**: the protocol, the address and the user
agent, taken together. A phone fetching mail over IMAP is one entry; the same
phone sending over SMTP is another; the same person's browser is a third. That
is the honest identity, and it is enough for the question the list exists to
answer - *is that thing mine?* - because beside each entry is the credential it
used, and where that credential is an app password, **its name**. A person who
made an app password called *Phone* and sees an IMAP client from an address
they do not know using it knows exactly what to revoke.

| Field | What it is |
|---|---|
| `id` | Thirty-two hex characters: the SHA-256 of the protocol, the address and the user agent, halved. What every revoke names. It reveals nothing about where its owner was. |
| `protocol` | `imap`, `pop3`, `smtp`, `web` (a browser session), `dav` (CalDAV or CardDAV), `autodiscover` (Outlook's Autodiscover asked with the account's credentials for the mailboxes delegated to it) or `managesieve`. |
| `address` | The address the client came from. |
| `agent` | The user agent, for the protocols that carry one (`web`, `dav` and `autodiscover`); empty for the rest. IMAP's `ID` command is deliberately not used: clients send it after `LOGIN`, so honouring it would describe a client that no longer exists and make a second entry at the next sign-in. |
| `credential` | `password` (the account's own), `app_password`, `token` (an OAuth2 bearer token), `certificate` (a client certificate) or `single_sign_on` (GSSAPI). SCRAM is always the account's own password, since an app password's hash is not one SCRAM can derive from - which is why an account with a second factor enrolled, or whose password has passed `PasswordPolicyMaximumAgeDays`, cannot sign in over SCRAM at all. |
| `credential_name` | The app password's name, where one was used. Never the password, and never anything derived from it. |
| `first_seen`, `last_seen`, `sign_ins` | When, and how often. The write is throttled to once a minute per client, so a mail program that reconnects every thirty seconds costs one row update a minute; the first sign-in of a client is always written. |
| `revoked`, `revoked_at` | Whether the entry was revoked, and when. |
| `current` | On the account holder's own list only: `true` for the client the request came in on, and for none when the request carried a password rather than a session. |

At most **fifty** entries are kept per account. When a fifty-first client
appears the least recently seen goes, so a client behind a carrier-grade NAT
that presents a new address on every connection cannot grow the table without
bound.

Two things are never in the list. A connection a trusted upstream vouches for
over `XCLIENT LOGIN=` authenticated nobody on this server and carries no
account. And anything that never authenticates at all.

What revoking does, exactly
---------------------------

Revoking an entry ends every session the client holds at that moment:

* a **browser session** - the portal, the webmail, the Deck - has its token
  invalidated, so the next request on that cookie is refused with 401;
* a **live IMAP, POP3 or SMTP connection** is dropped where it stands, mid-
  conversation if that is where it is. The server keeps a weak reference to
  every authenticated connection against its client for this purpose;
* with `credential=revoke` (the *Also turn off the app password it was using*
  box on the webmail, `--credential` in `hmctl`, `-Credential` in PowerShell,
  `RevokeCredential` over COM), the **app password** the client signed in with
  is deactivated - not deleted, so the person can see it on their list and put
  it back if the phone turns up.

CalDAV, CardDAV and ManageSieve clients have no connection to drop: each of
their requests stands on its own short connection and authenticates afresh.
Revoking such a client means revoking the app password behind it, which is
the third bullet.

**A revoke is not a ban.** The entry stays, marked revoked with the time, and a
later sign-in from that client with a credential that still works clears the
mark and starts the record again. A client that must never come back is one
whose credential has been taken away - which is why the app password option is
offered beside every revoke, and why this document says to use it. `forget=true`
removes the entry instead of marking it, for somebody who wants a client they no
longer own off the list; it ends the client's sessions first, so forgetting is
never weaker than revoking.

The account's own revoke is refused (403) to a request that came in on an app
password, or by a browser session that was started with one, as making an app
password is: a credential for a program does not revoke the person's
credentials. An administrator's revoke is recorded in the audit trail.

Where the list is
-----------------

| Surface | Read | Revoke |
|---|---|---|
| Webmail | *Settings* → *Security* → the **Devices** card; the client this page is on is marked *This browser* | *Revoke* beside the entry; the box under the table adds the app password |
| REST, the account | `GET /api/v1/me/devices` | `DELETE /api/v1/me/devices/{id}`, with `?credential=revoke` and `?forget=true` |
| REST, an administrator | `GET /api/v1/accounts/{address}/devices` (`view_accounts`) | `DELETE /api/v1/accounts/{address}/devices/{id}` (`manage_accounts`), the same query options |
| COM | `Account.GetDevices()` - the same JSON array as REST, rather than a collection, so that nothing needs an elevated registration | `Account.RevokeDevice(id, revokeCredential)` - `False` when the account has no such client |
| Control Deck | the **Devices** view: an address, then its list | *Revoke* beside the entry, with the app password box |
| `hmctl` | `hmctl device ann@example.com list` | `hmctl device ann@example.com revoke ID [--credential] [--forget]` |
| PowerShell | `Get-HmDevice -Address ann@example.com` | `Revoke-HmDevice -Address ann@example.com -DeviceId ID [-Credential] [-Forget]` |

The account holder's list and the administrator's are built by one piece of
code from the same rows, so the two can never show a different picture. A key
restricted to named domains reaches the accounts of those domains only.

The session policies
--------------------

A *session* here is a signed-in browser. A mail program's IMAP, POP3 or SMTP
connection is not one in this sense - it authenticates on every connection and
each protocol has its own idle timeout - so these policies govern the cookie,
and revoking a device is what ends a mail connection.

The cookie, `hmailsession`, is HttpOnly and `SameSite=Strict`, and it is marked
`Secure` whenever the browser came over HTTPS: to a listener with its own
certificate, or to the reverse proxy on this machine in front of the plain
loopback listener, which says so with `X-Forwarded-Proto: https`. A browser then
never sends it over plain HTTP. Until the audit before 6.3.4 only the first
counted, so behind the usual proxy the cookie of an HTTPS site went without
`Secure` - and so did the sign-in cookies beside it (`hmoidc`, the OpenID Connect
ticket's `hmsso`, `hmpasskey`), which follow the same rule now. A proxy that
terminates HTTPS in front of the loopback listener should set
`X-Forwarded-Proto: https`; one that does not leaves the cookies without `Secure`,
as they were.

| Policy | Server-wide (`hm_settings`, `Settings.*` over COM, the settings group over REST) | Per domain (a field of the domain over REST and the Deck; `Utilities.SetDomainSessionPolicy` over COM) | What it does |
|---|---|---|---|
| Idle timeout | `SessionIdleSeconds` / `session_idle_seconds` | `session_idle_seconds` | How long a session may go unused before it ends. 0 is *not set*: 1800 seconds applies. At most 2592000 (thirty days). |
| Absolute lifetime | `SessionLifetimeSeconds` / `session_lifetime_seconds` | `session_lifetime_seconds` | How long a session may live at all, however busy it is. 0 is *not set*: 43200 seconds - twelve hours - applies. At most 2592000. The cookie's own `Max-Age` is set to it. |
| One session per device | `SessionOnePerDevice` / `session_one_per_device` | `session_one_per_device`: 0 takes the server's answer, 1 on, 2 off | A second sign-in from a client that already has a session for the account ends the earlier one, so somebody who signs in on a shared machine and forgets leaves nothing behind the next time they sign in there. |
| Bound to the address | `SessionBindToAddress` / `session_bind_to_address` | `session_bind_to_address`: 0, 1 or 2 as above | The session is refused - and ended - from any address but the one it began on, so a stolen cookie is of no use away from the victim's network. It also ends the session of somebody whose address changes underneath them, a phone moving from wi-fi to a mobile network among them, which is why it is off unless asked for. |

A domain's value wins over the server's where it is set, and a domain's switch
takes three values because a domain has to be able to say *not here* about a
server-wide policy as well as *here too*. The domain of a session is the domain
of the account that signed in; the administrator's own session belongs to no
domain and is governed by the server-wide values alone.

The policy is **re-read at every request** of a session rather than remembered
from sign-in, so an administrator who shortens an idle timeout or turns a switch
on means it for the sessions that exist, not only for the next ones.

**The seconds are seconds.** A value under sixty is no use to an administrator
and is exactly what the regression suite needs to prove that an idle session is
refused without waiting a minute; the Deck's field says so. A number past thirty
days is refused at every interface with the same sentence rather than stored: a
session that may live for a year is not a session policy, it is the absence of
one.

To read what is in force - the resolved answer, what the server holds, the
ceilings that apply where nothing is set, and a domain's own row - ask
`GET /api/v1/domains/{domain}/session-policy`, `GET /api/v1/session-policy`
for the server alone, or `Utilities.GetSessionPolicy(domain)` over COM; the
three answer one document. The sign-in itself (`POST /api/v1/session`) says in
its body what the new session is held to.

Setting them:

```bash
hmctl settings set --set session_idle_seconds=900 --set session_bind_to_address=true
hmctl domain update example.com --set session_lifetime_seconds=28800 --set session_one_per_device=1
```

```powershell
Set-HmSetting -Property @{ session_idle_seconds = 900; session_bind_to_address = $true }
Set-HmDomain -Name example.com -Property @{ session_lifetime_seconds = 28800; session_one_per_device = 1 }
```

In the Control Deck the four server-wide values are on the Settings page under
the server group, and the four per-domain ones are the fifth group of a
domain's dialog. In the desktop Control Panel the four server-wide values are on
Connections & protocols > Webmail, the Browser sessions tab, over COM.

What is stored, and what a backup carries
-----------------------------------------

The list is `hm_signins`, one row per account and per client. It is
deliberately **not** in the backup: it describes the sessions of the
installation that wrote it, whose browser sessions lived in memory and whose
connections went with it, so restoring it would show a person a list of clients
none of whose sessions can be ended. The decision a revoke makes that outlives
a session - an app password deactivated - is a column of `hm_apppasswords`,
which the backup carries. The eight policy values are settings and domain
columns, which the backup carries as it does every other.

Browser sessions themselves live in the server's memory: a restart ends every
one, as it always has.

Sharp edges
-----------

* **Revoking is not blocking.** A client with a credential that still works
  signs in again and clears the mark. Use the app password option, or change
  the account's password (which ends every session and drops every connection
  of the account).
* **A password reset ends every browser session too.** The self-service reset
  a recovery address makes possible does exactly what a password change from
  the webmail does - every session of the account ends, and both the mailbox
  and the recovery address are told it happened. See
  [AccountRecovery.md](AccountRecovery.md), whose one-time codes also stand in
  for the second factor at sign-in, and are then named in the account's device
  list as the ordinary password sign-in they are.
* **The IMAP `ID` command is ignored** for the reason above, so an IMAP entry
  names the protocol and the address alone. Two mail programs on one machine
  fetching over IMAP are one entry.
* **Binding to the address ends sessions for mobile users** whose address
  changes. It is a policy for a desk, not a phone.
* **The throttle** means a second sign-in from a client within a minute of its
  first does not move `last_seen` or `sign_ins`. The first is always recorded.
* **A domain that says nothing** takes the server's answer; a domain that has
  set an idle timeout of 60 seconds while the server's is 900 gets 60. There is
  no "the stricter of the two" - the domain's row is the domain's decision.

The regression tests are `RegressionTests/API/RestApiDevices.cs`: two sign-ins
listed separately, a revoked session refused on its next request, a mail
client's connection dropped and its app password turned off, a session past its
idle timeout refused, one past its lifetime refused however busy, one whose
address changed refused when bound, one session per device, a domain's policy
winning over the server's, and an upgrade leaving every policy unset.
