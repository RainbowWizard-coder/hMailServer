// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The list manager over HTTP (ListManager.h, ListArchive.h,
// docs/MailingLists.md). Three kinds of route:
//
//    GET  /api/v1/lists/{address}/archive[?month=YYYY-MM]
//    GET  /api/v1/lists/{address}/archive/{id}
//    POST /api/v1/lists/{address}/members/{member}/reset-bounces
//    POST /api/v1/lists/{address}/members/{member}/approve
//                              the administrator's: the archive for programs
//                              and the owner's two verbs on one member, scoped
//                              to the domain in the list's address
//
//    GET    /api/v1/me/lists
//    PUT    /api/v1/me/lists/{address}      {delivery: each | digest | none}
//    DELETE /api/v1/me/lists/{address}
//                              the reader's own: the managed lists on this
//                              server their address is a member of
//
//    /lists/confirm?t=         confirm a request to join
//    /lists/unsubscribe?t=     the member's own page: leave, or choose each
//                              message, the digest or nothing for now; and
//                              RFC 8058's one-click POST. For an account the
//                              membership query answers, &m= and &l= name the
//                              address and the list, and it may only leave
//    /lists/approve?t=         the owner's approval of a request, pressed
//                              while signed in to the webmail as the owner
//    /lists/{address}/subscribe
//                              ask to join
//    /lists/{address}/archive  the archive: public, or for members
//                              - the pages list mail links to, reached with no
//                              credentials at all
//
// THE PAGES ACT ON POST AND NEVER ON GET, for the reason RestApiQuarantineDigest
// gives: mail security products follow every link in a message before its
// reader does, and a GET that confirmed or unsubscribed would have them doing
// both for everybody. The one-click POST is RFC 8058's, which is a POST by
// definition and needs no page. Every way a token can fail to name something -
// unknown, used, lapsed, another list's - is one answer. The page shape and its
// policy are the digest's: no script, nothing remote, no framing, no referrer,
// a form that posts only back here. And every page is held to a budget per
// address an hour, as the protected-message pages are; loopback is exempt.
//
// THE ARCHIVE IS SHOWN AS TEXT. A message's text part, or, for a message that
// has only HTML, the text its HTML holds with every tag, script and style taken
// out - and then escaped. No HTML from the archive is ever put in front of a
// reader, not even in a sandbox: a list takes postings from strangers. A sender
// is shown by name, or as "name at domain", and no member is ever listed.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "Unicode.h"
#include "JsonDocument.h"
#include "ListManager.h"
#include "ListArchive.h"

#include "../BO/Account.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../TCPIP/IPAddress.h"

