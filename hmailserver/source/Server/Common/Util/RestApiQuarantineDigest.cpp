// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The two pages a quarantine digest's links lead to:
//
//    /held/release?t=<token>   deliver a held message, or everything the digest
//                              listed
//    /held/stop?t=<token>      stop sending digests to this account
//
// GET shows what the token would do and a button; POST does it. Both are reached
// with no credentials at all, and that is the design rather than a shortcut: a
// person who has just been told that a message they were expecting is being held
// should be one click from having it, not one sign-in and three pages away. What
// makes that safe is that the capability is the whole token and nothing else -
// 32 random bytes recorded against one account and one entry, good once, dead in
// a few days - so there is no id to guess, no signature to forge and nothing to
// tamper with.
//
// WHY A GET DOES NOT ACT
//
// It was written to, and that was wrong. Mail security products follow every
// link in a message before a person sees it, to scan what is behind it; a link
// that released mail on a GET would have one of them releasing every held
// message in every digest, spam included, into every inbox, and spending every
// token so that the reader's own click then said "this link no longer works".
// So GET is a question and POST is the answer. The one exception is RFC 8058's
// one-click unsubscribe, which is a POST by definition - a mail client's
// unsubscribe button - and so needs no page.
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
// referrer, and a form that can post only back to this server. A page reached
// from an e-mail is a page reached by people who have been taught to be careful
// about pages reached from e-mails, so it says what it will do and asks for
// nothing but the click.

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

   // Only the characters a token is made of reach the page, so the one value
   // this file writes into an attribute cannot carry anything but hex.
   AnsiString TokenOnly(const AnsiString &value)
   {
      AnsiString out;

      for (int i = 0; i < value.GetLength() && i < 64; i++)
      {
         const char c = value.GetAt(i);

         if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            out += c;
         else
            return AnsiString();
      }

      return out;
   }

   HttpResponse Page(int status, const AnsiString &heading, const AnsiString &detail, const AnsiString &form)
   {
      HttpResponse response;
      response.status = status;
      response.content_type = "text/html; charset=utf-8";
      response.body =
         "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Held mail</title>"
         "<style>body{font-family:system-ui,sans-serif;background:#0f1419;color:#e6edf3;display:flex;justify-content:center;padding:48px 16px}main{max-width:460px;width:100%;background:#161b22;border:1px solid #30363d;border-radius:12px;padding:24px}h1{font-size:18px;margin:0 0 12px}p{margin:8px 0;color:#9aa4ae;font-size:14px;overflow-wrap:anywhere}button{margin-top:12px;padding:10px 16px;border-radius:8px;border:0;background:#36c2ff;color:#04121c;font-weight:600;font-size:15px;cursor:pointer}"
         // The ring round the button the keyboard is on: the webmail's focus
         // violet (Portal.html), not each browser's own.
         "button:focus-visible{outline:2px solid #b89cff;outline-offset:2px}</style></head>"
         "<body><main><h1>" + HtmlEscape(heading) + "</h1><p>" + HtmlEscape(detail) + "</p>" + form + "</main></body></html>";

      // No script, no remote source, no frame, no referrer, no index; a form may
      // post back here and nowhere else.
      response.extra_headers =
         "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'\r\n"
         "X-Frame-Options: DENY\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "X-Robots-Tag: noindex\r\n"
         "Cache-Control: no-store\r\n";

      return response;
   }

   HttpResponse Refused()
   {
      return Page(410, "This link no longer works",
         "A link in a digest can be followed once, and stops working after a few days. Sign in to the webmail to see what is still being held for you.",
         AnsiString());
   }
}

namespace HM
{
   HttpResponse
   RestApiServer::HandleDigestRelease_(const AnsiString &method, const AnsiString &query, const AnsiString &body)
   {
      AnsiString token = method == "POST" ? QueryParameter_(body, "t") : AnsiString();

      if (token.IsEmpty())
         token = QueryParameter_(query, "t");

      token = TokenOnly(token);

      if (token.IsEmpty())
         return Refused();

      QuarantineDigest::Token row;

      // A stop token arriving at the release page means somebody edited a URL.
      // It is answered as any other token that names nothing this person may do
      // here, and it is not spent.
      if (!QuarantineDigest::FindToken(token, row) || row.kind == QuarantineDigest::KindOptOut)
         return Refused();

      if (method == "GET")
      {
         const QuarantineDigest::Outcome preview = QuarantineDigest::Inspect(token);

         if (!preview.ok)
            return Page(preview.http_status, Utf8(preview.heading), Utf8(preview.detail), AnsiString());

         const AnsiString form =
            "<form method=\"post\" action=\"/held/release\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
            "<button type=\"submit\">" + AnsiString(preview.released == 1 ? "Deliver it" : "Deliver them") + "</button></form>";

         return Page(200, Utf8(preview.heading), Utf8(preview.detail), form);
      }

      const QuarantineDigest::Outcome outcome = QuarantineDigest::Redeem(token);

      return Page(outcome.http_status, Utf8(outcome.heading), Utf8(outcome.detail), AnsiString());
   }

   HttpResponse
   RestApiServer::HandleDigestStop_(const AnsiString &method, const AnsiString &query, const AnsiString &body)
   {
      AnsiString token = method == "POST" ? QueryParameter_(body, "t") : AnsiString();

      // RFC 8058's one-click POST carries "List-Unsubscribe=One-Click" as its
      // body and the token only in the URL it was given.
      if (token.IsEmpty())
         token = QueryParameter_(query, "t");

      token = TokenOnly(token);

      if (token.IsEmpty())
         return Refused();

      QuarantineDigest::Token row;

      if (!QuarantineDigest::FindToken(token, row) || row.kind != QuarantineDigest::KindOptOut)
         return Refused();

      if (method == "GET")
      {
         const QuarantineDigest::Outcome preview = QuarantineDigest::Inspect(token);

         if (!preview.ok)
            return Page(preview.http_status, Utf8(preview.heading), Utf8(preview.detail), AnsiString());

         const AnsiString form =
            "<form method=\"post\" action=\"/held/stop\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
            "<button type=\"submit\">Stop the digest</button></form>";

         return Page(200, Utf8(preview.heading), Utf8(preview.detail), form);
      }

      const QuarantineDigest::Outcome outcome = QuarantineDigest::Redeem(token);

      return Page(outcome.http_status, Utf8(outcome.heading), Utf8(outcome.detail), AnsiString());
   }
}
