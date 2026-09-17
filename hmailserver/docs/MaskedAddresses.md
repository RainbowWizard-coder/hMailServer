Masked addresses
================

A masked address is an address a person makes for one shop, one sign-up or one
newsletter - Fastmail's Masked Email, Apple's Hide My Email - so that the address they
give out is not the one they live in. Mail to it arrives in their inbox; they can reply
from it; and the day it starts to bring spam, or the shop sells its list, they switch it
off and mail to it is refused, with nothing else of theirs given away.

In the webmail
--------------

**Settings, Masked addresses.** The person says what the address is for and presses
**Make an address**. The server makes one in the account's own domain - a short form of
what it is for, a dot and six random characters, such as
`bookshop.k3v9qa@example.com` - and the page copies it. Each address is listed with what
it is for and how many messages it has brought, and can be copied, switched off and on,
or removed.

- While an address is on, mail to it is delivered to the account, and the account may
  write as it: it is in the compose From picker (`kind: masked`), a reply to mail that
  came through it goes out from it, and it is accepted in `MAIL FROM` on an
  authenticated session when `SmtpAuthenticatedSenderCheck` is on.
- Switched off, mail to it is refused at `RCPT` exactly as mail to an address that never
  existed is - not handed to the domain's catch-all - and the account no longer writes
  as it. Removed, likewise, and it is not made again.
- The six random characters come from an alphabet without the ones people misread
  (no `0`, `1`, `i`, `l`, `o`), and each new address is checked against the server's
  accounts, aliases, distribution lists and other masked addresses.

For the administrator
---------------------

- `reader_masked_addresses_max` on `PUT /api/v1/settings` (20 by default, at most 200;
  0 takes the page away).
- A masked address is found after the accounts, aliases and lists of the domain. An
  account, alias or list made later with the same address takes it over, so the
  administrator's address always wins.
- The addresses are the account's own: `hm_maskedaddresses` (schema 6067), carried by
  the backup with the account - a restore that dropped them would bounce the mail of
  every shop the person signed up with - and removed with the account.
- The count of messages is kept when mail is delivered through the address.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/masked-addresses` | `{addresses:[{id,address,label,active,received,created}],max}`, newest first. |
| `POST /api/v1/me/masked-addresses` | `{label}`: a new address, on. 403 past the ceiling. |
| `PUT /api/v1/me/masked-addresses/{id}` | `{label, active}`, either. |
| `DELETE /api/v1/me/masked-addresses/{id}` | Removed; mail to it refused. |
