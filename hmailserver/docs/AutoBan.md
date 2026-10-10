# Auto-ban, and who it never bans

An auto-ban is an IP range the server writes for itself: a single address -
widened, when `AutoBanWidenByReverseName` is on, to its reverse-name neighbours
(below) - at priority 100, allowing nothing, named `Auto-ban: ...` and expiring
on its own.
The listeners consult it when a connection is accepted, so a banned address is
refused by the server itself; with `AutoBanFirewall` or `AutoBanCommand` set it
is also blocked below the server (`Common/Util/AutoBanFirewall.h`). Four
things write one:

| Writer | When | Setting |
|---|---|---|
| A run of failed logons | `MaxInvalidLogonAttempts` failures from one address within `MaxInvalidLogonAttemptsWithin` minutes, on any protocol that authenticates. On ManageSieve a connection that runs out of time before it signs in counts as one too (since 6.3.6's second review): a client that comes back each time it is cut off, to hold the service, is banned as a guesser is | `AutoBanOnLogonFailure`, `AutoBanMinutes` |
| A spam trap | An address of a hosted domain that was never real is addressed ([SpamTraps.md](SpamTraps.md)) | `SpamTrapBanMinutes` |
| A null sender with several recipients | `MAIL FROM:<>` names a second recipient ([SmtpPolicy.md](SmtpPolicy.md)) | `BanMultiRcptNullSender`, `BanMultiRcptNullSenderHours` |
| AUTH on a delivery port | `AUTH` reaches an SMTP listener whose SMTP authentication is Off or Honeypot - port 25 on a new installation - where no password is ever evaluated ([SmtpAuthOnDeliveryPorts.md](SmtpAuthOnDeliveryPorts.md)); one range per address while it lasts, named `Auto-ban: AUTH on a delivery port <address>`, and never for this computer's loopback address | The listener's SMTP authentication, `AutoBanMinutes` |

Two lists say who is never banned, whatever they do, and both are consulted
by all four writers:

| List | Keyed by | Where |
|---|---|---|
| `AutoBanNeverBan` | Addresses and CIDR blocks: `192.0.2.10, 203.0.113.0/24, 2001:db8::/32` | A setting; the Control Panel's Auto-ban page, *Below the server: the firewall* |
| The never-ban list by reverse name | The reverse names of a provider's hosts: `.example.net` | A table, `hm_neverbannames`; the Auto-ban page, *Never auto-ban by reverse name* |

An address on either is never banned, and the widening of a ban below never
crosses one. The domain white list
([SmtpPolicy.md](SmtpPolicy.md)) is neither: it skips greylisting and the spam
score's refusals, and stops no ban.

## The bans in force, and lifting one

The Control Panel's Auto-ban page opens on *Banned now*: every range the server
wrote for itself - an expiring range named `Auto-ban: ...`, the test the
widening below and the firewall reconciliation make - with the
banned address, why it was banned (the user name a run of failed logons was
guessing at, the spam trap, the null sender or the AUTH on a delivery port) and
when the ban ends: "23 min from now" from a Control Panel on the server itself, the
server's time from one elsewhere, since the server gives no clock of its own.
*Lift ban*, on the row or under the list, asks first and deletes the range, as
deleting it on the IP ranges page does: the address may connect from its next
connection, a firewall rule `AutoBanFirewall` wrote for it is taken away, and it
is banned again only when it earns a new ban. The IP ranges page shows the same
bans among the administrator's ranges, with an *Expires* column and *Show
auto-bans only*. *Clear logon-failure list* lifts no ban: it forgets the failures
counted towards the next one.

## The never-ban list by reverse name (GitLab work item #56)

`AutoBanNeverBan` can spare a provider's nets only if the administrator knows
its addresses. A provider whose outbound hosts share a reverse name and whose
addresses move - a large mailbox provider's relays, a hosted gateway - had
nowhere to say *do not ban this name*. This list is that place. It is **empty by
default**, and an empty list changes nothing: a stock server bans exactly as it
did.

### What a row is

The domain white and black list's pattern rules:

| Pattern | Covers |
|---|---|
| `mail.example.net` | That name alone |
| `.example.net` | `example.net` and every name beneath it, at a label boundary: `out5.mail.example.net` yes, `notexample.net` no, `example.net.attacker.test` no |

A pattern is stored trimmed, lower-cased and without a trailing dot, and is
unique. Refused, in words:

- a **public suffix** - `.com`, `.co.uk`, a name of the list's private section
  such as `.github.io`, or a single label such as `localhost` - by the same
  compiled-in Public Suffix List the quarantine and DMARC use: anybody can
  register a name beneath one;
- an **address** or a **network** (`192.0.2.1`, `198.51.100.0/24`,
  `2001:db8::1`): those belong in `AutoBanNeverBan`;
- a **`*` wildcard** (`*.example.net`): the leading dot is the wildcard;
- an email address, a space, a name that is not a host name.

Each row has a note, kept for whoever reads the list next, and the time it
was added. Adding, changing and removing a row is on the audit trail as a
*never-ban name* under its pattern.

### When an address is spared

An address is spared when **one of its reverse names is covered by a row and
resolves back to the address**: the name's A records (IPv4) or AAAA records
(IPv6), followed through a CNAME, include the very address. An IPv4 address
mapped into IPv6 is judged as the IPv4 address.

That second half is not in the work item, and it is the half that makes the
list safe. A reverse name is written by whoever holds the address block's
reverse zone, and nearly every hosting company lets a customer write their own:
without the forward lookup, anybody could spare their scanner from every ban by
naming it `mail.google.com`. Only the holder of `google.com`'s forward zone can
make that name resolve to their address.

An address is **not** spared when:

- it has no reverse name, or its reverse lookup was not answered;
- its reverse name is an address written as a name, or a public suffix or
  shorter (`localhost`) - no row may cover those;
- no row covers its reverse name;
- a row covers it, but the name does not resolve back to the address, or its
  forward lookup was not answered.

At most eight reverse names of one address are looked at, and at most four
forward lookups made for one verdict. The work item asked for the registrable
domain of the reverse name to be matched; the list matches the whole name
instead, so that `mail.example.net` means that name alone, as it does on the
domain list, and a narrower row such as `.outbound.example.net` can be written
at all. The registrable domain is what decides that a public suffix is never
matched.

**Do not list a cloud whose customers' machines carry the provider's name.**
`*.compute.amazonaws.com`, `*.googleusercontent.com` and their kind resolve
forwards and backwards for every customer's machine, so a row for them spares
everybody who rents one. List the provider's own mail domain - the name its
outbound relays carry - and nothing wider.

### When it is asked, and why no session waits for DNS

The list is consulted in the ban writers, next to `AutoBanNeverBan`, and it is
**never looked up on a session's thread** - a spam trap's ban is placed inside
`RCPT TO`, whose reply must not wait for DNS or be timed apart from an unknown
address's.

1. **The SMTP session's own reverse lookup.** Every SMTP session already looks
   its client's reverse name up on the name lookup queue when it connects, for
   the `Received` header. While the list has rows, every name that lookup
   returns is handed to the list there, and a name a row covers is
   forward-confirmed on the same worker. The verdict is kept, so by the time a
   session addresses a trap or fails its logons the answer is usually waiting,
   and **no range is ever written**.
2. **A verdict already known** is read by the writers without a lookup: a spared
   address's failed logons are not counted at all (as an address on
   `AutoBanNeverBan`'s are, so no `logon failure` line is written for fail2ban
   to count either), and a trap hit or a null sender records why it was not
   banned.
