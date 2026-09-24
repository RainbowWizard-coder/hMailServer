// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A password-protected message to anyone (ProtectedMessages.h). The sender's
// side, the account's own credentials and nothing else:
//
//    GET    /api/v1/me/protected                   every one sent, newest first
//    POST   /api/v1/me/protected                   the record: recipient, hint, size
//    PUT    /api/v1/me/protected/{id}/content      the ciphertext, in chunks
//    DELETE /api/v1/me/protected/{id}              withdrawn now
//
// and the reader's side, which takes no credential because the reader has
// none - the token in the link is the whole capability:
//
//    GET    /portal/protected/{token}              the reading page
//    GET    /portal-protected.js                   its script
//    GET    /api/v1/protected/{token}              the ciphertext, the hint, the dates
//    POST   /api/v1/protected/{token}/reply        a reply, encrypted under the same password
//
// THE PLAINTEXT NEVER ARRIVES HERE. The page encrypts the text and the
// attachments to the password before the first byte is sent, and what this
// server files is refused unless it has the shape of an OpenPGP message to
// a passphrase (ProtectedMessages::IsSymmetricMessage: session keys, then
// one integrity-protected body, nothing else) - so a client that sent the
// text in the clear, or a literal packet beside the ciphertext, would find
// it discarded rather than stored. What the server cannot tell is
// ciphertext from noise, and it does not claim to. The
// notice that goes out by SMTP is written HERE, from the sender to the
// recipient, and carries a sentence, the hint and the link: this server can
// say what crossed the internet because it wrote it.
//
// The record comes first and the bytes follow, the way a file sent as a link
// does (RestApiFiles.cpp): a 25 MB ciphertext is larger than one request may
// be, and a chunk lost on the way is sent again rather than the whole. The
// notice goes when the last chunk lands and the blob is what it says it is.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "HttpCompression.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "ProtectedMessages.h"
#include "ConfidentialMessages.h"
#include "CompromiseDetector.h"
#include "MessageUtilities.h"
#include "FileUtilities.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../Cache/CacheContainer.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Util/Parsing/StringParser.h"
#include "../../SMTP/RecipientParser.h"
#include "RestrictedDelivery.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   extern const char *PortalHeaders;
   extern const char *const PortalProtectedHtmlPieces[];
   extern const char *const PortalProtectedScriptPieces[];

   namespace
   {
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

      AnsiString Escape(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      AnsiString Refused(int status, const AnsiString &sentence, const char *code = nullptr)
      {
         AnsiString json = "{\"error\":\"" + Escape(sentence) + "\"";
         if (code)
            json += AnsiString(",\"code\":\"") + code + "\"";
         return json + "}";
      }

      AnsiString RowJson(const ProtectedMessages::Entry &row, __int64 now)
      {
         AnsiString json;
         json.Format("{\"id\":%I64d,\"token\":\"%hs\",\"recipient\":\"%hs\",\"hint\":\"%hs\",\"size\":%I64d,\"stored\":%I64d,\"complete\":%hs,"
                     "\"created\":%I64d,\"expires\":%I64d,\"expired\":%hs,\"opened\":%I64d,\"revoked\":%hs,\"reply_to\":\"%hs\",\"link\":\"%hs\"}",
                     row.id, row.token.c_str(), Escape(Utf8(row.recipient)).c_str(), Escape(Utf8(row.hint)).c_str(), row.size, row.stored,
                     row.complete ? "true" : "false", row.created, row.expires, row.expires <= now ? "true" : "false", row.opened,
                     row.revoked ? "true" : "false", Escape(row.reply_to).c_str(), Escape(ProtectedMessages::LinkFor(row.token)).c_str());
         return json;
      }

      // The two refusals every sender's route makes first, in one place: the
      // switch, and the public address the links need. The body of the
      // refusal, or empty when the feature is offered; the route answers it
      // through BuildResponse_ so that it carries the API's own headers.
      AnsiString NotOffered(std::shared_ptr<const Account> account)
      {
         if (!EffectiveSetting::ResolveBool(_T("openpgp_portal_enabled"), std::shared_ptr<const Domain>(), account))
            return Refused(403, "OpenPGP is switched off in the webmail on this server, and a password-protected message is made with it", "openpgp_unavailable");
         if (ProtectedMessages::BaseUrl().IsEmpty())
            return Refused(403, "a password-protected message is not offered on this server: no public address is set for its links (confidential_base_url)", "protected_unavailable");
         return AnsiString();
      }
   }

   // ---- the sender's side ---------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeProtected_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The list carries every live link and every hint, which is more than
      // a program that reads the mailbox needs and more than a stolen app
      // password should be able to walk off with.
      HttpResponse failure;
      if (RefuseAppPasswordCaller_(caller, "listing password-protected messages", failure))
         return failure;

      std::vector<ProtectedMessages::Entry> entries;
      if (!ProtectedMessages::List(account->GetID(), entries))
         return BuildResponse_(500, "{\"error\":\"the password-protected messages could not be read\"}");

      const __int64 now = (__int64) time(nullptr);
      const __int64 held = ProtectedMessages::HeldBytes(account->GetID());
      const bool available = EffectiveSetting::ResolveBool(_T("openpgp_portal_enabled"), std::shared_ptr<const Domain>(), account) &&
                             !ProtectedMessages::BaseUrl().IsEmpty();
      AnsiString body;
      body.Format("{\"available\":%hs,\"days\":%d,\"limits\":{\"message_bytes\":%I64d,\"account_bytes\":%I64d,\"live\":%d,\"reply_bytes\":%I64d},\"used_bytes\":%I64d,\"items\":[",
                  available ? "true" : "false", ProtectedMessages::DefaultDays(), ProtectedMessages::MessageMaximum, ProtectedMessages::AccountMaximum,
                  ProtectedMessages::LiveMaximum, ProtectedMessages::ReplyMaximum, held < 0 ? 0 : held);
      for (size_t i = 0; i < entries.size(); i++)
         body += (i ? "," : "") + RowJson(entries[i], now);
      return BuildResponse_(200, body + "]}");
   }

   // The record: who it is for, what the sender said about the password, and
   // how many bytes of ciphertext are about to follow. Everything about it is
   // refused here, before a row exists; the bytes arrive at the content route.
   HttpResponse
   RestApiServer::HandleMeProtectedCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // An app password is a credential handed to a mail program, which reads
      // the mailbox and sends mail through it: it does not make this server
      // write a link to itself into a message and send it in the person's name.
      HttpResponse failure;
      if (RefuseAppPasswordCaller_(caller, "sending a password-protected message", failure))
         return failure;
      const AnsiString notOffered = NotOffered(account);
      if (!notOffered.IsEmpty())
         return BuildResponse_(403, notOffered);

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object: recipient, size, and hint or expires_in when wanted\"}");

      String recipient = Wide(AnsiString(document.GetString("recipient").c_str()));
      recipient.Trim();
      recipient.ToLower();
      if (recipient.IsEmpty() || !StringParser::IsValidEmailAddress(recipient) || recipient.GetLength() > 255)
         return BuildResponse_(400, "{\"error\":\"recipient: the e-mail address the message is for\"}");
      for (int i = 0; i < recipient.GetLength(); i++)
      {
         if (recipient.GetAt(i) < 0x20 || recipient.GetAt(i) == 0x7f)
            return BuildResponse_(400, "{\"error\":\"recipient: not an e-mail address\"}");
      }

      // Restricted delivery (RestrictedDelivery.h): the notice that carries
      // the link is the account sending, and a restricted account sends to
      // named domains only. Refused here, before the first chunk is uploaded,
      // in the sentence RCPT TO uses; the notice checks again when it goes.
      if (RestrictedDelivery::Check(account, recipient, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
         return BuildResponse_(400, "{\"error\":\"" + Escape(AnsiString(RestrictedDelivery::OutboundRefusal(recipient))) + "\"}");

      String hint = Wide(AnsiString(document.GetString("hint").c_str()));
      hint.Trim();
      if (hint.GetLength() > ProtectedMessages::HintMaximum)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"hint: at most %d characters\"}", ProtectedMessages::HintMaximum);
         return BuildResponse_(400, problem);
      }

      const JsonValue *sizeValue = document.Get("size");
      if (!sizeValue || !sizeValue->IsNumber() || sizeValue->AsNumber() < 1)
         return BuildResponse_(400, "{\"error\":\"size is the ciphertext's length in bytes\"}");
      const __int64 size = (__int64) sizeValue->AsNumber();
      if (size > ProtectedMessages::MessageMaximum)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"a password-protected message is at most %I64d MB of ciphertext here, text and attachments together\"}", ProtectedMessages::MessageMaximum / (1024 * 1024));
         return BuildResponse_(413, problem);
      }

      const __int64 longest = (__int64) ProtectedMessages::DefaultDays() * 24 * 60 * 60;
      __int64 expiresIn = longest;
      const int expiresState = JsonWholeNumber_(requestBody, "expires_in", expiresIn);
      if (expiresState < 0 || (expiresState > 0 && (expiresIn < 1 || expiresIn > longest)))
      {
         AnsiString problem;
         problem.Format("{\"error\":\"expires_in is a whole number of seconds from now, at most %d days (%I64d), which is what this server keeps a password-protected message for\"}", ProtectedMessages::DefaultDays(), longest);
         return BuildResponse_(400, problem);
      }
      if (expiresState == 0)
         expiresIn = longest;

      // The ceilings, counted from the declared size so that a thousand
      // declared uploads cannot outrun them.
      const int live = ProtectedMessages::CountLive(account->GetID());
      if (live < 0)
         return BuildResponse_(500, "{\"error\":\"the password-protected messages could not be counted\"}");
      if (live >= ProtectedMessages::LiveMaximum)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"this account has %d password-protected messages outstanding, which is the most it may; withdraw some, or wait for them to expire\",\"code\":\"protected_ceiling\"}", ProtectedMessages::LiveMaximum);
         return BuildResponse_(400, problem);
      }
      const __int64 held = ProtectedMessages::HeldBytes(account->GetID());
      if (held < 0)
         return BuildResponse_(500, "{\"error\":\"the password-protected messages could not be measured\"}");
      if (held + size > ProtectedMessages::AccountMaximum)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"the account's %I64d MB for password-protected messages outstanding is used up; withdraw some, or wait for them to expire\",\"code\":\"protected_ceiling\"}", ProtectedMessages::AccountMaximum / (1024 * 1024));
         return BuildResponse_(413, problem);
      }

      ProtectedMessages::Entry made;
      if (!ProtectedMessages::Create(account->GetID(), recipient, hint, size, (__int64) time(nullptr) + expiresIn, AnsiString(), made))
         return BuildResponse_(500, "{\"error\":\"the password-protected message could not be recorded\"}");

      return BuildResponse_(201, RowJson(made, (__int64) time(nullptr)));
   }

   // One chunk of the ciphertext, raw, appended at the offset the record has
   // reached - so a chunk lost on the way is sent again and a chunk sent
   // twice is refused. When the declared size is reached the blob is read
   // back and has to be an OpenPGP message to a passphrase; then, and only
   // then, the notice goes to the recipient and a copy to the sender's Sent.
   HttpResponse
   RestApiServer::HandleMeProtectedContent_(const Caller &caller, __int64 id, const AnsiString &query, const AnsiString &body)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");
      HttpResponse failure;
      if (RefuseAppPasswordCaller_(caller, "sending a password-protected message", failure))
         return failure;
      const AnsiString notOffered = NotOffered(account);
      if (!notOffered.IsEmpty())
         return BuildResponse_(403, notOffered);
      if (body.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"the body is the chunk's bytes\"}");

      const AnsiString offsetText = QueryParameter_(query, "offset");
      const __int64 offset = offsetText.IsEmpty() ? 0 : (__int64) strtoll(offsetText.c_str(), nullptr, 10);

      ProtectedMessages::Entry row;
      if (!ProtectedMessages::FindById(account->GetID(), id, row))
         return BuildResponse_(404, "{\"error\":\"no such password-protected message of this account\"}");
      if (row.complete)
         return BuildResponse_(409, "{\"error\":\"the message is complete\"}");
      if (row.revoked)
         return BuildResponse_(410, "{\"error\":\"the message was withdrawn\"}");
      if (offset != row.stored)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"the next chunk starts at %I64d\",\"stored\":%I64d}", row.stored, row.stored);
         return BuildResponse_(409, problem);
      }
      if (row.stored + body.GetLength() > row.size)
         return BuildResponse_(413, "{\"error\":\"more bytes than the size that was declared\"}");

      ProtectedMessages::Entry updated;
      const ProtectedMessages::AppendResult appended = ProtectedMessages::Append(row, body, updated);
      if (appended == ProtectedMessages::Behind)
      {
         // Two chunks for one offset, the second finding the first's bytes on
         // disk: the same answer as an offset the row disagreed with, so the
         // page sends the chunk that is really next.
         AnsiString problem;
         problem.Format("{\"error\":\"the next chunk starts at %I64d\",\"stored\":%I64d}", updated.stored, updated.stored);
         return BuildResponse_(409, problem);
      }
      if (appended != ProtectedMessages::Appended)
         return BuildResponse_(500, "{\"error\":\"the ciphertext could not be written\"}");

      const __int64 now = (__int64) time(nullptr);
      if (updated.stored < updated.size)
         return BuildResponse_(200, RowJson(updated, now));

      // The last chunk. What was stored has to be ciphertext to a passphrase,
      // or it is discarded and the sender is told why.
      ProtectedMessages::Entry completed;
      AnsiString problem;
      if (!ProtectedMessages::Complete(updated, completed, problem))
         return BuildResponse_(400, "{\"error\":\"" + Escape(problem) + "\",\"discarded\":true}");

      // The notice, from the sender's own address as the account's default
      // identity writes it, to the recipient, with the link. The plaintext is
      // not in it because this server never had it.
      String from;
      String fromHeader;
      AnsiString refusal;
      if (ResolveSender_(account, String(), from, fromHeader, refusal) != 0)
      {
         ProtectedMessages::Discard(completed);
         return BuildResponse_(500, "{\"error\":\"the sender's address could not be resolved\"}");
      }

      // Restricted delivery, again: the row was refused at its start when the
      // recipient was outside, but a list set since then is judged when the
      // notice actually goes, and a refused notice discards the ciphertext
      // rather than leaving a link nobody may be sent.
      if (RestrictedDelivery::Check(account, completed.recipient, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
      {
         ProtectedMessages::Discard(completed);
         return BuildResponse_(400, "{\"error\":\"" + Escape(AnsiString(RestrictedDelivery::OutboundRefusal(completed.recipient))) + "\",\"discarded\":true}");
      }

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetFromAddress(from);
      const String fileName = MessageStore::WorkingFile(message);
      const AnsiString link = ProtectedMessages::LinkFor(completed.token);
      if (!ProtectedMessages::WriteNotice(message, fileName, fromHeader, completed.recipient, ConfidentialMessages::DisplayName(fromHeader), completed, link))
      {
         ProtectedMessages::Discard(completed);
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(500, "{\"error\":\"the notice could not be written\"}");
      }
      message->SetSize((int) MessageStore::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(completed.recipient, message->GetRecipients(), recipientOk);
      if (message->GetRecipients()->GetCount() == 0)
      {
         ProtectedMessages::Discard(completed);
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(400, "{\"error\":\"the recipient address cannot be delivered to\"}");
      }

      // The shape of a takeover (CompromiseDetector.h), judged before the row
      // exists, exactly as the send route judges an ordinary message: a notice
      // with a link to this server is what a taken-over account would send by
      // the thousand.
      std::vector<String> recipients;
      recipients.push_back(completed.recipient);
      CompromiseDetector::Assessment shape;
      CompromiseDetector::Assess(account, caller.peer.ToString(), recipients, CompromiseDetector::SourceWebmail, shape);
      const bool held = shape.hold;
      message->SetState(held ? Message::HeldForReview : Message::Delivering);

      if (!PersistentMessage::SaveObject(message))
      {
         ProtectedMessages::Discard(completed);
         FileUtilities::DeleteFile(fileName);
         return BuildResponse_(500, "{\"error\":\"the notice could not be queued\"}");
      }
      CompromiseDetector::Apply(shape, message->GetID(), from, completed.recipient, _T("A password-protected message"), (__int64) message->GetSize());

      // A copy for the folder designated \Sent, marked read, before the queue
      // takes the file; a message held for review gets its copy when it is
      // released, as the send route's does.
      const bool sentCopy = !held && MessageUtilities::CopyToDesignatedSent(account, message);
      if (!held)
         Application::Instance()->SubmitPendingEmail();

      String line;
      line.Format(_T("RestApi: %s sent a password-protected message (row %I64d)."), account->GetAddress().c_str(), completed.id);
      LOG_APPLICATION(line);

      AnsiString json = RowJson(completed, now);
      AnsiString tail;
      tail.Format(",\"sent_copy\":%hs,\"held\":%hs}", sentCopy ? "true" : "false", held ? "true" : "false");
      json = json.Mid(0, json.GetLength() - 1) + tail;
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeProtectedDelete_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");
      HttpResponse failure;
      if (RefuseAppPasswordCaller_(caller, "withdrawing a password-protected message", failure))
         return failure;

      if (!ProtectedMessages::Revoke(account->GetID(), id))
         return BuildResponse_(404, "{\"error\":\"not one of your password-protected messages\"}");
      LOG_APPLICATION("RestApi: " + account->GetAddress() + " withdrew a password-protected message.");
      return BuildResponse_(200, "{\"revoked\":true}");
   }

   // ---- the reader's side ---------------------------------------------------

   // The ciphertext, to anyone who has the link. The token is the whole
   // capability and the route is rate-limited per address; a token that
   // names nothing is a plain 404 and counts for nothing else, because a
   // page fetches its link on load and a link older than its row is exactly
   // such a token - counting it would ban the reader of a stale link.
   HttpResponse
   RestApiServer::HandleProtectedFetch_(const AnsiString &token, const IPAddress &peer_address)
   {
      if (!ProtectedMessages::AllowUnauthenticated(peer_address))
         return BuildResponse_(429, Refused(429, "too many requests for password-protected messages from this address; wait an hour", "too_many"));

      ProtectedMessages::Entry entry;
      if (!ProtectedMessages::IsToken(token) || !ProtectedMessages::FindByToken(token, entry) || !entry.complete)
         return BuildResponse_(404, Refused(404, "there is no password-protected message at this address, or it has been removed", "not_found"));
      // The sender's switch: the message was made with the sender's OpenPGP.
      if (!EffectiveSetting::ResolveBool(_T("openpgp_portal_enabled"), std::shared_ptr<const Domain>(), CacheContainer::Instance()->GetAccount(entry.account_id)))
         return BuildResponse_(403, Refused(403, "OpenPGP is switched off in the webmail on this server, so a password-protected message cannot be read here", "openpgp_unavailable"));

      const __int64 now = (__int64) time(nullptr);
      if (entry.revoked)
         return BuildResponse_(410, Refused(410, "this message was withdrawn by its sender, and it can no longer be read", "withdrawn"));
      if (!entry.IsReadable(now))
         return BuildResponse_(410, Refused(410, "this message has expired; ask its sender if you still need it", "expired"));

      AnsiString text;
      if (!ProtectedMessages::ReadFile(entry, text))
         return BuildResponse_(500, "{\"error\":\"the ciphertext could not be read\"}");

      // Opened is when the ciphertext was first fetched, which is all this
      // server can know: whether the password was ever typed happens in the
      // reader's browser.
      ProtectedMessages::MarkOpened(entry);

      const int replies = entry.IsReply() ? 0 : ProtectedMessages::CountReplies(entry.token);
      AnsiString json;
      json.Format("{\"hint\":\"%hs\",\"created\":%I64d,\"expires\":%I64d,\"reply\":%hs,\"can_reply\":%hs,\"message\":\"",
                  Escape(Utf8(entry.hint)).c_str(), entry.created, entry.expires, entry.IsReply() ? "true" : "false",
                  (!entry.IsReply() && replies >= 0 && replies < ProtectedMessages::RepliesPerMessage) ? "true" : "false");
      json += Escape(text);
      json += "\"}";
      return BuildResponse_(200, json);
   }

   // A reply, written on the reading page and encrypted there under the same
   // password: stored under the ORIGINAL's sender, who is told by a short
   // internal message with a link that the same password opens.
   HttpResponse
   RestApiServer::HandleProtectedReply_(const AnsiString &token, const AnsiString &requestBody, const IPAddress &peer_address)
   {
      if (!ProtectedMessages::AllowUnauthenticated(peer_address))
         return BuildResponse_(429, Refused(429, "too many requests for password-protected messages from this address; wait an hour", "too_many"));

      ProtectedMessages::Entry original;
      if (!ProtectedMessages::IsToken(token) || !ProtectedMessages::FindByToken(token, original) || !original.complete)
         return BuildResponse_(404, Refused(404, "there is no password-protected message at this address, or it has been removed", "not_found"));
      if (!EffectiveSetting::ResolveBool(_T("openpgp_portal_enabled"), std::shared_ptr<const Domain>(), CacheContainer::Instance()->GetAccount(original.account_id)))
         return BuildResponse_(403, Refused(403, "OpenPGP is switched off in the webmail on this server, so a reply cannot be filed here", "openpgp_unavailable"));

      const __int64 now = (__int64) time(nullptr);
      if (original.IsReply())
         return BuildResponse_(400, Refused(400, "this is itself a reply; it cannot be replied to here", "not_replyable"));
      if (original.revoked)
         return BuildResponse_(410, Refused(410, "this message was withdrawn by its sender; a reply has nowhere to go", "withdrawn"));
      if (!original.IsReadable(now))
         return BuildResponse_(410, Refused(410, "this message has expired; a reply has nowhere to go", "expired"));

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object: message, the armoured OpenPGP message the page encrypted\"}");
      const JsonValue *messageValue = document.Get("message");
      if (!messageValue || !messageValue->IsString() || messageValue->AsString().empty())
         return BuildResponse_(400, "{\"error\":\"message: the armoured OpenPGP message the page encrypted under the password\"}");
      const AnsiString text(messageValue->AsString().c_str());
      if ((__int64) text.GetLength() > ProtectedMessages::ReplyMaximum)
      {
         AnsiString problem;
         problem.Format("{\"error\":\"a reply is at most %I64d MB of ciphertext\"}", ProtectedMessages::ReplyMaximum / (1024 * 1024));
         return BuildResponse_(413, problem);
      }
      AnsiString problem;
      if (!ProtectedMessages::IsSymmetricMessage(text, problem))
         return BuildResponse_(400, Refused(400, problem, "not_ciphertext"));

      const int replies = ProtectedMessages::CountReplies(original.token);
      if (replies < 0)
         return BuildResponse_(500, "{\"error\":\"the replies could not be counted\"}");
      if (replies >= ProtectedMessages::RepliesPerMessage)
      {
         AnsiString sentence;
         sentence.Format("this message has had %d replies, which is the most it may; write to the sender's address instead", ProtectedMessages::RepliesPerMessage);
         return BuildResponse_(400, Refused(400, sentence, "reply_ceiling"));
      }

      // The reply is the sender's to keep: it counts against the sender's
      // ceilings, and a sender whose allowance is used up is told so through
      // the reader rather than the reply being lost quietly.
      const int live = ProtectedMessages::CountLive(original.account_id);
      const __int64 held = ProtectedMessages::HeldBytes(original.account_id);
      if (live < 0 || held < 0)
         return BuildResponse_(500, "{\"error\":\"the sender's messages could not be counted\"}");
      if (live >= ProtectedMessages::LiveMaximum || held + (__int64) text.GetLength() > ProtectedMessages::AccountMaximum)
         return BuildResponse_(413, Refused(413, "the sender's allowance for password-protected messages is used up; write to their address instead", "protected_ceiling"));

      const __int64 expires = now + (__int64) ProtectedMessages::DefaultDays() * 24 * 60 * 60;
      ProtectedMessages::Entry made;
      if (!ProtectedMessages::Create(original.account_id, original.recipient, String(), (__int64) text.GetLength(), expires, original.token, made))
         return BuildResponse_(500, "{\"error\":\"the reply could not be recorded\"}");
      ProtectedMessages::Entry stored;
      ProtectedMessages::Entry completed;
      if (ProtectedMessages::Append(made, text, stored) != ProtectedMessages::Appended || !ProtectedMessages::Complete(stored, completed, problem))
      {
         ProtectedMessages::Discard(made);
         return BuildResponse_(500, "{\"error\":\"the reply could not be stored\"}");
      }

      const AnsiString link = ProtectedMessages::LinkFor(completed.token);
      const bool notified = ProtectedMessages::SendReplyNotice(original, completed, link);

      String line;
      line.Format(_T("RestApi: a reply to a password-protected message was filed for account %I64d (row %I64d)."), original.account_id, completed.id);
      LOG_APPLICATION(line);

      AnsiString json;
      json.Format("{\"token\":\"%hs\",\"link\":\"%hs\",\"expires\":%I64d,\"notified\":%hs}", completed.token.c_str(), Escape(link).c_str(), completed.expires, notified ? "true" : "false");
      return BuildResponse_(201, json);
   }

   // The reading page and its script: files beside RestApiPortal.cpp
   // (PortalProtected.html, PortalProtected.js), carried into the binary by
   // build/generate-portal-page.py, joined - and compressed - on first use.
   // The page is the same bytes for every token: its script reads the token
   // from the address bar and fetches the ciphertext. Served with the
   // portal's own headers, so the policy that runs nothing but this server's
   // scripts and fetches nothing from anywhere else is the same policy.
   HttpResponse
   RestApiServer::HandleProtectedPage_()
   {
      static HttpStaticBody page;
      HttpResponse response;
      response.content_type = "text/html; charset=utf-8";
      page.Fill(response, PortalProtectedHtmlPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   HttpResponse
   RestApiServer::HandlePortalProtectedScript_()
   {
      static HttpStaticBody script;
      HttpResponse response;
      response.content_type = "text/javascript; charset=utf-8";
      script.Fill(response, PortalProtectedScriptPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   AnsiString
   RestApiServer::OpenApiProtectedPaths_()
   {
      return
         ",\"/api/v1/me/protected\":{\"get\":{\"summary\":\"The password-protected messages this account has sent\",\"description\":\"{available,days,limits:{message_bytes,account_bytes,live,reply_bytes},used_bytes,items:[{id,token,recipient,hint,size,stored,complete,created,expires,expired,opened,revoked,reply_to,link}]}, newest first. available is whether this server offers them - OpenPGP has to be on in the webmail and a public address set for the links (confidential_base_url, or the digest's or the import sign-in's base URL). A message is the ciphertext the webmail made in the browser: the text and the attachments encrypted to a password as ONE OpenPGP message (RFC 9580, a symmetric-key encrypted session key and an integrity-protected body), and this server never had the plaintext. opened is when the ciphertext was first fetched (0 until then), which is all this server can know; a row whose reply_to is not empty is a reply somebody wrote on the reading page, stored under this account and opened with the password this account set. Seconds since 1970 throughout. A row stays thirty days after its expiry, the ciphertext not: it is deleted when the expiry passes or the message is withdrawn. What this promises, and all it promises: the plaintext never crosses the internet and never rests on this server - the page encrypts before it sends, an attachment is never sent as a link while a message is protected, and this server files only what has the shape of a message to a passphrase. It does not stop the recipient copying or forwarding what they read; and the sender's own Sent folder holds the notice alone, their text being readable only through the link with the password until it expires. Refused to a request authenticated with an app password: the list carries every live link and hint.\",\"responses\":{\"200\":{\"description\":\"The items\"},\"403\":{\"description\":\"An app password\"}}},"
         "\"post\":{\"summary\":\"Record a password-protected message; the ciphertext follows in chunks\",\"description\":\"Body: recipient (one address), size (the ciphertext's bytes, at most 25 MB), hint (what to say about the password, at most 255 characters; optional) and expires_in (seconds from now, at most the server's protected_message_days; optional, that many days by default). Nothing is sent yet: PUT the ciphertext to /api/v1/me/protected/{id}/content, and when the declared size is reached and the bytes prove to be an OpenPGP message to a passphrase, the notice goes to the recipient - a sentence, the hint and the link, from this account, with a copy in its Sent folder. Refused to a request authenticated with an app password. The ceilings: 25 MB a message, 250 MB and a thousand live messages an account, counted from the declared size; each refused with a sentence.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"recipient\",\"size\"],\"properties\":{\"recipient\":{\"type\":\"string\"},\"size\":{\"type\":\"integer\"},\"hint\":{\"type\":\"string\"},\"expires_in\":{\"type\":\"integer\"}}}}}},\"responses\":{\"201\":{\"description\":\"The record, with its token and link; stored is 0 and complete is false\"},\"400\":{\"description\":\"Not an address, a hint too long, an expiry out of range, or the account has a thousand outstanding\"},\"403\":{\"description\":\"Not offered here, or an app password\"},\"413\":{\"description\":\"The message or the account's allowance is too large\"}}}},"
         "\"/api/v1/me/protected/{id}/content\":{\"put\":{\"summary\":\"One chunk of the ciphertext\",\"description\":\"The body is the chunk's bytes, raw; offset in the query is where it starts, which must be exactly what has been stored so far (409 says where, with stored). A chunk for an offset the file has already passed - two sent for one offset - is 409 with stored, judged under the lock the append takes. When the declared size is reached the whole is read back and refused - discarded, with discarded:true and the reason - unless it has the shape of an ASCII-armoured PGP MESSAGE to a passphrase: one or more symmetric-key encrypted session keys and then one integrity-protected data packet running to the end, nothing else - what the page makes, and nothing a client could send in the clear or slip a literal packet into. What this server cannot tell is ciphertext from noise, and it does not claim to. Then the notice goes; sent_copy says whether a copy went to Sent and held whether the compromised-account detection kept it for review instead.\",\"responses\":{\"200\":{\"description\":\"The record as it now stands\"},\"400\":{\"description\":\"The bytes stored are not ciphertext to a password; the record is discarded\"},\"404\":{\"description\":\"No such record of this account\"},\"409\":{\"description\":\"The offset is not where the record has reached, or the message is complete\"},\"413\":{\"description\":\"More bytes than were declared\"}}}},"
         "\"/api/v1/me/protected/{id}\":{\"delete\":{\"summary\":\"Withdraw a password-protected message\",\"description\":\"The reading page refuses it from now on and the ciphertext is deleted; the row stays in the list as revoked. A reply is withdrawn the same way.\",\"responses\":{\"200\":{\"description\":\"Withdrawn\"},\"404\":{\"description\":\"Not one of this account's\"}}}},"
         "\"/api/v1/protected/{token}\":{\"get\":{\"summary\":\"The ciphertext of a password-protected message (no credentials)\",\"description\":\"What the reading page fetches. The token in the link is the whole access control: 128 bits of base64url from the system's random source, unguessable, and a request for a token that names nothing is answered 404 without saying whether one ever existed, and counts for nothing else: a page fetches its link on load, and a link older than its row is such a token. Answers hint, created, expires, reply (whether this is itself a reply), can_reply and message (the armoured OpenPGP message, which the browser decrypts with the password; this server cannot). The first fetch is recorded as opened for the sender. 410 with code withdrawn or expired once it cannot be read; 403 when OpenPGP is switched off in the webmail; 429 past a hundred and twenty requests an hour from one address (loopback exempt).\",\"responses\":{\"200\":{\"description\":\"hint, created, expires, reply, can_reply, message\"},\"404\":{\"description\":\"No such message\"},\"410\":{\"description\":\"Withdrawn or expired\"},\"429\":{\"description\":\"Too many requests from this address\"}}}},"
         "\"/api/v1/protected/{token}/reply\":{\"post\":{\"summary\":\"Reply to a password-protected message (no credentials)\",\"description\":\"Body: message, the armoured OpenPGP message the reading page encrypted under the SAME password. Held to the same shape as the original - a symmetric-key encrypted session key and an integrity-protected body, at most 1 MB - and stored under the original's sender, who is sent a short internal message from the domain's postmaster with a link to /portal/protected/{reply token}; the password they set opens it. An original may collect at most twenty replies, and a reply cannot be replied to. 404, 410 and 429 as the fetch answers them.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"message\"],\"properties\":{\"message\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"token, link, expires, notified\"},\"400\":{\"description\":\"Not ciphertext to a password, a reply to a reply, or the reply ceiling\"},\"404\":{\"description\":\"No such message\"},\"410\":{\"description\":\"Withdrawn or expired\"},\"413\":{\"description\":\"Too large, or the sender's allowance is used up\"}}}},"
         "\"/portal/protected/{token}\":{\"get\":{\"summary\":\"The reading page (no credentials)\",\"description\":\"A plain page, the same bytes for every token, served with the webmail's own headers: it loads the page's MIME and OpenPGP modules, fetches the ciphertext, asks for the password, decrypts in the browser, shows the text, offers each attachment to save and takes a reply. A wrong password says so and nothing else.\",\"responses\":{\"200\":{\"description\":\"The page\"}}}}";
   }
}
