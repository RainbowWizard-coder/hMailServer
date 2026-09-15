Signatures with images
======================

A signature can carry a logo. This is how the image gets from the webmail's
settings page to the recipient's mail client, what size it may be and why, and
what the server does to a message so that the image is actually shown when it
arrives.

Where a signature is kept
-------------------------

There are two, and a third thing that behaves like one:

* **The account's signature** — a plain one and a formatted one. The reader sets
  both in the webmail, **Settings > Name and signature**; an administrator can set
  them in the Control Panel's account dialog, over COM (`Account.SignatureEnabled`,
  `.SignaturePlainText`, `.SignatureHTML`) or with `PUT /api/v1/accounts/{address}`
  (`signature_enabled`, `signature_plain_text`, `signature_html`). They are the
  columns `accountsignatureplaintext` and `accountsignaturehtml` of `hm_accounts`.
* **The domain's signature** — the Control Panel's domain dialog, `Domain.SignatureHTML`
  and the rest over COM, `signature_*` on `PUT /api/v1/domains/{domain}`.
* **The domain's disclaimer** — see [DomainTransforms.md](DomainTransforms.md).

The webmail puts the account's signature on a message itself, as the reader
writes it: the plain one in a plain message, the formatted one when formatting is
switched on. What the domain adds — its signature, by the domain's method decided
against the account's, and its disclaimer — the server puts on a webmail send and
send-later as it does on an SMTP submission (`OutgoingFooters`; see
[DomainTransforms.md](DomainTransforms.md)). A message submitted over SMTP by a
mail client gets the account's or the domain's signature from the server
(`SignatureAdder`), when the account or the domain has one switched on.

An image in the formatted signature
-----------------------------------

**Add image** in the signature's toolbar takes a PNG, JPEG, GIF or WebP file and
puts it into the formatted signature as a `data:` URL — the only form a page can
keep an image in without somewhere to upload it to. The whole formatted signature
is saved as `signature.html` through `PUT /api/v1/me/settings`. The page believes
a file's first bytes rather than its name, as the server does when it sends the
image: a PNG saved with a `.jpg` name goes in as a PNG, and a file that is none of
the four is refused.

### How large

**64 KB — 64,000 bytes of UTF-8 — for the whole formatted signature, its images
included.** One number for the account's signature as the webmail sets it, which
the page, the route and the database agree on (what does not check it is at the
end of this section):

| Layer | What it does |
|---|---|
| The page | Says so beside the editor before a file is chosen. When a file does not fit in what is left, draws it smaller (below) and refuses it if even that does not fit, saying how many KB are free. Refuses to save a formatted signature over 64 KB, which an image pasted in rather than chosen can make. |
| `PUT /api/v1/me/settings` | Refuses `signature.html` over 64,000 bytes with 400 and `a formatted signature is at most 64 KB (64,000 bytes), its images included`. The plain signature stays at 20,000 characters. The route is a large request, like sending a message, so the listener's 64 KB cap on ordinary requests is not what refuses it. |
| The database | `accountsignaturehtml` is `text` on MySQL, which holds 65,535 bytes, and `ntext` or `text` on SQL Server, SQL Server Compact and PostgreSQL, which hold far more. The narrowest decides, so a signature saved on one backend can be saved on every other. |

Why a limit this size: a signature goes on every message the account sends, and
on every reply in a thread. An image is stored in it as base64, a third larger
than the file, so 64 KB leaves room for a logo of up to about 45 KB with a few
lines of text — which is a logo, not a photograph. The limit is the account's
signature through the webmail; the Control Panel, COM and the administrator's
REST route do not check it, and a MySQL server refuses anything over 65,535 bytes
from them.

### Made smaller to fit

When the chosen file does not fit, and the browser can draw it (every current
browser can), the page draws it at most **600 by 200 pixels** — twice the size a
signature shows a logo at, so it stays sharp on a high-density screen — and then
three-quarters of that, and so on, until it fits or it is too small to be a logo
any longer — 48 pixels wide or 16 high — when it is refused. A JPEG stays a JPEG, at quality
0.85; anything else becomes a PNG, which keeps transparency. A GIF drawn smaller
loses its animation. A file that already fits is kept exactly as it is.

What the recipient receives
---------------------------

Gmail and Outlook do not display a `data:` image in a message. What every mail
client that embeds an image sends instead is RFC 2387 `multipart/related`: the
HTML, and beside it each image as a part of its own with a `Content-ID`, which the
HTML names with a `cid:` URL (RFC 2392). The server turns the first form into the
second just before it writes the message (`InlineImageParts`):

```text
multipart/alternative
├── text/plain
└── multipart/related; type="text/html"
    ├── text/html          <img src="cid:img1.3f2a…@host" alt="">
    └── image/png          Content-ID: <img1.3f2a…@host>
                           Content-Disposition: inline; filename="image1.png"
```

The related part goes around the HTML alone, inside the alternative, as
Thunderbird writes it — so the top of the message is still `multipart/alternative`
(or `multipart/mixed` when there are attachments) and a mail client shows no
paperclip for a logo. A message whose HTML already sits in a related part — the
shape Outlook writes, related around the alternative — has the images added to
that part.

What is moved:

* an image whose `data:` URL is the whole, quoted value of a `src` attribute
  (`<img src="data:image/png;base64,…">`);