3. **Nothing known yet** - IMAP, POP3, ManageSieve, the REST API, or an SMTP
   session whose lookup had not finished: the ban is written as it always was,
   and the address is resolved on a queue of its own, the never-ban
   verification queue. **Nothing below the range happens until the answer is
   in**: the `Auto-ban: <address> is banned for ...` line the packaged fail2ban
   filter matches, the alert count and the firewall reconciliation all wait for
   it. The address *waits* from before its range is written, and every
   reconciliation in the meantime - another ban's, an administrator's delete,
   the once-a-minute expiry pass - leaves its range out of the firewall rule,
   `AutoBanCommand` and the packaged hook. A spared address then has its range
   deleted and none of the three happens; any other address is announced as
   before. A listed provider is refused by the server itself for as long as its
   lookups take, and its ban reaches neither the firewall nor fail2ban, unless
   its answer is not in by the time the bounds below give it. **The
   failed logons counted before then do**: nothing is looked up for a failed
   logon until the one that bans, so each of them - the one that bans included -
   writes its `Auto-ban: logon failure <n> of <m> from <address>` line as it
   always did, and the packaged fail2ban jail counts those lines on its own
   (five in ten minutes). With `MaxInvalidLogonAttempts` below five, as shipped,
   a listed provider does not normally reach that count: once the answer is in, its
   failures are not counted for the hour the verdict is kept. At five or more,
   fail2ban bans it below the server: keep the setting below the jail's
   `maxretry`, or raise `maxretry` above it.

