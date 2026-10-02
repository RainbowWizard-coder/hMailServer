Response compression on the HTTP listeners
==========================================

The REST listener - which serves the webmail at `/portal`, the Control Deck at
`/` and the API under `/api/v1` - and the web-services listener - ACME
challenges, MTA-STS, `security.txt`, autoconfiguration, CalDAV and CardDAV -
compress what they send when the client says it can decode it. Nothing has to
be switched on. This page says what is compressed, what is never compressed,
and what a proxy or a client in front of the server should expect.

Why it exists
-------------

The webmail's shell is three text files: the page (`Portal.html`, 102,792
bytes), its script (`Portal.js`, 349,442 bytes) and its S/MIME module
(`PortalSmime.js`, 61,457 bytes) - 513,691 bytes on every cold load, over a
link the reader may be paying for by the megabyte. Until 16 September 2026 every
byte of it went out as it was, although zlib had been linked into the server for
IMAP's `COMPRESS=DEFLATE` extension since 7 September. Measured with the same
zlib at the same level (6) the server uses:

| File | Uncompressed | gzip | Of the original |
|---|---:|---:|---:|
| `Portal.html` (the page) | 102,792 | 23,196 | 22.6 % |
| `Portal.js` (the script) | 349,442 | 90,595 | 25.9 % |
| `PortalSmime.js` | 61,457 | 16,860 | 27.4 % |
| The shell | 513,691 | 130,651 | 25.4 % |
| `PortalServiceWorker.js` | 2,432 | 1,020 | 41.9 % |
| `Portal.webmanifest` | 885 | not compressed | under the floor |

The three big files are compressed once, when first served, and held both ways
for the life of the process; the page is compressed per request, because the
branding, the reader's language catalogue and the text direction are put into
each copy before it goes out. An API answer is compressed per request too.

The rules
---------

1. **Only when the client asked.** `Accept-Encoding` is the whole of the
   negotiation. A client that sends none, or that gives `gzip` and `deflate` a
   weight of zero, gets exactly the bytes it always got. Weights are honoured:
   `gzip;q=0, deflate` gets `deflate`; `*` stands for whatever the client did not
   name; `x-gzip` is `gzip`. Where both are acceptable `gzip` is chosen. `br`
   (Brotli) and anything else are not offered.

2. **Only what is worth compressing.** Text of every subtype (HTML, plain, CSS,
   JavaScript, calendar, vCard, CSV), JSON, XML and anything with a `+json` or
   `+xml` suffix, a manifest, a message file (`message/rfc822`) and an icon. A
   media type not on that list - an image, a PDF, a zip, a 7-Zip backup, a font,
   `application/octet-stream`, `application/mbox` - is served as it is, because
   its bytes are already compressed and DEFLATE would spend a worker thread
   making them very slightly larger. A type the server has never heard of is
   left alone.

3. **Only between a floor and a ceiling.** An answer under 1,024 bytes is served
   as it is: gzip's framing and the poor ratio on a short string eat the saving,
   and the answer was going out in one segment either way. An answer over 4 MB
   is served as it is too: compressing it would be a tenth of a second of one of
   the listener's four worker threads, chosen by whoever asked, and the answers
   that large are message files whose bulk is base64 of something already
   compressed. And an answer DEFLATE cannot shrink at all goes out unchanged
   rather than larger with a `Content-Encoding` on it.

4. **Never a download.** An attachment fetched from a message
   (`/api/v1/me/messages/{id}/attachments/{index}`) and a file sent as a link
   (`/files/{token}`) are never compressed whatever their media type says,
   because the type is the one the sender declared and the bytes are whatever
   they sent; a part calling itself `text/plain` over thirty megabytes of
   anything is not a reason to believe it.

The headers
-----------

- `Content-Encoding: gzip` or `Content-Encoding: deflate` on a compressed
  answer. `deflate` is the zlib format of RFC 1950 - the two-byte header and the
  Adler-32 checksum - which is what RFC 9110 means by the name, not the bare
  DEFLATE stream some servers sent under it after reading RFC 2616 the other
  way. A client that decodes `deflate` with a raw inflater will fail on the
  header; ask for `gzip`, which every browser does.
