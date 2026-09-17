# Mail flow rules

The rules the server runs on every message, before delivery - Exchange's transport rules, Google's routing - and where they live.

## Three collections

| Collection | Where | Runs for |
| --- | --- | --- |
| The server's (`Application.Rules`, `/api/v1/rules`, the Control Panel's Rules page, the Deck's) | `hm_rules` with no account and no domain | every message, first |
| A domain's (`Domain.Rules`, `/api/v1/domains/{name}/rules`, the same pages with the domain chosen at the top) | `hm_rules` with `ruledomainid` (schema 6063) | every message with a recipient in the domain or a sender it hosts, after the server's - which is how a domain administrator has rules of their own: a domain-restricted key reaches its domain's rules and no other's |
| An account's (`Account.Rules`, `/api/v1/accounts/{address}/rules`) | `hm_rules` with the account | that account's mail, at local delivery |

All three run in `SMTPDeliverer` on every delivery attempt, after virus scanning and before DKIM signing and the split between local and external delivery, with the same conditions and actions.

## Conditions

From, To, CC, Subject, body, size, the recipient list, delivery attempts, any named header, and - since 17 September 2026 - **the attachment name** (`attachment_name`): every attachment's file name, `;` between them, so `contains .exe`, a wildcard `*.zip` or a regular expression reads as "any attachment is". Matched by equals, contains, their negations, less or greater, wildcard or regular expression, all or any.

Four more, in the same list shape, for the outbound content policy ([OutboundContentPolicy.md](OutboundContentPolicy.md)): **sensitive content** (`sensitive_content`: the detectors that found something in the readable text - `card_number`, `iban`, `account_number`), **attachment type** (`attachment_type`: every attachment's type by its first bytes, not its name), **external recipients** (`external_recipient`: every recipient this server does not host) and **readable text** (`content`: the plain and HTML parts and every attachment whose bytes are text, for a pattern of the rule's own).

## Actions

Delete, forward, reply, move to a folder, set a header, send using a route, create a copy, bind to an address, run a script function, stop - and four more:

* **Add a disclaimer** (`add_disclaimer`, `body`): the text appended to the message's plain and HTML parts when the rule matches, with `X-hMailServer-Disclaimer` naming the rule; the domain disclaimer stays the unconditional footer it was.
* **Reject with a text** (`reject`, `value`): the SMTP session answers `550 5.7.1` with the text **before the message is accepted**, where the sender is still there to be told - the server's and the recipients' and the sender's domains' rules are asked at the end of DATA, with the message parsed for the match only when one of them has a reject action at all, and the webmail's send route asks the same rules before it queues and answers `403`, code `policy_rejected`, with the text. A message that reached delivery another way is dropped, as a delete is, with the rule named in the log, and a sender this server hosts is mailed a notice saying it was not sent ([OutboundContentPolicy.md](OutboundContentPolicy.md)). The text is one line: a control character in it, or in the rule's name, is refused over REST.
* **Require TLS** (`require_tls`): every external delivery of the message needs STARTTLS, as a remote domain policy would require it; a destination that offers none is deferred with a reason naming the rule, and the message is not sent in the clear.
* **Quarantine** (`quarantine`): the message is held in the quarantine under `Rule: <name>` rather than delivered, for an administrator to release or delete; a quarantine that could not store it delivers the message rather than lose it, and says so in the error log.
* **Warn the sender** (`warn_sender`, `value`): the message goes where the rule's other actions send it, and a sender this server hosts is mailed a notice naming the rule, with the action's text, from the postmaster of their domain, marked auto-generated; an outside sender gets nothing, since that would be backscatter ([OutboundContentPolicy.md](OutboundContentPolicy.md)).

A reject with no text of its own names the rule in its 550, since 17 September 2026.

## What is tested

`Rules/MailFlowRules.cs` (both benches): a domain's rule setting a header on its own mail and not another domain's; the attachment condition deleting the executable and keeping the text file; the disclaimer appended with the rule named; the refusal at the SMTP session with the rule's words and the unmatched message delivered; the quarantine holding the message under the rule's name; the deferral naming the rule when the destination has no STARTTLS. `API/RestApiRules.cs`: a domain's rule created with the new condition and actions, listed under the domain and not the server, replaced and deleted there. The Deck's harness chooses a domain and posts to its route.
