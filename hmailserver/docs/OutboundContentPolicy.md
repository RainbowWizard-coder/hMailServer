# Outbound content policy

Rules on what leaves: a card number or a bank account number in the text, an attachment that is a program whatever it is called, a message marked internal with an outside address on it - and what to do about it: refuse it while the sender is still on the line, hold it for someone to look at, or let it go and tell the sender. Exchange calls this data loss prevention; here it is four more conditions and one more action in the rules the server already runs on every message ([MailFlowRules.md](MailFlowRules.md)), so a policy is a rule, lives with the rules, and is edited where they are.

## What a rule can ask

Each condition is answered as a list with `;` between the entries, in the shape the attachment-name condition set, so `contains x` reads as "any of them is x", `equals ""` as "none", and a regular expression or a wildcard runs over the whole list.

| Condition | REST word | Control Panel | Answers |
| --- | --- | --- | --- |
| Sensitive content | `sensitive_content` | Sensitive content found | The detectors that found something in the readable text: `card_number`, `iban`, `account_number`. |
| Attachment type | `attachment_type` | Attachment type by its bytes | Every attachment's type from its first bytes, not its name: `executable`, `elf`, `pdf`, `zip`, `office_xml`, `jar`, `office`, `rar`, `7z`, `gzip`, `png`, `jpeg`, `gif`, `rtf`, `script`, `text`, `empty`, `binary`. |
| External recipients | `external_recipient` | External recipients | Every envelope recipient this server does not host; empty when every recipient is local. |
| Readable text | `content` | Readable text | The text itself, for a pattern of the rule's own. |

