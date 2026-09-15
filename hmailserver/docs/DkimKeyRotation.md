DKIM keys that rotate
=====================

A DKIM key used to be generated once, written to a file and used until somebody
remembered it. This page is about the ordinary practice instead, which the server
now does per domain on a schedule or when asked: a new key is generated and its
TXT record shown; the record is checked from the server's own resolver; signing
moves to the new key only once the record has resolved with exactly that key for
a propagation period; the old selector is kept published for a retention period
for the verifiers and the reports that arrive late; and then it is retired - its
key file removed, and its record listed as one that may go.

**It is off on every domain until it is switched on**, and a domain whose
rotation is off and has nothing in flight is never looked up and never changed.
The server never edits DNS. It says what to publish, checks it, and waits.

---

The walk
--------

A rotation is three slots, each of them a selector with a key file:

| Slot | What it is | Its record |
|---|---|---|
| **current** | The domain's selector and private key file - the only key that signs. | keep |
| **next** | The domain's *secondary* selector and key: generated, published, waiting to sign. It never signs before the switch. | publish |
| **retiring** | The selector that signed before the last switch, kept for mail signed with it that is still on its way. | keep |

And, once it is gone, the **retired** selector, whose record may be removed.

1. **Start.** Either the schedule starts a rotation when the current key is
   older than the interval, or somebody presses *Rotate now*. The server
   generates an RSA key, writes it to its DKIM directory (below), and stages it
   as the domain's secondary under a new selector - the one you give, or `s`
   and today's UTC date (`s20260915`, then `s20260915b` if that is taken). The
   status shows the TXT record to publish.
2. **Publish** the record at your DNS provider: the host is
   `<selector>._domainkey.<domain>` and the value `v=DKIM1; k=rsa; p=...`.
3. **First sighting.** Every pass of the rotation - every fifteen minutes, and
   whenever *Check DNS now* is pressed - looks the record up. The first pass that
   finds a record carrying exactly the new key's public half records the moment.
   **Any pass that does not find it matching sends that moment back to zero**: a
   record that was there and has gone, or that was edited, is not evidence of
   anything, and the wait starts again when it is back.
4. **Propagation.** Signing moves when the record has matched for the whole
   propagation period (72 hours by default) since the first sighting, in a pass
   that has just checked it again, and the retiring slot is free.
5. **Switch.** The new key becomes the domain's selector and key file, the
   secondary is emptied, and the old selector goes into the retiring slot. The
   domain's cache entry is dropped at the same moment, so the next message is
   signed with the new key.
6. **Keep.** The old selector's record stays published for the retention period
   (7 days by default). Verifiers that receive mail late, and the DMARC and
   ARF reports that arrive days later, still find it.
7. **Retire.** At the end of the retention period - or when *Retire the old key
   now* is pressed - the old selector is retired. A key file the rotation
   generated is deleted (with the guard described below); one it did not
   generate is left where it is, and the status says so.
8. **Remove** the retired selector's record from DNS. The status lists it with
   the action `remove`.

A second rotation that becomes ready while the first one's old selector is still
being kept waits for it, and switches in the pass that retires it: two retiring
selectors at once would be two records to track and nothing gained.

A rotation started with *Rotate now* is carried to its end with the schedule
off. Only the *start* of a rotation is the schedule's decision.

A domain with the schedule on and **no key at all** is given its first one the
same way - generated, published, seen, then made the signing key. Signing itself
is still the domain's *DKIM signing* switch: the rotation moves keys, it does not
switch signing on.

---

Switching it on
---------------

| Where | How |
|---|---|
| Control Panel | *Domains*, the domain, the *DKIM* tab, *Key rotation*: the schedule is saved with the domain; *Rotate now*, *Check DNS now*, *Switch now*, *Retire the old key now* and *Cancel the next key* act at once. The *DNS records* page lists the next key's records beside the current one. |
| Control Deck | *Domains*, the domain: the DKIM group holds the schedule, and the *DKIM key rotation* card below it holds the records, the checks, what is waiting and the same five buttons. |
| REST | `PUT /api/v1/domains/{domain}` with the keys below; the rotation's own routes are listed at the end of this page. |
| Command line | `hmctl domain update example.com --set dkim_rotation_enabled=true`, then `hmctl dkim status example.com`. PowerShell: `Set-HmDomain`, `Get-HmDkimRotation -Records`. See [CommandLine.md](CommandLine.md). |
| COM | `Domain.DKIMRotationEnabled` and the four properties below, then `Domain.Save()`. |

The schedule is five columns of the domain in the database (`hm_domains`), so it
travels with the domain: through the COM object, the REST route, the Deck, the
Control Panel and the XML backup. Nothing is written to `hMailServer.ini`.