A verdict that spares is kept an hour, one that does not ten minutes, and every
verdict is forgotten the moment a row is added, changed or removed: a row
removed because it was a mistake spares nobody from the next ban on. The rows
are re-read when they change here and every five minutes in any case, so that a
row another node added to a shared database is in force there too.

#### While the answer is not in

The verifications run on four threads of their own, so the sessions' own
reverse lookups - which an outsider whose reverse zone does not answer can keep
backed up - never hold one up, and **each lookup is given up after five
seconds** whatever `DNSQueryTimeout` says. A lookup given up is not waited for
again; at most 32 such lookups may still be running in the resolver, and past
that a lookup is not begun and counts as not answered.

A ban **waits for its turn rather than being announced**. The waiting bans are
taken by network in turn - an IPv4 `/24`, an IPv6 `/48` - so that one network's
bans, however many, delay another network's by one lookup each: an outsider who
earns ban after ban from addresses whose reverse lookups hang cannot push a
listed provider's ban to the back. At most 16 bans of one network wait at once,
and a seventeenth of that network is announced at once. At most 1,024 wait in
all: a new ban past that gives up - announces unverified - the oldest waiting ban
of the network with the most bans that no thread is asking about at that
moment, so that the bans of one network cannot keep another's out.

| What happens to a waiting ban | When |
|---|---|
| Lifted, never announced | Its answer spares it |
| Announced | Its answer does not spare it |
| Asked again a minute later | Its lookups were not answered |
| Announced unverified | Four attempts were not answered, or it waited ten minutes in all. A minute a ban waits past its time without a turn - every thread busy - counts as an attempt |
| Forgotten, never announced | Its range has gone - expired, or deleted on the IP ranges page - before the answer was in |

So under a sustained flood of bans from addresses whose reverse zones do not
answer, a listed provider's ban is refused by the server and **held back from
the firewall and fail2ban** until it is verified; only a flood from more
networks than the threads can work through in four minutes - some two hundred
networks, each with a reverse zone that never answers - delays its turn past
its attempts, and then it is announced as it would be without the list. A
waiting ban is held in memory: a service restart forgets it, and the start-up
reconciliation mirrors its range as any other.

### What it writes in the log

| Line | When |
|---|---|
| `Auto-ban: <address> is not counted or banned for failed logons: its reverse name <name> resolves back to it and is covered by <pattern> on the never-ban list` | The first failed logon from a spared address while its verdict is kept (then in the debug log) |
| `Auto-ban: <address> was not banned: its reverse name <name> resolves back to it and is covered by <pattern> on the never-ban list; the ban written for it was lifted.` | A ban written before the answer was in, deleted |
| `... not banned: its reverse name <name> resolves back to it and is covered by <pattern> on the never-ban list` | In a spam trap hit's line and record, and in a refused null sender's line |
| `Never-ban list: <pattern> added. ...` / `removed.` | A row added or removed over COM or the Control Panel; `RestApi: Never-ban list entry ...` over REST |

None of them is the shape the packaged fail2ban filter matches. Two errors
belong to the list: `HM7640`, the table could not be read (the rows last read
stay in force; at most once an hour), and `HM7641`, a spared address's ban
could not be lifted (delete its `Auto-ban:` range on the IP ranges page).

### What it does not do

- It skips nothing else. Greylisting, SPF, DKIM, DMARC, the spam score and the
  domain list's black rows all still apply: a spared host can still be refused
  at `MAIL FROM`.
- It does not key off `HELO` or the envelope. A scanner on a residential
  address that says `HELO google.com` is not spared.
- It does not replace `AutoBanNeverBan`. A net with no reverse name, or a
  reverse name you do not trust, still belongs there.
- It does not lift a ban an administrator wrote, or one covering more than one
  address: only the single-address `Auto-ban:` ranges the server writes.
- The XCLIENT `NAME` an upstream asserts is not believed for it; the address
  XCLIENT asserts is looked up like any other.

### Where it is

