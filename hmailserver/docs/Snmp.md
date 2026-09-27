SNMP
====

The metrics are Prometheus (`/metrics`) and OpenTelemetry (OTLP), which is right
for anything built this decade and wrong for the monitoring system a bank already
owns. This server also has an **SNMP agent**: read-only, off by default, answering
the mail MIB (RFC 2789, MTA-MIB) and the standard SNMP groups from **the same
counters `/metrics` serves** - there is no second set of counters, and a value
`/metrics` has no counter for is answered `noSuchObject`, not made up.

What it speaks:

* **SNMPv3** with the User-based Security Model (RFC 3414), for **one user**, at
  **authPriv only**: HMAC-SHA-256 authentication (`usmHMAC192SHA256AuthProtocol`,
  RFC 7860) and AES-128 privacy (`usmAesCfb128Protocol`, RFC 3826). Discovery,
  the time window and every refusal the USM names are answered the RFC 3414 way,
  with Report PDUs.
* **SNMPv2c**, as an option that is off even when the agent is on, for a
  monitoring system that cannot do SNMPv3. What it costs is below.
* **SNMPv1 is not answered.**
* **Get, GetNext and GetBulk.** A **Set** is answered `noAccess`: the agent is
  read-only. **No notifications** - no traps and no informs - are sent: nothing
  leaves this agent unasked.

Turning it on
-------------

**An empty allow list answers nobody** - it is not "everyone" - so switching the
agent on does nothing until you have said who it is for. A datagram from an
address outside the list is never answered at all. An allowed address is given
**data** only when the request carries the user's keys at authPriv (or, with
SNMPv2c on, the community). An SNMPv3 request without them is given **Reports**
and nothing else - the discovery Report, which names the engine, and then
`usmStatsUnknownUserNames` while no user is configured; an SNMPv2c request
without the community is not answered.

The settings are rows of `hm_settings`, not keys of `hMailServer.ini`. They are
keys of `/api/v1/settings` (the server group), fields of the Control Deck's
**Settings** page under **Server**, and `hmctl settings get` prints them with the
rest of the server group.
**Every one is read when the agent starts**, which is at the next service start
or when you press *Reinitialize*; saving writes the value at once and the running
agent keeps the one it started with. The Deck marks each field *restart* for that
reason.

| Setting | `hm_settings` name | REST key | Default | What it means |
|---|---|---|---|---|
| The agent | `SnmpEnabled` | `snmp_enabled` | off | The whole feature. Off, no socket is opened. |
| Port | `SnmpPort` | `snmp_port` | `161` | The UDP port, 1 to 65535. |
| Bind address | `SnmpBindAddress` | `snmp_bind_address` | *(empty)* | Empty is every address: one socket for IPv6 and IPv4 where the machine has IPv6, IPv4 otherwise. Or one address of this machine. **`127.0.0.1` is the careful choice** when the manager runs on the same machine (a local collector that forwards); otherwise bind the agent to an address on a **management interface or network**, not to every address. |
| Allowed managers | `SnmpAllowedAddresses` | `snmp_allowed_addresses` | *(empty)* | Addresses and CIDR ranges, separated by commas (`192.0.2.10, 192.0.2.11`), at most 64 of them. **Empty answers nobody.** **Name the managers one by one**, as single addresses, rather than by range: the list is judged on a datagram's source address, which UDP does not prove (below), and every address the list covers is one a forged request can have answers sent to. A datagram from any other address is dropped before a byte of it is read: it moves no counter, costs no cryptography and is never answered. An IPv4 manager reaching a dual-stack socket is judged by its IPv4 address. |
| SNMPv3 user | `SnmpV3User` | `snmp_v3_user` | *(empty)* | The one user, at most 32 visible ASCII characters. Empty is no user: an SNMPv3 request is given a Report and never data - discovery's, and then `usmStatsUnknownUserNames`. |
| Authentication password | `SnmpV3AuthPassword` | `snmp_v3_auth_password` (write-only) | *(empty)* | 8 to 64 ASCII characters (RFC 3414 section 11.2 asks for at least 8). |
| Privacy password | `SnmpV3PrivPassword` | `snmp_v3_priv_password` (write-only) | *(empty)* | 8 to 64 ASCII characters. It may be the same as the authentication password, and a different one is better. |
| SNMPv2c | `SnmpV2cEnabled` | `snmp_v2c_enabled` | off | Whether SNMPv2c is answered too. Off even when the agent is on. |
| Community | `SnmpV2cCommunity` | `snmp_v2c_community` (write-only) | *(empty)* | At most 64 visible ASCII characters. Empty answers no SNMPv2c request, whatever the switch says. |
| sysContact | `SnmpContact` | `snmp_contact` | *(empty)* | One line of printable ASCII, at most 255 characters: a DisplayString (RFC 2579) holds nothing else. |
| sysLocation | `SnmpLocation` | `snmp_location` | *(empty)* | The same. |

