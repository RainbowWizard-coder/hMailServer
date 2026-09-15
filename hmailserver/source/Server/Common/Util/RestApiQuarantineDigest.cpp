// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The two pages a quarantine digest's links lead to:
//
//    GET /held/release?t=<token>   deliver a held message, or everything the
//                                  digest listed
//    GET /held/stop?t=<token>      stop sending digests to this account
//
// Both are reached with no credentials at all, and that is the design rather
// than a shortcut: a person who has just been told that a message they were
// expecting is being held should be one click from having it, not one sign-in
// and three pages. What makes that safe is that the capability is the whole
// token and nothing else - 32 random bytes recorded against one account and one
// entry, good once, dead in a few days - so there is no id to guess, no
// signature to forge, no parameter to tamper with, and nothing an attacker can
// do with a stolen link that the person who received the mail could not do
// anyway.
//
// WHY EVERY REFUSAL LOOKS THE SAME
//
// Unknown, another account's, already used, expired, deleted: one answer for all
// of them. Distinguishing them would turn the page into an oracle - "that token
// exists but is used" tells somebody with a half-guessed token that the other
// half is worth finishing.
//
// WHY THE PAGE IS WHAT IT IS
//
// The same shape and the same policy as the public file-download page in
// RestApiFiles.cpp: no script, no remote anything, no indexing, no framing, no
// referrer. A page reached from an e-mail is a page reached by people who have
// been taught to be careful about pages reached from e-mails, so it says what
// happened and asks for nothing.
//
// WHY A GET CHANGES SOMETHING
//
// It does, and that is a deliberate exception to the rule. The alternative is a
// page with a button, which means a form, which means the reader's first click
// does nothing and their second does the thing - and half of them never make the
// second click. The risk a GET normally carries is that somebody else's page can
// cause one; here that would need the token, and anybody with the token can
// simply visit the link. The mail client's own link prefetch is the real cost,
// and it is bounded: the worst it can do is deliver a message to the person the
// message was addressed to.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "Unicode.h"

#include "../AntiSpam/QuarantineDigest.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   AnsiString Utf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   AnsiString HtmlEscape(const AnsiString &value)
   {
      AnsiString out;

      for (int i = 0; i < value.GetLength(); i++)
      {
         const char c = value.GetAt(i);

         switch (c)
         {
         case '&':  out += "&amp;"; break;
         case '<':  out += "&lt;"; break;
         case '>':  out += "&gt;"; break;
         case '"':  out += "&quot;"; break;
         case '\'': out += "&#39;"; break;
         default:   out += c; break;
         }
      }

      return out;
   }

   HttpResponse Page(int status, const AnsiString &title, const AnsiString &heading, const AnsiString &detail)
   {
      HttpResponse response;
      response.status = status;
      response.content_type = "text/html; charset=utf-8";
      response.body =
         "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + HtmlEscape(title) + "</title>"
         "<style>body{font-family:system-ui,sans-serif;background:#0f1419;color:#e6edf3;display:flex;justify-content:center;padding:48px 16px}main{max-width:460px;width:100%;background:#161b22;border:1px solid #30363d;border-radius:12px;padding:24px}h1{font-size:18px;margin:0 0 12px}p{margin:8px 0;color:#9aa4ae;font-size:14px}</style></head>"
         "<body><main><h1>" + HtmlEscape(heading) + "</h1><p>" + HtmlEscape(detail) + "</p></main></body></html>";

      // No script, no remote source, no form, no frame, no referrer, no index.
      // The same policy the public file page is served under.
      response.extra_headers =
         "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"
         "X-Frame-Options: DENY\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "X-Robots-Tag: noindex\r\n"
         "Cache-Control: no-store\r\n";

      return response;
   }
}

namespace HM
{
   HttpResponse
   RestApiServer::HandleDigestRelease_(const AnsiString &query)
   {
      const AnsiString token = QueryParameter_(query, "t");

      const QuarantineDigest::Outcome outcome = QuarantineDigest::Redeem(token);

      return Page(outcome.http_status, "Held mail", Utf8(outcome.heading), Utf8(outcome.detail));
   }

   HttpResponse
   RestApiServer::HandleDigestStop_(const AnsiString &query)
   {
      const AnsiString token = QueryParameter_(query, "t");

      QuarantineDigest::Token row;

      // The stop link and the release link are the same kind of token and the
      // same table, and the redeeming is the same call - but a release token
      // arriving here, or a stop token arriving at the release page, means
      // somebody has hand-edited a URL. Answered as any other token that names
      // nothing this person may act on.
      if (!QuarantineDigest::FindToken(token, row) || row.kind != QuarantineDigest::KindOptOut)
      {
         return Page(410, "Held mail", "This link no longer works",
            "A link in a digest can be followed once, and stops working after a few days. You can switch the digest off in the webmail, under Held mail.");
      }

      const QuarantineDigest::Outcome outcome = QuarantineDigest::Redeem(token);

      return Page(outcome.http_status, "Held mail", Utf8(outcome.heading), Utf8(outcome.detail));
   }
}