- `Content-Length` is the length of the bytes actually sent. The server never
  uses chunked transfer encoding and never serves a byte range, so there is no
  partial answer for a coding to apply to half of.
- `Content-Type` describes what is under the coding, unchanged.
- `Vary: Accept-Encoding` on every answer whose type the server *would*
  compress, whether or not it did, so that a cache between the server and the
  reader keys the answer on what the client can decode. A download carries no
  `Vary`, since nothing about it varies. (The webmail and the API are served
  `Cache-Control: no-store` anyway; the header matters for the web-services
  listener, whose answers may be cached.)

Behind a reverse proxy
----------------------

First, the name: since 6.3.6 the plain loopback listener refuses with 421 a
request whose `Host` it does not expect, so a proxy that forwards the browser's
`Host` needs that name set as the webmail's public base URL before it is put in
front ([DevicesAndSessions.md](DevicesAndSessions.md), "Which `Host` the listener
answers"); one that sends its own upstream address needs nothing.

A proxy that compresses on its own account should either leave
`Accept-Encoding` alone and pass the server's answer through - the server will
not encode twice, and a proxy must not either - or strip `Accept-Encoding` from
the request and compress itself. Both work. What does not work is a proxy that
forwards `Accept-Encoding: gzip`, receives `Content-Encoding: gzip`, and hands
the bytes to a client that did not ask for them; that is the proxy's defect,
and `Vary` is there to make a caching proxy get it right.

Whatever it does with the encoding, a proxy hands the Control Deck's page on as
the server wrote it. Since 6.3.6 the page is served under a `script-src` that is
the SHA-256 of its one inline script, which the server takes from the file it
serves, so that no other script can run in it. Both of the arrangements above
keep the page's bytes as they were, and the Deck works through either - measured
in Chromium, Firefox and WebKit through a proxy of each kind. A proxy that changes
the page's script on its way through - nginx's `sub_filter` or an IIS outbound
rewrite rule whose text it matches, a content network's script optimiser or
minifier - leaves a script whose hash is not the one the server sent, the browser
refuses it, and the Deck stops at a sign-in card that does nothing. The browser's
console says so and names the hash the changed script has. Turn the rewriting off
for the Deck's page (`/` and `/index.html`); there is no setting that loosens the
policy instead. A script the proxy adds to the page of its own, such as a content
network's loader, is refused in the same way, and the Deck runs without it. The
webmail is not affected: its scripts are files of their own, allowed as files of
this server.

BREACH, and why it does not reach this server
---------------------------------------------

Compressing an answer that mixes a secret with text an attacker controls, where
the attacker can also make the reader's browser send the request and watch the
size of the answer, is the BREACH attack, and it is why many APIs refuse to
compress anything. It does not reach this one, and the reason is written here
rather than left to be rediscovered: a browser cannot make a credentialed
request to this server from another site at all. The session cookie is
`SameSite=Strict`, so another origin's request carries no cookie; HTTP Basic
and Bearer credentials are not attached across origins; a request that changes
something must also carry `X-Requested-With: hMailServer`, which another origin
cannot add; and there is no CSRF token in any answer for a size oracle to
chase, because the cross-site defence is the cookie's own attribute. The
secret is in the request's headers; the server compresses answers.

Switching it off
----------------

There is no setting. The behaviour is correct for every client that speaks
HTTP, and a client that wants uncompressed answers has the standard way of
saying so: send no `Accept-Encoding`, or send `Accept-Encoding: identity`.

Where it is
-----------

`hmailserver/source/Server/Common/Util/HttpCompression.cpp` holds the policy,
the negotiation and the compressor; `HttpServer.cpp` applies it as the last
step before an answer becomes bytes, for both listeners, so a handler needs to
know nothing about it. A handler that must not be compressed sets
`HttpResponse::no_compress`. `HttpCompressionTester` (run by the server's own
`--test-internals` and by the regression suite's `TestInternals`) proves the
negotiation, the media-type list and that a compressed body decodes to exactly
what went in; `API/RestApiCompression` proves the same over a socket - a
request with and without `Accept-Encoding`, the compressed answer decompressed
and compared byte for byte, a download left alone, and a short answer left
alone.
