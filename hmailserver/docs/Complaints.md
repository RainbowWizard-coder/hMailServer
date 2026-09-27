Complaints, read: the feedback loops
====================================

The large mailbox providers run a **feedback loop**. When one of their readers
presses the button that says *this is spam*, they post an abuse report - RFC
5965, a `multipart/report` whose `report-type` is `feedback-report` - to an
address the sending operator registered with them. The report says which of
their readers complained, about which message, when it arrived, from which IP
and about which domain.

Until this existed nothing here read one. They arrived in a mailbox and sat
there, and the first an operator knew about a complaint rate was a provider
throttling the mail.

With complaint processing on, and a mailbox named:

* a report delivered to the complaint mailbox is **parsed** and **recorded**
  against the sending domain and, where the sender is an account, against that
  account: who complained, about what, when, and which provider reported it;
* the complained-of address is **counted** beside that domain's bounce counts,
  in a column of its own;
* past a threshold the address goes on that domain's **suppression list** - the
  same list the bounces feed - with its own reason, so this server stops mailing
  somebody who has said they do not want it;
* a **complaint rate per domain** is exposed in the metrics, readable over REST
  and over COM, and raises an alert when it crosses the rule's threshold.

**It is off by default, and inert without a mailbox.** With
`ComplaintProcessingEnabled` off, or with `ComplaintMailbox` empty, every point
where the feature could act reads one in-memory setting and returns: nothing is
parsed, counted, stored, looked up or refused, and an upgraded server behaves
exactly as it did.

Which mailbox, and why it is a configured one
---------------------------------------------

**Only a message delivered to `ComplaintMailbox` is read as a complaint.** A
message carrying a `message/feedback-report` part to any other address on this
server is ordinary mail and is left alone.

That is a decision, and the reason is worth reading before changing it.

A delivery report has evidence of its own: it carries a null envelope sender,
this server can require that it arrived over an unauthenticated SMTP session it
recorded itself, and - the part that matters - it must reproduce a key built
from the Message-ID, the recipient and the envelope sender of a message this
server really sent, which the server wrote into a register when a remote
accepted it. See [BounceSuppression.md](BounceSuppression.md).

A feedback report has none of that. Its envelope sender is the provider's, not
null. Nothing in it is known only to the provider: the Message-ID and the
recipient of the message complained about are known to every other recipient of
that message and to the reader themselves. The only thing that distinguishes a
report from the provider from a report somebody wrote is that **it arrived where
the provider was told to send it**.

If any mailbox were an intake, a stranger who can mail one user of this server
could stop it sending to any address of that user's domain, with one message.
That is why the address is the administrator's to name, and why it is empty
until they do.

**What that does not stop.** Anybody who learns the complaint address can post a
report to it, and `abuse@` is a published address by RFC 2142. So:

* use a dedicated address the feedback loops know and nobody else does -
  `fbl@example.com` rather than `abuse@example.com` - and do not publish it;
* leave `ComplaintSuppressionThreshold` at its shipped 0, which records and
  counts complaints and lists nobody, and read the entries before raising it to
  1 and letting the list act;
* every entry a complaint made says so (`source` is `complaint`) and can be
  removed by hand, and the complaints themselves are visible per domain.

Three further gates apply, and none of them is a setting:

* the report must be addressed to the complaint mailbox **and to nothing else**
  of this server's, because a provider posts a report to the one address it
  registered;
* the domain the report is about - read from `Original-Mail-From`, then the
  returned message's `From`, then `Reported-Domain` - must be one **this server
  hosts**. A report about somebody else's domain is delivered and recorded
  nowhere, so no amount of mail from outside can create a row for a domain that
  is not the operator's own. Each step is taken when the one before it does not
  *resolve*, not merely when it is absent, which matters on any installation
  running BATV or SRS: the `Original-Mail-From` a provider saw is then a `prvs=`
  or `SRS0=` address, which belongs to a return path rather than to a sender, and
  stopping there would drop every report about that mail. Whichever step resolves
  is what the row records as the sender;
* an `Original-Rcpt-To` that **this server hosts** lists nobody. It is not an
  address the domain sent to outside, and listing it would stop this
  installation mailing itself.

Turning it on
-------------

The four values are properties of the COM `Settings` object, keys of
`/api/v1/settings/complaints`, a group on the Control Deck's Settings page,
`hmctl settings get complaints`, and the Control Panel's Mail flow & delivery >
Delivery of e-mail page, the Feedback tab. They live in `hm_settings`, not in
`hMailServer.ini`, and each is read at the moment it matters, so a change applies
to the next report without a restart.