#include <ctime>
#include <cwchar>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   // The most of an archived message the pages and the route read.
   const int MostArchiveMessageBytes = 25 * 1024 * 1024;

   // The most of a message's HTML read for its text, and the most of a
   // message's text a page or the route shows: past either, a sentence says
   // where it was cut. An archive page is reached with no credentials on a
   // public list, and a message is anybody's posting.
   const int MostHtmlCharacters = 1024 * 1024;
   const int MostShownCharacters = 256 * 1024;

   AnsiString Utf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   String FromUtf8(const AnsiString &value)
   {
      String wide;
      Unicode::MultiByteToWide(value, wide);
      return wide;
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

   AnsiString Html(const String &value)
   {
      return HtmlEscape(Utf8(value));
   }

   // Only the characters a token is made of reach the page.
   AnsiString TokenOnly(const AnsiString &value)
   {
      AnsiString out;

      if (value.GetLength() != 64)
         return out;

      for (int i = 0; i < value.GetLength(); i++)
      {
         const char c = value.GetAt(i);

         if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            out += c;
         else
            return AnsiString();
      }

      return out;
   }

   const char *PageStyle =
      "<style>body{font-family:system-ui,sans-serif;background:#0f1419;color:#e6edf3;display:flex;justify-content:center;padding:48px 16px;margin:0}"
      "main{max-width:720px;width:100%;background:#161b22;border:1px solid #30363d;border-radius:12px;padding:24px}"
      "h1{font-size:18px;margin:0 0 12px}h2{font-size:15px;margin:18px 0 8px}p{margin:8px 0;color:#9aa4ae;font-size:14px;overflow-wrap:anywhere}"
      "a{color:#36c2ff}ul{padding-left:20px}li{margin:4px 0;font-size:14px}"
      "pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:13px;background:#0f1419;border:1px solid #30363d;border-radius:8px;padding:12px}"
      "label{display:block;margin:12px 0 4px;font-size:14px}input,select{font-size:15px;padding:8px;border-radius:8px;border:1px solid #30363d;background:#0f1419;color:#e6edf3}"
      "button{margin-top:12px;padding:10px 16px;border-radius:8px;border:0;background:#36c2ff;color:#04121c;font-weight:600;font-size:15px;cursor:pointer}"
      "button.plain{background:#30363d;color:#e6edf3}</style>";

   HttpResponse Page(int status, const AnsiString &title, const AnsiString &heading, const AnsiString &detail, const AnsiString &rest)
   {
      HttpResponse response;
      response.status = status;
      response.content_type = "text/html; charset=utf-8";
      response.body =
         "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + HtmlEscape(title) + "</title>" +
         AnsiString(PageStyle) + "</head><body><main><h1>" + HtmlEscape(heading) + "</h1>" +
         (detail.IsEmpty() ? AnsiString() : "<p>" + HtmlEscape(detail) + "</p>") + rest + "</main></body></html>";

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

   HttpResponse OutcomePage(const ListManager::Outcome &outcome, const AnsiString &rest)
   {
      return Page(outcome.http_status, "Mailing list", Utf8(outcome.heading), Utf8(outcome.detail), rest);
   }

   HttpResponse TooMany()
   {
      HttpResponse response = Page(429, "Mailing list", "Too many requests", "Wait a while and try again.", AnsiString());
      response.extra_headers += "Retry-After: 3600\r\n";
      return response;
   }

   const char *DeliveryWord(int delivery)
   {
      switch (delivery)
      {
      case DistributionListRecipient::DeliveryDigest:
         return "digest";
      case DistributionListRecipient::DeliveryNone:
         return "none";
      default:
         return "each";
      }
   }

   bool ParseDelivery(const AnsiString &word, int &delivery)
   {
      if (word.CompareNoCase("each") == 0)
         delivery = DistributionListRecipient::DeliveryEach;
      else if (word.CompareNoCase("digest") == 0)
         delivery = DistributionListRecipient::DeliveryDigest;
      else if (word.CompareNoCase("none") == 0)
         delivery = DistributionListRecipient::DeliveryNone;
      else
         return false;

      return true;
   }

   // A sender as the archive shows it: the name they wrote, or, for an address
   // alone, "name at domain" - an archive page is a page harvesters read.
   String DisplayOf(const String &sender)
   {
      String value = sender;
      value.Trim();

      const int open = value.Find(_T("<"));
      if (open > 0)
      {
         String name = value.Mid(0, open);
         name.Trim();
         name.Replace(_T("\""), _T(""));
         name.Trim();
         if (!name.IsEmpty())
            return name;
      }

      String address = value;
      if (open >= 0)
      {
         const int close = value.Find(_T(">"), open);
         address = close > open ? value.Mid(open + 1, close - open - 1) : value.Mid(open + 1);
      }

      address.Trim();
      address.Replace(_T("@"), _T(" at "));
      return address;
   }

   String DateOf(__int64 unixTime)
   {
      std::time_t value = (std::time_t) unixTime;
      struct tm utc;

#ifdef HM_PLATFORM_POSIX
      if (::gmtime_r(&value, &utc) == nullptr)
         return String();
#else
      if (gmtime_s(&utc, &value) != 0)
         return String();
#endif

      String text;
      text.Format(_T("%04d-%02d-%02d %02d:%02d UTC"), utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min);
      return text;
   }

   // True when text at offset begins with word, which is lower case: compared
   // where it stands, never searched for through the rest of the text.
   bool StartsAt(const String &text, int offset, const wchar_t *word)
   {
      const size_t length = wcslen(word);
      if (offset < 0 || (size_t) offset + length > (size_t) text.GetLength())
         return false;

      return wcsncmp(text.c_str() + offset, word, length) == 0;
   }

   // The text an HTML part holds: every tag taken out, the content of script
   // and style with it, block elements as line breaks and the common entities
   // read. What comes out is text, and is escaped again where it is shown.
   //
   // Linear in the length, because a posting is anybody's and its page is
   // anybody's to ask for: each step moves forward from where the last one
   // stopped - a tag is compared where it starts, the end of a tag or of a
   // script is searched for once and jumped to, and an entity's ';' is looked
   // for in the next nine characters only. And at most MostHtmlCharacters of
   // the HTML is read; cut says whether it was all.
   String HtmlToText(const String &source, bool &cut)
   {
      cut = source.GetLength() > MostHtmlCharacters;
      const String html = cut ? source.Mid(0, MostHtmlCharacters) : source;

      String lowered = html;
      lowered.ToLower();

      String text;
      text.reserve(html.GetLength());

      // The line breaks at the end of the text so far: never more than two in
      // a row, which is what tag removal would otherwise leave by the dozen.
      int trailingBreaks = 0;

      const int length = html.GetLength();
      int i = 0;

      while (i < length)
      {
         const wchar_t c = html.GetAt(i);

         if (c == L'<')
         {
            // Script and style are dropped whole, their content with them.
            const bool script = StartsAt(lowered, i, L"<script");
            const bool style = !script && StartsAt(lowered, i, L"<style");

            if (script || style)
            {
               const int end = lowered.Find(script ? _T("</script") : _T("</style"), i);
               if (end < 0)
                  break;

               const int close = lowered.Find(_T(">"), end);
               i = close < 0 ? length : close + 1;
               continue;
            }

            const int close = html.Find(_T(">"), i);
            if (close < 0)
               break;

            // A block element starts a line.
            if (StartsAt(lowered, i + 1, L"br") || StartsAt(lowered, i + 1, L"p") || StartsAt(lowered, i + 1, L"/p") ||
                StartsAt(lowered, i + 1, L"div") || StartsAt(lowered, i + 1, L"/div") || StartsAt(lowered, i + 1, L"li") ||
                StartsAt(lowered, i + 1, L"tr") || StartsAt(lowered, i + 1, L"h"))
            {
               if (trailingBreaks < 2)
               {
                  text += _T("\n");
                  trailingBreaks++;
               }
            }

            i = close + 1;
            continue;
         }

         if (c == L'&')
         {
            int semicolon = -1;
            for (int j = i + 1; j < length && j <= i + 9; j++)
            {
               if (html.GetAt(j) == L';')
               {
                  semicolon = j;
                  break;
               }
            }

            if (semicolon > i)
            {
               const String entity = lowered.Mid(i, semicolon - i + 1);
               String replacement;

               if (entity == _T("&amp;")) replacement = _T("&");
               else if (entity == _T("&lt;")) replacement = _T("<");
               else if (entity == _T("&gt;")) replacement = _T(">");
               else if (entity == _T("&quot;")) replacement = _T("\"");
               else if (entity == _T("&#39;") || entity == _T("&apos;")) replacement = _T("'");
               else if (entity == _T("&nbsp;")) replacement = _T(" ");

               if (!replacement.IsEmpty())
               {
                  text += replacement;
                  trailingBreaks = 0;
                  i = semicolon + 1;
                  continue;
               }
            }
         }

         if (c == L'\r')
         {
            i++;
            continue;
         }

         if (c == L'\n')
         {
            if (trailingBreaks < 2)
            {
               text += _T("\n");
               trailingBreaks++;
            }

            i++;
            continue;
         }

         text += c;
         trailingBreaks = 0;
         i++;
      }

      text.Trim();
      return text;
   }

   // An archived message's text, as the pages and the route show it: at most
   // MostShownCharacters of it, and a sentence where it was cut.
   bool ReadArchivedText(const ListArchiveEntry &entry, String &text)
   {
      const String path = ListArchive::PathOf(entry);
      if (path.IsEmpty() || !FileUtilities::Exists(path))
         return false;

      if (FileUtilities::FileSize(path) > MostArchiveMessageBytes)
      {
         text = _T("(This message is too large to show here.)");
         return true;
      }

      std::shared_ptr<Message> message(new Message());
      MessageData data;
      if (!data.LoadFromMessage(path, message))
         return false;

      bool cut = false;

      text = data.GetBody();
      if (text.IsEmpty())
         text = HtmlToText(data.GetHTMLBody(), cut);

      text.Replace(_T("\r\n"), _T("\n"));

      if (text.GetLength() > MostShownCharacters)
      {
         text = text.Mid(0, MostShownCharacters);
         cut = true;
      }

      if (cut)
         text += _T("\n\n(The rest of this message is not shown here: it is longer than an archive page shows.)");

      return true;
   }

   // A list's id from a link: digits only, at most eighteen of them; 0 for
   // anything else.
   __int64 ParseId(const AnsiString &text)
   {
      if (text.IsEmpty() || text.GetLength() > 18)
         return 0;

      __int64 id = 0;
      for (int i = 0; i < text.GetLength(); i++)
      {
         const char c = text.GetAt(i);
         if (c < '0' || c > '9')
            return 0;
         id = id * 10 + (c - '0');
      }

      return id;
   }
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiMailingListPaths_()
   {
      // Begins with a comma and does not end with one, as every piece
      // HandleOpenApi_ appends does.
      return AnsiString(
         ",\"/api/v1/lists/{address}/archive\":{"
         "\"get\":{\"summary\":\"A managed list's archive\",\"description\":\"The months that hold messages (months, YYYY-MM in UTC, newest first) and the messages of one: month=YYYY-MM for that month's, oldest first, or none for the newest fifty. Each message: id, message_id, subject, sender (the From field as written), sent (Unix time) and size. What the list distributed while its archive was on - its subject prefix and footer applied - kept for archive_days whether or not the archive is still on; a copy kept only for the digest is not listed. Scoped to the domain in the address. Not reachable over IMAP or NNTP.\",\"responses\":{\"200\":{\"description\":\"address, archive, months, month, messages\"},\"400\":{\"description\":\"month is not YYYY-MM\"},\"404\":{\"description\":\"No such list, or not a managed one\"}}}},"
         "\"/api/v1/lists/{address}/archive/{id}\":{"
         "\"get\":{\"summary\":\"One archived message, as text\",\"description\":\"id, message_id, subject, sender, sent, size and text: the message's text part, or for a message with only HTML the text its HTML holds, with every tag, script and style taken out - at most 256 KB of it, with a sentence where it was cut. No HTML is ever answered.\",\"responses\":{\"200\":{\"description\":\"The message\"},\"404\":{\"description\":\"No such list or message, or its file is gone\"}}}},"
         "\"/api/v1/lists/{address}/members/{member}/reset-bounces\":{"
         "\"post\":{\"summary\":\"Clear a member's bounces\",\"description\":\"The hard and soft counts and the reason go. A member the bounces stopped is sent each message again - one who had chosen the digest included, who chooses it again on their page; a member who paused by their own choice stays paused, whatever its counts. What the owner does when the address works again.\",\"responses\":{\"200\":{\"description\":\"The member, as the members route shows a row\"},\"404\":{\"description\":\"No such managed list, or not a member of it - a request, or an address that has left, has no bounces to reset\"}}}},"
         "\"/api/v1/lists/{address}/members/{member}/approve\":{"
         "\"post\":{\"summary\":\"Approve a request to join\",\"description\":\"For a list whose subscribe_policy is 2: a request its address has confirmed (state awaiting_owner) becomes a member. Approving a member is nothing; a request not yet confirmed by its address is a 409.\",\"responses\":{\"200\":{\"description\":\"The member\"},\"404\":{\"description\":\"No such managed list, or not a request of it\"},\"409\":{\"description\":\"The request is not confirmed yet\"}}}},"
         "\"/api/v1/me/lists\":{"
         "\"get\":{\"summary\":\"The managed lists the signed-in account is a member of\",\"description\":\"Every managed list on this server with a row for the account's own address, but a row saying the address has left. Each entry: address, delivery (each, digest or none), state (member, pending or awaiting_owner), subscribed (Unix time), digest (whether the list sends one), reason (when bounces stopped the account's delivery) and archive_url when the list's archive is on and list_base_url is set.\",\"responses\":{\"200\":{\"description\":\"lists\"}}}},"
         "\"/api/v1/me/lists/{address}\":{"
         "\"put\":{\"summary\":\"Choose how a list's mail comes\",\"description\":\"Body: delivery - each, digest (only on a list that sends one) or none, which keeps the account a member and sends it nothing. Turning delivery back on clears the bounces that stopped it.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"delivery\"],\"properties\":{\"delivery\":{\"type\":\"string\",\"enum\":[\"each\",\"digest\",\"none\"]}}}}}},\"responses\":{\"200\":{\"description\":\"The membership\"},\"400\":{\"description\":\"delivery is not one of the three, or the list sends no digest\"},\"404\":{\"description\":\"Not a member of that managed list\"}}},"
         "\"delete\":{\"summary\":\"Leave a list\",\"responses\":{\"200\":{\"description\":\"Left\"},\"404\":{\"description\":\"Not a member of that managed list\"}}}},"
         "\"/lists/confirm\":{"
         "\"get\":{\"summary\":\"The page a confirmation link opens (no credentials)\",\"description\":\"Unauthenticated; t= is the token the confirmation carried. GET says what confirming does and offers the button; POST (t= in the form) confirms. A token that is unknown, used or lapsed answers 410 whichever it is.\",\"responses\":{\"200\":{\"description\":\"The page\"},\"410\":{\"description\":\"The link no longer works\"},\"429\":{\"description\":\"Too many requests from this address\"}}}},"
         "\"/lists/unsubscribe\":{"
         "\"get\":{\"summary\":\"A member's own page (no credentials)\",\"description\":\"Unauthenticated; t= is the member's token, from the List-Unsubscribe header of any copy the list sent them. GET shows the page; POST with action=leave leaves the list, with action=delivery and delivery=each, digest or none changes how its mail comes; and a POST whose body is RFC 8058's List-Unsubscribe=One-Click, the token in the URL, leaves with no page at all. For an account the list's membership query answers, which has no row, m= is its address and l= the list's id and t= an HMAC of the address under the list's secret: it may leave, and a row is kept saying it has left. A token that names no member - a request's, one that has been used, another list's - answers 410.\",\"responses\":{\"200\":{\"description\":\"The page, or done\"},\"400\":{\"description\":\"A POST that asks for nothing this page does\"},\"410\":{\"description\":\"The link no longer works\"},\"429\":{\"description\":\"Too many requests from this address\"}}}},"
         "\"/lists/approve\":{"
         "\"get\":{\"summary\":\"The owner's approval of a request (no credentials to open it)\",\"description\":\"t= is the token the owner's approval notice carried. GET says who asked and offers the button; POST (t= in the form) approves - only for a webmail session of the owner's own account, which the page's POST carries as its cookie. No session, or another account's, answers 403 and approves nothing. A token that is unknown, spent or lapsed answers 410.\",\"responses\":{\"200\":{\"description\":\"The page, or approved\"},\"403\":{\"description\":\"Not signed in as the list's owner\"},\"410\":{\"description\":\"The link no longer works\"},\"429\":{\"description\":\"Too many requests from this address\"}}}},"
         "\"/lists/{address}/subscribe\":{"
         "\"get\":{\"summary\":\"Ask to join a managed list (no credentials)\",\"description\":\"Unauthenticated. GET is the form; POST address= sends that address a confirmation, at most one an hour per address, twenty an hour for one requester and two hundred an hour in all. The answer is the same whether or not the address was already a member, so the page tells nobody who is on the list. A closed list says so and sends nothing.\",\"responses\":{\"200\":{\"description\":\"The page\"},\"400\":{\"description\":\"Not an e-mail address\"},\"404\":{\"description\":\"No such managed list\"},\"429\":{\"description\":\"Too many requests from this address\"}}}},"
         "\"/lists/{address}/archive\":{"
         "\"get\":{\"summary\":\"A managed list's archive page\",\"description\":\"Unauthenticated when the list's archive is public (2); for members (1) it needs t= a member's token or a webmail session of a member. Lists the months, a month's messages (month=YYYY-MM) and one message as text (id=). Senders are shown by name or as name at domain, and members are never listed.\",\"responses\":{\"200\":{\"description\":\"The page\"},\"403\":{\"description\":\"The archive is for members\"},\"404\":{\"description\":\"No such list, or no archive\"},\"429\":{\"description\":\"Too many requests from this address\"}}}}");
   }

   // ---- the administrator's routes -----------------------------------------------

   HttpResponse
   RestApiServer::HandleListArchive_(const String &address, const AnsiString &query)
   {
      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged())
         return BuildResponse_(404, "{\"error\":\"no such managed list\"}");

      const String month = String(QueryParameter_(query, "month"));
      if (!month.IsEmpty() && ListArchive::MonthStart(month) < 0)
         return BuildResponse_(400, "{\"error\":\"month must be YYYY-MM\"}");

      const std::vector<String> months = ListArchive::Months(list->GetID());
      const std::vector<ListArchiveEntry> entries = ListArchive::List(list->GetID(), month, month.IsEmpty() ? 50 : 1000);

      AnsiString body;
      body.Format("{\"address\":\"%hs\",\"archive\":%d,\"month\":\"%hs\",\"months\":[",
         JsonEscapeUtf8(list->GetAddress()).c_str(), list->GetArchive(), JsonEscapeUtf8(month).c_str());

      for (size_t i = 0; i < months.size(); i++)
      {
         if (i > 0)
            body += ",";
         body += "\"" + JsonEscapeUtf8(months[i]) + "\"";
      }

      body += "],\"messages\":[";

      for (size_t i = 0; i < entries.size(); i++)
      {
         const ListArchiveEntry &entry = entries[i];

         AnsiString item;
         item.Format("{\"id\":%I64d,\"message_id\":\"%hs\",\"subject\":\"%hs\",\"sender\":\"%hs\",\"sent\":%I64d,\"size\":%d}",
            entry.id, JsonEscapeUtf8(entry.message_id).c_str(), JsonEscapeUtf8(entry.subject).c_str(),
            JsonEscapeUtf8(entry.sender).c_str(), entry.sent, entry.size);

         if (i > 0)
            body += ",";
         body += item;
      }

      body += "]}";
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleListArchiveGet_(const String &address, __int64 id)
   {
      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged())
         return BuildResponse_(404, "{\"error\":\"no such managed list\"}");

      ListArchiveEntry entry;
      if (!ListArchive::Get(list->GetID(), id, entry))
         return BuildResponse_(404, "{\"error\":\"no such message in the archive\"}");

      String text;
      if (!ReadArchivedText(entry, text))
         return BuildResponse_(404, "{\"error\":\"the archived copy of that message is gone\"}");

      AnsiString body;
      body.Format("{\"id\":%I64d,\"message_id\":\"%hs\",\"subject\":\"%hs\",\"sender\":\"%hs\",\"sent\":%I64d,\"size\":%d,\"text\":\"%hs\"}",
         entry.id, JsonEscapeUtf8(entry.message_id).c_str(), JsonEscapeUtf8(entry.subject).c_str(),
         JsonEscapeUtf8(entry.sender).c_str(), entry.sent, entry.size, JsonEscapeUtf8(text).c_str());

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleListMemberVerb_(RouteKind kind, const String &address, const String &member)
   {
      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged())
         return BuildResponse_(404, "{\"error\":\"no such managed list\"}");

      std::shared_ptr<DistributionListRecipient> row = ListManager::MemberOf(list->GetID(), member);
      if (!row)
         return BuildResponse_(404, "{\"error\":\"not a member or a request of this list\"}");

      String reason;

      if (kind == RouteListMemberApprove)
      {
         if (row->GetConfirmed() == DistributionListRecipient::Pending)
            return BuildResponse_(409, "{\"error\":\"the request has not been confirmed by its address yet\"}");

         if (!ListManager::Approve(list, member, reason))
            return BuildResponse_(500, "{\"error\":\"" + JsonEscapeUtf8(reason) + "\"}");

         LOG_APPLICATION("RestApi: the request of '" + member + "' to join '" + list->GetAddress() + "' was approved.");
      }
      else
      {
         if (!row->GetIsMember())
            return BuildResponse_(404, "{\"error\":\"not a member of this list\"}");

         if (!ListManager::ResetBounces(list, member, reason))
            return BuildResponse_(500, "{\"error\":\"" + JsonEscapeUtf8(reason) + "\"}");

         LOG_APPLICATION("RestApi: the bounces of '" + member + "' on '" + list->GetAddress() + "' were reset.");
      }

      std::shared_ptr<DistributionListRecipient> saved = ListManager::MemberOf(list->GetID(), member);
      return BuildResponse_(200, saved ? ListMemberJson_(saved) : AnsiString("{}"));
   }

   // ---- the reader's own --------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeLists_(const Caller &caller, RouteKind kind, const String &address, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const String own = account->GetAddress();

      auto entryOf = [](std::shared_ptr<DistributionList> list, std::shared_ptr<DistributionListRecipient> row) -> AnsiString
      {
         const char *state = row->GetConfirmed() == DistributionListRecipient::Confirmed ? "member"
                           : row->GetConfirmed() == DistributionListRecipient::AwaitingOwner ? "awaiting_owner" : "pending";

         AnsiString entry;
         entry.Format("{\"address\":\"%hs\",\"delivery\":\"%hs\",\"state\":\"%hs\",\"subscribed\":%I64d,\"digest\":%hs,\"reason\":\"%hs\"",
            JsonEscapeUtf8(list->GetAddress()).c_str(), DeliveryWord(row->GetDelivery()), state, row->GetSubscribed(),
            list->GetDigest() ? "true" : "false", JsonEscapeUtf8(row->GetReason()).c_str());

         const String baseUrl = ListManager::BaseUrl();
         if (!baseUrl.IsEmpty() && list->GetArchive() != DistributionList::ArchiveNone)
            entry += ",\"archive_url\":\"" + JsonEscapeUtf8(baseUrl + _T("/lists/") + list->GetAddress() + _T("/archive")) + "\"";

         entry += "}";
         return entry;
      };

      if (kind == RouteMeLists)
      {
         String lowered = own;
         lowered.ToLower();

         // The account's own address, compared without case on every backend.
         SQLCommand command("select distributionlistrecipientid, distributionlistrecipientlistid from hm_distributionlistsrecipients where lower(distributionlistrecipientaddress) = @ADDRESS");
         command.AddParameter("@ADDRESS", lowered);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return BuildResponse_(503, "{\"error\":\"the lists could not be read\"}");

         std::vector<__int64> listIds;
         while (!recordset->IsEOF())
         {
            listIds.push_back(recordset->GetInt64Value("distributionlistrecipientlistid"));
            recordset->MoveNext();
         }

         AnsiString body = "{\"lists\":[";
         int count = 0;

         for (__int64 listId : listIds)
         {
            std::shared_ptr<DistributionList> list = ListManager::ListById(listId);
            if (!list || !list->GetManaged() || !list->GetActive())
               continue;

            std::shared_ptr<DistributionListRecipient> row = ListManager::MemberOf(listId, own);
            if (!row || row->GetConfirmed() == DistributionListRecipient::OptedOut)
               continue;

            if (count > 0)
               body += ",";
            body += entryOf(list, row);
            count++;
         }

         body += "]}";
         return BuildResponse_(200, body);
      }

      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged())
         return BuildResponse_(404, "{\"error\":\"not a member of that list\"}");

      String reason;

      if (kind == RouteMeListLeave)
      {
         if (!ListManager::Leave(list, own, reason))
            return BuildResponse_(404, "{\"error\":\"not a member of that list\"}");

         return BuildResponse_(200, "{\"left\":true}");
      }

      // RouteMeListUpdate.
      JsonValue body;
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      const JsonValue *deliveryValue = body.Get("delivery");
      int delivery = 0;
      if (!deliveryValue || !deliveryValue->IsString() || !ParseDelivery(AnsiString(deliveryValue->AsString().c_str()), delivery))
         return BuildResponse_(400, "{\"error\":\"delivery must be each, digest or none\"}");

      if (delivery == DistributionListRecipient::DeliveryDigest && !list->GetDigest())
         return BuildResponse_(400, "{\"error\":\"this list sends no digest\"}");

      if (!ListManager::SetDelivery(list, own, delivery, reason))
         return BuildResponse_(404, "{\"error\":\"not a member of that list\"}");

      std::shared_ptr<DistributionListRecipient> row = ListManager::MemberOf(list->GetID(), own);
      if (!row)
         return BuildResponse_(404, "{\"error\":\"not a member of that list\"}");

      return BuildResponse_(200, entryOf(list, row));
   }

   // ---- the pages ------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleListConfirmPage_(const AnsiString &method, const AnsiString &query, const AnsiString &body, const IPAddress &peer_address)
   {
      if (!ListManager::AllowUnauthenticated(peer_address))
         return TooMany();

      AnsiString token = method == "POST" ? QueryParameter_(body, "t") : AnsiString();
      if (token.IsEmpty())
         token = QueryParameter_(query, "t");

      token = TokenOnly(token);

      if (method == "GET")
      {
         const ListManager::Outcome preview = ListManager::InspectConfirmation(String(token));
         if (!preview.ok)
            return OutcomePage(preview, AnsiString());

         const AnsiString form =
            "<form method=\"post\" action=\"/lists/confirm\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
            "<button type=\"submit\">Confirm</button></form>";

         return OutcomePage(preview, form);
      }

      return OutcomePage(ListManager::Confirm(String(token)), AnsiString());
   }

   HttpResponse
   RestApiServer::HandleListMemberPage_(const AnsiString &method, const AnsiString &query, const AnsiString &body, const IPAddress &peer_address)
   {
      if (!ListManager::AllowUnauthenticated(peer_address))
         return TooMany();

      AnsiString token = method == "POST" ? QueryParameter_(body, "t") : AnsiString();
      if (token.IsEmpty())
         token = QueryParameter_(query, "t");

      token = TokenOnly(token);

      // An account the membership query answers: the address and the list are
      // in the link, beside the HMAC that proves this server wrote it.
      AnsiString memberText = QueryParameter_(query, "m");
      AnsiString listText = QueryParameter_(query, "l");
      if (method == "POST")
      {
         if (memberText.IsEmpty())
            memberText = QueryParameter_(body, "m");
         if (listText.IsEmpty())
            listText = QueryParameter_(body, "l");
      }

      const String queryAddress = FromUtf8(memberText);
      const __int64 queryListId = ParseId(listText);
      const bool queryMember = !memberText.IsEmpty() || !listText.IsEmpty();

      if (method == "POST")
      {
         // RFC 8058: a mail client's unsubscribe button POSTs
         // List-Unsubscribe=One-Click to the URL the header gave it, the token
         // in that URL. Form-encoded or as multipart form data; either way the
         // POST is the confirmation, and there is no page to answer with.
         const bool oneClick = QueryParameter_(body, "List-Unsubscribe") == "One-Click" ||
            (body.Find("name=\"List-Unsubscribe\"") >= 0 && body.Find("One-Click") >= 0);

         const AnsiString action = QueryParameter_(body, "action");

         if (oneClick || action == "leave")
            return OutcomePage(ListManager::LeaveByToken(String(token), queryAddress, queryListId), AnsiString());

         if (action == "delivery" && !queryMember)
         {
            int delivery = 0;
            if (!ParseDelivery(QueryParameter_(body, "delivery"), delivery))
               return Page(400, "Mailing list", "That could not be changed", "Choose each message, the daily digest or nothing for now.", AnsiString());

            return OutcomePage(ListManager::SetDeliveryByToken(String(token), delivery), AnsiString());
         }

         return Page(400, "Mailing list", "Nothing was asked", "Use the buttons on the page.", AnsiString());
      }

      const ListManager::Outcome member = ListManager::InspectMember(String(token), queryAddress, queryListId);
      if (!member.ok)
         return OutcomePage(member, AnsiString());

      // An account the query answers may leave, and choose nothing else: its
      // mail is the list's own, since it has no row of its own to hold a
      // choice.
      if (member.query_member)
      {
         AnsiString listId;
         listId.Format("%I64d", queryListId);

         const AnsiString leave =
            "<form method=\"post\" action=\"/lists/unsubscribe\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
            "<input type=\"hidden\" name=\"m\" value=\"" + Html(member.member_address) + "\">"
            "<input type=\"hidden\" name=\"l\" value=\"" + listId + "\">"
            "<input type=\"hidden\" name=\"action\" value=\"leave\">"
            "<button type=\"submit\">Leave the list</button></form>";

         return OutcomePage(member, leave);
      }

      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(member.list_address);
      const bool digestOffered = list && list->GetDigest();

      auto option = [&member](int value, const char *word, const char *label) -> AnsiString
      {
         return AnsiString("<option value=\"") + word + "\"" + (member.delivery == value ? " selected" : "") + ">" + label + "</option>";
      };

      AnsiString forms =
         "<form method=\"post\" action=\"/lists/unsubscribe\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
         "<input type=\"hidden\" name=\"action\" value=\"delivery\">"
         "<label for=\"delivery\">How the list's mail comes</label><select id=\"delivery\" name=\"delivery\">" +
         option(DistributionListRecipient::DeliveryEach, "each", "Every message") +
         (digestOffered ? option(DistributionListRecipient::DeliveryDigest, "digest", "One digest a day") : AnsiString()) +
         option(DistributionListRecipient::DeliveryNone, "none", "Nothing for now (stay a member)") +
         "</select> <button type=\"submit\" class=\"plain\">Save</button></form>"
         "<form method=\"post\" action=\"/lists/unsubscribe\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
         "<input type=\"hidden\" name=\"action\" value=\"leave\">"
         "<button type=\"submit\">Leave the list</button></form>";

      return OutcomePage(member, forms);
   }

   HttpResponse
   RestApiServer::HandleListApprovePage_(const AnsiString &method, const AnsiString &query, const AnsiString &body, const AnsiString &request, const IPAddress &peer_address)
   {
      if (!ListManager::AllowUnauthenticated(peer_address))
         return TooMany();

      AnsiString token = method == "POST" ? QueryParameter_(body, "t") : AnsiString();
      if (token.IsEmpty())
         token = QueryParameter_(query, "t");

      token = TokenOnly(token);

      if (method == "GET")
      {
         // The link from the owner's mail is opened from the mail client, and
         // a strict cookie does not come with it; the button, pressed on this
         // page, brings it, and the POST is where the owner is asked for.
         const ListManager::Outcome preview = ListManager::InspectApproval(String(token));
         if (!preview.ok)
            return OutcomePage(preview, AnsiString());

         const AnsiString form =
            "<form method=\"post\" action=\"/lists/approve\"><input type=\"hidden\" name=\"t\" value=\"" + token + "\">"
            "<button type=\"submit\">Let them in</button></form>";

         return OutcomePage(preview, form);
      }

      // Who is pressing the button: a webmail session of an account on this
      // server, or nobody.
      String signedIn;
      Caller caller;
      if (AuthenticateSession_(request, peer_address, caller) && caller.account)
         signedIn = caller.account->GetAddress();

      return OutcomePage(ListManager::ApproveByToken(String(token), signedIn), AnsiString());
   }

   HttpResponse
   RestApiServer::HandleListSubscribePage_(const AnsiString &method, const String &address, const AnsiString &body, const IPAddress &peer_address)
   {
      if (!ListManager::AllowUnauthenticated(peer_address))
         return TooMany();

      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged() || !list->GetActive())
         return Page(404, "Mailing list", "There is no such list here", AnsiString(), AnsiString());

      const AnsiString listText = Utf8(list->GetAddress());

      if (list->GetSubscribePolicy() == DistributionList::SubscribeClosed)
         return Page(200, "Mailing list", "Join " + listText, "This list does not take requests to join: its members are added by the person who runs it.", AnsiString());

      if (method == "GET")
      {
         const AnsiString form =
            "<form method=\"post\" action=\"/lists/" + HtmlEscape(listText) + "/subscribe\">"
            "<label for=\"address\">Your e-mail address</label><input id=\"address\" name=\"address\" type=\"email\" maxlength=\"255\" required>"
            "<br><button type=\"submit\">Send me a confirmation</button></form>";

         return Page(200, "Mailing list", "Join " + listText,
            list->GetSubscribePolicy() == DistributionList::SubscribeModerated
               ? "You are sent a message to confirm the address is yours; then the person who runs the list decides."
               : "You are sent a message to confirm the address is yours. Nothing happens until you do.",
            form);
      }

      const String requested = FromUtf8(QueryParameter_(body, "address"));
      if (requested.IsEmpty() || !StringParser::IsValidEmailAddress(requested))
         return Page(400, "Mailing list", "That is not an e-mail address", "Go back and check it.", AnsiString());

      // The answer is the same whether the address was a member, a request, or
      // nobody: the page must not tell a stranger who is on the list. The
      // ceilings are counted against the address asking.
      const ListManager::RequestOutcome outcome = ListManager::RequestSubscription(list, requested, String(peer_address.ToString()));
      LOG_APPLICATION(Formatter::Format("ListManager: a request for {0} to join {1} came from the page: outcome {2}.", requested, list->GetAddress(), (int) outcome));

      return Page(200, "Mailing list", "Check your mail",
         "If that address can join " + listText + ", a message has been sent to it. Follow the link in it, or reply to it, to confirm. It may take a few minutes to arrive.",
         AnsiString());
   }

   HttpResponse
   RestApiServer::HandleListArchivePage_(const String &address, const AnsiString &query, const AnsiString &request, const IPAddress &peer_address)
   {
      if (!ListManager::AllowUnauthenticated(peer_address))
         return TooMany();

      std::shared_ptr<DistributionList> list = ListManager::ListByAddress(address);
      if (!list || !list->GetManaged() || list->GetArchive() == DistributionList::ArchiveNone)
         return Page(404, "Archive", "There is no archive here", AnsiString(), AnsiString());

      const AnsiString listText = Utf8(list->GetAddress());
      const AnsiString token = TokenOnly(QueryParameter_(query, "t"));

      // A members-only archive: the member's token from a copy of the list's
      // mail, or a webmail session of a member. Neither says anything about who
      // else is a member.
      if (list->GetArchive() == DistributionList::ArchiveMembers)
      {
         bool allowed = false;

         if (!token.IsEmpty())
         {
            std::shared_ptr<DistributionListRecipient> member(new DistributionListRecipient);
            allowed = PersistentDistributionListRecipient::ReadByToken(member, String(token)) &&
                      member->GetListID() == list->GetID() && member->GetIsMember();
         }

         if (!allowed)
         {
            Caller caller;
            if (AuthenticateSession_(request, peer_address, caller) && caller.account)
            {
               std::shared_ptr<DistributionListRecipient> member = ListManager::MemberOf(list->GetID(), caller.account->GetAddress());
               allowed = member && member->GetIsMember();
            }
         }

         if (!allowed)
            return Page(403, "Archive", "This archive is for the list's members",
               "Open it from the link in a message of the list, or sign in to the webmail first.", AnsiString());
      }

      const AnsiString keep = token.IsEmpty() ? AnsiString() : AnsiString("&amp;t=") + token;
      const AnsiString title = "Archive of " + listText;

      // One message, as text.
      const AnsiString idText = QueryParameter_(query, "id");
      if (!idText.IsEmpty())
      {
         __int64 id = 0;
         for (int i = 0; i < idText.GetLength() && i < 18; i++)
         {
            const char c = idText.GetAt(i);
            if (c < '0' || c > '9')
            {
               id = 0;
               break;
            }
            id = id * 10 + (c - '0');
         }

         ListArchiveEntry entry;
         String text;
         if (id <= 0 || !ListArchive::Get(list->GetID(), id, entry) || !ReadArchivedText(entry, text))
            return Page(404, "Archive", "That message is not in the archive", AnsiString(), AnsiString());

         const AnsiString month = Utf8(ListArchive::MonthOf(entry.sent));

         AnsiString rest;
         rest += "<p>" + Html(DisplayOf(entry.sender)) + " &middot; " + Html(DateOf(entry.sent)) + "</p>";
         rest += "<pre>" + Html(text) + "</pre>";
         rest += "<p><a href=\"?month=" + month + keep + "\">" + HtmlEscape(month) + "</a></p>";

         return Page(200, title, Utf8(entry.subject.IsEmpty() ? String(_T("(no subject)")) : entry.subject), AnsiString(), rest);
      }

      const String month = String(QueryParameter_(query, "month"));
      if (!month.IsEmpty() && ListArchive::MonthStart(month) < 0)
         return Page(400, "Archive", "That is not a month", AnsiString(), AnsiString());

      AnsiString rest;

      if (month.IsEmpty())
      {
         const std::vector<String> months = ListArchive::Months(list->GetID());

         if (months.empty())
            return Page(200, title, "Archive of " + listText, "Nothing has been archived yet.", AnsiString());

         rest += "<h2>By month</h2><ul>";
         for (const String &each : months)
            rest += "<li><a href=\"?month=" + Html(each) + keep + "\">" + Html(each) + "</a></li>";
         rest += "</ul><h2>Newest</h2>";
      }
      else
         rest += "<h2>" + Html(month) + "</h2>";

      const std::vector<ListArchiveEntry> entries = ListArchive::List(list->GetID(), month, month.IsEmpty() ? 20 : 1000);

      rest += "<ul>";
      for (const ListArchiveEntry &entry : entries)
      {
         AnsiString id;
         id.Format("%I64d", entry.id);

         rest += "<li><a href=\"?id=" + id + keep + "\">" + Html(entry.subject.IsEmpty() ? String(_T("(no subject)")) : entry.subject) +
                 "</a> &middot; " + Html(DisplayOf(entry.sender)) + " &middot; " + Html(DateOf(entry.sent)) + "</li>";
      }
      rest += "</ul>";

      if (!month.IsEmpty())
         rest += "<p><a href=\"?" + (token.IsEmpty() ? AnsiString() : AnsiString("t=") + token) + "\">All months</a></p>";

      return Page(200, title, "Archive of " + listText, AnsiString(), rest);
   }
}