A value a check refuses is refused with a sentence naming the setting - an
address that is not one, an entry of the allow list that is not an address or a
range, a password shorter than eight characters, a contact outside printable
ASCII - and nothing in the request is applied. A user with only one of the two
passwords is given no data: its requests get `usmStatsUnknownUserNames`, and the
application log says why when the agent starts. The Deck leaves a password or the
community as it is unless its box is filled; over REST an empty value clears it.

**Where it is set.** There is no COM property for any of these, as there is none for
the Prometheus listener: the REST API, the Deck and `hmctl` are the surfaces, and
the Control Panel has them on Monitoring & troubleshooting > Logging, the SNMP
tab, which reads and writes them over the REST API - so the tab shows them only
while the REST API is on, and otherwise says why. The environment layer names
them as it names every `hm_settings` row (`HM_PROPERTY_SnmpEnabled` and so on,
with `_FILE` for the three secrets;
[EnvironmentConfiguration.md](EnvironmentConfiguration.md)).

### An example

Nothing here puts a password on a command line, where the process list and the
shell's history would keep it. On the server, for a manager at 192.0.2.10, the
keys go in a file only you can read, `hmctl` ([CommandLine.md](CommandLine.md))
sends them in one `PUT /api/v1/settings` - asking at the console for a
credential when `HMAIL_API_KEY`, `HMAIL_PASSWORD_FILE` and the rest name none -
and the file goes:

```console
$ umask 077
$ cat > snmp.json
{"snmp_allowed_addresses": "192.0.2.10", "snmp_v3_user": "monitor",
 "snmp_v3_auth_password": "...", "snmp_v3_priv_password": "...",
 "snmp_contact": "postmaster@example.com", "snmp_location": "Rack 4, London",
 "snmp_enabled": true}
^D
$ hmctl settings set --from-json snmp.json
$ rm snmp.json
$ hmctl api POST /api/v1/server/reinitialize
```

`hmctl settings set --from-json -` reads the same object from standard input
instead, and the Deck's password boxes are the third way.

On the manager, with Net-SNMP 5.9 or later, the credentials go in
`~/.snmp/snmp.conf`, readable by you alone (`chmod 600`), rather than in `-A` and
`-X`:

```
defVersion        3
defSecurityName   monitor
defSecurityLevel  authPriv
defAuthType       SHA-256
defAuthPassphrase ...
defPrivType       AES
defPrivPassphrase ...
```

```console
$ snmpget mail.example.com SNMPv2-MIB::sysUpTime.0 1.3.6.1.2.1.28.1.1.3.1
$ snmpwalk mail.example.com 1.3.6.1.2.1.28
```

The engine ID is discovered; nothing needs to be told it. It is in the
application log line the agent writes when it starts, and it is
`snmpEngineID.0` (1.3.6.1.6.3.10.2.1.1.0).

What it costs, and what it protects
-----------------------------------

