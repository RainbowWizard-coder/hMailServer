// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The send API: an application's message, sent under its API key's own sender,
// and asked about afterwards. See RestApiServer.h, ApiSends.h and
// docs/SendApi.md.
//
//    POST /api/v1/send              the message; 202 {queue_id, message_id}
//    GET  /api/v1/send              the key's own sends, newest first, paged
//    GET  /api/v1/send/{queue_id}   one of them, and each recipient's state
//
// WHO. An API key and nothing else (Authorize_). The send needs the key's role
// to hold send_through_api AND the key to name a sender (SendFrom in the key
// store): an address of a domain this server hosts, or *@domain for any address
// of it. The two reads are the key's own record and answer only the key that
// sent; a send another key made is the same 404 as one nobody made.
//
// WHAT THE SERVER DOES TO THE MESSAGE. It is written and checked by the webmail
// send's own code (ResolveSendRecipients_, WriteComposedMessage_ and
// CheckComposedMessage_ in RestApiServer.cpp): every recipient through what RCPT
// TO asks of an authenticated sender, the suppression list, restricted delivery
// when the sender is an account; the domain's signature and disclaimer, the
// rules that block, the server's maximum message size. On top of that: a
// Received field naming the send API and the key's label, the envelope sender
// set to the From address, no Sent copy anywhere - an application is not a
// person and has no Sent folder - and, when the sender is an account's address,
// that account's sending quota consumed as an SMTP submission consumes it. The
// message is DKIM-signed at delivery as every message of the domain is.
//
// THE CEILINGS. The key's own, SendPerMinute and SendPerDay, through RateLimiter
// - a sliding minute keyed "api-send:<key id>", and a day window keyed
// "api-send-day:<key id>" on the per-account quota's period machinery, which is
// persisted across a restart - and then the server's: the REST listener's
// request budget, the recipient ceiling, the size, and the account's quota. A
// ceiling reached is 429 with Retry-After and the sentence. A message is counted
// only once it has passed every other check, so a refused body costs nothing.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "ApiSends.h"
#include "MessageTrace.h"
#include "JsonDocument.h"
#include "RateLimiter.h"
#include "Unicode.h"
#include "Utilities.h"
#include "FileUtilities.h"
#include "Time.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A page of the key's own sends: 50 unless asked, at most 200, and no
      // further back than the first ten thousand - a key that has sent more than
      // that asks about one by its queue id.
      const int SendListDefaultSize = 50;
      const int SendListMaximumSize = 200;
      const int SendListMaximumReach = 10000;

      // RFC 5322's line limit, for a header value an application writes.
      const int HeaderValueMaximum = 998;

      // The members the body may have. Anything else is refused by name, because
      // a key misspelt - "reply-to" for reply_to, "body" for text - is otherwise
      // a message sent without what its sender meant it to carry.
      const char *const SendMembers[] =
      {
         "from", "to", "cc", "bcc", "reply_to", "subject", "text", "html", "headers", "attachments"
      };

      AnsiString Sentence(const AnsiString &text)
      {
         return "{\"error\":\"" + text + "\"}";
      }

      // A JSON string as the server's text. False for a value that is not a
      // string, or that holds a NUL, which no header or part may carry.
      bool ReadText(const JsonValue &value, String &text)
      {
         if (!value.IsString())
            return false;

         const std::string &utf8 = value.AsString();
         if (utf8.find('\0') != std::string::npos)
            return false;

         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), text);
         return true;
      }

      bool HasLineBreak(const String &text)
      {
         return text.Find(_T("\r")) >= 0 || text.Find(_T("\n")) >= 0;
      }

      // "Name <address>" or "address", as a From or a Reply-To gives it: the
      // address, and the display name without its quotes. False when the text
      // is neither.
      bool SplitMailbox(const String &text, String &name, String &address)
      {
         String entry = text;
         entry.Trim();

         const int open = entry.Find(_T("<"));
         const int close = entry.ReverseFind(_T(">"));

         if (open >= 0 || close >= 0)
         {
            if (open < 0 || close < open || close != entry.GetLength() - 1)
               return false;

            address = entry.Mid(open + 1, close - open - 1);
            name = entry.Mid(0, open);
         }
         else
         {
            address = entry;
            name = _T("");
         }

         address.Trim();
         name.Trim();

         if (name.GetLength() >= 2 && name.Left(1) == _T("\"") && name.Right(1) == _T("\""))
            name = name.Mid(1, name.GetLength() - 2);

         return StringParser::IsValidEmailAddress(address);
      }

      // The header entry for a mailbox: the address alone, or the name quoted
      // with its quotes and backslashes escaped, as RFC 5322 quotes a phrase.
      String MailboxHeader(const String &name, const String &address)
      {
         if (name.IsEmpty())
            return address;

         String quoted = name;
         quoted.Replace(_T("\\"), _T("\\\\"));
         quoted.Replace(_T("\""), _T("\\\""));

         return _T("\"") + quoted + _T("\" <") + address + _T(">");
      }

      // The header names an application may set: its own X- fields, and the
      // four a sender of bulk or threaded mail needs - List-Unsubscribe with
      // its RFC 8058 companion List-Unsubscribe-Post, In-Reply-To and
      // References. Never one of this server's own X-hMailServer- fields, which
      // its rules and its loop detection read. Everything else - From, To, Date,
      // Message-ID, Received, a MIME field - is the server's to write.
      bool HeaderAllowed(const String &name)
      {
         if (name.CompareNoCase(_T("List-Unsubscribe")) == 0 ||
             name.CompareNoCase(_T("List-Unsubscribe-Post")) == 0 ||
             name.CompareNoCase(_T("In-Reply-To")) == 0 ||
             name.CompareNoCase(_T("References")) == 0)
            return true;

         if (name.GetLength() > 2 && name.Left(2).CompareNoCase(_T("X-")) == 0)
            return !(name.GetLength() >= 13 && name.Left(13).CompareNoCase(_T("X-hMailServer")) == 0);

         return false;
      }

      // RFC 5322's field name: printable ASCII but the colon, at most 76.
      bool HeaderNameWellFormed(const String &name)
      {
         if (name.IsEmpty() || name.GetLength() > 76)
            return false;

         for (int i = 0; i < name.GetLength(); i++)
         {
            const wchar_t c = name[i];
            if (c < 33 || c > 126 || c == ':')
               return false;
         }

         return true;
      }

      // Base64 as a body carries it: the alphabet, padding and line breaks.
      bool LooksLikeBase64(const std::string &data)
      {
         for (char c : data)
         {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '+' || c == '/' || c == '=' || c == '\r' || c == '\n';
            if (!ok)
               return false;
         }

         return true;
      }

      // The key's label as a Received comment can carry it: printable ASCII,
      // with the characters a comment gives meaning to replaced.
      String CommentSafe(const String &text)
      {
         String safe;
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text[i];
            const bool bad = c < 32 || c > 126 || c == '(' || c == ')' || c == '\\' || c == '"';
            safe += bad ? L'_' : c;
         }
         return safe;
      }

      // A query parameter that is a whole number, and nothing else.
      bool ParseWhole(const AnsiString &text, __int64 &value)
      {
         if (text.IsEmpty() || text.GetLength() > 9)
            return false;

         for (int i = 0; i < text.GetLength(); i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return false;
         }

         value = _atoi64(text.c_str());
         return true;
      }
   }

   HttpResponse
   RestApiServer::HandleSendCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      // Authorize_ has already seen to it that this is an API key whose role
      // holds send_through_api and which names a sender in a domain it may
      // reach. Everything below is about the message.
      JsonValue body;
      {
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), body, parseError) || !body.IsObject())
            return BuildResponse_(400, Sentence("the body must be a JSON object: {from?, to, cc?, bcc?, reply_to?, subject, text?, html?, headers?, attachments?}"));
      }

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         bool known = false;
         for (const char *name : SendMembers)
            known = known || member.first == name;

         if (!known)
            return BuildResponse_(400, Sentence("the body has a member this route does not take (" + JsonEscape_(AnsiString(member.first.c_str())) +
                                                "); it takes from, to, cc, bcc, reply_to, subject, text, html, headers and attachments"));
      }

      // The sender: the key's own unless the body names one, and then one the
      // key's sender allows - the address itself, or an address of the
      // pattern's domain. The display name is the application's to choose.
      String fromName;
      String from;
      {
         const JsonValue *requested = body.Get("from");
         if (requested && !requested->IsNull())
         {
            String text;
            if (!ReadText(*requested, text) || HasLineBreak(text) || !SplitMailbox(text, fromName, from))
               return BuildResponse_(400, Sentence("from must be an address, or \\\"Name <address>\\\""));
         }
         else if (caller.send_from.Left(2) == _T("*@"))
            return BuildResponse_(400, Sentence("from is required: this key's sender is " + JsonEscape_(Utf8_(caller.send_from)) + ", so the body names the address to send as"));
         else
            from = caller.send_from;

         from.MakeLower();

         if (!SendFromAllows_(caller.send_from, from))
            return BuildResponse_(400, Sentence("from must be " + JsonEscape_(Utf8_(caller.send_from)) + ", the sender this key may use, and " + JsonEscape_(Utf8_(from)) + " is not"));

         // The key was made with a sender this server hosted; a domain removed
         // since is not one it may send as now.
         if (!CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(from)))
            return BuildResponse_(403, Sentence("this key's sender is at a domain this server no longer hosts, so it cannot send"));
      }

      // The recipients: arrays of addresses (a string is taken too, split as
      // the webmail's lists are), each "address" or "Name <address>".
      std::vector<String> toAddresses, toEntries, ccAddresses, ccEntries, bccAddresses, bccEntries;
      {
         struct Field { const char *name; std::vector<String> *addresses; std::vector<String> *entries; };
         const Field fields[] =
         {
            { "to", &toAddresses, &toEntries },
            { "cc", &ccAddresses, &ccEntries },
            { "bcc", &bccAddresses, &bccEntries }
         };

         for (const Field &field : fields)
         {
            const JsonValue *value = body.Get(field.name);
            if (!value || value->IsNull())
               continue;

            std::vector<const JsonValue *> items;
            if (value->IsArray())
            {
               for (const JsonValue &item : value->Items())
                  items.push_back(&item);
            }
            else
               items.push_back(value);

            for (const JsonValue *item : items)
            {
               String text;
               if (!ReadText(*item, text) || HasLineBreak(text))
                  return BuildResponse_(400, Sentence(AnsiString(field.name) + " must be an array of addresses, each \\\"address\\\" or \\\"Name <address>\\\""));

               SplitAddressList_(text, *field.addresses, *field.entries);
            }
         }
      }

      std::vector<String> recipients;
      recipients.insert(recipients.end(), toAddresses.begin(), toAddresses.end());
      recipients.insert(recipients.end(), ccAddresses.begin(), ccAddresses.end());
      recipients.insert(recipients.end(), bccAddresses.begin(), bccAddresses.end());

      if (recipients.empty())
         return BuildResponse_(400, Sentence("at least one recipient is required, in to, cc or bcc"));

      ComposedMessage composed;
      composed.from_header = MailboxHeader(fromName, from);
      composed.to = JoinEntries_(toEntries);
      if (!ccEntries.empty())
         composed.cc = JoinEntries_(ccEntries);

      // The subject is required and may be empty; text and html are the two
      // parts, and a message has at least one of them.
      {
         const JsonValue *subject = body.Get("subject");
         if (!subject || !ReadText(*subject, composed.subject) || HasLineBreak(composed.subject))
            return BuildResponse_(400, Sentence("subject is required: a string on one line, which may be empty"));

         const JsonValue *text = body.Get("text");
         if (text && !text->IsNull() && !ReadText(*text, composed.text))
            return BuildResponse_(400, Sentence("text must be a string"));

         const JsonValue *html = body.Get("html");
         if (html && !html->IsNull() && !ReadText(*html, composed.html))
            return BuildResponse_(400, Sentence("html must be a string"));

         if (composed.text.IsEmpty() && composed.html.IsEmpty())
            return BuildResponse_(400, Sentence("a message needs text, html or both"));
      }

      // Reply-To, when the application wants answers to go somewhere else.
      {
         const JsonValue *replyTo = body.Get("reply_to");
         if (replyTo && !replyTo->IsNull())
         {
            String text, name, address;
            if (!ReadText(*replyTo, text) || HasLineBreak(text) || !SplitMailbox(text, name, address))
               return BuildResponse_(400, Sentence("reply_to must be an address, or \\\"Name <address>\\\""));

            composed.fields.push_back(std::make_pair(String(_T("Reply-To")), MailboxHeader(name, address)));
         }
      }

      // The header fields an application may set, and no other: a field this
      // route does not take is refused by name rather than dropped, so that
      // nobody believes a message carried what it did not.
      {
         const JsonValue *headers = body.Get("headers");
         if (headers && !headers->IsNull())
         {
            if (!headers->IsObject())
               return BuildResponse_(400, Sentence("headers must be an object of field names and their values"));

            std::vector<String> seen;
            for (const std::pair<std::string, JsonValue> &member : headers->Members())
            {
               String name;
               Unicode::MultiByteToWide(AnsiString(member.first.c_str()), name);

               if (!HeaderNameWellFormed(name))
                  return BuildResponse_(400, Sentence("the header " + JsonEscape_(AnsiString(member.first.c_str())) + " is not a field name"));

               if (!HeaderAllowed(name))
                  return BuildResponse_(400, Sentence("the header " + JsonEscape_(AnsiString(member.first.c_str())) + " is not one an application may set: the send api takes X- fields (not X-hMailServer-), List-Unsubscribe, List-Unsubscribe-Post, In-Reply-To and References; the server writes the rest"));

               for (const String &earlier : seen)
               {
                  if (earlier.CompareNoCase(name) == 0)
                     return BuildResponse_(400, Sentence("the header " + JsonEscape_(AnsiString(member.first.c_str())) + " is named twice"));
               }
               seen.push_back(name);

               String value;
               if (!ReadText(member.second, value) || HasLineBreak(value) || value.GetLength() > HeaderValueMaximum)
                  return BuildResponse_(400, Sentence("the header " + JsonEscape_(AnsiString(member.first.c_str())) + " needs a string value on one line, at most 998 characters"));

               composed.fields.push_back(std::make_pair(name, value));
            }
         }
      }

      // The attachments, checked here and handed to the webmail's writer in the
      // one shape it reads - an "attachments" array and nothing else - so that
      // nothing else in the body can be mistaken for one.
      {
         const JsonValue *attachments = body.Get("attachments");
         if (attachments && !attachments->IsNull())
         {
            if (!attachments->IsArray())
               return BuildResponse_(400, Sentence("attachments must be an array of {filename, content_type, data}"));

            AnsiString canonical = "{\"attachments\":[";
            bool first = true;

            for (const JsonValue &attachment : attachments->Items())
            {
               const JsonValue *filename = attachment.Get("filename");
               const JsonValue *contentType = attachment.Get("content_type");
               const JsonValue *data = attachment.Get("data");

               String name, type;
               if (!attachment.IsObject() || !filename || !ReadText(*filename, name) || name.IsEmpty() || HasLineBreak(name) ||
                   !data || !data->IsString() || !LooksLikeBase64(data->AsString()) ||
                   (contentType && !contentType->IsNull() && (!ReadText(*contentType, type) || HasLineBreak(type))))
                  return BuildResponse_(400, Sentence("each attachment is {filename, content_type, data}: a file name, a MIME type (optional) and the bytes in base64"));

               if (!first)
                  canonical += ",";
               first = false;

               // The base64 without its line breaks, which a JSON string could
               // only carry escaped: what is written here is read again as JSON.
               std::string base64;
               base64.reserve(data->AsString().size());
               for (char c : data->AsString())
               {
                  if (c != '\r' && c != '\n')
                     base64 += c;
               }

               canonical += "{\"filename\":\"" + JsonEscape_(Utf8_(name)) + "\",\"content_type\":\"" + JsonEscape_(Utf8_(type)) +
                            "\",\"data\":\"" + AnsiString(base64.c_str()) + "\"}";
            }

            canonical += "]}";
            composed.attachments_body = canonical;
            composed.attachment_name_key = "filename";
            composed.attachment_type_key = "content_type";
         }
      }

      // The trace field of this hop: from where, which key, through what.
      {
         String received;
         received.Format(_T("from [%s] (hMailServer send API, key \"%s\" %s) by %s with %s; %s"),
            String(caller.peer.ToString()).c_str(),
            CommentSafe(caller.key_label).c_str(),
            caller.key_id.c_str(),
            Utilities::ComputerName().c_str(),
            use_tls_ ? _T("HTTPS") : _T("HTTP"),
            Time::GetCurrentMimeDate().c_str());
         composed.received = received;
      }

      // The account the sender is, when it is one: its restricted delivery is
      // judged, its quota consumed, and its name fills the domain signature.
      std::shared_ptr<const Account> fromAccount = CacheContainer::Instance()->GetAccount(from);

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetFromAddress(from);

      {
         HttpResponse refusal;
         if (!ResolveSendRecipients_(fromAccount, from, recipients, message, refusal))
            return refusal;
      }

      const String fileName = PersistentMessage::GetFileName(message);

      String messageId;
      {
         AnsiString problem;
         const int refused = WriteComposedMessage_(message, fileName, composed, messageId, problem);
         if (refused != 0)
            return BuildResponse_(refused, problem);
      }

      {
         HttpResponse refusal;
         if (!CheckComposedMessage_(message, fileName, fromAccount, refusal))
            return refusal;
      }

      // The ceilings, last, so that only a message that would otherwise go is
      // counted. The key's minute, then its day, then the account's own quota
      // when the sender is an account - each refusal a 429 with when to try
      // again and the sentence, and the file removed.
      {
         AnsiString limitSentence;
         int retryAfter = 0;

         if (caller.send_per_minute > 0 &&
             !RateLimiter::Instance()->TryConsume(_T("api-send:") + caller.key_id, caller.send_per_minute))
         {
            limitSentence.Format("this key has sent its %d messages for this minute (send_per_minute); try again when the minute has passed", caller.send_per_minute);
            retryAfter = 60;
         }
         else if (caller.send_per_day > 0)
         {
            AccountSendingLimits day;
            day.max_messages = caller.send_per_day;
            day.period_hours = 24;

            if (RateLimiter::Instance()->TryConsumeAccountMessage(_T("api-send-day:") + caller.key_id, day) != AccountQuotaResult::QuotaAllowed)
            {
               limitSentence.Format("this key has sent its %d messages for the last 24 hours (send_per_day); try again later", caller.send_per_day);
               retryAfter = 3600;
            }
         }

         if (limitSentence.IsEmpty() && fromAccount)
         {
            // The account's own sending quota, as an SMTP submission from it
            // consumes it: one message, then its recipients.
            const AccountSendingLimits limits = RateLimiter::Instance()->GetAccountLimits(from);
            if (limits.IsEnabled())
            {
               AccountQuotaResult result = RateLimiter::Instance()->TryConsumeAccountMessage(from, limits);
               if (result == AccountQuotaResult::QuotaAllowed)
                  result = RateLimiter::Instance()->TryConsumeAccountRecipients(from, limits, (int) recipients.size());

               if (result == AccountQuotaResult::QuotaMessageLimitReached)
                  limitSentence = "Sending limit for this account has been reached. Please try again later.";
               else if (result == AccountQuotaResult::QuotaRecipientLimitReached)
                  limitSentence = "Recipient sending limit for this account has been reached. Please try again later.";

               retryAfter = 3600;
            }
         }

         if (!limitSentence.IsEmpty())
         {
            FileUtilities::DeleteFile(fileName);

            String logged;
            logged.Format(_T("REST API: send API key '%s' (%s) was refused a message from %s: %s"),
               caller.key_label.c_str(), caller.key_id.c_str(), from.c_str(), String(limitSentence).c_str());
            LOG_APPLICATION(logged);

            AnsiString header;
            header.Format("Retry-After: %d\r\n", retryAfter);
            return BuildResponse_(429, Sentence(limitSentence), header);
         }
      }

      message->SetState(Message::Delivering);

      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(500, Sentence("the message could not be queued"));
      }

      // The key's record of it, which is what lets the key ask afterwards. The
      // message is queued either way; a record that could not be written is
      // said in the answer and in the error log, since the key then cannot ask.
      const bool recorded = ApiSends::Record(caller.key_id, message->GetID(), messageId, from, recipients);
      if (!recorded)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7180, "RestApiServer::HandleSendCreate_",
            Formatter::Format(_T("Message {0}, sent through the send API by the key '{1}' ({2}), was queued but could not be recorded in hm_apisends, so the key cannot ask what became of it."),
               message->GetID(), caller.key_label, caller.key_id));
      }

      Application::Instance()->SubmitPendingEmail();

      {
         String logged;
         logged.Format(_T("REST API: send API key '%s' (%s) queued message %I64d from %s to %d recipient(s)."),
            caller.key_label.c_str(), caller.key_id.c_str(), message->GetID(), from.c_str(), (int) recipients.size());
         LOG_APPLICATION(logged);
      }

      AnsiString answer;
      answer.Format("{\"queue_id\":%I64d,\"message_id\":\"%hs\",\"recipients\":%d,\"recorded\":%hs}",
         message->GetID(), JsonEscape_(Utf8_(messageId)).c_str(), message->GetRecipients()->GetCount(),
         recorded ? "true" : "false");

      return BuildResponse_(202, answer);
   }

   namespace
   {
      AnsiString RecipientsJson(const std::vector<String> &recipients)
      {
         AnsiString json = "[";
         for (size_t i = 0; i < recipients.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += "\"" + RestApiServer::JsonEscapeUtf8(recipients[i]) + "\"";
         }
         return json + "]";
      }
   }

   HttpResponse
   RestApiServer::HandleSendList_(const Caller &caller, const AnsiString &query)
   {
      // ?page= from 1 and ?size= up to 200; anything else refused rather than
      // read as the default, since a client that wrote one meant something.
      int page = 1;
      int size = SendListDefaultSize;
      {
         const AnsiString pageText = QueryParameter_(query, "page");
         const AnsiString sizeText = QueryParameter_(query, "size");

         __int64 value = 0;
         if (!pageText.IsEmpty())
         {
            if (!ParseWhole(pageText, value) || value < 1 || value > SendListMaximumReach)
               return BuildResponse_(400, Sentence("page is a whole number from 1"));
            page = (int) value;
         }

         if (!sizeText.IsEmpty())
         {
            if (!ParseWhole(sizeText, value) || value < 1 || value > SendListMaximumSize)
               return BuildResponse_(400, Sentence("size is a whole number from 1 to 200"));
            size = (int) value;
         }

         if ((__int64) (page - 1) * size + size > SendListMaximumReach)
            return BuildResponse_(400, Sentence("the listing reaches back ten thousand sends; ask about an older one by its queue id"));
      }

      std::vector<ApiSendRecord> records;
      int total = 0;
      if (!ApiSends::List(caller.key_id, (page - 1) * size, size, records, total))
         return BuildResponse_(500, Sentence("the sends could not be read"));

      bool traceEnabled = MessageTrace::GetEnabled();

      AnsiString items;
      for (size_t i = 0; i < records.size(); i++)
      {
         const ApiSendRecord &record = records[i];

         std::vector<ApiSendRecipientState> states;
         const String state = ApiSends::Describe(record, states, traceEnabled);

         AnsiString item;
         item.Format("{\"queue_id\":%I64d,\"message_id\":\"%hs\",\"sender\":\"%hs\",\"recipients\":%hs,\"created\":\"%hs\",\"state\":\"%hs\"}",
            record.queue_id,
            JsonEscape_(Utf8_(record.message_id)).c_str(),
            JsonEscape_(Utf8_(record.sender)).c_str(),
            RecipientsJson(record.recipients).c_str(),
            JsonEscape_(Utf8_(record.created)).c_str(),
            JsonEscape_(Utf8_(state)).c_str());

         if (i > 0)
            items += ",";
         items += item;
      }

      AnsiString answer;
      answer.Format("{\"page\":%d,\"size\":%d,\"total\":%d,\"trace\":%hs,\"sends\":[%hs]}",
         page, size, total, traceEnabled ? "true" : "false", items.c_str());

      return BuildResponse_(200, answer);
   }

   HttpResponse
   RestApiServer::HandleSendGet_(const Caller &caller, const AnsiString &identifier)
   {
      // One answer for "no such send" whatever the reason - not a number, a
      // number nobody sent, a number another key sent - so that a key cannot
      // learn from this route what other keys have sent.
      const HttpResponse notFound = BuildResponse_(404, Sentence("this key has sent no message with that queue id"));

      __int64 queueId = 0;
      for (int i = 0; i < identifier.GetLength(); i++)
      {
         if (identifier[i] < '0' || identifier[i] > '9')
            return notFound;
      }

      if (identifier.IsEmpty() || identifier.GetLength() > 18)
         return notFound;

      queueId = _atoi64(identifier.c_str());

      ApiSendRecord record;
      if (!ApiSends::Find(caller.key_id, queueId, record))
         return notFound;

      bool traceEnabled = false;
      std::vector<ApiSendRecipientState> states;
      const String state = ApiSends::Describe(record, states, traceEnabled);

      AnsiString items;
      for (size_t i = 0; i < states.size(); i++)
      {
         const ApiSendRecipientState &recipient = states[i];

         AnsiString item;
         item.Format("{\"address\":\"%hs\",\"state\":\"%hs\",\"when\":\"%hs\",\"status\":\"%hs\",\"code\":%d,\"reason\":\"%hs\"}",
            JsonEscape_(Utf8_(recipient.address)).c_str(),
            JsonEscape_(Utf8_(recipient.state)).c_str(),
            JsonEscape_(Utf8_(recipient.when)).c_str(),
            JsonEscape_(Utf8_(recipient.status)).c_str(),
            recipient.code,
            JsonEscape_(Utf8_(recipient.reason)).c_str());

         if (i > 0)
            items += ",";
         items += item;
      }

      AnsiString answer;
      answer.Format("{\"queue_id\":%I64d,\"message_id\":\"%hs\",\"sender\":\"%hs\",\"recipients\":%hs,\"created\":\"%hs\","
                    "\"state\":\"%hs\",\"trace\":%hs,\"recipients_state\":[%hs]}",
         record.queue_id,
         JsonEscape_(Utf8_(record.message_id)).c_str(),
         JsonEscape_(Utf8_(record.sender)).c_str(),
         RecipientsJson(record.recipients).c_str(),
         JsonEscape_(Utf8_(record.created)).c_str(),
         JsonEscape_(Utf8_(state)).c_str(),
         traceEnabled ? "true" : "false",
         items.c_str());

      return BuildResponse_(200, answer);
   }

   AnsiString
   RestApiServer::OpenApiSendPaths_()
   {
      static const char *paths =
         ",\"/api/v1/send\":{"
         "\"post\":{\"summary\":\"Send a message as an application, under this API key's own sender\","
         "\"description\":\"The send API (docs/SendApi.md). An API key only - not the administrator password and not an account, whose own send is POST /api/v1/me/messages - whose role holds send_through_api and which names a sender (send_from): an address of a domain this server hosts, or *@domain for any address of it; a key with the permission and no sender is refused (403). Body: from (optional; the key's own sender when it is an address, required when it is *@domain; \\\"address\\\" or \\\"Name <address>\\\", and the address must be the key's sender or of its domain - the display name is free), to, cc and bcc (arrays of \\\"address\\\" or \\\"Name <address>\\\"; at least one recipient), reply_to, subject (required, may be empty), text and html (at least one), headers (an object: X- fields other than X-hMailServer-, List-Unsubscribe, List-Unsubscribe-Post, In-Reply-To and References; any other field is 400) and attachments ([{filename, content_type, data}], data in base64, at most 20 and twelve megabytes together). A member the route does not take is 400. The message is written and checked by the webmail send's own code: every recipient through what RCPT TO asks of an authenticated sender, with the server's recipient ceiling (400 with the server's sentence), the suppression list and, when the sender is an account's address, its restricted delivery; the domain's signature and disclaimer, the rules that block (403, policy_rejected) and the server's maximum message size (413). The envelope sender is the From address; a Received field names the send API and the key's label; nothing is kept in any Sent folder; the domain's DKIM key signs it at delivery. Ceilings: the key's send_per_minute and send_per_day, and when the sender is an account's address that account's sending quota - each 429 with Retry-After and the sentence; a message is counted only once every other check has passed. 202 {queue_id, message_id, recipients, recorded}: recorded false when the message is queued and the key's record of it could not be written, so GET /api/v1/send/{queue_id} will not find it.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"to\",\"subject\"],\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"cc\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"bcc\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"reply_to\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"html\":{\"type\":\"string\"},\"headers\":{\"type\":\"object\"},\"attachments\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\"},\"content_type\":{\"type\":\"string\"},\"data\":{\"type\":\"string\"}}}}}}}}},"
         "\"responses\":{\"202\":{\"description\":\"{queue_id, message_id, recipients, recorded}\"},\"400\":{\"description\":\"The body, a recipient, a header or an attachment is refused, with the sentence\"},\"401\":{\"description\":\"Not an API key that authenticates\"},\"403\":{\"description\":\"Not an API key, a key whose role does not hold send_through_api, a key that names no sender, or a rule that blocks the message\"},\"413\":{\"description\":\"Larger than the server's maximum message size\"},\"429\":{\"description\":\"A ceiling reached: Retry-After and the sentence\"}}},"
         "\"get\":{\"summary\":\"This API key's own sends, newest first\","
         "\"description\":\"The sends this key made through POST /api/v1/send and nobody else's - an administrator's key that sent nothing reads an empty list; the server-wide view of the same deliveries is the message trace. Query parameters page (from 1) and size (1 to 200, default 50); the listing reaches back ten thousand sends. Each send: queue_id, message_id, sender, recipients (as the body named them), created and state - the send's state as GET /api/v1/send/{queue_id} answers it. trace says whether the message trace records events now (MessageTraceEnabled): with it off, a send reads queued or deferred while it is in the queue and unknown once it has left.\","
         "\"parameters\":[{\"name\":\"page\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"size\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}],"
         "\"responses\":{\"200\":{\"description\":\"{page, size, total, trace, sends: [{queue_id, message_id, sender, recipients, created, state}]}\"},\"400\":{\"description\":\"page or size is not a whole number in range\"},\"403\":{\"description\":\"Not an API key\"}}}},"
         "\"/api/v1/send/{queue_id}\":{\"get\":{\"summary\":\"What became of one of this key's sends\","
         "\"description\":\"For a send this key made; any other id - not a number, one nobody sent, one another key sent - is the same 404. {queue_id, message_id, sender, recipients, created, state, trace, recipients_state: [{address, state, when, status, code, reason}]}. A recipient's state comes from the message trace's events for the queue id and, while it waits, from the queue: queued (in the queue, not tried yet), deferred (tried and waiting; status and reason are the last attempt's, from the remote's own reply where it gave one), delivered (with when), bounced (failed for good: the remote refused it, or the retries ran out; status is the enhanced status and reason the remote's reply), refused (the suppression list stopped it) and unknown (the message has left the queue and nothing recorded what became of it at that address - the trace is off, its rows were removed, or the address resolved to others, an alias or a list, which are listed under their own addresses). The send's state is deferred or queued while any recipient waits, and otherwise bounced, refused or delivered in that order of precedence, or unknown. A forward or a list's re-send is a new queue id and is not followed.\","
         "\"responses\":{\"200\":{\"description\":\"The send and each recipient's state\"},\"403\":{\"description\":\"Not an API key\"},\"404\":{\"description\":\"This key has sent no message with that queue id\"}}}}";

      return AnsiString(paths);
   }
}
