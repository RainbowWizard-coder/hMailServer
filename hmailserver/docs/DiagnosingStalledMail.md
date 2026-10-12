Diagnosing slow or stalled mail
===============================

This guide covers the case where mail is not moving and the server appears
healthy: no crash, no error, the service is running, and the logs seem to stop
mid-transaction. It exists because that exact shape of problem took three
releases to track down
(GitHub discussion 18, on the project's former home),
almost entirely because the server used to be silent about it.

It is no longer silent. Read this before opening an issue — the answer is
usually in one line of the log.

First: which half is stuck?
---------------------------

The two halves fail differently and have different causes. Work out which one
you have before anything else.

**Accepting** — the sending server connects, sends the message, and then waits.
From its side you will see a timeout *after* the message body was transmitted.
Postfix reports this as `timed out while sending end of data`. The message never
appears in your queue.

**Delivering** — the message is accepted (the sender got a `250`), is visible in
the delivery queue, and never leaves.

Everything below is split along that line.

Turn on debug logging first
---------------------------

Control Panel → **Logging** → tick **Debug messages**, and make sure logging is
enabled. Reproduce the problem once. The lines this guide refers to are written
at debug level; the *slow* ones are also written at application level, so if you
only have application logging you will still see the important ones.

Remember to turn debug logging off afterwards on a busy server.

Accepting: the sender times out after sending the message
---------------------------------------------------------

Between the `354` and the `250` the server runs the accept pipeline: spam tests,
message modifications, archiving, the `OnAcceptMessage` script, and the database
save. All of it happens on a bounded pool of threads, and the reply is only sent
when it finishes.

The log now times each stage:

```
SMTPConnection - accept: start spam-protection.
Spam test: SpamTestDNSBlackLists, Score: 0, Time: 12 ms
Spam test: SpamTestSpamAssassin, Score: 0, Time: 120000 ms
SMTPConnection - accept: done spam-protection in 120047 ms (session 5).
SMTPConnection - accept: done message-modifications in 0 ms (session 5).
SMTPConnection - accept: start script/save.
SMTPConnection - accept: done script/save in 3 ms (session 5).
```

**How to read it.** Read it as a sequence and look for the last line written —
the stage that is stuck is the one after it. Two stages announce themselves with
a `start` line (`spam-protection` and `script/save`), so for those a `start` with
no matching `done` is the whole diagnosis. The middle stage does **not** have a
`start` line: message modifications run between `done spam-protection` and
`start script/save`, so a stall there shows up as `done spam-protection` followed
by silence. If every stage completes but one took a long time, the `Time:` or
`done ... in` value names the culprit directly. A stage taking ten seconds or
more is logged at application level even without debug logging, and so is any
individual spam test that takes ten seconds or more.

Common causes, in the order they actually occur:

| What the log shows | Cause |
|---|---|
| `SpamTestSpamAssassin` with a large `Time:` | spamd is unreachable, overloaded, or accepting connections without answering |
| `SpamTestDNSBlackLists`, `SpamTestSURBL` or `SpamTestSPF` slow | the resolver is not answering. Look in the TCP/IP log for `DNS - Query timed out` and `DNS - Query failure`. A DNS blacklist or SURBL lookup that fails is asked once more (below), so one that times out waits for `DNSQueryTimeout` twice for each of its A and AAAA queries; and SURBL, which stopped at the first lookup that failed, goes on to the message's other names until its ten seconds for beginning lookups are up, the ones begun then waited for. **Do not reach for `DNSServer` as the fix** — see the warning below |
| `done spam-protection`, then silence | message modifications: the spam headers, the signature, the List-\* headers, or the write of the modified message back to disk |
| `script/save` slow | an `OnAcceptMessage` event script, or the database |
| Nothing between `354` and silence | you are on a version older than 6.2.17; upgrade, because that is the version that added these lines |

> **`DNSServer` was broken in 6.2.17 and 6.2.18. It was fixed in 6.2.19 (issue
> #25).** On those two versions every lookup through a configured custom server
> failed: AAAA and CNAME queries returned `ERROR_TIMEOUT` in the same millisecond
> they were issued, and the A query returned a status the resolver treated as "no
> records". SpamAssassin was the visible symptom because it reports a failed
> lookup, but DNSBL, SURBL, SPF, DKIM and MX lookups failed the same way in
> silence — a server in that state had quietly stopped most of its spam filtering
> and was still accepting mail. **If you are on 6.2.17 or 6.2.18, upgrade, or
> leave `DNSServer` empty so the system resolvers are used.**
>
> The cause is worth knowing, because the fix looks like a bug. Moving to the
> asynchronous `DnsQueryEx` replaced a `PIP4_ARRAY`, which has no port field,
> with a `DNS_ADDR`, which carries a full `SOCKADDR` — and a destination port of
> **zero** is what that structure wants, because the DNS client supplies the port
> itself. Filling in 53 looks like the obvious correction and breaks every
> lookup: measured against a real server, port 53 returns status 87 and no
> records for all three record types, while port 0 returns the A record and a
> correct `DNS_INFO_NO_RECORDS` for a host with no AAAA. `DNSResolverWinApi.cpp`
> says *do not "fix" this line* directly above it, for this reason.
>
> One caveat survives the fix: a custom server list is only honoured together
> with `DNS_QUERY_BYPASS_CACHE`, so setting `DNSServer` still means every lookup
> pays full price. Leave it empty unless you need a specific resolver.

> **SPF did not honour `DNSServer` at all until 15 September 2026, and could not
> be evaluated on Linux.** The SPF evaluator was a vendored library that resolved
> names itself, through the Windows DNS client, rather than through this server's
> resolver. So on Windows an SPF check went to the system resolvers whatever
> `DNSServer` said — a server pointed at an internal resolver was asking the
> outside world about its senders — and on Linux there was no Windows DNS client
> to ask, so every SPF check returned a temporary error and said so in the error
> log once per process (`HM6406`). Because `SpamTestDMARC` evaluates SPF, DMARC on
> that platform lost its SPF half with it: a domain that authenticates by SPF
> alone could not align. The RFC 7208 evaluator that replaced the library resolves
> through the same `DNSResolver` as every other lookup, so `DNSServer`, the DNS
> cache setting, the query timeout and DNSSEC validation all apply to SPF now as
> they always did to everything else. If you run 6.3.3 or earlier on Linux, an SPF
> or DMARC verdict from it is not a verdict.

### When a spam test cannot look a name up

A spam test that cannot complete a DNS lookup lets the message through without
its verdict: it does not score, refuse or reject for a lookup that failed. From
6.3.7 a lookup has failed when it found nothing and any of its queries failed -
a SERVFAIL, a timeout, no DNS server answering - and not only when all of them
did (upstream PR 640). An address lookup asks A and AAAA (AAAA where IPv6 is
available, which is everywhere in practice), and the name's CNAME when neither
found anything. Before 6.3.7 an A query that failed beside an AAAA query
answered "no AAAA records" - the answer of every IPv4-only host, and of every
address a DNS blacklist does not list - was read as "this name has no
addresses". NXDOMAIN and NODATA are answers, and still mean "no addresses".

| Test | When its lookup fails | Before 6.3.7 |
|---|---|---|
| The HELO host (`CheckHostInHelo`) | not scored | scored when one of A and AAAA failed and the other was answered without an address, or when both were and the CNAME query failed |
| PTR (`CheckPTR`) | not scored, whether the PTR lookup or the address lookup of the name it gives failed | a failed address lookup of the reverse name was scored |
| MX records (`UseMXChecks`) | not scored | scored when the MX query found nothing and the CNAME query failed |
| A DNS blacklist | asked once more, past the DNS cache; when that fails too, the client is not checked against that list, and the log says so | asked once; a lookup that failed was "not listed", logged as `0 addresses found` like a list's answer |
| SURBL | asked once more, past the DNS cache; when that fails too, the name is passed over and the message's other names are asked | the first lookup that failed ended the list for the message, so a listed link after it was never asked about |
| DKIM keys, DMARC policies, SPF records | a temporary error, as for any lookup that fails: DKIM `TempFail`, DMARC and SPF `temperror` | a lookup that found no record and whose CNAME query failed read as "not published" - for a DKIM key, a `PermFail` |

The second attempt of a DNS blacklist or SURBL lookup bypasses the Windows DNS
cache, which would otherwise answer it from the first; on Linux the server keeps
no cache, and the second attempt is asked of the DNS server again. Setting
`UseDNSCache=0` or a `DNSServer` bypasses the cache for every query already.
A lookup that answered - with addresses or without - is not asked again.

Delivery makes the same address lookup of each of a recipient's mail servers,
and retries a message whose servers it cannot reach either way. When the lookup
failed for every one of them, the notice to the sender now says
`Unable to find the recipient's email server. The DNS query has failed.`
(`4.4.4`); before 6.3.7 a failed A query beside an AAAA query answered without
an address made it `No mail servers appear to exists for the recipient's
address` (`4.1.2`), which asks the sender to check the address.

What the log says:

| Line | Log | When |
|---|---|---|
| `DNS lookup failed: <address reversed>.<list>. The client could not be checked against this blacklist.` | TCP/IP | A DNS blacklist lookup failed twice. In place of the `DNS lookup: ..., 0 addresses found: (none), Match: False` line an answered lookup writes |
| `DNSBL: The lookup of <name> did not complete, asked twice. The client could not be checked against <list>, and its messages are accepted without that list's verdict.` | application | The same, at most once every 15 minutes |
| `SURBL: The lookup of <name> did not complete[, nor did <n> other lookups of the same message]. The message's other names were asked about, and it is accepted without the list's verdict on the names whose lookups did not complete.` | application | A SURBL lookup failed twice and no other name of the message was listed, at most once every 15 minutes |
| `DNS - The lookup of <name> did not complete. Asking once more, past the DNS cache.` | debug | A DNS blacklist or SURBL lookup is asked a second time |
| `SpamTestHeloHost: The lookup of <host> did not complete. The HELO host is not scored.` and `SpamTestPTR: The lookup of <name>, the reverse name of <address>, did not complete. The address is not scored.` | debug | The HELO or PTR test passed over a lookup that failed |

Since 6.2.17 acceptance is also bounded: if it exceeds `FinalizationTimeout`
(240 seconds by default) the server answers `451` and the sender retries, rather
than leaving it waiting for a reply that never comes. If you see `451 4.3.1`
responses, the accompanying error entry names the deadline that was hit, and the
stage timings above it name what consumed the time.

### If every message stalls, not just one

Look for this:

```
Task SMTP-accept session=42 ip=203.0.113.9 waited 8 seconds for a thread in
work queue Asynchronous task queue. 14 task(s) are still queued.
```

and, when it is severe:

```
All 15 threads in work queue Asynchronous task queue have been busy for at
least 120 seconds, so no further task on this queue can start. 14 task(s) are
queued. Running: SMTP-accept session=42 ip=203.0.113.9 (thread 4120, 168s), ...
```

That means every worker is occupied and messages are queuing behind them. One
slow dependency does this to the whole server, which is why a single wedged
scanner used to look like "the server stopped responding". The task names tell
you which sessions are stuck; the stage timings tell you what they are stuck on.

Delivering: the message is accepted but never leaves
----------------------------------------------------

Delivery runs on a separate, smaller pool. The usual causes:

* **A virus scanner that stops responding.** ClamAV is contacted after the
  message is accepted, so a wedged clamd shows up as "accepted but never
  delivered". Each socket operation is bounded by `ClamMinTimeout` /
  `ClamMaxTimeout` and a clamd that never answers is reported rather than held.
  Be precise about what that bound is, though: it is a deadline on *one* read or
  write, armed fresh each time, and the message is streamed to clamd in chunks —
  so a clamd that answers each chunk just before the deadline can make a single
  large message take considerably longer than `ClamMaxTimeout` in total. It cannot
  hold the thread forever; it can hold it for a while.
* **A remote server that answers extremely slowly.** The idle timeout is re-armed
  on every byte received, so a host that sends one byte occasionally used to hold
  a delivery thread indefinitely. Since 6.2.18 there is an absolute ceiling
  (`ClientSessionCeiling`, 30 minutes by default), armed once and never re-armed.
* **A custom virus scanner or external tool that hangs** — bounded by
  `ExternalProcessTimeout`.
* **A message held for a sandbox's verdict.** With `SandboxEnabled` on, a message
  none of your own signed-in users submitted, whose attachment is of a submitted
  type, waits for the sandbox you named to open the file — up to
  `SandboxTimeoutMinutes`, 15 by default — and while it waits it is in no
  mailbox, not in the quarantine and not the delivery queue's to retry: its
  Retry and Delete answer 409, naming the sandbox's routes. It is listed by
  `GET /api/v1/sandbox/held`, by `GET /api/v1/queue` marked `held: sandbox` (the
  `held` column of `hmctl queue list`), and in the Deck's queue page under
  *Held for a sandbox*, where it can be delivered without the verdict or
  discarded. The application log says when it was submitted and what the sandbox
  answered. See [Sandbox.md](Sandbox.md).
* **A deferral that names DNSSEC, with no host attempted.** With
  `DaneEnforcementEnabled` and `DnssecValidationEnabled` on (both are by
  default), the server validates the recipient domain's MX RRset, and each
  host's TLSA records, before it connects. A host whose lookup is bogus, or
  indeterminate - it did not complete, or a signed zone answered without its
  signatures or its proof of absence - is skipped (RFC 7672), and with every
  host skipped the delivery is deferred: `Delivery to example.com deferred: ...
  No host was attempted.` The application log line before it names the lookup,
  and since the release after 6.3.6 ends with the resolver's reason in brackets:

  ```
  SMTPDeliverer - Message 123: the MX RRset for example.com failed DNSSEC
  validation (the MX RRset of example.com does not validate: the RRSIG by
  example.com with key tag 44331 (algorithm 13) expired at 2026-10-08 04:18:05
  UTC, and this server's clock says 2026-10-09 09:12:40 UTC). Skipping
  mx.example.com (RFC 7672 section 2.2).
  ```

  The debug log has the same reason for every bogus or indeterminate MX, TLSA or
  TXT lookup - `DNSSEC: the MX lookup for example.com is bogus: ...` - and a line
  for a zone whose keys do not validate, `DNSSEC: the keys of example.com are
  bogus: ...`. What the reasons mean:
  * *does not match the records as received* - the signature is over other
    records than the ones that came. Either something on the way changed them
    or the zone is broken; ask a validating resolver (`delv`, or `dig +dnssec`
    against 1.1.1.1 and look for the `ad` flag) or DNSViz. **Until the release
    after 6.3.6 this was also what every signed domain whose MX answer came
    with its exchange compressed got - nearly every signed domain - through no
    fault of its own (work item 80).** On 6.2.19 to 6.3.6, a signed recipient
    domain whose mail is deferred this way while a validating resolver says it
    validates is that defect; `DaneEnforcementEnabled=0` delivers without DANE
    until the server is updated.
  * *expired at ... / is not valid until ..., and this server's clock says ...*
    - check the clock first; if it is right, the zone's signatures have lapsed.
  * *names no key ... publishes* or *no DNSKEY of ... matches its DS records* -
    a key rollover gone wrong in the recipient's zone, or at its parent.
  * *came without its signatures* or *came without an RRSIG: the DNS server did
    not return the DNSSEC records asked for* - the resolver this server asks
    (`DNSServer`, or the system's) strips DNSSEC records or does not support
    them. Point `DNSServer` at one that does; a validating resolver's verdict is
    not trusted here, its signatures are needed (the server asks with the DO and
    CD bits).
  * *did not complete* - no server answered, it answered SERVFAIL, or the reply
    did not parse: the message is retried, as for any temporary failure.
* **The database.** Check for errors mentioning the connection pool.

Check the delivery queue in the Control Panel, and the SMTP log for the
destination in question.

Settings that bound each stage
------------------------------

All are stored in the database and all are in seconds. Change one in the Control
Panel, over the REST API at `/api/v1/settings/ini/<name>`, or with `hmctl`; the
save applies it, because each is read where it is used ([SettingsApply.md](SettingsApply.md)).
They also appear in the `[Settings]`
section of `hMailServer.ini`, which since schema 6042 is a copy the server keeps
current rather than the place they are set — editing one there is named in
`hMailServer_ERROR.log` and put back at the next start. Defaults are chosen to be
well inside a typical sending server's timeout.

In the Control Panel, seven of them are the *Timeouts and queue bounds* card of the
*Server limits & expert settings* page; `SAMaxTimeout` and `ClamMaxTimeout` are on
the *Anti-spam settings* and *Anti-virus settings* pages. The same Server limits
page's *Windows service account* card - which account the service logs on as, and
the folders it is granted - is [ServiceAccount.md](ServiceAccount.md).

| Setting | Default | Bounds | `0` means |
|---|---|---|---|
| `FinalizationTimeout` | 240 | The whole accept pipeline, after which the sender gets a `451` | no bound |
| `SAMaxTimeout` | 90 | SpamAssassin: idle timeout, and a session ceiling of this plus 30s | **not** "no bound" — see below |
| `ClamMaxTimeout` | 90 | ClamAV: idle timeout per socket operation | **not** "no bound" — see below |
| `DNSQueryTimeout` | 10 | A single DNS query | no bound |
| `ScriptTimeout` | 60 | One event script invocation | no bound |
| `ExternalProcessTimeout` | 300 | An external scanner process | no bound |
| `ClientSessionCeiling` | 1800 | An entire outbound delivery session | no bound |
| `AsyncQueueStallThreshold` | 120 | How long every worker may be busy before the saturation report | reporting off |
| `DBConnectionAcquireTimeout` | 60 | Waiting for a pooled database connection | no bound |

**The two exceptions matter, because `0` tightens them instead of removing them.**
`SAMaxTimeout` and `ClamMaxTimeout` are not passed straight through: they go to
`TimeoutCalculator::Calculate(min, max)`, which returns the *minimum* whenever the
maximum is lower than it. So `SAMaxTimeout=0` gives a 30-second idle timeout
(`SAMinTimeout`) and a 30-second session ceiling — a *shorter* bound than the
default, not an absent one — and `ClamMaxTimeout=0` gives 15 seconds
(`ClamMinTimeout`). To lengthen either, raise the value; to lengthen it a long way,
raise the matching `...MinTimeout` too, or the calculator will pull it back under
load. There is no way to make either unbounded.

When the pool deadline expires, a recipient lookup answers `451 4.3.2` rather than
`550`: the server can tell "the database did not answer" from "no such address",
so a database locked by a backup defers the mail instead of bouncing it.

What to include in a report
---------------------------

If the above does not identify it, open an issue with:

1. The log from the `354` (or from the delivery attempt) onwards, including the
   stage timing lines.
2. The `ERROR_hmailserver_<date>.log` for the same window.
3. Which scanners and event scripts are enabled.
4. Whether the message eventually arrives, arrives twice, or never arrives.

See [SUPPORT.md](../../SUPPORT.md).

Verified against the code
-------------------------

Every log line quoted here was copied from the source rather than from a running
server, and every default was read from the code that reads the ini file. Checked
13 August 2026 against: `SMTPConnection::LogFinalizationStage_` and
`FinalizationDeadlineExceeded_` (the stage lines, the ten-second escalation, the
`451 4.3.1` and error 5525); `SpamTestRunner::RunSpamTest` (the `Spam test:` line
and its own ten-second escalation); `WorkQueue::ExecuteTask` and
`WorkQueue::ReportStalledTasks`, plus `Application`'s `"Asynchronous task queue"`
(the two saturation lines, the second of which is error 5526);
`IniFileSettings::LoadSettings` (every default in the table);
`TimeoutCalculator::Calculate` (the `0` exceptions); `SpamAssassinClient`'s
`SetSessionCeiling(GetSAMaxTimeout() + 30)`; `SMTPConnection::ProtocolRCPT_`'s two
`DatabaseUnavailableMarker::Scope` blocks (`451 4.3.2` rather than `550`);
`DNSResolverWinApi::Query` (the `DNSServer` regression and the cache bypass);
`SPFDnsResolver` and `DNSResolver::GetRecordsOfType` (SPF reaching the configured
resolver at all); and `Logger`'s `ERROR_hmailserver_%s.log`. The DNSSEC deferral
lines were checked on 7 October 2026 against `ExternalDelivery::DeliverToSingleDomain_`
(the application log lines and their bracketed reason) and `DnssecResolver.cpp`
(`QueryValidatedRrset_`, `ValidateRrset`, `ValidateZoneKeys`, `FetchDsRecords`,
`WhereUnproven` and `DescribeCheck`, which word every reason). The lookups a spam
test cannot complete were checked on 12 October 2026 against
`DNSResolver::GetIpAddressesRecursive_`, `GetIpAddressesWithRetry`,
`GetRecordsOfTypeRecursive_` and `GetMXRecordsRecursive_` (what counts as a failed
lookup, and the second attempt), `DNSResolverWinApi::Query` (the cache bypass),
`BLCheck::ClientExistsInDNSBL`, `SURBL::Run`, `SpamTestHeloHost::CheckHostInHelo_`,
`SpamTestPTR::CheckPTR_`, `SpamTestMXRecords::HasAnyMXRecords_`, `DKIM.cpp`'s key
lookup, `DMARC.cpp` and `SPFEvaluator` (the table and every line quoted),
`DNSResolver::GetEmailServersRecursive_` and `ExternalDelivery::HandleNoRecipientServers_`
(the delivery notice), and `AntiSpamDiagnostics` (the fifteen minutes). If you change one of
those, this page is the second place to look.
