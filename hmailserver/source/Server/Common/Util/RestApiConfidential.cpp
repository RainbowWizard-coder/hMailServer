// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A message that expires (ConfidentialMessages.h). The sender's side:
//
//    GET  /api/v1/me/confidential              every one sent, newest first
//    POST /api/v1/me/confidential/{id}/revoke  withdrawn now
//
// and sending one is POST /api/v1/me/messages with expires_in (RestApiServer.cpp).
// The reader's side is the page at /confidential/{token}, which takes no
// credentials: the token names one message, and the reader proves they are one
// of its recipients before the page shows it.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "Unicode.h"
#include "ConfidentialMessages.h"
#include "Encoding/Base64.h"
#include "../BO/Account.h"
#include "../BO/Attachment.h"
#include "../BO/Attachments.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *CookieName = "hmconfidential";
      const int InlineImageMaximum = 5 * 1024 * 1024;

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String Wide(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Html(const String &value)
      {
         const AnsiString bytes = Utf8(value);
         AnsiString out;
         for (int i = 0; i < bytes.GetLength(); i++)
         {
            char c = bytes[i];
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '"') out += "&quot;";
            else if (c == '\'') out += "&#39;";
            else out += c;
         }
         return out;
      }

      bool IsToken(const AnsiString &value)
      {
         if (value.GetLength() != 64)
            return false;
         for (int i = 0; i < value.GetLength(); i++)
         {
            const char c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }
         return true;
      }

      String UtcText(__int64 seconds)
      {
         struct tm parts;
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d UTC"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
         return text;
      }

      AnsiString CookieValue(const AnsiString &cookies, const AnsiString &name)
      {
         const AnsiString prefix = name + "=";
         int start = 0;
         while (start < cookies.GetLength())
         {
            int end = cookies.Find(";", start);
            if (end < 0)
               end = cookies.GetLength();
            AnsiString part = cookies.Mid(start, end - start);
            part.Trim();
            if (part.GetLength() > prefix.GetLength() && part.Mid(0, prefix.GetLength()) == prefix)
               return part.Mid(prefix.GetLength());
            start = end + 1;
         }
         return AnsiString();
      }

      // Text from HTML, for a message that has no text part: the blocks that end
      // a line end one, what is inside style and script is dropped, every tag
      // goes, and the entities people write are read.
      String TextOfHtml(const String &html)
      {
         String lower = html;
         lower.ToLower();
         String text;
         int i = 0;
         const int length = html.GetLength();
         while (i < length)
         {
            if (html[i] != '<')
            {
               text += html[i];
               i++;
               continue;
            }

            int close = html.Find(_T(">"), i);
            if (close < 0)
               break;
            String tag = lower.Mid(i + 1, close - i - 1);
            tag.Trim();

            if (tag.Find(_T("style")) == 0 || tag.Find(_T("script")) == 0)
            {
               const String ending = tag.Find(_T("style")) == 0 ? _T("</style") : _T("</script");
               int end = lower.Find(ending, close);
               if (end < 0)
                  break;
               int after = html.Find(_T(">"), end);
               i = after < 0 ? length : after + 1;
               continue;
            }

            if (tag.Find(_T("br")) == 0 || tag == _T("/p") || tag == _T("/div") || tag == _T("/li") || tag == _T("/tr") ||
                tag == _T("/h1") || tag == _T("/h2") || tag == _T("/h3") || tag == _T("/blockquote"))
               text += _T("\n");
            i = close + 1;
         }

         text.Replace(_T("&nbsp;"), _T(" "));
         text.Replace(_T("&lt;"), _T("<"));
         text.Replace(_T("&gt;"), _T(">"));
         text.Replace(_T("&quot;"), _T("\""));
         text.Replace(_T("&#39;"), _T("'"));
         text.Replace(_T("&amp;"), _T("&"));
         text.Trim();
         return text;
      }

      HttpResponse Page(int status, const AnsiString &title, const AnsiString &inner)
      {
         HttpResponse response;
         response.status = status;
         response.content_type = "text/html; charset=utf-8";
         response.body =
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + title + "</title>"
            "<style>"
            ":root{--bg:#0f1419;--card:#161b22;--line:#30363d;--text:#e6edf3;--muted:#9aa4ae;--accent:#36c2ff;--on:#04121c;--err:#ff7b72;--focus:#b89cff;--focus-glow:rgba(184,156,255,.28)}"
            "@media (prefers-color-scheme: light){:root{--bg:#f6f8fa;--card:#ffffff;--line:#d0d7de;--text:#1f2328;--muted:#57606a;--accent:#0969da;--on:#ffffff;--err:#cf222e;--focus:#7542e5;--focus-glow:rgba(117,66,229,.25)}}"
            "body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;justify-content:center;padding:32px 16px;margin:0}"
            "main{max-width:760px;width:100%;box-sizing:border-box;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px}"
            "h1{font-size:20px;margin:0 0 12px;overflow-wrap:anywhere}p{margin:8px 0;color:var(--muted);font-size:14px;overflow-wrap:anywhere}"
            "label{display:block;font-size:14px;color:var(--muted);margin-top:12px}"
            "input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--text);font-size:15px;margin:6px 0}"
            "button{padding:10px 16px;border-radius:8px;border:0;background:var(--accent);color:var(--on);font-weight:600;font-size:15px;cursor:pointer;margin-top:6px}"
            "button.quiet{background:transparent;color:var(--accent);padding:6px 0}"
            // Where the keyboard is, as on the webmail (Portal.html): the focus
            // violet as a field's border with a glow outside it, and as the
            // ring round a button; under forced colours, the system's highlight.
            "input{outline:none}input:focus{border-color:var(--focus);box-shadow:0 0 0 1px var(--focus),0 0 8px 2px var(--focus-glow)}"
            "a:focus-visible,button:focus-visible{outline:2px solid var(--focus);outline-offset:2px}"
            "@media (forced-colors:active){input:focus{outline:2px solid Highlight;outline-offset:2px}}"
            ".err{color:var(--err)}.meta{font-size:13px;color:var(--muted);border-bottom:1px solid var(--line);padding-bottom:12px;margin-bottom:16px;overflow-wrap:anywhere}"
            ".badge{display:inline-block;margin-top:8px;padding:4px 8px;border-radius:6px;border:1px solid var(--line);font-size:12px}"
            ".body{white-space:pre-wrap;overflow-wrap:anywhere;font-size:15px;line-height:1.5;-webkit-user-select:none;user-select:none}"
            "img{display:block;max-width:100%;height:auto;margin:12px 0;pointer-events:none;-webkit-user-drag:none}"
            "ul{padding-left:20px;color:var(--muted);font-size:14px}"
            "@media print{body *{display:none!important}body::after{content:'This message cannot be printed.'}}"
            "</style></head><body><main>" + inner + "</main></body></html>";

         // No script at all, images only as data the page itself carries, forms
         // only back to here, no framing, no referrer, no index, no cache.
         response.extra_headers =
            "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'\r\n"
            "X-Frame-Options: DENY\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Referrer-Policy: no-referrer\r\n"
            "X-Robots-Tag: noindex, noarchive\r\n"
            "Cache-Control: no-store\r\n";
         return response;
      }

      HttpResponse Gone(const AnsiString &heading, const AnsiString &detail)
      {
         return Page(410, heading, "<h1>" + heading + "</h1><p>" + detail + "</p>");
      }

      HttpResponse ProvePage(const ConfidentialMessages::Entry &entry, int status, const AnsiString &problem)
      {
         AnsiString inner = "<h1>A confidential message</h1>";
         inner += "<p>" + Html(entry.sender) + " sent you a confidential message. It can be read on this page until " + Html(UtcText(entry.expires)) + ", and not after.</p>";
         if (!problem.IsEmpty())
            inner += "<p class=\"err\">" + problem + "</p>";
         inner += "<form method=\"post\" action=\"/confidential/" + entry.token + "\"><input type=\"hidden\" name=\"step\" value=\"code\">"
                  "<label for=\"address\">The address it was sent to</label><input id=\"address\" type=\"email\" name=\"address\" autocomplete=\"email\" required>"
                  "<button type=\"submit\">Send me a code</button></form>";
         if (!entry.passcode_hash.IsEmpty())
         {
            inner += "<form method=\"post\" action=\"/confidential/" + entry.token + "\"><input type=\"hidden\" name=\"step\" value=\"passcode\">"
                     "<label for=\"passcode\">Or the passcode the sender gave you</label><input id=\"passcode\" type=\"password\" name=\"passcode\" autocomplete=\"off\" required>"
                     "<button type=\"submit\">Read</button></form>";
         }
         inner += "<p>Signed in to this server's webmail with the address it was sent to? Open the link from the webmail and it opens without a code.</p>";
         return Page(status, "A confidential message", inner);
      }

      HttpResponse CodePage(const ConfidentialMessages::Entry &entry, const String &address, int status, const AnsiString &problem)
      {
         AnsiString inner = "<h1>Enter the code</h1>";
         inner += "<p>If " + Html(address) + " is an address this message was sent to, a code is on its way there. It works for ten minutes.</p>";
         if (!problem.IsEmpty())
            inner += "<p class=\"err\">" + problem + "</p>";
         inner += "<form method=\"post\" action=\"/confidential/" + entry.token + "\"><input type=\"hidden\" name=\"step\" value=\"verify\">"
                  "<input type=\"hidden\" name=\"address\" value=\"" + Html(address) + "\">"
                  "<label for=\"code\">Code</label><input id=\"code\" name=\"code\" inputmode=\"numeric\" autocomplete=\"one-time-code\" maxlength=\"6\" required>"
                  "<button type=\"submit\">Read</button></form>";
         inner += "<form method=\"post\" action=\"/confidential/" + entry.token + "\"><input type=\"hidden\" name=\"step\" value=\"code\">"
                  "<input type=\"hidden\" name=\"address\" value=\"" + Html(address) + "\"><button class=\"quiet\" type=\"submit\">Send another code</button></form>";
         return Page(status, "Enter the code", inner);
      }

      HttpResponse MessagePage(const ConfidentialMessages::Entry &entry)
      {
         const String path = ConfidentialMessages::PathOf(entry);
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         MessageData data;
         if (path.IsEmpty() || !data.LoadFromMessage(path, message))
            return Gone("This message is no longer here", "It has expired or been withdrawn by its sender.");

         String text = data.GetBody();
         if (text.IsEmpty())
            text = TextOfHtml(data.GetHTMLBody());

         AnsiString inner = "<h1>" + Html(data.GetSubject().IsEmpty() ? String(_T("(no subject)")) : data.GetSubject()) + "</h1>";
         inner += "<div class=\"meta\">From: " + Html(data.GetFrom()) + "<br>To: " + Html(data.GetTo());
         if (!data.GetCC().IsEmpty())
            inner += "<br>Cc: " + Html(data.GetCC());
         inner += "<br>Date: " + Html(data.GetSentTime());
         inner += "<br><span class=\"badge\">Confidential: readable here until " + Html(UtcText(entry.expires)) + ". Not offered for download, forwarding, copying or printing.</span></div>";
         inner += "<div class=\"body\">" + Html(text) + "</div>";

         std::shared_ptr<Attachments> attachments = data.GetAttachments();
         AnsiString images;
         AnsiString others;
         for (size_t i = 0; attachments && i < attachments->GetCount(); i++)
         {
            std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
            if (!attachment)
               continue;
            AnsiString type = attachment->GetContentType();
            type.ToLower();
            AnsiString size;
            size.Format("%d", attachment->GetSize());
            const bool image = type == "image/png" || type == "image/jpeg" || type == "image/gif" || type == "image/webp";
            AnsiString content;
            if (image && attachment->GetSize() <= InlineImageMaximum && attachment->GetContent(content) && !content.IsEmpty())
            {
               images += "<img alt=\"" + Html(attachment->GetFileName()) + "\" src=\"data:" + type + ";base64," +
                  Base64::Encode(content.c_str(), content.GetLength()) + "\">";
               continue;
            }
            others += "<li>" + Html(attachment->GetFileName()) + " (" + size + " bytes)</li>";
         }
         inner += images;
         if (!others.IsEmpty())
            inner += "<p>Attached, and not offered for download from this page:</p><ul>" + others + "</ul>";

         inner += "<p>This page keeps the message on its sender's server and ends when it expires. It cannot stop a photograph of the screen, and it does not claim to.</p>";
         return Page(200, Html(data.GetSubject().IsEmpty() ? String(_T("A confidential message")) : data.GetSubject()), inner);
      }
   }

   AnsiString
   RestApiServer::OpenApiConfidentialPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/confidential\":{\"get\":{\"summary\":\"The messages that expire this account has sent\",\"description\":\"{available,max_seconds,items:[{id,subject,recipients,created,expires,expired,opened,opened_by,revoked,passcode,link}]}, newest first. available is whether this server offers them at all - it needs a public address in confidential_base_url (or the digest's or the import sign-in's base URL) for the links - and max_seconds the longest expiry. Sent with expires_in on POST /api/v1/me/messages: the message is kept on this server and the recipients are sent a notice with link, where they read it after proving they are a recipient - signed in to the webmail as one, a one-time code sent to the address, or the passcode the sender set. opened is when it was first read (0 until then) and opened_by the address, or passcode. Seconds since 1970 throughout. A row stays thirty days after its expiry, the kept copy not: it is deleted when the expiry passes or the message is withdrawn. Not end-to-end encryption, and no protection against a photograph of the screen: what it stops is a message being forwarded on by accident or found in a mailbox a year later.\",\"responses\":{\"200\":{\"description\":\"The items\"}}}},"
         "\"/api/v1/me/confidential/{id}/revoke\":{\"post\":{\"summary\":\"Withdraw a message that expires\",\"description\":\"The reading page refuses it from now on and the kept copy is deleted; the row stays in the list as revoked.\",\"responses\":{\"200\":{\"description\":\"Withdrawn\"},\"404\":{\"description\":\"Not one of this account's\"}}}},"
         "\"/confidential/{token}\":{\"get\":{\"summary\":\"Read a message that expires (no credentials)\",\"description\":\"The page a recipient's notice links to. A browser signed in to the webmail as one of the recipients (the account's own session, not a support session) reads it at once; any other is asked for proof - a one-time code sent to one of the recipient addresses (POST step=code&address=, then step=verify&address=&code=; five tries a code, ten minutes, one code a minute per address, five a day per address, six an hour per message, and after thirty wrong answers in all codes no longer open the message until the sender sends it again), or the passcode the sender set (step=passcode&passcode=; ten wrong pause it for a quarter of an hour). An address the message did not go to is answered exactly as a recipient's, under the same limits, and no code is mailed. A wrong code or passcode counts against the source address as a failed logon does. A proof sets a cookie scoped to this path for half an hour. The page is text only, HTML-escaped, with the images the message carried inline as data, under a policy that runs nothing and frames nowhere, no-store, no referrer, no print stylesheet and nothing to download. 404 for no such message, 410 once it has expired or been withdrawn.\",\"responses\":{\"200\":{\"description\":\"The message, or the proof form\"},\"404\":{\"description\":\"No such message\"},\"410\":{\"description\":\"Expired or withdrawn\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeConfidential_(const Caller &caller, RouteKind kind, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (kind == RouteMeConfidential)
      {
         std::vector<ConfidentialMessages::Entry> entries;
         if (!ConfidentialMessages::List(account->GetID(), entries))
            return BuildResponse_(500, "{\"error\":\"the messages could not be read\"}");

         const __int64 now = (__int64) time(nullptr);
         const AnsiString base = Utf8(ConfidentialMessages::BaseUrl());
         AnsiString body;
         body.Format("{\"available\":%hs,\"max_seconds\":%d,\"items\":[", base.IsEmpty() ? "false" : "true", ConfidentialMessages::MaximumSeconds);
         for (size_t i = 0; i < entries.size(); i++)
         {
            const ConfidentialMessages::Entry &entry = entries[i];
            AnsiString numbers;
            numbers.Format("{\"id\":%I64d,\"created\":%I64d,\"expires\":%I64d,\"expired\":%hs,\"opened\":%I64d,\"revoked\":%hs,\"passcode\":%hs,",
               entry.id, entry.created, entry.expires, entry.expires <= now ? "true" : "false", entry.opened,
               entry.revoked ? "true" : "false", entry.passcode_hash.IsEmpty() ? "false" : "true");
            body += (i ? "," : "") + numbers;
            body += "\"subject\":\"" + JsonEscape_(Utf8_(entry.subject)) + "\"";
            body += ",\"opened_by\":\"" + JsonEscape_(Utf8_(entry.opened_by)) + "\"";
            body += ",\"link\":\"" + JsonEscape_(base) + "/confidential/" + entry.token + "\"";
            body += ",\"recipients\":[";
            for (size_t r = 0; r < entry.recipients.size(); r++)
               body += (r ? ",\"" : "\"") + JsonEscape_(Utf8_(entry.recipients[r])) + "\"";
            body += "]}";
         }
         return BuildResponse_(200, body + "]}");
      }

      if (kind == RouteMeConfidentialRevoke)
      {
         if (!ConfidentialMessages::Revoke(account->GetID(), id))
            return BuildResponse_(404, "{\"error\":\"not one of your messages that expire\"}");
         LOG_APPLICATION("RestApi: " + account->GetAddress() + " withdrew a message that expires.");
         return BuildResponse_(200, "{\"revoked\":true}");
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   // The reading page. GET shows the message to a browser that has proved itself
   // - with a grant cookie from an earlier proof, or signed in to the webmail as
   // a recipient - and otherwise asks for proof; POST takes a step of it: ask for
   // a code, give the code, or give the passcode. A proof sets the grant cookie,
   // scoped to this message's path, and sends the browser back to GET.
   HttpResponse
   RestApiServer::HandleConfidentialPage_(const AnsiString &method, const AnsiString &token, const AnsiString &request, const IPAddress &peer_address)
   {
      ConfidentialMessages::Entry entry;
      if (!IsToken(token) || !ConfidentialMessages::FindByToken(token, entry))
         return Page(404, "No such message", "<h1>No such message</h1><p>There is no message at this address, or it has been removed.</p>");

      const __int64 now = (__int64) time(nullptr);
      if (entry.revoked)
         return Gone("This message was withdrawn", "Its sender withdrew it, and it can no longer be read.");
      if (!entry.IsReadable(now))
         return Gone("This message has expired", "It could be read until " + Html(UtcText(entry.expires)) + ". Ask its sender if you still need it.");

      String by;
      const AnsiString grant = CookieValue(GetHeader_(request, "cookie"), CookieName);
      if (!grant.IsEmpty() && ConfidentialMessages::Granted(entry, grant, by))
      {
         if (method == "GET")
         {
            ConfidentialMessages::MarkOpened(entry, by);
            return MessagePage(entry);
         }
      }

      if (method == "GET")
      {
         // A webmail session as a recipient opens it at once - the account's
         // own session, not a support session an administrator opened as the
         // account: that is not the recipient reading it, and the sender's
         // record of who opened it must stay true.
         Caller caller;
         if (AuthenticateSession_(request, peer_address, caller) && caller.account && caller.support_by.IsEmpty() &&
             entry.IsRecipient(caller.account->GetAddress()))
         {
            ConfidentialMessages::MarkOpened(entry, caller.account->GetAddress());
            return MessagePage(entry);
         }
         return ProvePage(entry, 200, AnsiString());
      }

      const AnsiString body = GetRequestBody_(request);
      const AnsiString step = QueryParameter_(body, "step");
      String address = Wide(QueryParameter_(body, "address"));
      address.Trim();
      address.ToLower();
      if (address.GetLength() > 254)
         address = address.Mid(0, 254);

      auto granted = [&](const String &who)
      {
         const AnsiString value = ConfidentialMessages::Grant(entry, who);
         if (value.IsEmpty())
            return ProvePage(entry, 500, "The page could not remember that you proved it; try again.");
         AnsiString headers;
         headers.Format("Set-Cookie: %hs=%hs; Path=/confidential/%hs; HttpOnly; SameSite=Strict; Max-Age=1800%hs\r\nLocation: /confidential/%hs\r\nReferrer-Policy: no-referrer\r\n",
            CookieName, value.c_str(), entry.token.c_str(), use_tls_ ? "; Secure" : "", entry.token.c_str());
         return BuildResponse_(303, "{}", headers);
      };

      // A wrong code or passcode is a failed proof, counted against the source
      // address the way a failed logon is (RegisterAuthenticationFailure_), so
      // that guessing at this page trips the same short refusal as guessing at
      // the API - and the codes' own ceilings hold whatever the source.
      if (step == "code")
      {
         if (address.IsEmpty())
            return ProvePage(entry, 400, "Enter the address the message was sent to.");
         // The same answer, page and status alike, whether or not the address
         // is a recipient's - and the same limits, since an address the
         // message did not go to gets an entry like any other - so the page
         // does not tell a stranger who the message went to.
         const ConfidentialMessages::CodeResult sent = ConfidentialMessages::SendCode(entry, address);
         if (sent == ConfidentialMessages::CodeTooSoon)
            return CodePage(entry, address, 429, "A code was sent less than a minute ago; wait for it, or try again in a minute.");
         if (sent == ConfidentialMessages::CodeTooMany)
            return ProvePage(entry, 429, "Too many codes have been asked for recently, for this message or for that address. Wait a while, or ask the sender.");
         if (sent == ConfidentialMessages::CodeClosed)
            return ProvePage(entry, 403, "Too many wrong codes have been entered for this message, so codes no longer open it. Ask its sender to send it again.");
         if (sent == ConfidentialMessages::CodeFailed)
            return ProvePage(entry, 500, "The code could not be sent; try again in a minute.");
         return CodePage(entry, address, 200, AnsiString());
      }

      if (step == "verify")
      {
         AnsiString code = QueryParameter_(body, "code");
         code.Trim();
         const ConfidentialMessages::CheckResult result = ConfidentialMessages::CheckCode(entry, address, code);
         if (result == ConfidentialMessages::CheckRight)
            return granted(address);
         RegisterAuthenticationFailure_(peer_address);
         if (result == ConfidentialMessages::CheckClosed)
            return ProvePage(entry, 403, "Too many wrong codes have been entered for this message, so codes no longer open it. Ask its sender to send it again.");
         if (result == ConfidentialMessages::CheckLocked)
            return ProvePage(entry, 403, "That code no longer works. Ask for a new one.");
         return CodePage(entry, address, 403, "That code is not right.");
      }

      if (step == "passcode" && !entry.passcode_hash.IsEmpty())
      {
         const String passcode = Wide(QueryParameter_(body, "passcode"));
         const ConfidentialMessages::CheckResult result = ConfidentialMessages::CheckPasscode(entry, passcode);
         if (result == ConfidentialMessages::CheckRight)
            return granted(_T("passcode"));
         RegisterAuthenticationFailure_(peer_address);
         if (result == ConfidentialMessages::CheckLocked)
            return ProvePage(entry, 429, "Too many wrong passcodes. Wait a quarter of an hour, or ask for a code instead.");
         return ProvePage(entry, 403, "That passcode is not right.");
      }

      return ProvePage(entry, 400, AnsiString());
   }
}