| Surface | What |
|---|---|
| Control Panel | *Settings > Auto-ban*, the card *Never auto-ban by reverse name*: the rows, *Add name...*, *Remove name*, and *Check an address...*, which says what the list would decide about an address and, when no row covers its reverse name, whether a row covering it would work |
| Control Deck | *Lists*, the table *Never-ban list by reverse name*, with an address check under it |
| REST | `GET`/`POST /api/v1/never-ban-names`, `PUT`/`DELETE /api/v1/never-ban-names/{id}`, `POST /api/v1/never-ban-names/check` with `{"address": "..."}`. Server-wide: refused for a credential confined to named domains; the writes and the check (which sends DNS queries) are refused for read-only keys; the check of an address that is not public - private, loopback, link-local, carrier NAT, reserved or documentation - is the administrator password's alone, 403 for an API key or an account holding a role, since its reverse names are this server's view of its own network ([Roles.md](Roles.md), What manage_settings reaches) |
| COM | `Settings.AutoBanNeverBanNames` (the rows as JSON), `AddAutoBanNeverBanName(Pattern, Note)`, `RemoveAutoBanNeverBanName(ID)`, `CheckAutoBanNeverBanName(Address)` |
| Schema | 6108, `hm_neverbannames` (`nbnid`, `nbnpattern` unique, `nbnnote`, `nbncreated`) |

The check answers
`{address, reverse_names: [{name, usable, pattern, resolves_back}], answered, spared, name, pattern, reason}`.
`resolves_back` is asked whether or not a row covers the name, and is `null`
when it was not asked or not answered. Nothing is kept, banned or lifted by a
check.

### Verified against the code (the never-ban list)

`Common/Util/NeverBanNameList.{h,cpp}` holds the decision and its tester,
`NeverBanNameListTester` (ClassTester on Windows, `never-ban-tests` on Linux);
`Common/BO/NeverBanName.cpp` the pattern rules; the writers' questions are in
`AccountLogon::RegisterFailedLogin`, `SpamTrapMonitor::OnHit`,
`SMTPConnection`'s null-sender ban and its `RefuseAuthOnDeliveryPort_`, the deferred
announcement in `AutoBanFirewall::Banned` and its three siblings, the session's names in
`SMTPConnection::PrefetchPtrRecord_`, the waiting bans in
`NeverBanWaitingBans` (`NeverBanNameList.cpp`), the hold before the range is
written in `AccountLogon::CreateIPRange`, the reconciliation's leaving them out
in `AutoBanFirewall::ActiveBans_`, the queue in
`Application::CreateSessionWorkQueues_`, and the minute's sweep in
`RemoveExpiredRecords`. The regression fixtures `Security/AutoBanNeverBanNames`
(REST and SMTP, both suites) and `Security/AutoBanNeverBanNamesCom` (COM,
Windows) hold the behaviour, each case beside the one that must come out the
other way.

## Widening a ban to its reverse-name neighbours (GitLab work item #55)

A scanner that works through `203.0.113.10`, `.11` and `.12` under one reverse
domain earns three bans, three firewall rules and three sweeps. With
`AutoBanWidenByReverseName` on, the first ban is written as it always was, at
once, for the one address, and then widened **in the background** to the
contiguous run of addresses around it, inside its `/24`, whose reverse names
share its registrable domain - so the neighbours are refused before they earn
a ban of their own:

```
Auto-ban: 203.0.113.14 was widened to 203.0.113.10-203.0.113.22 via example.net: the 13 addresses of the run have reverse names under that domain, and its own, mail5.outbound.example.net, resolves back to it; the run stopped below because 203.0.113.9 is named under other-isp.test and above because 203.0.113.23 has no reverse name.
```

It is **off by default**. Off, every ban is a single address exactly as before,
and nothing is queued or looked up.

### What is walked

The walk starts only when all of these hold for the banned address:

- it is **IPv4**. An IPv6 ban is never widened, not even to its `/64`;
- it is an **Internet address**, not one of a network's own: `10.0.0.0/8`,
  `172.16.0.0/12` and `192.168.0.0/16` (RFC 1918), `100.64.0.0/10` (RFC 6598,
  the space behind a carrier's NAT), `127.0.0.0/8`, `169.254.0.0/16` and
  `0.0.0.0/8` are never walked. Their neighbours are the network's own machines,
  and its reverse zone - an Active Directory domain's, a home router's - names
  every one of them under one domain, so one colleague's stale password on a
  phone would otherwise ban the office. Such a ban stays a single address and
  says so (`Auto-ban: 10.0.0.25 stays a single address: it is a private, shared,
  loopback or link-local address, whose neighbours are this network's own
  machines.`), without a lookup. That is not in the work item either;
- it has at least one reverse name, at most eight, and **every one** is a host
  name under **one** registrable domain - the compiled-in public suffix list
  the quarantine and DMARC use, both sections, so `mail5.outbound.example.net`
  is under `example.net` (not "one label stripped"), and an address, a public
  suffix, `localhost` or a reverse-mapping name under `.arpa` is no domain at all;
- **one of those names resolves back to the address** (its A records include it).
  That is not in the work item, and it is what keeps the widening from being
  aimed at somebody else. Whoever holds an address writes its reverse name, and
  most hosting companies let a customer write their own: without the forward
  lookup a scanner could name itself `x.bigcorp.example` in a `/24` it shares
  with bigcorp's servers and have bigcorp banned with it. Only the holder of
  bigcorp's forward zone can make that name resolve to the scanner;
- the never-ban list by reverse name covers none of its names. That list decides
  about the address itself, and may yet lift its ban; a ban it might lift is
  never widened.

Then the walk goes down from the address and then up, one address at a time,
and each direction stops at the first address that is not like it. The run is
what was seen, contiguous: a hole is never filled.

| A direction stops at | Looked up? |
|---|---|
| The edge of the `/24` (`.0` or `.255`), which is never crossed | - |
| 32 reverse lookups | - |
| An address on `AutoBanNeverBan` | No |
| An address that an IP range an administrator wrote tells apart from the banned one: a range covering one of the two and not the other. Such a range is a statement about that address, and the ban's priority of 100 would otherwise override it | No |
| An address an administrator's IP range lets in at a priority not below the ban's 100: the server's own refusal would not reach it, and the run's firewall rule must not either | No |
| An address with no reverse name, or whose reverse names are not all host names under the same registrable domain | Yes |
| An address one of whose reverse names the never-ban list by reverse name covers (forward-confirmed or not) | Yes |

A neighbour's reverse name need not resolve back to it: whoever wrote it put
that neighbour in the domain. **A reverse lookup that is not answered anywhere in
the walk, or a walk that takes longer than 30 seconds, leaves the single address
as it was**, whatever had already been seen.

A host under a shared-hosting suffix of the public suffix list's private section
is never under the provider's domain, so neighbouring customers of such a cloud
are never widened together: a `github.io` tenant's hosts are under the tenant's
own domain (`www.tenant.github.io` is `tenant.github.io`), and where a wildcard
rule makes a host's whole name a public suffix -
`ec2-203-0-113-14.compute-1.amazonaws.com`, under `*.compute-1.amazonaws.com` -
the name is no domain at all and its ban stays a single address. A hosting `/24` whose reverse zone names every customer
under the provider's own domain (`203-0-113-14.hosting.example`) **is** widened
whole: that is the trade the work item names. List such a net in
`AutoBanNeverBan`, or leave the switch off.

### What is written

The ban's own row, and nothing else: the newest unexpired single-address
`Auto-ban: ` range for the address gets the run's lower and upper. One row, not
one per neighbour, with the ban's own name, priority and expiry; the expiry
sweep lifts it as it lifts any ban, and deleting it on the IP ranges page lifts
the whole run. The update applies only while the row is still that single
address, so an administrator's edit or a never-ban lift made while the
neighbours were looked up wins. A range an administrator wrote is never
widened, whatever it is called.

Below the server, the firewall follows the row
(`Common/Util/AutoBanFirewall.h`):

| Where | What a widened ban becomes |
|---|---|
| Windows Defender Firewall (`AutoBanFirewall`) | One rule, named for the run - `hMailServer auto-ban 203.0.113.10-203.0.113.22` - whose remote addresses are that range. The single address's rule is removed |
| `AutoBanCommand` and the packaged Linux hook | `ban <address> <minutes> <ports>` for each address of the run, one at a time, as they have always been told; `unban` for each when it goes. A program written for one address is never handed a range. A command that runs past `ExternalProcessTimeout` is not run again in the same reconciliation, so a hook that hangs costs one timeout, not one per address of a run. A ban it was not run for is not asked again, as a failed run never was. An `unban` it was not run for, or that was stopped for the timeout, is owed: the address is asked for again by the next reconciliation and, each time the command runs past the timeout again, after a wait that starts at a minute and doubles to an hour, until it has been run for every command - with iptables or a command of your own nothing else would ever lift that block (until 6.3.6 it was forgotten). An `unban` that ran and exited with an error is reported (HM6523) and not asked again |
| fail2ban | Nothing new. The widening line is not the shape the packaged filter matches; fail2ban goes on counting the banned address's own lines |

### When

A ban is announced - its `is banned for` line, the alert count, the firewall -
by `AutoBanFirewall`, once the never-ban list by reverse name has not spared it.
That announcement queues the address for widening and returns: nothing is looked
up or read from the database on a session's thread, so a spam trap's `RCPT TO`
reply waits for nothing. The walks run one at a time on the name lookup queue,
each up to 65 reverse lookups made one after another; at most 64 addresses wait,
each once, and past that a ban stays a single address. Turning the switch off
stops a walk that has not yet written anything.

Nor does a ban decision wait for what is below the server. `AutoBanNeverBan`,
which every writer of a ban asks first and the walk asks of every neighbour, is
read under a lock of its own, not the one a reconciliation holds while it runs
`AutoBanCommand` or the packaged hook. Until 6.3.6 it was that lock, so a hook
that hung until `ExternalProcessTimeout` (300 seconds as shipped) held up, for
as long as it ran, a spam trap's `RCPT TO` reply - timed apart from an unknown
address's after all - every failed logon's count, and each step of a walk. Nor
does the report of an entry that is no address (HM6524) hold that lock: it runs
the event script's `OnError` on the thread that read the new value, and is made
once the lock is let go, so a slow handler holds up nothing but that thread.

A ban that stays a single address says why, in the application log too:

```
Auto-ban: 203.0.113.14 stays a single address: the reverse lookup of 203.0.113.18 was not answered.
Auto-ban: 203.0.113.14 stays a single address: its reverse name mail5.outbound.example.net does not resolve back to it, so anybody could have written it.
```

Neither line is the shape the packaged fail2ban filter matches, and every name in
one - a reverse name, an IP range's name - is written as printable text, since
the holder of a reverse zone chooses its bytes.

### Where it is

| Surface | What |
|---|---|
| Setting | `AutoBanWidenByReverseName` in `hm_settings`, `0` (off); no schema step |
| Control Panel | *Settings > Auto-ban*, the card *Widen a ban to its neighbours* |
| Control Deck | *Settings*, the *Server* group, `auto_ban_widen_by_reverse_name` |
| REST | `auto_ban_widen_by_reverse_name` on `GET`/`PUT /api/v1/settings` |
| COM | `Settings.AutoBanWidenByReverseName` |

### Verified against the code

`Common/Util/AutoBanWidener.{h,cpp}` holds the walk, the queue and its tester,
`AutoBanWidenerTester` (ClassTester on Windows, `autoban-widen-tests` on Linux);
`PersistentSecurityRange::WidenAutoBan` the conditional update; and
`AutoBanFirewall::ActiveBans_` the shape a widened ban is mirrored in. The
regression fixture `Security/AutoBanWiden` (REST and SMTP, both suites) holds
the behaviour over the suite's own DNS zone, each case beside the one that must
come out the other way, and `Security/AutoBanWidenCom` (COM, Windows) the COM
property, the Windows rule for a run, a hook told each address, a hook that hangs
costing one timeout a run, and a trap's refusal waiting neither for it nor for a
slow `OnError` handler.

## Below the server across a stop and a start (6.3.6)

What is below the server - a Windows Defender Firewall rule, an entry in the
packaged Linux hook's nftables set or iptables chain, whatever `AutoBanCommand`
did - outlives the service. A ban that ends while the service is stopped, an
`unban` still owed when it stopped (a hook that ran past
`ExternalProcessTimeout`), a switch turned off while it was stopped: each would
leave an address blocked below the server with nothing left to lift it. Until
6.3.6 that was so for the iptables variant of the hook and for `AutoBanCommand`,
whose account of what they had been told lived in the server's memory only.

At every start - and every reinitialise, which is a start in the same process,
and the COM API's `Start` after a `Stop` - `AutoBanFirewall::SynchroniseAtStart`
learns what the firewall holds and then announces the bans in force again, all
before the first listener opens:

| Below the server | At a start | When the switch goes off (at the next reconciliation, the expiry pass's within a couple of minutes at the latest) |
|---|---|---|
| Windows Defender Firewall (`AutoBanFirewall=1`) | The rules of the group `hMailServer auto-ban` are enumerated; every one with no ban behind it is removed, every ban without one gets one | Every rule of the group is removed |
| The packaged Linux hook (`AutoBanFirewall=1`) | Told `reset`, which removes its nftables table or its iptables chains, and then `ban` for every ban in force. Wherever it is installed, **whether `AutoBanFirewall` is on or off**: with the switch off and no capability to change the firewall, `reset` finds nothing and says nothing. A `reset` that fails is said once (HM6523) and asked again a minute later, then at doubling waits up to an hour; `autoban-hook reset` run as root does the same by hand | Told `reset` |
| `AutoBanCommand` | Told `unban` for every address its ledger says it was told `ban` for and that is not banned now, then `ban` for every ban in force | Its addresses are named (below) |

**A command run beside the server leaves it alone.** The commands that start the
server short of its listeners - `--sample-domain`, `--apply-environment` (which
the container runs at every start), `--apply-profile` and the rest - do none of
this, and from 6.3.6 tell the firewall nothing at their start: run beside a
running server, a `reset` would lift every ban below it until each was told
again - and, if the command stopped in between, for the rest of each ban or until
that server's next start, since the running server believes the hook still holds
them and never tells them again. Until 6.3.6 they told every ban in force again.

**`AutoBanCommand`'s ledger.** Its contract is `ban` and `unban` and has no
`reset`, so the server keeps the account itself: `autoban-command.ledger` in the
temp folder (`[Directories] TempFolder`) - every address the command was told
`ban` for and has not since run `unban` for, an `unban` owed after a timeout
included - written whenever that changes. It names the command by the SHA-256
of its command line, never its text, since a command line can carry a
credential. Delete it, and a start knows nothing of what the command was told
before, as before 6.3.6.

**A command replaced or cleared is never run again.** A command taken out of the
configuration may have been taken out because it misbehaves. When
`AutoBanCommand` is changed or cleared while the service runs, or the ledger at a
start was written for another command, the addresses that command was told to
ban and not to un-ban are named in the error log (HM6523, up to twenty, and how
many more), for the administrator to lift where that command put them; the new
command is told every ban in force.

**A command's own bans should still expire.** It is handed the ban's `<minutes>`:
a block that lifts itself after them is never left behind, whatever happens to
the server, the ledger or the machine. The packaged hook's nftables set does
exactly that.

**Uninstalling.** The Windows uninstaller's `hMailServer.exe /Unregister` removes
every rule of the group, which Windows would otherwise keep across restarts and
which would then block their addresses on those ports for good, whatever
listens on them next. An iptables entry goes at the next reboot, an nftables one
when its timeout runs out.

**iptables waits for its lock.** The hook's `iptables` and `ip6tables` calls wait
for the xtables lock (`-w`) rather than failing at once while fail2ban, a
firewall manager or another run of the hook holds it: a failed `unban` there is
a block that never lifts. A wait longer than `ExternalProcessTimeout` is stopped,
and the `unban` asked for again.

**The reset and the gap.** Between the packaged hook's `reset` and the `ban` that
follows it for each ban in force, those addresses are not blocked below the
server. At a start no listener of the server is open yet, so none of them can
reach one. A `reset` that failed at the start and is asked again later runs with
the listeners open, and for that moment the server refuses those addresses
itself, when they connect, as it refuses every banned address.

**A pass that cannot read the IP ranges changes nothing.** The ranges are what a
reconciliation makes the firewall match, and a read of them that failed was taken,
until 6.3.6, for no bans at all: every hook was told `unban` for every address and
every Windows rule removed, until a later pass put them back. Such a pass now
leaves everything below the server as it is - and at a start leaves the reset and
the ledger to the next pass, at the latest the expiry pass a minute later.

Held by `Security/AutoBanHookRetry` (both suites): the ledger kept and emptied
(an owed `unban` kept in it), an address it names that is not banned un-banned at
a start and a ban in force announced again, another command's ledger named and
not run, a command replaced at run time naming its addresses while the new one is
told every ban, a pass whose read of the ranges failed telling the command
nothing (Linux, and Windows on a database other than SQL Server Compact), and -
on Linux, with a stand-in for the packaged hook - `reset`
at a start before the bans, when the switch goes off, and at a start with it
off, and `--apply-environment` run beside the running server telling neither
hook anything.