| Setting | `hm_settings` name | REST key | Default | What it means |
|---|---|---|---|---|
| Read the abuse reports | `ComplaintProcessingEnabled` | `processing_enabled` | off | The whole feature. With it off nothing below has any effect. |
| The complaint mailbox | `ComplaintMailbox` | `mailbox` | empty | The address the feedback loops are registered against, or several separated by commas. Each must be an address this server hosts. Empty means no report is ever read. |
| Complaints before an address is listed | `ComplaintSuppressionThreshold` | `suppression_threshold` | `0` | 0 to 100. **0, the default, records and counts every report and lists nobody** - observe-only, which is where an operator should start, for the reason two paragraphs up: the only thing that tells a genuine report from a forged one is that it arrived at the mailbox. Raise it to 1 once the entries have been read; a reader who pressed the spam button *received* the message and did not want it. |
| Days a complaint is kept | `ComplaintRetentionDays` | `retention_days` | `180` | 0 keeps them for ever; at most 3650. The hourly sweep removes complaints older than this and the daily tally rows behind the rate. A suppression list entry a complaint made is **not** removed by this, and not by `BounceSuppressionExpiryDays` either: that applies to entries a bounce or a delivery report made. A complaint's entry stays until an administrator removes it, as an entry an administrator added by hand does, because a reader who said they did not want this mail has not changed their mind by the calendar. |

A value out of range is refused, over COM and over REST, with a sentence naming
the range - it is never silently clamped. A mailbox that is not an address this
server hosts is refused with the address it could not accept.

**Registering the loop.** Each provider has its own form, and most of them want
the sending IP addresses and a mailbox. Once one is registered, name that mailbox
here. Reading a provider's reports needs nothing else from this server.

What a report has to be
-----------------------

A `multipart/report` whose `Content-Type` carries `report-type=feedback-report`,
holding a `message/feedback-report` part with at least a `Feedback-Type` field.
Everything else is optional, because providers differ in what they redact. Read
from the report part: `Feedback-Type`, `User-Agent`, `Original-Mail-From`,
`Original-Rcpt-To`, `Arrival-Date`, `Source-IP` and `Reported-Domain`. Read from
the message the report wraps - its header block alone, which is what most
providers send: `Message-ID`, `From`, `To` and `Subject`.

The parser is handed hostile input by construction, so: a size cap of 2 MB, one
level of MIME and no deeper, an address field that is not one address is empty
rather than a guess, a `Source-IP` that is not an address and a
`Reported-Domain` that is not a domain name are dropped, and nothing throws. A
report it cannot read is simply not a report.

**A message to the complaint mailbox that is not a readable report is delivered
to the mailbox unchanged, and a line in the application log says so.** That is
deliberate: a provider that changes its format must not silently stop being
read, and the administrator who reads that mailbox is the one who can say what
arrived.

Which types count
-----------------

`not-spam` is the reader saying the opposite - a message was wrongly filtered -
so it is recorded, and counts towards neither the rate nor the suppression list.
Every other type in the IANA registry (`abuse`, `fraud`, `virus`, `other`), and
any type a provider invents tomorrow, is somebody saying they did not want this
mail, and counts.

A complaint is not a bounce
---------------------------

A bounce says the mailbox may not be there. A complaint proves that it is, and
that the person behind it read the message and did not want it. The code keeps
them apart:

* a complaint touches **no bounce counter** and never the run of hard bounces;
* it has a counter of its own, `hm_bouncecounts.bccomplaints`, shown as
  `complaints` beside the bounce counts;
* its suppression list entry carries `source` `complaint` and no category, and
  the refusal a sender meets says *its provider reported mail to it as spam* with
  `5.7.1` - refused on policy - and never `5.1.1`, which would claim the mailbox
  does not exist.

Everything that enforces the suppression list applies to a complaint's entry
unchanged - the refusal at `RCPT TO`, the removal at delivery, the page, removal
by hand, the expiry sweep - because **it is the same list**. There is no second
list to look at.

**The list is applied whenever either feature is on.** Reading bounces and
reading complaints are separate switches, and each writes to the list; an
operator who switches on the feedback loops alone gets a list that is written
*and enforced*. Switching on the complaints does not start counting bounces:
everything that reads a bounce still asks `BounceProcessingEnabled` alone.

