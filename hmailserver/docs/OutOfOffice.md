# The out-of-office policy

An absence notice is a message, and it is also a policy: who is answered, with
which text, how often, and who is never answered. This page is the policy as
the server applies it, for an account's own reply and for a domain's.

## Whose reply answers

At most one reply answers any message. The account's own reply, when it is on
and inside its dates, comes first; a domain's reply answers only for accounts
that have none of their own. The `SMTP/OutOfOfficeScope` fixture pins that
order.

## Colleagues and strangers

A sender is *internal* when the envelope sender resolves to an account hosted
here or to an active alias; everybody else is *external*. That is an honest but
weak claim - the envelope sender is whatever the submitting client said - so an
internal text is a courtesy gradient, not access control, and must never carry
anything that cannot be shown to a stranger.

Both the account and the domain can say different things to the two:

| | Account | Domain |
|---|---|---|
| general text | `VacationSubject`, `VacationMessage` | `VacationSubject`, `VacationMessage` |
| text for colleagues | `VacationInternalSubject`, `VacationInternalMessage`; empty means the general text | `VacationInternalSubject`, `VacationInternalMessage`; empty means the general text |
| colleagues only | `VacationMessageInternalOnly`: strangers get nothing | leave the general text empty |
| strangers get the domain's text instead | - | `VacationExternalOverride`: on, and a stranger writing to an account with a reply of its own gets the domain's general text, while colleagues still get the account's |

With the domain's override on, an internal-only account reply is still silent
to strangers on its own behalf, but the domain's text answers them in its
place. An internal text is never sent outside.

## How often

Each sender is answered once per absence: switching the reply off and on again
is a new absence and forgets everybody. A window - `VacationMessageDays` on the
account, `VacationDays` on the domain, 0 to 3650 - answers the same sender again
once that many days have passed while the reply stays on, the way Sieve's
`vacation :days` does. 0 is once per absence, which is what every account had
before.

The memory of who was answered when is a table, `hm_autoreplies`, one row per
account and sender with a unix-time stamp, kept until the reply is switched off
or the account is deleted. It used to be held in process memory, which meant a
restart repeated every reply and "N days" could only mean "since the service
started". Two messages from the same sender delivered at the same moment can
both find no row; the loser's duplicate is ignored, and the sender was answered
once more than intended rather than the log filled with an error about it.

## What is never answered

RFC 3834, applied before any reply is composed and before the once-per-sender
slot is spent: no reply to a null return path, to `Auto-Submitted` other than
`no`, to `Precedence: bulk`, `list` or `junk`, to anything with a `List-*`
header, to a sender that asked not to be answered with
`X-Auto-Response-Suppress`, or to `postmaster` and `mailer-daemon`; none to
ourselves; none to a message the recipient's own spam settings marked when the
account says so (`VacationMessageAbortSpamFlagged`), and none at all from the
domain reply to marked mail. A remote domain policy that refuses automatic
replies refuses both kinds. Every reply goes out with a null return path and
`Auto-Submitted: auto-replied`, so that a bounce of it cannot start a loop.

## Where it is set

- **Control Panel**: the account dialog's Auto-reply tab and the domain dialog's
  Out of office tab hold the texts, the window and the switches.
- **Control Deck**: the account editor's Auto-reply group and the domain
  editor's Out of office group, the same fields.
- **REST**: `PUT /api/v1/accounts/{address}` takes `vacation_days`,
  `vacation_internal_subject`, `vacation_internal_message` and
  `vacation_internal_only` beside the existing `vacation_*` fields;
  `PUT /api/v1/domains/{domain}` takes `vacation_days` beside
  `vacation_internal_subject`, `vacation_internal_message` and
  `vacation_external_override`.
- **COM**: the properties named above on `Account` and `Domain`.
- **The webmail** sets the reader's own reply (`PUT /api/v1/me/vacation`); the
  policy fields are the administrator's.

The code: `SMTP/SMTPVacationMessageCreator.cpp` composes every reply and keeps
the memory; `SMTP/LocalDelivery.cpp` chooses whose text is sent
(`SendAutoReplyMessage_` for the account's, `SendDomainAutoReplyMessage_` for the
domain's). Schema 6060 added the columns and the table.