* of type PNG, JPEG, GIF or WebP, whose bytes start as that type does;
* once: the same image named twice in the HTML is one part named twice;
* at most **64** different images in one message. A new image past the 64th stays
  the `data:` URL it was. 64 is what the webmail's view of a message puts back in
  place (below), so every image of a Sent copy or a draft still shows there — a
  part put back, or a `data:` URL the view shows as it is; a recipient's client
  shows the 64 parts and not what was left. It is more than a message written by
  hand carries, a logo and a few screenshots. A footer the server appends
  afterwards is a pass of its own, so a footer's logo comes on top of the
  message's 64; the webmail's view still puts back 64.

What is left as it was: a `data:` URL in CSS (`url(...)`) or in another attribute
(`data-src`), an SVG or any other type, bytes that are not the image the URL says,
and anything under `multipart/signed` or `multipart/encrypted`.

Where it happens:

* **A message sent from the webmail** (`POST /api/v1/me/messages`) — the
  signature's image and any image pasted into a formatted message. The copy kept
  in **Sent** is the same message.
* **A draft saved from the webmail** (`POST /api/v1/me/drafts`), so that a draft
  sent later by the schedule, or opened in another mail client, shows its images
  too.
* **A signed or an encrypted message from the webmail.** The page seals these
  itself and the server cannot change them afterwards, so the page's S/MIME module
  builds the same `multipart/related` part before it signs (`PortalSmime.js`).
* **A message the server puts a footer on** — the account's or the domain's
  signature, or the domain's disclaimer, on an SMTP submission; the domain's
  signature or disclaimer on a webmail send or send-later, which get them after
  the route's own move above — when what was added carries a `data:` image. Every
  `data:` image in that HTML is moved then, the sender's own included. A message
  the server adds nothing to is sent exactly as the client wrote it.

In the webmail
--------------

The Sent copy and a draft show their images: the message page asks for the HTML
through `GET /api/v1/me/messages/{id}/html`, which puts each `cid:` image back in
place as a `data:` URL for its frame — up to 64 images, 4 MB each and 12 MB
together — and the image is listed under the message as an attachment marked
*in the message*. The page frames the HTML only of a message of up to 1 MB (a
larger one is shown as its text, as it always was), so it is the count of 64, the
same count the move keeps to, that could bind there, and it does not.

A draft opened again in the compose form comes back as its text, as it always
has. The images this webmail made parts of its HTML — those whose Content-ID is
`img<n>.<32 hex digits>@<host>`, as the server writes one, or
`img<n>.<16 hex digits>@webmail.invalid`, as the S/MIME module does — are not
attached to it again as files: before they were parts they were `data:` URLs in
that HTML, which a draft never brought back either, and the signature's logo comes
back with the formatted signature when formatting is switched on. An image another
mail client embedded in a draft it saved is carried as a file, as it always was.
A forward, whose HTML does not go with it, still carries every one of the
original's images as files.

Sharp edges
-----------

* **Not every mail client displays WebP.** The page accepts it, and a WebP logo
  arrives as a part like any other; a logo that has to show everywhere is safer as
  a PNG.
* **An attachment added by a script after the message is written** —
  `Message.Attachments.Add` in an event handler, or setting `Message.Body` on a
  message that has no text part — rebuilds the message in `MessageData::CreatePart`.
  Before 15 September 2026 that rebuild knew only a text part, an HTML part and an
  alternative at the top of the message. It dropped a `multipart/related` part,
  and the HTML and images inside it, so a message Thunderbird sent with an image,
  or Outlook with an image and an attachment, lost its HTML to a script that added
  a file, and every message this move writes would have too. It now carries a
  related part whose first part is the HTML, or an alternative with the HTML in
  it, across whole; a message that is itself the related part (an HTML-only
  message with images, or Outlook's related around the alternative) is put into a
  related part of its own first. The added file goes beside it in
  `multipart/mixed`, and a text part set afterwards beside it in
  `multipart/alternative`.
* **The size is checked on the way in, not on the way out.** A domain signature or
  a disclaimer set in the Control Panel with a large image is moved into a part
  like any other, whatever its size; only the account's signature through the
  webmail is held to 64 KB.

The code
--------

| Where | What |
|---|---|
| `Common/Util/InlineImageParts.*` | The move from `data:` URLs to `cid:` parts, `MostImages`, and the in-process tests (the move, and a file added afterwards to each shape it leaves). |
| `Common/BO/MessageData.cpp` | `CreatePart`, with `FindFormattedBodyNoRecurse_`, `IsFormattedRelated_` and `LiftRelatedBody_`: a related part kept whole when a file or a text part is added. |
| `Common/Util/RestApiServer.cpp` | `HandleMeSettingsPut_` (the limit), `IsLargeRequest_` (the request cap), the one call each in `HandleMeMessageSend_` and `HandleMeDraftSave_`, and `HandleMeMessageHtml_`, which puts back as many images as `MostImages`. |
| `Common/Util/SignatureAdder.cpp`, `DisclaimerAdder.cpp` | The call after the HTML they append. |
| `Common/Util/Portal.js` | `SIGNATURE_HTML_BYTES`, the page's check, `shrinkSignatureImage`. |
| `Common/Util/PortalSmime.js` | `embedDataImages`, for a message the page signs or encrypts, keeping to the same 64. |
| `test/RegressionTests/API/RestApiSignatureImages.cs` | The limit at 64,000 bytes and one byte over, the delivered message's parts and bytes, the Sent copy and the draft in the webmail, the domain signature over SMTP, a message with nothing to move, a file added afterwards through COM to a draft with and without a file, and sixty-five images of which sixty-four become parts. Linked into the Linux suite too, where the COM step is skipped. |
