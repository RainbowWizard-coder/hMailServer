Sending as another address
==========================

Who an account may write as - in the webmail's **From** and in `MAIL FROM` on an
authenticated SMTP session when `SmtpAuthenticatedSenderCheck` is on - is four kinds of
address:

| Kind | Given by | How |
|---|---|---|
| The account's own address | - | Always. |
| An alias that points at it | The administrator | An active alias, followed a few hops. |
| Another mailbox on this server | That mailbox's owner | The post (`p`) right on their INBOX, which any IMAP client's SETACL can grant. |
| An address at another provider | The person themselves | Proved with a code mailed to it (below). |

The webmail's identity picker (`GET /api/v1/me/identities`) lists exactly these, with
`kind` `account`, `alias`, `granted` or `verified`, and the send route accepts exactly
these - the list is the SMTP rule turned around, so what the picker offers is what the
server takes.

Proving an address elsewhere
----------------------------

**Settings, Other accounts, Send mail as** in the webmail: the person types the address
(and, if they like, the name to send under) and presses **Send the code**. The server
mails a six-digit code to that address from the postmaster of the account's domain, with
the null sender, and the person types it back. From then on the address is theirs to
write as, until they remove it.

- The code lives in the server's memory for an hour and is never stored: five wrong
  tries end it, a new one goes out no sooner than a minute after the last, and a restart
  means asking for another. Nothing in a database dump proves an address.
- An address of a domain on this server is refused. It belongs to someone here, who
  shares it with an alias or the post right; a code mailed to it would be delivered
  to them, not proved by the person asking.
- An address the account can already write as, and one added already, are refused.
- `reader_sender_addresses_max` (5 by default, at most 20; 0 takes the section away)
  bounds how many an account has, proved or not.
- The addresses are the account's own data: `hm_senderaddresses`, carried by the
  backup with the account, and removed with it.

**What this cannot change.** Mail this server sends as `someone@gmail.com` is not
aligned with `gmail.com`: it is not signed by Gmail's DKIM key and not sent from an
address Gmail's SPF record names. A receiver that honours the domain's DMARC policy will
quarantine or refuse it when that policy says so. The page reads each address's policy
and says so beside an address whose domain publishes `quarantine` or `reject`; the API
gives it as `dmarc_policy`. For such an address, send from its own provider.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/sender-addresses` | `{addresses:[{id,address,name,verified,created,dmarc_policy}],max}` |
| `POST /api/v1/me/sender-addresses` | `{address, name}`: adds it unproved and mails the code; `code_sent` says whether the message was queued. 400 for an address on this server, 409 for one already writable or added, 403 past the ceiling. |
| `POST /api/v1/me/sender-addresses/{id}/verify` | `{code}`: 200 proved; 400 with `attempts_left`; 410 when no code is waiting (expired, restarted, or five wrong). |
| `POST /api/v1/me/sender-addresses/{id}/resend` | 202 sent; 429 with `retry_after` within a minute of the last; 409 when already proved. |
| `DELETE /api/v1/me/sender-addresses/{id}` | Removes it; the account no longer writes as it. |

Each is reachable by the account's own credential or session only.