| Setting | REST key | COM property | Default | Range |
|---|---|---|---|---|
| Rotate on a schedule | `dkim_rotation_enabled` | `DKIMRotationEnabled` | off | |
| Days between rotations | `dkim_rotation_interval_days` | `DKIMRotationIntervalDays` | 180 | 1 to 3650 |
| Propagation period, hours | `dkim_rotation_propagation_hours` | `DKIMRotationPropagationHours` | 72 | 1 to 720 |
| Retention period, days | `dkim_rotation_retention_days` | `DKIMRotationRetentionDays` | 7 | 1 to 365 |
| Key size, bits | `dkim_rotation_key_bits` | `DKIMRotationKeyBits` | 2048 | 2048, 3072 or 4096 |

Every interface refuses a value out of range rather than clamping it.

**Why those defaults.** Six months is the longest interval M3AAWG's *DKIM Key
Rotation Best Common Practices* (2019) recommends; a shorter one costs nothing but
records. Seventy-two hours is far longer than a resolver holds a negative answer
(RFC 2308 suggests one to three hours, and BIND caps it at three by default), with
room for a record published on a Friday evening and looked at on Monday. Seven days
covers mail that sat in a queue through a receiver's outage and the aggregate
reports that are sent the day after. 2048 bits is what every verifier accepts: its
record is a little over 400 characters, which most DNS providers split into two
TXT strings themselves. A 4096-bit key's record is over 700 characters, which
some providers refuse, and generating one takes seconds.

---

What the status says
--------------------

`GET /api/v1/domains/{domain}/dkim/rotation` and `Domain.DKIMRotationStatus`
answer the same document: the policy; the phase; the current, next, retiring and
retired slots with their times (seconds since the epoch), key files, whether the
rotation generated each file, the records with their action, and each slot's last
check; what is waiting and until when; and the last error. The Deck and the
Control Panel draw it.

| Phase | Meaning |
|---|---|
| `off` | The schedule is off and nothing is in flight. |
| `idle` | Nothing is staged. The schedule is on and waiting for the interval, or an old selector is still being kept. |
| `awaiting_dns` | A next key is staged and its record has not been seen matching. |
| `propagating` | The record matches; the propagation period is running. |
| `ready` | The period is over and the next pass switches. |
| `waiting_for_retiring_slot` | Ready, but an earlier rotation's old selector is still being kept. |

| Check | Meaning |
|---|---|
| `unchecked` | Not looked up yet. |
| `matches` | Every name the selector signs for (below) has a TXT record carrying exactly this key. |
| `not_found` | A name has no TXT record. `detail` names it. |
| `different` | A name has TXT records, and none carries this key. `detail` names it and quotes the first record. **Waiting will not fix this.** |
| `lookup_failed` | The lookup itself failed - the resolver did not answer, or DNSSEC validation failed. Tried again at the next pass. |
| `key_unreadable` | The key file could not be read by the server. |

The waiting reasons are the same words and a few more: for a start,
`not_scheduled`, `interval` and `domain_inactive` (the schedule starts nothing for
an inactive domain); for the switch, `unchecked`, `record_not_found`,
`record_differs`, `lookup_failed`, `key_unreadable`, `propagation` and
`retiring_slot`; for the retire, `retention`.

The next key's record is checked at every pass. The current and retiring records
are checked once an hour, and only to report on them: a current record that has
gone missing is worth knowing about, and it moves nothing.

### What "checked from the resolver" proves

The record is looked up with the resolver the server itself uses for its own
lookups - the one DKIM verification of incoming mail uses, including the DNS
servers configured on the server and DNSSEC validation when it is on. A match
proves that **this server's resolver** sees the new key. It does not prove that
every receiver's resolver does: a receiver that asked for the name before it
existed may hold the negative answer until its cache expires. The propagation
period is what covers that, which is why it runs from the first sighting and
starts again whenever a pass does not see the record.

A record matches when it is `v=DKIM1` or has no `v=`; its `k=` is absent (rsa) or
the key's own type; its `h=`, if any, allows `sha256`; its `s=`, if any, allows
`email`; and its `p=`, with every space removed, decodes to exactly the key's
public half. An empty `p=` is a revoked key and never a match. One matching record
among several at the same name is a match, which is what a verifier does.

### Domain aliases

With *sign aliases too* (`dkim_sign_aliases`) on, the server signs mail from a
domain alias with `d=<alias>` and the domain's own selector, so the record has to
exist under every alias as well. The status lists one record per name, and the
switch waits until **every** one of them matches: a switch that looked only at
the main name would break DKIM for every message sent from an alias.

---

Where the keys are, and who can read them
-----------------------------------------

Generated keys are written to **`<DataDirectory>/DKIM`** - `C:\Program
Files\hMailServer\Data\DKIM` on a default Windows install, `/var/lib/hmailserver/DKIM`
from the Linux packages - as `<selector>._domainkey.<domain>.pem`, PKCS#8 PEM. The
directory is made on first use.

