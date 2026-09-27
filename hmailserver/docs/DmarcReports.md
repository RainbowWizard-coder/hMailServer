The reports our own domains receive, read
==========================================

Every receiver that evaluates DMARC sends the domain owner an **aggregate
report** once a day: who sent as the domain, from which addresses, how many
messages, and whether DKIM and SPF aligned. Every sender that honours TLS-RPT
sends a **TLS report** saying whether its sessions to the domain's MX hosts
succeeded and, when they did not, why. This server has sent both kinds to other
people's domains since 2026 (`DmarcRptReporterTask`, `TlsRptReporterTask`). It
did not read the ones that came back for its own: they landed in whatever
mailbox the `rua=` tag named and sat there as gzip attachments nobody opened.
The paid services - dmarcian, Postmark's tool, Valimail - exist to open them,
and what they show is one chart: who sends as your domain, how much, and
whether it passed. This is that chart.

Naming the mailbox
------------------

Each domain has one **report mailbox**: `Domain.ReportMailbox` over COM,
`report_mailbox` on `PUT /api/v1/domains/{domain}`, the *Reports received* tab
of the Control Panel's domain dialog, and the *Reports the domain receives*
group of the Control Deck's domain editor. It is empty by default, and empty
means the domain takes no reports in: nothing is read anywhere until an
administrator names one.

The address has to be one in a domain this server hosts - this domain or
another - because a receiver delivers a report to the address the `rua=` tag
names, and an address somewhere else would read no report and never say so. It
is refused otherwise, over REST and over COM in the same sentence. Whether an
account or an alias answers at the address is the administrator's to arrange;
the server does not make one, because the domain may be new and its mailboxes
not yet made.

A mailbox in *another* hosted domain makes this server open every attachment
delivered to that address alone, so a credential confined to a list of domains
(an API key issued with a domain list, a COM domain administrator) may name one
only in a domain it administers; naming a mailbox in some other tenant's domain
is refused (403 over REST), and the cross-domain form is otherwise the server
administrator's.

The value is judged when it is written, not on every save: a `PUT` that does
not carry `report_mailbox`, and the Control Panel's Save, keep whatever is
stored. What is stored is kept true by the domain itself - when the domain that
hosts the mailbox is **renamed**, every domain's mailbox at the old name follows
it to the new one, its own and any sister's, and the log says so; when that
domain is **deleted**, every other domain's mailbox in it is cleared, the log
names each domain that lost its mailbox, and the domain's DMARC page says no
mailbox is named until another is. A mailbox that nevertheless names a domain
this server does not host - a restore, a row edited by hand - is logged once
and reads nothing.

Then publish it:

    _dmarc.example.com.     TXT  "v=DMARC1; p=quarantine; rua=mailto:dmarc@example.com"
    _smtp._tls.example.com. TXT  "v=TLSRPTv1; rua=mailto:dmarc@example.com"

An organisation with several domains names the same mailbox on each of them. A
report is recorded against the domain it says it is about (`policy_published/
domain`, or the TLS-RPT `policy-domain`), whichever domain's mailbox it arrived
at, provided that domain is hosted here and names a mailbox of its own. A
report about a domain this server does not host, or about one that names no
mailbox, is not recorded, so naming a mailbox on one domain never starts a
chart for another whose administrator asked for none.

What is read
------------

A message is opened only when it is addressed to a report mailbox **and to
nothing else** of this server's - the rule the complaint mailbox is held to. A
report to that address and to somebody else is somebody making this server read
a report on the way past. Every message is delivered to the mailbox unchanged,
read or not, because a report this server cannot read is still mail somebody
sent, and the administrator reading that mailbox can say what it was.

From the message, every MIME part that could be a report is taken: a part whose
type is one of `application/gzip`, `application/zip`, `application/tlsrpt+gzip`,
`application/tlsrpt+json`, `text/xml`, `application/xml` or `application/json`,
or - for `application/octet-stream` and anything else - whose name ends in
`.xml`, `.gz`, `.zip` or `.json`. A message whose whole body is the archive, as
one large reporter sends it, is read the same way. Each candidate is unwrapped
by its bytes rather than its name: a gzip is inflated, a zip's first file is
taken through its central directory (stored or deflate; a streamed local header
with no sizes is fine), anything else is taken as it is. Then the first
character decides: `<` is read as a DMARC aggregate report, `{` as a TLS-RPT
report.