**SNMPv3 at authPriv** authenticates every request and every answer with the
user's key and encrypts the PDU, so the counters are read by nobody but the
manager and a request is accepted from nobody but it. A request is also refused
outside RFC 3414's time window (150 seconds either side of the agent's clock, in
the agent's current boot), which is what stops one recorded on the network being
replayed later - but not sooner: see below. The keys are made from the passwords
once, when the agent starts, and localised to its engine ID, so a key taken from
one server opens no other.

**Why authPriv only.** authNoPriv would hand the counters to anyone on the path;
noAuthNoPriv would hand them to anyone who can reach the port. A request at a
lower level is answered with a Report (`usmStatsUnsupportedSecLevels`) and no
data.

**SNMPv2c costs this.** The community crosses the network **in the clear** in
every request, and it is the whole of SNMPv2c's authentication: anyone who can
see the traffic learns it, and with it can read everything the agent serves, for
as long as the community stays the same. Nothing is encrypted and nothing is
protected from being changed on the way. The allow list still applies, so a
stolen community is useful only from an allowed address - but on a shared
network an allowed address is not hard to borrow. Use it only on a network you
would let read these numbers anyway, and prefer SNMPv3 wherever the manager can
speak it.

### What a forged source address can do

The allow list is judged on a datagram's source address, and UDP does not prove
one: anything that can put a datagram on the network can write any source
address into it. The agent answers to that address, so a forged request is a way
to have answers sent to an allowed address that did not ask for them.

* **SNMPv2c.** A request with an allowed source and the community is answered,
  wherever it really came from, and the community is in the clear. So that the
  agent cannot be made into an amplifier, **an SNMPv2c answer is never longer
  than eight times the octets of its request** (or 484 octets, when that is
  more); a GetBulk whose answer would be longer is answered with as many
  bindings as fit. A forged request therefore sends the allowed address at most
  eight times what was forged.
* **SNMPv3.** A request cannot be forged without the user's keys. But a request
  **recorded** on the network can be sent again, from a forged allowed source,
  for as long as it is inside the 150-second window: RFC 3414 accepts such a
  replay, and this agent keeps no record of the requests it has answered. Each
  copy is answered in full - up to the smaller of the request's `msgMaxSize`
  and 8192 octets - to the allowed address. The copy reveals nothing (the answer
  is encrypted to the manager's key), but it is reflection toward the manager,
  and a recorded GetBulk of a few hundred octets can bring back eight kilobytes
  each time.

What limits it is the network, not the agent: **name the managers one by one**,
so that the only addresses answers can be sent to are theirs; **bind the agent to
a management interface or network**; drop, at the network's edge, datagrams from
outside that claim an inside source address (reverse-path filtering, or a
firewall rule); and prefer SNMPv3, whose requests cannot be made up.

### The secrets

Both passwords and the community are stored protected, as the relayer's password
and the sandbox's token are (DPAPI on Windows, the key file on Linux), written and
never read back: the REST keys are write-only, no interface emits them, the audit
trail records that one changed and never its value, the list of what is changed
from stock names them without showing them, no log line carries them, and **a
backup does not carry them** - after a restore they are entered again. Nor does
a backup carry the engine's seed and boot counts (below). A community is compared
by its SHA-256 digest, so that the time the comparison takes says nothing about
the community's length or where a guess went wrong.

**No log line per packet.** A packet that does not decode, a wrong community, a
wrong digest, a datagram from an address outside the list: each is counted
where the MIB counts it (`snmpInASNParseErrs`, `snmpInBadCommunityNames`,
`usmStatsWrongDigests`) or not at all, and none is logged, so the port cannot be
used to fill the log. The application log gets one line when the agent starts,
saying what it answers and who is allowed, and one for each thing in the
configuration it cannot use.

The engine
----------

**The engine ID** is RFC 3411's enterprise form: `80 00 7E D9` (the enterprise
number 32473, below), format `05` (octets), and twelve octets of SHA-256 over a
random seed made for the installation and this machine's name. So it is the same
at every start, and two servers that share one database - an active-active pair -
are two engines, as SNMP needs them to be. A server that is renamed gets a new
engine ID; a manager rediscovers it.

