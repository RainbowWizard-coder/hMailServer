// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The HTML of an account's formatted signature, reduced to the subset the
   // webmail's own cleaner admits, on the server.
   //
   // WHY THIS IS ON THE SERVER. The webmail stores a formatted signature as
   // HTML (hm_accounts.accountsignaturehtml) and the page puts it back on the
   // screen with innerHTML - Portal.js renderSettings (shown at every sign-in)
   // and the rich compose editor. The page cleans what it sends with a
   // client-side cleaner (Portal.js cleanHtml), but a signature that does not
   // come through the page - PUT /api/v1/me/settings by hand, the administrator
   // account route, a COM write, a restore, a row edited by hand - never ran
   // that cleaner, so it could carry a script into the next browser that
   // renders it. That browser is the account holder's own, and - because an
   // administrator's support session opens the webmail AS the account - an
   // administrator's. The webmail is served under Trusted Types since 6.3.6,
   // which stops the markup at the sink in a browser that has it; this is the
   // second layer, on the server, for every browser.
   //
   // WHERE IT IS APPLIED. Where the webmail is given a signature and where the
   // account holder saves one: RestApiServer::SettingsJson_, the one answer
   // every rendering of it comes from (GET and PUT /api/v1/me/settings), and
   // HandleMeSettingsPut_ before it stores one. NOT in Account itself: the same
   // column is the signature the server appends to mail an account submits
   // (SignatureAdder), which an administrator sets in the Control Panel, over COM
   // or the account route, with the styles and hosted images any mail client
   // shows - reducing it there would strip every such signature from outgoing
   // mail, and rewrite it in the database at the account's next save.
   //
   // WHAT IT KEEPS. Exactly what Portal.js cleanHtml keeps, so that a signature
   // written through the page passes through this unchanged (a fixed point):
   // b/strong, i/em, u, br, p/div, ul, ol, li, blockquote, h1-h3, a, img, and a
   // simple table (table/thead/tbody/tfoot/tr/td/th). An element not on the list
   // is dropped and its cleaned contents kept. Elements map as the page maps
   // them (strong->b, em->i, h1-h3->b, div->p, thead/tfoot->tbody), a table is
   // written with the page's fixed border attributes, an empty paragraph becomes
   // a line break.
   //
   //   * No attribute survives but a link's href (http(s)/mailto only), an
   //     image's src (a data:image/{png,jpeg,gif,webp};base64 URL only, as the
   //     page admits) and an image's alt. No on* handler, no style, no class,
   //     no id - nothing else of any element.
   //   * script, style, iframe, svg, math, template, title, textarea, xmp,
   //     noscript, noframes, noembed and plaintext are dropped WITH their
   //     contents (the parser consumes a raw-text or foreign element whole),
   //     never kept as text, so nothing they hold is parsed. Any other element
   //     not on the list - object, embed, form, meta, link, base, applet and the
   //     rest - is dropped as a tag, its contents cleaned like everything else.
   //   * Text is escaped as the page escapes it. A numeric character reference
   //     and the page's own named ones are read; any other named one (&copy;) is
   //     kept as written, for the browser to read.
   //   * The tree is built no deeper than 128 elements, so neither the rebuild
   //     nor the tree's destruction, which recurse per level, can run out of
   //     stack on a signature of nested tags; and every scan is bounded, so the
   //     time grows with the signature and not with its square.
   //
   // Idempotent: sanitising a sanitised signature returns it unchanged, so
   // sanitising on the write path and again as it is answered does not drift. A
   // signature with no '<' - the common plain one - is returned as it is, so an
   // account without a formatted signature costs nothing.
   //
   // Only the ACCOUNT signature. A domain's signature is appended to outgoing
   // mail and shown in the Control Panel and the Deck as text, never handed to
   // a browser's innerHTML; it is left as it is.
   class SignatureHtmlSanitizer
   {
   public:
      // The signature reduced to the subset above. A table start tag is
      // rewritten to the page's fixed one, which is longer than "<table>", so a
      // value built from many bare table tags can grow - the account holder's
      // write measures the result as well as what was sent.
      static String Sanitize(const String &html);
   };

   // SignatureHtmlSanitizer held to Portal.js cleanHtml's result: the elements
   // and attributes it keeps and drops, the element mapping, the data: image
   // rule, the http(s)/mailto link rule, a raw-text element dropped whole, the
   // character references, the fixed point on the page's own output,
   // idempotence, the byte-for-byte round trip of the signature the image
   // fixture stores, and the depth and time bounds on a hostile one. A function
   // of values only: no database, setting, file or network. Runs inside the
   // service with the other class tests (ClassTester) and on Linux as
   // signature-html-sanitizer-tests. Throws std::logic_error naming the first
   // thing that is wrong.
   class SignatureHtmlSanitizerTester
   {
   public:
      void Test();
   };
}