On Windows the file is created with a protected security descriptor naming only
SYSTEM, the Administrators group and the account the service runs as; it does not
inherit the data directory's permissions, which on a default install let every
local user read. On Linux it is created with mode 0600 at `open(2)`, never widened
afterwards. It is the same rule the server applies to its update token.

That has one visible consequence: **a Control Panel that is not elevated cannot
read a generated key**, even on the server itself. It does not need to. The
records it shows come from the server's status document, not from the key file.

The key file path is stored in the domain's 255-character key file column. A data
directory with a very long path, a long selector and a long domain name can add up
past that; the rotation refuses to generate such a key and says what to shorten.

### What is deleted, and what never is

The rotation deletes a key file - when a selector retires, when a next key is
cancelled, and when a domain is deleted - only when **all** of these hold:

* the rotation generated that file (a flag in `hm_dkimrotations` that no
  interface can set);
* the file is inside `<DataDirectory>/DKIM`, after resolving links;
* it holds a private key;
* no domain's key file or secondary key file - the retiring domain's own included -
  and no other domain's retiring slot, names it.

A key file an administrator supplied - from the Control Panel's own generator, or
anywhere else - is left where it is, which is what the manual walk has always done.
Deleting a domain deletes the keys its rotation generated, as it deletes the
domain's mail - once the domain is gone, so a delete that fails leaves a domain
that can still sign; a domain recreated under the same name needs new keys and
records. **A restore from backup deletes no key file**, although it deletes every
domain before it loads the backup's: the domains it puts back name the same files.

---

Rotation and the other ways to change a key
-------------------------------------------

A domain whose rotation is on, or that has a next or retiring selector in flight,
is **managed**. On a managed domain, a save that would change the selector, the key
file, the secondary selector or the secondary key file is refused - over COM with
an error, over REST with `409` - with a sentence that says to use the rotation's own
operations. That is the one place a COM `Save` and a REST `PUT` both pass, and it
exists for one case: a Domain object, a dialog or an exported configuration
holding values from **before** the rotation last moved. Saved blindly, it would put
the old selector back, and once the old key had been retired that is a domain
signing with a file that no longer exists. A restore from backup is not affected.

The clients avoid tripping it: the Control Panel writes the selector and key file
only when they were changed in the dialog, the Deck sends only the keys that
changed, and `hmctl config` and the PowerShell module leave those four keys out of
a configuration document for a domain whose rotation is on.

To take a domain back to managing its key by hand: cancel the next key if there is
one, retire the old selector (or wait for it), and switch the schedule off. The
domain is then unmanaged, and the secondary selector and *DKIMPromoteSecondary*
work as they always have.

A secondary selector staged by hand **before** the schedule is switched on - or in
the same save that switches it on - is adopted by the rotation: it is checked, and
switched to once its record has resolved for the propagation period. It is not
deleted when it retires, because the rotation did not generate it. Once the
schedule is on, the domain is managed and a save that stages a secondary is
refused; stage the next key with *Rotate now* instead.

---

When it goes wrong
------------------

**Alerts.** The `dkim.rotation.stalled` condition is raised when a next key's record
has not been seen for the rule's threshold in days (7 by default) or when the last
attempt to move a running rotation failed - running meaning the schedule is on, or
a next or retiring selector is in flight - and cleared when neither is true. A
*Rotate now* that failed on a domain with the schedule off and nothing staged is in
the status document's `last_error`, and raises nothing: nothing would ever try it
again, so the alert could never clear. It is on
by default and held for the daily digest; it reads the rotation's stored state and
never looks anything up. See [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md).

**The audit trail.** A switch is a change to the domain, and it is recorded as one.
A switch made by the scheduled pass is recorded with the actor `DKIM key rotation`
of kind `system`; one made over REST or COM is recorded with its caller. The key
file columns are withheld as `(changed)`, as every key file is.

**The application log** has a line for every key generated, every switch (with the
record to keep and until when), every retire (with the record that may go and what
happened to the file), every cancel, and every pass run with the clock moved.

**The error log** is for faults, not for waiting. A record that is missing or
different is the ordinary state of a rotation waiting for somebody, and is never an
error.

| Code | What failed |
|---|---|
| 6610 | OpenSSL could not generate the key. |
| 6611 | The key file could not be written with its protected permissions. |
| 6612 | The DKIM key directory could not be created. |
| 6613 | `hm_dkimrotations` could not be read or written. |
| 6614 | The domain's selectors could not be updated in the database. |
| 6615 | A generated key file that nothing uses any more could not be deleted. |

### A key that has been compromised

1. If an earlier rotation's old selector is still being kept (the status has a
   *retiring* key), *Retire the old key now* first. A switch never leaves two
   selectors retiring, and force does not change that: *Switch now* is refused
   with `retiring_slot` while one is.