The same report twice is one complaint. A key built from the Message-ID, the
reader, the sender and the arrival time is stored with each one, with a unique
index behind it, so a provider that resends does not push an address over the
threshold on its own. A report that identifies no message at all - no Message-ID
*and* no arrival date, which a heavily redacting provider sends - cannot be
recognised as a repeat, and is counted: losing a real complaint is worse than
counting a resent one.

The complaint rate
------------------

A complaint rate is the number the providers themselves throttle and block on,
and an operator who learns it from the providers learns it too late.

    rate = complaints in the window / messages delivered in the window

reported in **parts per million**. 1000 - one complaint in a thousand messages -
is the figure the large providers describe as too high, and is what the alert
rule is seeded with.

* **The window is seven days** and is not a setting. A rate measured over a
  window the operator can change is not the number the provider is looking at.
* **The denominator** is the recipients a remote server accepted, counted per
  sending domain, once per delivery pass - so a message to a hundred addresses is
  one round trip and not a hundred. It is kept in `hm_complaintvolume`, one row
  per domain per day. What that costs per outbound message from a hosted sender,
  stated plainly: a select of the day's row by its unique index and then an
  update of it, both against the same single row, so deliveries for one busy
  domain serialise briefly on that row on a backend that locks coarsely. Nothing
  is written at all while the feedback loops are off or no mailbox is named.
* **A domain that has sent fewer than 100 messages in the window is not judged.**
  The alert does not fire on it, the metric reports its rate as `0`, and the
  listing says `rate_judged: false`. Three complaints out of ten messages is not
  a 300,000-per-million problem; it is a domain that has barely sent anything.

The figures are read from the database at most once a minute and shared by the
metrics endpoint and the alert pass, so a scrape every fifteen seconds is not a
query every fifteen seconds. **A recorded complaint throws that snapshot away at
once**, so the page an administrator opens after a complaint arrives has the
complaint in it; a delivery does not, because being a minute behind on the
denominator changes nothing anybody reads.

### The metrics

Present when `MetricsPerDomainEnabled` is set and complaint processing is on.
All three are gauges, not counters: a seven-day window goes down as well as up,
and a counter that goes down reads as a process restart to every Prometheus
`rate()` in the world.

| Series | What it is |
|---|---|
| `hmailserver_domain_complaints{domain}` | Abuse reports received about the domain in the last seven days. |
| `hmailserver_domain_complaint_window_messages{domain}` | Messages the domain had accepted by a remote in the same window: the denominator. |
| `hmailserver_domain_complaint_rate{domain}` | The ratio. Alert on `hmailserver_domain_complaint_rate > 0.001`. |

A read that failed leaves all three series out rather than reporting a rate of
zero, which is a number an operator would act on.

### The alert

`complaints.rate.high`, seeded by schema 6050, **on**, threshold `1000` parts per
million, cooldown a day, held for the digest. The alert pass raises it at High
severity naming the worst domain, its counts and its rate, and clears it when no
judged domain is over the threshold. With complaint processing off it clears the
condition - leaving it raised would say a rate is high that nobody is measuring -
and reads no complaint data at all.

That off path is not free, and it is the same cost every other seeded rule has:
the pass reads the rule, and clearing reads the rule and the newest event for the
condition, so three indexed statements a minute on an installation that never
switched the feedback loops on. What it never does with the feature off is touch
`hm_complaints` or `hm_complaintvolume`. An operator who does not want the rule at
all disables it on the alerts page.

Reading them
------------

**The Control Deck.** A domain's row carries a *Complaints* button beside its
*Suppressions* one. The view shows the rate above the table, because a page that
lists the rows without the denominator shows half the answer, and says plainly
when processing is off or no mailbox is named.

The per-domain document says `mailbox_configured: true|false` and **not the
mailbox address**, and carries no `suppression_threshold`. Both are server-wide
settings that are not one domain's business, and the address in particular is the
one thing a forger needs: a key confined to a single domain, and that domain's
administrator over COM, can read this document. The address is in
`GET /api/v1/settings/complaints`, which is server-wide.

**REST.**