**The readable text** is the plain part, the HTML part with its tags removed and its entities unwrapped (so a number laid out in table cells is still a number), and every attachment whose bytes are text - a `.csv` sent as `application/octet-stream` included, because a list of numbers is readable whatever its declared type. An attachment that begins with a UTF-16 byte-order mark (Excel's "Unicode Text", a Notepad file saved as Unicode) is read through the mark; one without a mark is read as UTF-8, of which plain ASCII is a subset. It is bounded at a megabyte.

**The detectors:**

* `card_number`: a run of 13 to 19 digits with at most one space or hyphen between any two of them, that passes the Luhn check and is not one digit repeated. A run that fails as a whole is tried again up to each of its separators, so `4111 1111 1111 1111 123` - a card and then its security code - is found by the sixteen digits before the last space. A run longer than 19 digits is a reference, a hash or a phone list, and is not searched for a card inside it (a card followed by a four-digit group is twenty digits and is left alone), and a solid run with no separator is only tried whole.
* `iban`: two letters and two digits at the start of a word, the two letters a country in the IBAN registry, then exactly that country's number of characters with spaces allowed between them (the way a bank prints one, in fours), followed by something that is not a letter or a digit, and the ISO 13616 check (mod 97 = 1) passes. A country the registry does not list is never asked the question, so a part number beginning `PO12` is not tried at every length.
* `account_number`: the `dlp_account_number_pattern` setting's regular expression, searched anywhere in the text - an account-number format of this organisation, such as `\b\d{2}-\d{2}-\d{2} \d{8}\b` for a sort code and account. Empty, the default, is a detector that never fires. Over REST the setting is refused unless it compiles. The search runs through the same breaker the rules' own `regex` condition has ([MailFlowRules.md](MailFlowRules.md)): a pattern that reached the database another way and does not compile is reported as `HM6821`, a search Boost abandons as too complex or that takes over the 250 ms budget is reported as `HM6822` - as an abandonment, not as a pattern that does not compile - and either way the pattern is rested for five minutes, finding nothing meanwhile. That is fail-open, on purpose and said out loud: the text is up to a megabyte of what a sender chose to write, and a pattern that back-tracks hard over it is a cost any sender could make recur per message.

**Marked internal.** There is no new condition for it, because the existing header condition is the right one: `header` `Sensitivity` `contains` `Company-Confidential` (RFC 2156's value; Outlook writes it when a message is marked confidential), combined with `external_recipient` `contains` `@`. A rule-set marker works the same way: an earlier rule sets a header, a later rule tests it.

**A sealed message** - S/MIME or PGP/MIME encrypted end to end, decided from its body and not from its `Content-Type` label, so a `multipart/encrypted` header over ordinary text parts hides nothing - has no text this server can read, so `sensitive_content` and `content` decline for it exactly as the body condition does ([SealedMail.md](SealedMail.md)): the condition does not match, whichever way it is phrased, and the application log says why once per rule. `attachment_type` and `external_recipient` still answer, since the envelope and the first bytes are not sealed.

## What a rule can do

* **Block** - the existing `reject` action: the SMTP session answers `550 5.7.1` with the action's text before the message is accepted, where the sender is still there to be told. Since 17 September 2026 a reject with no text of its own names the rule: `Message refused by the rule "No card numbers leave" of this server.` The message is not queued, not bounced and not stored. The webmail, the Deck and `POST /api/v1/me/messages` ask the same rules before they queue, and refuse the send with `403`, code `policy_rejected` and the rule's text in `error`, which the webmail shows the sender; nothing is queued and no copy is kept in Sent. A reject that meets a message only after acceptance - a scheduled send, a marker an earlier rule wrote that the DATA-time check cannot see, a rule added while the message waited in the queue - drops it as a delete does, and a sender this server hosts is mailed the notice below saying it was not sent (`X-hMailServer-Policy-Outcome: not-sent`), because the alternative is a Sent folder that says it was.
* **Quarantine for review** - the existing `quarantine` action: held under `Rule: <name>` for an administrator to release or delete from the Quarantine page, the Deck or `/api/v1/quarantine`. A released message is not held again by the rule that held it: the deliverer runs the rules again on the way back and the rule matches again, and the release mark - the one the recipients' own block lists honour - is what says a person has judged it, so the review can end in delivery. The rule's other actions still run.
* **Warn the sender** - `warn_sender`, new, with an optional text: the message goes on its way (or wherever the rule's other actions send it), and the sender is mailed a notice from `postmaster@<their domain>` with the subject `Policy notice: <rule name>`, the recipients and subject of their message, the rule's name, the action's text, `X-hMailServer-Policy: <rule name>` and `X-hMailServer-Policy-Outcome: delivered`. It is `Auto-Submitted: auto-generated`, so nothing answers it, and it is sent only when this server hosts the sender's domain: a notice to an outside sender about a policy of ours is backscatter, and an outside sender gets nothing. It goes once per message, on the first delivery attempt: a recipient that greylists or is down for an hour does not mean a notice per retry, and a message released from the quarantine does not mean a second. That is the server's and the domains' rules; an account's own rule runs once for its recipient, when the recipient's copy is stored, and its notice goes then, whichever attempt that is. No notice is sent in answer to an auto-submitted message, none once the rule loop limit is reached (`HM6820`), none to a sender in a hosted domain who is no mailbox here (`HM6823`), and a notice that could not be queued is `HM6824`.

## Where a policy runs

A policy on what leaves is a rule of the server or of the sender's domain, and runs where they run: at the end of DATA and before the webmail queues for `reject`, and in the deliverer for everything else, before DKIM signing and the split between local and external delivery. An account's rules are the recipient's own filters: they run at local delivery on what that account receives, and never see the account's own outgoing mail, so a policy cannot be one of them. A rule with `external_recipient contains @` fires only on a message with a recipient this server does not host, which is what "what leaves" means; a message between two local mailboxes carrying the same card number is not touched by it, and a rule without that condition applies to inbound mail too.

An external recipient is one not resolved to a local name when the message was accepted, not a local account, not in a domain this server hosts, and not reached through a route marked *treat recipient as local* (for every address or for that one). The first of those four is known only while the message is being accepted; the other three are known on a retry as well - the route is asked again, the way it was at RCPT - so a message on its second delivery attempt is judged the same way.

## Setting up a policy

The Control Panel's Rules page (the server, a domain, or an account), the Deck's Rules page, or REST:

```json
POST /api/v1/rules
{
  "name": "No card numbers leave",
  "all_criteria": true,
  "criteria": [
    { "field": "external_recipient", "match": "contains", "value": "@" },
    { "field": "sensitive_content", "match": "contains", "value": "card_number" }
  ],
  "actions": [
    { "type": "reject", "value": "Card numbers may not be sent by mail. Use the payment portal." }
  ]
}
```

`{ "field": "attachment_type", "match": "contains", "value": "executable" }` with `delete` or `quarantine` catches a program called `report.txt`; `{ "field": "content", "match": "regex", "value": "(?s).*PROJECT-[0-9]{4}.*" }` is a rule's own pattern (the rule engine's regex condition matches the whole value, so bracket it as shown, or use `contains`). The account-number pattern is `PUT /api/v1/settings` with `dlp_account_number_pattern`, or the Deck's Settings page.

## What it does not do

* **It is pattern matching, and patterns have false positives.** One 16-digit reference number in ten passes Luhn. The conditions a rule combines it with - an external recipient, a domain, a subject - are what keep a policy from firing on the company's own invoices, and a `warn_sender` or `quarantine` action is the safer first choice for a new pattern than `reject`.
* **It reads text, not documents.** A card number inside a PDF, a Word file or a spreadsheet is not found; those are `pdf`, `office_xml` and `office` to the attachment-type condition, and a rule can hold or refuse them by type, but their text is not extracted. An image of a card is an image. An encrypted zip is `zip`.
* **Attachment types are the first bytes.** A file type whose signature is not in the list above is `binary` (or `text` if its bytes read as text). An Office document is told from any other zip by its first entry, which is how Office writes them and how a hand-made one may not be.
* **No fingerprints, no classifiers, no dictionaries** of medical or legal terms, no per-user exceptions, no "policy tip" in the sender's mail client before they press Send. The sender learns at DATA, from the 550, or from the notice.
* **`warn_sender` does not hold the message.** It is a notice beside whatever the rule's other actions decide. A rule with only that action delivers the message.
* **Size** is the existing `message_size` condition; there is no per-attachment size.
* **A rule's name and a reject's text are one line.** The name goes into the 550 reply and into the notice's `Subject` and `X-hMailServer-Policy` headers, and the text is the 550 line, so REST refuses a control character in either (a `warn_sender` text is a body and may have paragraphs), and a line break that arrived another way - the Control Panel, COM - is written as a space where the value is used.

## What is tested

`Rules/OutboundContentPolicy.cs`, on its own ports (9636-9640): a card number to an outside address refused at DATA in the rule's words, the same text between colleagues delivered, and a number that fails Luhn leaving through the route; an IBAN in a text attachment quarantined under the rule's name and never attempted; a message held by a rule and released by a person reaching the outside once, not held again, its sender told once; a recipient answering 451 on the first attempt and one notice, not two; an account's own warn_sender rule sending its notice when its recipient is reached on a later attempt; a card number sent through `POST /api/v1/me/messages` refused with 403 and the rule's text, and the same text to a colleague going; a reject met after acceptance (through a marker an earlier rule writes) dropping the message and telling the hosted sender it was not sent; a program called `report.txt` deleted by its bytes while a real text file of the same name is kept; the notice to a hosted sender with its subject, headers, text and the message still delivered; no notice to an outside sender; a message marked `Sensitivity: Company-Confidential` refused with the rule named in the default text and delivered to a colleague; the account-number setting refused when it does not compile, matching when set, and the `content` condition reading a `.csv` declared as `application/octet-stream` and a UTF-16 file through its mark. `API/RestApiRules.cs` round-trips the new words through the listing and refuses a line break in a name and in a reject text while allowing paragraphs in a warn_sender text. The in-process `ContentPolicyTester` holds the Luhn and IBAN checks to their published vectors, the phone number and the run of zeros that must not count, a card followed by its security code, the twenty-digit run and the solid run that must not count, the UTF-16 marks either way round, and the file signatures.