**A DMARC aggregate report** (RFC 7489 Appendix C, and the RFC 9990 form, which
puts every element in a namespace and adds a few optional ones) becomes one row
in `hm_dmarcreports` - the reporter, its contact, the report id, the date range,
the policy the reporter saw published (`p`, `sp`, `adkim`, `aspf`, `pct`), the
messages added up and how many passed - and one row in `hm_dmarcreportrecords`
per `<record>`: the source address, the count, what the receiver did
(`disposition`), the two aligned verdicts, the identities, and the raw DKIM and
SPF results as `d=domain s=selector result=pass` pairs. A record with no source
address is dropped, since nothing in it can be charted, and a report with no
record at all is refused.

**A TLS-RPT report** (RFC 8460) becomes one row in `hm_tlsreports` per policy
it covers: the reporter, the report id, the date range, the policy type (`sts`,
`tlsa` or `no-policy-found`), the policy domain, the sessions that succeeded
and failed, and the failure details as a short JSON list (`result_type`,
`sending_mta_ip`, `receiving_mx_hostname`, `receiving_ip`, `count`) that stops
before it would not fit the column's 2,000 characters.

**The same report twice is one row.** The key is the reporter's name, the report
id and the policy domain (RFC 7489 7.2.1.1), so a retry, or two nodes reading one
mailbox, counts once; the second is logged at debug level and dropped.

**What carried it is kept too.** Nothing inside a report proves who sent it:
`org_name` is text a stranger can write. What this server's own inbound path
made of the *message* is evidence, so the `dmarc=` verdict and the
`header.from=` domain of the `Authentication-Results` field this server wrote
on the message are kept on the report row (`message_dmarc`, `message_from`),
and the summary counts per reporter how many of its reports arrived in a
message that was evaluated (`checked`) and how many of those passed
(`authenticated`). The Deck shows the count beside the reporter and the verdict
under each report. Both are empty unless `AuthenticationResultsEnabled` is on
in `hMailServer.ini`, since that is what writes the field; with it off the
reporter is shown as *Not checked*, which is the truth.

Nothing read here **acts**: no list is fed, no mail is refused, no setting
changes. The rows are read by an administrator. That is also why a forged
report is harmless beyond its rows: the mailbox address is published in DNS,
so it is not a secret, and a report from a stranger can only add rows to a
chart about the stranger's own claims - which is why those rows are bounded
(below) and marked with what carried them (above), because they are still the
chart an administrator reads before tightening `p=`.

Safety
------

Every byte here is hostile by construction, so:

* the message is not read past 32 MB, at most ten of its parts are looked at,
  and an attachment is not inflated past 16 MB (a gzip of zeros is a thousand
  to one);
* a zip's central directory is what is trusted for sizes, a local header never;
* the XML reader is the server's own and knows the five predefined entities and
  numeric references and nothing else - a reference it does not know stays as
  it was written - and a document type declaration that declares any entity is
  refused outright, since an entity is the one thing in XML that makes a small
  document large or reads a file;
* elements nest at most 32 deep, a document holds at most 400,000 of them, and a
  report at most 2,000 records - past that the rest are counted, not kept, and
  the application log says the chart under-counts that report; a day's report
  from the largest receivers is far under that for any domain this server is
  likely to host;
* a policy domain that is not a host name - an address, a space, anything but
  letters, digits, dots and hyphens - is refused before it is looked up;
* one message may add at most 2,000 records however many reports it carries,
  and a domain takes in at most 20,000 records and 1,000 TLS-RPT rows in any
  twenty-four hours - a report past either ceiling is refused whole (never
  half-recorded) and the log says so, once an hour per domain. A real day of
  reports is a few hundred records; the ceiling is for whoever found the
  published address and is posting to it on purpose, and it bounds what they
  can add to the disk and to the chart to a few megabytes a day;
* a TLS-RPT report holds at most 100 policies and 200 failure details each.

Each record is still one `INSERT`: the default database (SQL Server Compact)
has neither multi-row inserts nor transactions, so the caps above are the
bound on the delivery thread's work rather than a batch.

The class tester (`DomainReportsTester`, run by `hMailServer.exe /Test`) hands
every reader well-formed input, the RFC 9990 form, CDATA and references, a
report one record past the cap, the header-field reader every shape of
`Authentication-Results`, and then the refusals: an entity declaration, a wrong
root, no records, a policy domain that is an address, a truncated gzip, bytes
that only begin like a zip.

What the chart shows
--------------------

`GET /api/v1/domains/{domain}/dmarc-reports/summary?days=30` is the chart's
data, and the Deck's DMARC page (from the domain row, beside *Complaints*),
`hmctl dmarc summary` and `Get-HmDmarcSummary` draw it:

* `messages`, `passed`, `failed` - a message **passed DMARC** when the receiver
  found DKIM aligned or SPF aligned (`policy_evaluated/dkim` or `/spf` is
  `pass`); `dkim_passed` and `spf_passed` count each on its own;