The seed is made by the first start that finds none, kept in the
`SnmpEngineSeed` row of `hm_settings`, and written only where there is none yet
(a row name is unique), so that two nodes starting together for the first time
agree on one seed: the second to write finds the first's and reads it back.
**Nothing is written for the agent while it is off**: the seed's row and the
boot counts below are made by its first start, and a server that never switches
it on has neither.

**snmpEngineBoots** goes up by one each time the agent starts (a service start or
a Reinitialize). Each engine keeps its own count, in a row of `hm_settings` named
`SnmpBoots` and twenty hex digits of its engine ID, read from the database -
not from the settings the process loaded, which another node may have moved on
since - and written one higher **before** the socket opens. An agent that cannot
read or write it, or finds in it anything but a count, does not start (error
7421): a boot count used twice would let a message recorded in one run be
accepted in the next, and a damaged row read as none would be exactly that. At
2147483647 it latches, as RFC 3414 says it must, and no SNMPv3 request is
answered. `snmpEngineTime` is seconds since the agent started.

**To make a new engine, empty the `SnmpEngineSeed` row**, and nothing else: the
next start makes a new seed, so a new engine ID with a boot count of its own
(its own `SnmpBoots` row, starting at 1), and a manager rediscovers it. That is
the way out of the latch, and out of a boot count refused as damaged. The old
engine's `SnmpBoots` row is not read again, and may be deleted. **Do not empty
a `SnmpBoots` row on its own**: an empty row is read as a missing one, so the
same engine, with the same keys, would start its count again at 1 and accept a
message recorded in any of the boots it has had.

These rows are the agent's own records and are not settings: no interface sets
them, the list of what is changed from stock leaves them out, and a backup leaves
them out, since a restore that brought an older boot count back would reopen the
replay window and one onto another server would give two servers one engine.

**Two nodes can still be one engine** when they have the same machine name -
cloned virtual machines, or containers started with one fixed hostname - or when
neither can read its machine name, since both then hash an empty one. They then
share one engine ID and one boot count, and a manager that polls both sees one
engine whose time jumps back and forth. Give every node its own host name.

What a request gets back, and the bounds
----------------------------------------

* A datagram longer than **8192 octets** (this engine's
  `snmpEngineMaxMessageSize`) is not decoded.
* An answer is never longer than 8192 octets, nor than the request's
  `msgMaxSize` for SNMPv3, nor than eight times its request (at least 484
  octets) for SNMPv2c. One that would be is answered `tooBig` with no bindings;
  a GetBulk is answered with as many bindings as fit instead. So over SNMPv2c a
  Get of long values - `sysContact.0` and `sysLocation.0` at 255 characters
  each, say - can be `tooBig` where the same Get of one of them is not: ask for
  long strings one to a request.
* A request of more than **64 bindings** is answered `tooBig`.
* GetBulk's max-repetitions is held to **64**, and an answer to **512
  bindings**; a GetBulk whose columns all reach the end of the MIB stops there.
* The decoder reads definite lengths of at most four octets, single-octet tags,
  INTEGERs of one to eight octets and OBJECT IDENTIFIERs of at most 128
  sub-identifiers each below 2^32; it checks every length against what remains
  before it reads, never recurses, and never goes more than eight elements deep.
  Anything else is a packet that does not decode.
* Reports: `usmStatsUnknownEngineIDs` (discovery), `usmStatsNotInTimeWindows`
  (authenticated, carrying the boots and time a manager needs),
  `usmStatsUnknownUserNames`, `usmStatsUnsupportedSecLevels`,
  `usmStatsWrongDigests` and `usmStatsDecryptionErrors`, and - authenticated,
  since the request they answer was - `snmpUnknownPDUHandlers` for a context
  engine that is not this one and `snmpUnknownContexts` for a context name that
  is not the default; each only for a request that set the reportable flag.

The objects it serves
---------------------

