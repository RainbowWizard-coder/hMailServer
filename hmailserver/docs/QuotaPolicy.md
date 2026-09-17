# The quota policy

A mailbox's limit (`Account.MaxSize`, `max_size_mb`) was one number that mail either fitted under or did not. The policy puts three points around it, lets a domain choose its own, in its own words, and reports who is nearest theirs.

## The three points

| Point | Server-wide | Per domain | What it does |
| --- | --- | --- | --- |
| The warning | `QuotaWarningPercent` in hMailServer.ini (90; 0 warns nobody) | `Domain.QuotaWarningPercent` / `quota_warning_percent` (-1 = the server's) | The delivery that takes the mailbox across this share of its limit sends the owner one notice, from their domain's postmaster. One: the threshold is something the mailbox crosses, not a state it is in. |
| The grace | `Settings.QuotaGracePercent` / `quota_grace_percent` (0) | `Domain.QuotaGracePercent` / `quota_grace_percent` (-1) | How far past the limit incoming mail is still accepted, as a share of the limit. A mailbox at 1 GB with a 50% grace still receives until it holds 1.5 GB. The owner's own saves and copies over IMAP and the webmail stop at the limit itself: the grace is for mail other people send, which is the mail that is lost otherwise. |
| The send-stop | `Settings.QuotaSendStopPercent` / `quota_send_stop_percent` (0 = never) | `Domain.QuotaSendStopPercent` / `quota_send_stop_percent` (-1) | The share of the limit at which the account can no longer send. An authenticated `MAIL FROM` is refused with `552 5.2.2` and the reason, keyed on the account that signed in, not on the address it named. Sending stops first because it is the failure the owner sees at once, in their own client, with the reason in front of them; mail no longer arriving is noticed by everybody else. |

## The words

The warning is the `QUOTA_WARNING` and `QUOTA_WARNING_SUBJECT` server messages, with `%MACRO_MAILBOX%`, `%MACRO_QUOTA%` (MB), `%MACRO_USED%` (MB) and `%MACRO_PERCENT%` filled in. A domain sets its own subject and text (`Domain.QuotaWarningSubject`, `Domain.QuotaWarningText`; `quota_warning_subject`, `quota_warning_text`), which is how a domain whose people read another language gets the warning in that language. Subject and text are chosen apart: a domain that sets only the text keeps the server's subject.

## The report

`GET /api/v1/reports/quota` is the `mailboxes` section read as a share of each mailbox's limit rather than as a size: every mailbox with a limit and at least one message, nearest first, with `megabytes`, `quota_megabytes`, `percent`, the `warns_at` and `sending_stops_at` points that apply to that mailbox (its domain's, or the server's) and a `state` of `ok`, `warned`, `stopped` or `over`. It takes the same `domain`, `top` and `format=csv` as the other sections and, like `mailboxes`, ignores the window: it is a share now, not a history. The Deck's Reports page shows it as "Near their quota".

## Where it is set

- **Control Deck:** the domain form's "Quota policy" group; the two server-wide values on the Settings page.
- **Control Panel:** the domain dialog's Limits tab; the two server-wide values under SMTP, beside `QuotaWarningPercent`.
- **REST:** the five domain keys on `PUT /api/v1/domains/{name}` and the two server keys on `PUT /api/v1/settings`.
- **COM:** the five `Domain` properties and the two `Settings` properties named above. A backup taken before the policy restores with the server's values, as it had.

## What is tested

`SMTP/QuotaWarning.cs`: the domain's threshold and words, the grace admitting the second message and refusing the third with a domain without grace refusing at the limit, and the send-stop refusing the authenticated sender with the reason while receiving goes on, with the server's value applying where the domain has none. `API/RestApiReports.cs`: the quota section's row for a limited mailbox, with the unlimited one absent. All but the report run on the Linux bench too.