* `quarantined` and `rejected` - what the receivers did, by `disposition`;
* `reporters` - who sent the reports, with how many and covering how many
  messages, and `checked` and `authenticated` - how many of them arrived in a
  message this server evaluated for DMARC, and how many of those passed;
* `by_day` - one entry per day of the window when it is 400 days or fewer, so
  a day nothing arrived on is a gap in the chart rather than a missing bar; a
  report's records are charted on the day the report begins, which for the
  daily reports the large receivers send is the day;
* `sources` - one entry per address that sent as the domain, most messages
  first, at most 200, each with its counts, the `header_from` it used and how
  many reporters saw it; `sources_total` says how many there were.

A report counts when any of its range is inside the window. `mailbox_configured`
and `report_mailbox` say whether anything is being read at all, and the page
says so in words when nothing is, because an empty chart and a chart nobody is
filling look the same otherwise.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/domains/{domain}/dmarc-reports/summary?days=` | The chart's data, above. `days` 1 to 3650, default 30. |
| `GET /api/v1/domains/{domain}/dmarc-reports?limit=&offset=` | The aggregate reports, newest first: `id`, `org`, `email`, `report_id`, `begin`, `end`, `policy_domain`, `adkim`, `aspf`, `p`, `sp`, `pct`, `messages`, `passed`, `records` (kept), `received`, `source`, `message_dmarc`, `message_from`. |
| `GET /api/v1/domains/{domain}/dmarc-reports/{id}` | One report with its `records`. 404 when the id is not one of this domain's, or the domain segment is not a host name. |
| `GET /api/v1/domains/{domain}/tls-reports?days=&limit=&offset=` | The TLS-RPT rows over the window (`days` 0 is every one kept), with `sessions_succeeded` and `sessions_failed` for the whole window. |
| `POST /api/v1/domain-reports/expire` | The retention sweep now; body `older_than_days` (0 to 3650) instead of the setting, 0 removing every report. Answers the counts and `complete`, false when a hundred passes of 5,000 still left rows - another call removes them. |
| `PUT /api/v1/domains/{domain}` | `report_mailbox` beside the domain's other fields. |
| `PUT /api/v1/settings` | `domain_report_retention_days`. |

The four under a domain are that domain's: a key confined to one domain reads
its own and gets 403 on another's; a read-only key reads them and cannot run
the sweep. A row names an address that sent as the domain and what a receiver
made of it - configuration's evidence, never a person - so `view_configuration`
alone reads them, as it reads the complaints. The sweep prunes every domain's
rows and takes `manage_server`.

`hmctl dmarc summary|reports|report|tls|expire` and `Get-HmDmarcSummary`,
`Get-HmDmarcReport`, `Get-HmTlsReport` and `Invoke-HmDomainReportMaintenance`
are the same documents from the command line ([CommandLine.md](CommandLine.md)).

Retention
---------

`DomainReportRetentionDays` (365 by default, which is what the paid services
keep; 0 keeps for ever; at most 3650) is how long a report is kept, by the day
it was read; it is on the Control Panel's Delivery of e-mail page, the Feedback
tab, which reaches it over the REST API. The six-hourly retention task - `Utilities.RunMessageRetention` on
demand - removes older ones, a report's records with it by the cascade, in
passes of 5,000 of each kind until a pass finds nothing or a hundred have run;
the route, `hmctl dmarc expire` and the cmdlet say when rows remain, and the
task's next pass takes them. Deleting a domain removes its reports the same way.

The rows are not in the backup: a backup carries the mailboxes, and the reports
are still in the mailbox they arrived in, which is carried. The mailbox address
itself is a column of `hm_domains` and travels with the domain.

What it does not do
-------------------

* It does not look the sources up: no reverse DNS, no "this address is
  Google's", no geography. A source is the address the reporter named.
* It does not read forensic (`ruf=`) reports, which almost no receiver sends
  and which carry message content.
* It does not alert when the pass rate falls; the numbers are in the summary
  for whatever reads it.
* It does not stop anything. A source that fails DMARC is a row, not a block;
  the domain's `p=` is what the receivers act on, and the chart is how an
  administrator sees whether it is safe to move from `none` to `quarantine` to
  `reject`.
* A report with more than 2,000 records is cut short and says so in the log;
  the chart under-counts that report rather than refusing it. A report that
  would take a domain past 20,000 records in a day is refused whole instead,
  and the log says so.
* It does not prove who sent a report. `message_dmarc` says what this server
  made of the carrying message when `AuthenticationResultsEnabled` is on, and a
  stranger whose own domain passes DMARC still passes; the reader compares
  `message_from` with the reporter, as they would with any mail.