**system** (RFC 3418): `sysDescr` ("hMailServer" with the version and the
architecture), `sysObjectID` (1.3.6.1.4.1.32473.1.1, below), `sysUpTime` (since
the agent started), `sysContact`, `sysName` (the server's host name setting, or
the machine's name), `sysLocation`, `sysServices` (72: an application host) and
`sysORLastChange` (0; the `sysORTable` is empty).

**snmp** (RFC 3418): `snmpInPkts`, `snmpInBadVersions`,
`snmpInBadCommunityNames`, `snmpInBadCommunityUses` (a Set over SNMPv2c),
`snmpInASNParseErrs`, `snmpEnableAuthenTraps` (2, disabled), `snmpSilentDrops`,
`snmpProxyDrops`. **snmpEngine** (RFC 3411), **snmpMPDStats** (RFC 3412),
`snmpUnavailableContexts` and `snmpUnknownContexts` (RFC 3413) and **usmStats**
(RFC 3414). All of these count from the moment the agent started, as
`sysUpTime` does.

### The mail MIB (RFC 2789)

The tables are indexed by `applIndex` of NETWORK-SERVICES-MIB (RFC 2788). This
server is `applIndex` **1**; the `applTable` itself is not served.
`mtaGroupTable` has one row per protocol the server runs - **1 SMTP, 2 POP3,
3 IMAP**, each with its own index whether or not the others run, and absent while
its service is off (`service_smtp`, `service_pop3`, `service_imap`). Columns are
`1.3.6.1.2.1.28.1.1.<column>.1` and `1.3.6.1.2.1.28.2.1.<column>.1.<group>`.

"Delivered" below is `hmailserver_messages_delivered_total`: a delivery pass
that delivered to every recipient, a local mailbox or another server. "The
delivery queue" is `hmailserver_delivery_queue_messages` and its oldest message
`hmailserver_delivery_queue_oldest_message_age_seconds`, read from the database
the way `/metrics` reads them, by a thread of the agent's own: once when the
agent starts, and after that at most every ten seconds and only while a manager
is asking - so the thread that answers never waits on the database.

**mtaTable** (`mtaEntry` = 1.3.6.1.2.1.28.1.1):

| Column | Object | Answer |
|---|---|---|
| 1 | `mtaReceivedMessages` | `noSuchObject`. No counter of messages received exists; `hmailserver_processed_messages_total` counts delivery passes, a retry again, which is not the same number. |
| 2 | `mtaStoredMessages` | The delivery queue's depth (Gauge32). |
| 3 | `mtaTransmittedMessages` | Delivered (Counter32). |
| 4 | `mtaReceivedVolume` | `noSuchObject`: no counter of octets received. |
| 5 | `mtaStoredVolume` | `noSuchObject`: `/metrics` has no gauge of the queue's size in octets. |
| 6 | `mtaTransmittedVolume` | `noSuchObject`: no counter of octets delivered. |
| 7 | `mtaReceivedRecipients` | `noSuchObject`: recipients are not counted. |
| 8 | `mtaStoredRecipients` | `noSuchObject`: likewise. |
| 9 | `mtaTransmittedRecipients` | `noSuchObject`: likewise. |
| 10 | `mtaSuccessfulConvertedMessages` | `noSuchObject`: this server does not convert messages between formats in the MTA-MIB's sense, and counts nothing of the kind. |
| 11 | `mtaFailedConvertedMessages` | `noSuchObject`: likewise. |
| 12 | `mtaLoopsDetected` | `noSuchObject`: a message refused for its hop count is not counted. |

**mtaGroupTable** (`mtaGroupEntry` = 1.3.6.1.2.1.28.2.1):

| Column | Object | Answer |
|---|---|---|
| 2 | `mtaGroupReceivedMessages` | `noSuchObject`, as `mtaReceivedMessages`. |
| 3 | `mtaGroupRejectedMessages` | `noSuchObject`: refusals are not counted per protocol. |
| 4 | `mtaGroupStoredMessages` | SMTP: the delivery queue's depth. POP3 and IMAP store nothing in transit: `noSuchInstance`. |
| 5 | `mtaGroupTransmittedMessages` | SMTP: delivered. POP3, IMAP: `noSuchInstance`. |
| 6 - 11 | the group's volumes and recipients | `noSuchObject`, as in `mtaTable`. |
| 12 | `mtaGroupOldestMessageStored` | SMTP: the age of the oldest message in the delivery queue, as a TimeInterval (hundredths of a second); 0 when the queue is empty. POP3, IMAP: `noSuchInstance`. |
| 13 | `mtaGroupInboundAssociations` | The sessions open now: `hmailserver_sessions` for the protocol (Gauge32). |
| 14 | `mtaGroupOutboundAssociations` | `noSuchObject`: outbound SMTP connections are not a gauge. |
| 15 | `mtaGroupAccumulatedInboundAssociations` | `noSuchObject`: connections accepted are not counted. |
| 16 | `mtaGroupAccumulatedOutboundAssociations` | `noSuchObject`: likewise. |
| 17 | `mtaGroupLastInboundActivity` | `noSuchObject`: not recorded. |
| 18 | `mtaGroupLastOutboundActivity` | `noSuchObject`: not recorded. |
| 19 | `mtaGroupRejectedInboundAssociations` | `noSuchObject`: the refusals `/metrics` counts (country policy) are not per protocol. |
| 20 | `mtaGroupFailedOutboundAssociations` | `noSuchObject`: not counted. |
| 21 | `mtaGroupInboundRejectionReason` | `noSuchObject`. |
| 22 | `mtaGroupOutboundConnectFailureReason` | `noSuchObject`. |
| 23 | `mtaGroupScheduledRetry` | `noSuchObject`: retries are scheduled per message, not per group. |
| 24 | `mtaGroupMailProtocol` | `applTCPProtoID` and the protocol's registered port, as the MIB asks: 1.3.6.1.2.1.27.4.25, .110 and .143 - whatever port the server is configured on. |
| 25 | `mtaGroupName` | `SMTP`, `POP3`, `IMAP`. |
| 26, 27 | converted messages | `noSuchObject`, as in `mtaTable`. |
| 28 | `mtaGroupDescription` | A sentence saying what the group is. |
| 29 | `mtaGroupURL` | `noSuchObject`. |
| 30 | `mtaGroupCreationTime` | Since the agent started, which is when the servers started (TimeInterval). |
| 31 | `mtaGroupHierarchy` | 0: no group contains another. |
| 32 | `mtaGroupOldestMessageId` | `noSuchObject`: the queue's figures are an aggregate, not a message. |
| 33 | `mtaGroupLoopsDetected` | `noSuchObject`, as `mtaLoopsDetected`. |
| 34 | `mtaGroupLastOutboundAssociationAttempt` | `noSuchObject`: not recorded. |

**mtaGroupAssociationTable** (1.3.6.1.2.1.28.3) is empty: its one column refers
into NETWORK-SERVICES-MIB's association table, which is not served.
**mtaGroupErrorTable** (1.3.6.1.2.1.28.5) is empty: replies are not counted per
status code. A walk passes over both.

What a Get of something the agent does not have answers:

* `noSuchObject` for an object above that answers it, and for columns 4, 5 and
  12 **while SMTP is off** - only the SMTP row has them, so with SMTP off the
  agent has no row that does.
* `noSuchInstance` for a row a served column does not have - the IMAP row's
  column 4, or column 13 of a group whose service is off while another's runs -
  and for a served object named without its instance (`sysUpTime` rather than
  `sysUpTime.0`), as RFC 3416 section 4.2.1 says.

GetNext and GetBulk step over all of them.

### The enterprise arc - not registered

**1.3.6.1.4.1.32473** is the enterprise number RFC 5612 reserves for
documentation and examples. It is **not registered to this project with IANA**;
this project has no enterprise number of its own yet, and the agent uses this
one as a placeholder, as the syslog sink's structured data does
([Syslog.md](Syslog.md)). When a number is registered, `sysObjectID`, the
objects below and the engine ID's prefix move to it in one release, and a
manager configured for these OIDs will need them changed. There is no MIB module
file for them yet, for the same reason.

`sysObjectID` is 1.3.6.1.4.1.32473.1.1. The rest are scalars
`1.3.6.1.4.1.32473.1.2.<n>.0`, each the `/metrics` series named:

| n | Type | `/metrics` series |
|---|---|---|
| 1 | Counter32 | `hmailserver_processed_messages_total` |
| 2 | Counter32 | `hmailserver_messages_deferred_total` |
| 3 | Counter32 | `hmailserver_messages_bounced_total` |
| 4 | Counter32 | `hmailserver_spam_messages_total` |
| 5 | Counter32 | `hmailserver_viruses_removed_total` |
| 6 | Counter32 | `hmailserver_auth_success_total` |
| 7 | Counter32 | `hmailserver_auth_failures_total` |
| 8 | Counter32 | `hmailserver_tls_handshakes_total` |
| 9 | Counter32 | `hmailserver_tls_handshake_failures_total` |
| 10 | Counter32 | `hmailserver_spam_trap_hits_total` |
| 11 | Counter32 | `hmailserver_country_blocked_total` |
| 12 | Counter32 | `hmailserver_db_slow_queries_total` |
| 13 | Gauge32 | `hmailserver_messagestore_missing_files` |
| 14 | INTEGER | `hmailserver_state`, as a number: 0 unknown, 1 stopped, 2 starting, 3 running, 4 stopping, 5 draining |
| 15 | Gauge32 | `hmailserver_delivery_queue_messages` |
| 16 | Gauge32 | `hmailserver_delivery_queue_oldest_message_age_seconds` |

A Counter32 wraps at 2^32, as every counter in SNMP does, and a manager computes
a rate across the wrap; a Gauge32 holds at its largest.

On Linux, in a container, and through a firewall
------------------------------------------------

The packaged `hmailserver.service` runs as its own user with
`CAP_NET_BIND_SERVICE`, which it already has for port 25; that is what lets the
agent bind UDP 161, and nothing more is needed. A container publishes it as UDP
(`-p 161:161/udp`, or a Service port with `protocol: UDP`), and **each container
needs a host name of its own**: two started with one fixed hostname are one
engine (above). The agent is a UDP listener on one port: a firewall in front of
it should let the managers in and nobody else, and drop datagrams that claim a
manager's address from the wrong side - the allow list judges the address a
datagram states, and only the network can say whether that is where it came
from.

When it does not start
----------------------

| Error | Meaning |
|---|---|
| 7420 | The port could not be bound: another program holds it (an operating system's own SNMP service, on Windows or Linux, listens on 161 when it is installed), or, on Linux outside the packaged unit, the process lacks `CAP_NET_BIND_SERVICE`. The server runs on without the agent. |
| 7421 | The engine's seed or this engine's boot count could not be read or written in `hm_settings`, or its row holds something that is not a seed or a count. The agent does not start, for the reason above; the error names the row. Emptying the `SnmpEngineSeed` row makes a new engine with a count of its own (above); emptying the `SnmpBoots` row alone is not the way. |
| 7422 | An exception left the agent's thread, which has stopped answering. A Reinitialize starts it again. |

None of the three can happen while `SnmpEnabled` is off, which is the default.

What is tested
--------------

`SnmpTester`, inside the service with the other class tests on Windows
(`Utilities.RunTestSuite`) and as the `snmp-tests` program on Linux, holds the
BER both ways; the decoder's bounds - a length past the end, an indefinite or
five-octet length, a tag of two octets, an INTEGER of nine octets, an OBJECT
IDENTIFIER of 129 sub-identifiers or with one past 2^32, nesting past eight;
the cryptography - the key localisation on RFC 3414 A.3's password and engine ID
with SHA-256 (a value computed independently, since RFC 3414 publishes it for MD5
and SHA-1 only), HMAC-SHA-256-192 against Python's `hmac` module on the same
inputs, and AES-128-CFB against NIST SP 800-38A F.3.13, a final block cut short
included; SNMPv2c on, off, wrong community, SNMPv1, the cap of eight times the
request, and a Get of two 255-character values answered `tooBig` with no
bindings under it and in full over SNMPv3; SNMPv3 discovery, the time window and its edges, the right keys, a
wrong authentication password, a wrong privacy password, authNoPriv, an unknown
user, an unknown context answered with an authenticated Report, a latched
engine, Set, the same request answered twice under two salts, a GetBulk held to
a `msgMaxSize` of 484, a walk of every object in order, GetBulk to the end of the
view and to its caps, and `noSuchInstance` for an object named without its
instance; a valid request cut at every length; every element of an SNMPv2c
request, of an SNMPv3 request and of an SNMPv3 scoped PDU cut short at every
length **inside true outer lengths** - the scoped PDU then encrypted and signed,
so that the digest holds and the decoder behind it is reached; thirteen malformed
shapes; four thousand requests turned over at random, and two thousand SNMPv3
plaintexts turned over and then signed. None of them may throw, be answered with
anything that does not read back, be answered longer than its bound, or - for an
SNMPv3 message changed after it was signed - be answered with anything but a
Report. **Every cut that leaves a child of a constructed element unfinished**,
or an INTEGER or OBJECT IDENTIFIER empty, must go unanswered and move
`snmpInASNParseErrs` by exactly one, and each message must have such cuts at
every level it has - the message, the header, the USM parameters, the PDU, the
binding list and one binding - so a check missing at any level fails the test.

Built with `-DHM_SANITIZE=ON` (hmailserver/source/Server/CMakeLists.txt, which
gives the three commands), `snmp-tests` runs under AddressSanitizer and
UndefinedBehaviorSanitizer, and an undefined behaviour ends the run; the
`linux-sanitize` CI job is the one that runs it.

`RegressionTests/Infrastructure/SnmpAgent.cs` runs the agent in the service, on
a test port, with a client of its own - its own BER, USM, HMAC-SHA-256 and
AES-128-CFB, written in the test and sharing no code with the server - and
proves discovery, a Get of `sysUpTime`, `sysObjectID` and an MTA-MIB counter,
two answers under two salts, that counter moving when a message is delivered, a
GetNext walk, a bounded GetBulk, a wrong password answered with a Report and no
data, an address outside the allow list not answered until a CIDR range names
it, SNMPv2c refused while it is off and answered when it is on with the right
community and not with a wrong one, and the REST keys: the refusals - a contact
outside ASCII and a location with a line break among them - and the secrets
never read back.

Not proved by any test: a real manager (Net-SNMP) against the agent, which is
the proof that matters most and is a bench step rather than a fixture; the
Linux bind to 161 under the packaged unit; the delivery queue's two figures over
SNMP (the fixture reads a counter, not the queue); two nodes sharing a database;
and errors 7420 to 7422.

What it does not do
-------------------

* **No notifications.** No traps and no informs; the agent answers and nothing
  else. A manager that wants to be told polls.
* **No Set.** Every Set is `noAccess`.
* **One user, one level.** No usmUserTable, no VACM beyond "this user may read
  everything", no SHA-1, MD5 or DES, no authNoPriv.
* **No record of answered requests.** A recorded SNMPv3 request replayed inside
  its window is answered again, as RFC 3414 allows (above).
* **No SNMPv1**, and no SNMP over TCP or TLS (RFC 6353).
* **No applTable** (NETWORK-SERVICES-MIB), and none of the MTA-MIB objects
  above that answer `noSuchObject`: each would need a counter this server does
  not keep, and a counter kept only for SNMP would be a second set of numbers
  that `/metrics` and OTLP do not agree with.
* **No registered enterprise number**, and so no MIB module file for the
  enterprise arc.
