// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The headers the webmail's page and script are served with. The page itself
// is Portal.html, its script Portal.js, its manifest Portal.webmanifest and
// its service worker PortalServiceWorker.js, beside this file: real files,
// edited as such and run as such by build/portal-script-test.js, and carried
// into the binary by build/generate-portal-page.py as PortalPageData.cpp - the
// same arrangement as the S/MIME module (PortalSmime.js, PortalSmimeData.cpp),
// and build/check-portal-script.py runs the page and the script as files.
//
// img-src names 'self' because the page shows images the server answers from
// its own routes - a colleague's directory photo, the person's own - by their
// URL: an <img src="/api/v1/me/directory/7/photo">. Without 'self' a policy
// that lists data: and blob: alone forbids exactly that, and those images
// silently did not appear. It is this origin and no other: nothing remote is
// allowed here, which is what check-portal-script.py holds the whole policy to.
//
// require-trusted-types-for 'script' with trusted-types default: in a browser
// that has Trusted Types, a string reaches an HTML sink, or is used as a
// script's address, only through the one policy Portal.js makes at its top,
// named default, and no other policy can be made. It passes the HTML of
// messages to DOMParser as it is - that document is never shown and runs
// nothing - takes out of anything put into the page itself the elements the
// page never builds (script, style, link, meta, base, frames, object, form
// and the like), and admits two script addresses, the worker's and the
// OpenPGP module's. The protected-message page and the worker make no policy
// and use no sink, so for them every sink is simply refused.
//
// X-Frame-Options: DENY says what frame-ancestors 'none' says, for a browser
// that reads only the older header. Cross-Origin-Opener-Policy: same-origin
// takes from a page of another site that opened the webmail - a link in a
// message read elsewhere, the protected-message page reached from one - the
// handle on its window that would let it navigate the webmail's tab later, or
// count its frames. The webmail opens its own windows with noopener and keeps
// no handle on them, so nothing of its own needs that handle.
//
// Still in the binary, deliberately: the portal answers on a listener that may
// be the only thing an installation has, and a page that depends on a file
// beside the binary is a page that is missing where somebody most needs it.
// The headers stay a literal here, in the shape check-portal-script.py reads:
// a name, "=", and one string literal per line until the semicolon.

#include "StdAfx.h"

namespace HM
{
   const char *PortalHeaders =
      "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; frame-src 'self' blob:; manifest-src 'self'; worker-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'none'; require-trusted-types-for 'script'; trusted-types default\r\n"
      "X-Frame-Options: DENY\r\n"
      "Cross-Origin-Opener-Policy: same-origin\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Referrer-Policy: no-referrer\r\n"
      "Cache-Control: no-store\r\n";
}