2. *Rotate now*, and publish the new record at once.
3. *Check DNS now* until the record matches, then *Switch now* with the box that
   skips the propagation period (`hmctl dkim switch --force`,
   `Complete-HmDkimRotation -Force`). Force skips the period and never the check:
   the switch is still refused while the record does not resolve.
4. *Retire the old key now* again - the compromised key is the one retiring now -
   which deletes its key file if the rotation made it.
5. Do **not** simply remove the old selector's record. Publish it with an empty key,
   `v=DKIM1; p=`, which tells every verifier the key is revoked rather than missing.

---

Several machines, backups and restores
--------------------------------------

**Warm standby and shared databases.** The rotation's state is in the database, and
its keys are files. Replicate `<DataDirectory>/DKIM` with the rest of the data
directory, at the same path, as [WarmStandby.md](WarmStandby.md) already asks for
DKIM keys. Every change the rotation makes to a domain is a compare-and-set on the
selectors it expects, read back, so two nodes cannot stage or switch twice; a node
that cannot read the next key reports `key_unreadable` and does not switch, and a
node that does not see a file does not delete it.

**Backups.** A backup that includes the data directory includes the key files, and
the backup archive is not encrypted, so it holds the private keys in the clear:
treat it as you treat the keys. A copy is not guaranteed to keep the protected
permissions the rotation wrote; after restoring the data directory, check the DKIM
directory's permissions, or rotate. The domain's XML backup carries the schedule
and its periods (a backup taken before this feature restores the schedule off and
the periods at their defaults) and not the rotation's state: after a restore the
first pass adopts the domain's selectors, with the current key as old as its file.
A key that was retiring when the backup was taken is forgotten, and its file stays.
The restore itself deletes no key file (above). The keys it adopts are no longer
marked as generated by the rotation - that mark was in the state the backup does
not carry - so when one of them retires, its file is kept, and the status says so.

**Moving an installation.** `hm_dkimrotations` holds key file paths too; see
[RelocatingAnInstallation.md](RelocatingAnInstallation.md).

---

What it does not do
-------------------

* **It does not edit DNS.** It shows the records, checks them and waits.
* **It does not generate Ed25519 keys.** An Ed25519 key staged by hand is read and
  its record checked (`k=ed25519`, the raw 32-byte `p=`), but a domain that signed
  with Ed25519 alone would fail DKIM at every verifier without Ed25519 support, so
  generated keys are RSA.
* **It does not sign twice** with the old and new key during the switch. The
  propagation period is what makes one signature enough.
* **It does not publish a revocation.** Removing or emptying a record is yours to do.

---

The routes
----------

| Method and path | What it does |
|---|---|
| `GET /api/v1/domains/{domain}/dkim/rotation` | The status document. |
| `POST /api/v1/domains/{domain}/dkim/rotation/start` | Rotate now. Body `{"selector": "..."}`, optional. `201`, or `400` for a selector that is not lower-case DNS labels, or `409` while a next key is staged. |
| `POST /api/v1/domains/{domain}/dkim/rotation/check` | Check the records now. |
| `POST /api/v1/domains/{domain}/dkim/rotation/switch` | Switch now. Body `{"force": true}` skips the propagation period. `409` with a `reason` and the status beside it when refused. |
| `POST /api/v1/domains/{domain}/dkim/rotation/retire` | Retire the old selector now. `409` when nothing is retiring. |
| `DELETE /api/v1/domains/{domain}/dkim/rotation/next` | Cancel the next key. `409` when nothing is staged. |
| `POST /api/v1/dkim/rotation/run` | One pass over every domain now. Body `{"seconds_ahead": N}` moves the clock for that pass - a test's tool, below. |

The six domain routes are scoped to an API key's domains, and the five that change
something are refused to a read-only key. The run is refused to a key restricted to
domains, because it advances every domain's rotation.

`seconds_ahead` (and `Utilities.RunDkimRotation(SecondsAhead)` over COM) is how the
regression suite waits out a propagation period of three days and a retention
period of a week in a few seconds: from 0 to 316224000 (3660 days), for the
administrator password or an API key that is neither read-only nor restricted to
domains (a server administrator over COM), and written to the application log when
it is not zero. The status document is always computed on the real clock.

**It is a test's tool, not for a production server.** The offset itself is not
stored, but what the pass does is real: a switch or a retire it decides happens at
once - before the real propagation or retention period is over - and every time it
records is the moved clock's and stays in `hm_dkimrotations`. A first sighting it
records makes the real switch wait the offset longer; a switch it makes holds the
next scheduled rotation and the retiring slot back by the offset; a retire it makes
has already dropped the old selector while late mail may still need it. On a
production server, use *Switch now* and *Retire the old key now*, which say what
they skip.
