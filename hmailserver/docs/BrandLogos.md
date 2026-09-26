# Brand logos (BIMI)

The webmail shows the logo a sender's domain publishes as the avatar of its mail, the way Gmail and Outlook.com do. A bank, a shop or a newsletter that publishes a BIMI record appears in the list with its logo instead of its initials.

A logo is a claim that the mail came from that brand. So this server shows one only when it has checked that the claim is true, and only in the way the domain asked.

## What has to be on

| Setting | Where | Why |
|---|---|---|
| `AuthenticationResultsEnabled=1` | `[Settings]`, applies when it is saved | The server records its DMARC verdict in its own `Authentication-Results` header. The logo is shown only for mail that header says passed. Without the header there is no verdict to read, and no logo. |
| Anti-spam → DMARC on | Control Panel, REST `antispam` group | Without the DMARC check there is no verdict at all. |
| `BrandLogosEnabled` | `hm_settings`, REST `brand_logos_enabled`, on by default | The switch for the feature itself. Off, the listing names no brand and the logo route refuses. |

## What is checked

For a message, the listing and the message's JSON carry `brand` - the `header.from` domain of a `dmarc=pass` in this server's own Authentication-Results header. A header another server wrote is never read, and one claiming to be this server's is removed on receipt.

For the logo, `GET /api/v1/me/brand-logo?domain=` answers the SVG itself only when all of these hold:

1. `default._bimi.<domain>` - or, when the domain has none, the same name at its organizational domain - is a `v=BIMI1` record with an `l=` location. An empty `l=` means the domain declines to show a logo.
2. The domain's DMARC policy, found the way the DMARC check finds it, is `quarantine` or `reject` with `pct=100`. When the policy is the organizational domain's, its `sp=` must not be `none`. A domain at `p=none` gets no logo, even for mail that passed.
3. The location is `https`, reached only at a public address, within ten seconds and 32 KB, with no redirect followed.
4. The file is SVG Tiny Portable/Secure: an `svg` root with `baseProfile="tiny-ps"`, no `x` or `y` on it, a `title`, and none of script, `foreignObject`, `image`, links, animation, `style` elements, event attributes, entities, `data:` or external references.

Every answer, found or refused, is kept a day per domain. A newsletter that sends a hundred messages causes one lookup a day.

An answer that is kept is served at once. One that has to be asked for waits on the DNS and the logo's host on one of the REST listener's four workers, so it takes its turn with the other requests that wait on another server - a one-click unsubscribe, an OpenPGP key lookup, a test push: an account has one of them in flight at a time, and the server two at most in all. A request past that is answered `429` with `code` `reaching_out_busy` and `Retry-After: 1`, and the webmail shows the initials for it until the list is shown again. However slow the hosts a caller names, the other two workers go on answering everybody else.

A logo host that gives no answer at all for three seconds or more - a host that takes the connection and says nothing, or one that drops it - is held against the whole organizational domain of the name asked (the Public Suffix List's answer, as DMARC finds it) for an hour: every name under it is answered `fetch_failed` at once, without asking the DNS or the host. Whoever owns a domain has a new subdomain for every request, each publishing a logo at such a host, and without the hold each would take ten seconds of a worker. A host that refuses at once, or answers with an error, costs nothing and is not held. `BrandLogo::NoteUnreachable`.

A refusal is `404` with `error` (the reason in a sentence) and `outcome`: `no_record`, `declined`, `weak_policy`, `not_https`, `fetch_failed`, `not_tiny_ps` or `switched_off`. A domain that is not a domain name is `400 bad_domain`. The webmail shows the initials whenever the logo does not come.

## What is not checked

A Verified Mark Certificate (`a=`) is not validated. Gmail and Apple Mail require one; this server shows the logo the domain publishes, which is exactly what DMARC proves and no more. The `a=` value is read and ignored.

## Privacy

The server fetches the logo, not the reader's browser, so the brand learns nothing about who opened what. The only thing the brand's server sees is a request from this mail server, at most once a day.