| Route | Who | What |
|---|---|---|
| `GET /api/v1/domains/{domain}/complaints` | `view_configuration` **and** `view_message_trace`, scoped to the domain | The reports, newest first, with the domain's own rate. `search` matches the complained-of address, the sender, the subject or the provider; `account_id` narrows to one account's mail. |
| `GET /api/v1/complaints/rates` | `view_configuration`, server-wide | Every hosted domain's rate, worst first. |
| `POST /api/v1/complaints/expire` | `manage_server`, server-wide | The hourly sweep, now. |
| `GET` / `PUT /api/v1/settings/complaints` | `view_configuration` / `manage_settings` | The four settings. |

A complaint names a reader outside this server that the domain mailed and what
they said about it - who mails whom - so the listing needs `view_message_trace`
as well as `view_configuration`, exactly as a domain's suppression list does.
The rates carry no address at all, so they need `view_configuration` alone.

**COM.** `Utilities.GetComplaints(DomainName, Search, MaxRows)` answers the same
document as the domain route, to the server administrator or that domain's
administrator; `Utilities.GetComplaintRates()` answers the rates, to the server
administrator alone, since one domain's rate is in that domain's own document.

**The command line.**

    hmctl complaint list --domain example.com
    hmctl complaint list --domain example.com --search anna@example.com
    hmctl complaints rates
    hmctl complaints expire
    hmctl settings get complaints
    hmctl settings set complaints --set processing_enabled=true --set-string mailbox=fbl@example.com

    Get-HmComplaint -Domain example.com | Format-Table address, feedback_type, sender, arrival
    Get-HmComplaintRate | Format-Table domain, sent, complaints, rate_ppm, judged
    Invoke-HmComplaintMaintenance
    Get-HmSetting -Group complaints

What is stored, and what a sweep removes
-----------------------------------------

| Table | One row is | Removed by |
|---|---|---|
| `hm_complaints` | One abuse report this server believed: the domain, the account, the complained-of address, the sender, the feedback type, the provider, the source IP, the reported domain, the Message-ID and Subject of the message complained about, when it arrived and when the report was read. | The hourly sweep, past `ComplaintRetentionDays`; an address erasure; the domain being deleted. |
| `hm_complaintvolume` | One sending domain's messages delivered and complaints received on one day. | The hourly sweep, past `ComplaintRetentionDays` **or the seven-day rate window, whichever is longer**; the domain being deleted. A retention of three days would otherwise delete four of the seven days every rate is worked out from, and the rate would read low with nothing to say so. The tally holds no address - a domain, a day and two numbers - so keeping it the extra days costs nobody their privacy. |
| `hm_bouncecounts.bccomplaints` | The complaints counted for one address of one domain, with the date of the last of them in `bclastcomplaint`. | The bounce sweep's `BounceCountRetentionDays` (90 by default), measured from the **last** complaint - not `ComplaintRetentionDays`. That is what `bclastcomplaint` is for: without it a row that holds nothing but complaints has no date at all and is swept on the next hourly pass, and an address complained about once a quarter would never reach a threshold above 1. |

Schema 6050 adds them; nothing is written to any of them until processing is on
*and* a mailbox is named.

The sweep runs hourly and once at start, **whether or not complaint processing is
on**, so switching the feature off does not leave a record of who complained
about what standing for ever. On an installation that never switched it on the
pass is two indexed deletes that count nothing and delete nothing.

The audit trail, and erasure
----------------------------

`hm_complaints` and `hm_complaintvolume` are **not** audited, and that is written
down in `build/check-audit-choke-point.py` rather than left silent. Nobody here
decides a complaint: it is written by an arriving report, with no actor at all,
and the tally by delivery. What an administrator *does* decide about the feedback
loops is a setting or a suppression list entry, and both of those are audited
already.

`Utilities.EraseAddressTraces` removes every complaint naming an address, as the
reader who complained and as the sender complained about, beside the bounce
counts and suppression entries it already removed. An erasure is somebody asking
to be forgotten, and that includes being forgotten as somebody who complained.

A backup does **not** carry the complaints, and
`Server/Common/Application/AccountStores.cpp` says why: a complaint is not the
account's property but a record of what somebody outside said, it is pruned on a
clock of its own, and its effect - the count and the list entry - belongs to the
domain and travels with it. Restoring an account would otherwise bring back
complaints that had already aged out, into a seven-day window a restore cannot
honestly reconstruct.

Related
-------

* [BounceSuppression.md](BounceSuppression.md) - the suppression list a complaint
  writes to, and how a bounce gets on it.
* [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) - the alert rules and what
  each raises.
* [Roles.md](Roles.md) - which permission reads and changes what.
