// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "QuarantineDigest.h"
#include "QuarantineStore.h"
#include "SenderListCache.h"

#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/PropertySet.h"
#include "../Application/Constants.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentSenderListEntry.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Util/FileUtilities.h"
#include "../Util/Time.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Unicode.h"
#include "../../SMTP/RecipientParser.h"

#include <openssl/rand.h>

#include <algorithm>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // 32 bytes, 64 hex characters, which is the width of the dttoken column.
      const size_t TokenBytes = 32;

      // What hm_digeststate.dssenttime holds for an account that has never had
      // a digest - a row an opt-out made. Read back as "never".
      const wchar_t *const NeverSent = L"2000-01-01 00:00:00";
      const int NeverSentYear = 2000;

      AnsiString LowerHex_(const unsigned char *bytes, size_t count)
      {
         static const char *digits = "0123456789abcdef";

         AnsiString out;
         for (size_t i = 0; i < count; i++)
         {
            out += digits[(bytes[i] >> 4) & 0x0f];
            out += digits[bytes[i] & 0x0f];
         }

         return out;
      }

      // A token as it may appear in a URL. Anything else never reaches the
      // database, so a query parameter carrying a quote is a 404 rather than a
      // parameter.
      bool IsTokenText_(const AnsiString &token)
      {
         if (token.GetLength() != (int) (TokenBytes * 2))
            return false;

         for (int i = 0; i < token.GetLength(); i++)
         {
            const char c = token.GetAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }

         return true;
      }

      // UTF-8, which is what the message declares. Assigning a String to an
      // AnsiString converts through the system code page instead, and a subject
      // in Greek or Japanese would then arrive as question marks under a charset
      // that promised better.
      AnsiString Utf8_(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String Flattened_(const String &value)
      {
         String flat = value;
         flat.Replace(_T("\r"), _T(" "));
         flat.Replace(_T("\n"), _T(" "));
         flat.Replace(_T("\t"), _T(" "));
         return flat;
      }

      // One header value, with anything that could start a second header taken
      // out. Every value here comes from a message somebody else wrote.
      AnsiString HeaderSafe_(const String &value)
      {
         return Utf8_(Flattened_(value));
      }

      // One line of the digest body. A quarantined subject is attacker-chosen
      // text: a newline in it would let the sender forge a whole extra entry,
      // with a link of their choosing, into a message the reader trusts because
      // it came from their own server. So every line of every entry is flattened
      // and cut - cut in characters, before the conversion, so that the cut never
      // falls inside a multi-byte sequence.
      AnsiString BodySafe_(const String &value, int maxLength)
      {
         String flat = Flattened_(value);
         const bool cut = flat.GetLength() > maxLength;

         if (cut)
            flat = flat.Mid(0, maxLength);

         AnsiString out = Utf8_(flat);

         if (cut)
            out += "...";

         return out;
      }

      String TimeInHours_(int hours)
      {
         // The database's own clock, as every other expiry in this server
         // measures against, so a server and a database on different clocks
         // cannot disagree about when a link died.
         DateTime expires = DateTime::GetCurrentTime();

         DateTimeSpan span;
         span.SetDateTimeSpan(0, hours, 0, 0);
         // operator+ and not +=: DateTime declares the compound operators but
         // defines only the plain ones, so += compiles and does not link.
         expires = expires + span;

         return Time::GetTimeStampFromDateTime(expires);
      }
   }

   String
   QuarantineDigest::BaseUrl()
   {
      String url = Configuration::Instance()->GetQuarantineDigestBaseUrl();
      url.Trim();

      while (url.Right(1) == _T("/"))
         url = url.Mid(0, url.GetLength() - 1);

      // Only http and https, and only an absolute URL. A relative one, or one
      // whose scheme somebody has been talked into changing, would put a link
      // into everybody's mailbox pointing wherever that value said.
      String lowered = url;
      lowered.MakeLower();

      if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
         return _T("");

      return url;
   }

   int
   QuarantineDigest::LinkHours()
   {
      int hours = Configuration::Instance()->GetQuarantineDigestLinkHours();

      if (hours <= 0)
         hours = 72;

      // A month is the ceiling. A capability that outlives the memory of the
      // message it belongs to is one nobody will revoke.
      if (hours > 720)
         hours = 720;

      return hours;
   }

   bool
   QuarantineDigest::GetState(__int64 accountId, State &out_state)
   {
      State empty;
      out_state = empty;

      SQLCommand command("select dsoptout, dslastid, dssenttime from hm_digeststate where dsaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return true;

      out_state.exists = true;
      out_state.opted_out = recordset->GetLongValue("dsoptout") == 1;
      out_state.last_id = recordset->GetInt64Value("dslastid");
      out_state.last_sent = Time::GetDateFromSystemDate(recordset->GetStringValue("dssenttime"));

      // A row made by an opt-out before any digest was sent carries the
      // placeholder SetOptOut writes, which is "never", not a date to show.
      if (out_state.last_sent.GetStatus() == DateTime::valid && out_state.last_sent.GetYear() <= NeverSentYear)
         out_state.last_sent.SetStatus(DateTime::invalid);

      return true;
   }

   bool
   QuarantineDigest::SetOptOut(__int64 accountId, bool optOut)
   {
      State state;
      if (!GetState(accountId, state))
         return false;

      SQLStatement statement;
      statement.SetTable("hm_digeststate");

      if (state.exists)
      {
         statement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("dsaccountid = %I64d"), accountId);
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("dsid");
         statement.AddColumnInt64("dsaccountid", accountId);
         statement.AddColumnInt64("dslastid", 0);

         // The column is NOT NULL and no digest has been sent. Not the current
         // time: switching the digest off or on is not sending one, and writing
         // "now" here would both show the reader a digest that never came and
         // hold back the next real one for twenty hours.
         statement.AddColumn("dssenttime", String(NeverSent));
      }

      statement.AddColumn("dsoptout", optOut ? 1L : 0L);

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   QuarantineDigest::RecordSent(__int64 accountId, __int64 highestId)
   {
      State state;
      if (!GetState(accountId, state))
         return false;

      SQLStatement statement;
      statement.SetTable("hm_digeststate");

      if (state.exists)
      {
         statement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("dsaccountid = %I64d"), accountId);
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("dsid");
         statement.AddColumnInt64("dsaccountid", accountId);
         statement.AddColumn("dsoptout", 0L);
      }

      statement.AddColumnInt64("dslastid", highestId);
      statement.AddColumn("dssenttime", Time::GetCurrentDateTime());

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   AnsiString
   QuarantineDigest::IssueToken(__int64 accountId, int kind, __int64 quarantineId)
   {
      unsigned char bytes[TokenBytes];

      if (RAND_bytes(bytes, (int) TokenBytes) != 1)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6748, "QuarantineDigest::IssueToken",
            "No entropy was available for a quarantine digest link, so no digest has been sent to this account on this pass.");
         return AnsiString();
      }

      const AnsiString token = LowerHex_(bytes, TokenBytes);

      SQLStatement statement;
      statement.SetTable("hm_digesttokens");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("dtid");
      statement.AddColumnInt64("dtaccountid", accountId);
      statement.AddColumn("dtkind", (long) kind);
      statement.AddColumnInt64("dtquarantineid", quarantineId);
      statement.AddColumn("dttoken", String(token));
      statement.AddColumn("dtexpirestime", TimeInHours_(LinkHours()));
      statement.AddColumn("dtused", 0L);
      statement.AddColumn("dtcreated", Time::GetCurrentDateTime());

      if (!Application::Instance()->GetDBManager()->Execute(statement))
         return AnsiString();

      return token;
   }

   bool
   QuarantineDigest::FindToken(const AnsiString &token, Token &out_token)
   {
      Token empty;
      out_token = empty;

      if (!IsTokenText_(token))
         return false;

      SQLCommand command("select dtid, dtaccountid, dtkind, dtquarantineid, dtused, dtexpirestime from hm_digesttokens where dttoken = @TOKEN");
      command.AddParameter("@TOKEN", String(token));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return false;

      out_token.id = recordset->GetInt64Value("dtid");
      out_token.account_id = recordset->GetInt64Value("dtaccountid");
      out_token.kind = recordset->GetLongValue("dtkind");
      out_token.quarantine_id = recordset->GetInt64Value("dtquarantineid");
      out_token.used = recordset->GetLongValue("dtused") == 1;

      const DateTime expires = Time::GetDateFromSystemDate(recordset->GetStringValue("dtexpirestime"));

      // An unreadable expiry counts as expired. The row said it had one, and the
      // safe reading of a time that cannot be read is that it has passed.
      out_token.expired = expires.GetStatus() != DateTime::valid || expires < DateTime::GetCurrentTime();

      return true;
   }

   bool
   QuarantineDigest::MarkUsed(__int64 tokenId)
   {
      SQLCommand command("update hm_digesttokens set dtused = 1 where dtid = @ID and dtused = 0");
      command.AddParameter("@ID", tokenId);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   int
   QuarantineDigest::Sweep()
   {
      int swept = 0;

      SQLCommand tokens(_T("delete from hm_digesttokens where dtused = 1 or dtexpirestime < @TIME"));
      tokens.AddParameter("@TIME", Time::GetCurrentDateTime());

      if (Application::Instance()->GetDBManager()->Execute(tokens))
         swept++;

      if (PersistentSenderListEntry::DeleteExpired())
         swept++;

      return swept;
   }

   QuarantineDigest::Outcome
   QuarantineDigest::Inspect(const AnsiString &token)
   {
      Outcome outcome;

      Token row;

      if (!FindToken(token, row) || row.used || row.expired)
      {
         outcome.http_status = 410;
         outcome.heading = _T("This link no longer works");
         outcome.detail = _T("A link in a digest can be followed once, and stops working after a few days. Sign in to the webmail to see what is still being held for you.");
         return outcome;
      }

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(row.account_id);

      if (!account)
      {
         outcome.http_status = 410;
         outcome.heading = _T("This link no longer works");
         outcome.detail = _T("The account this link belonged to no longer exists.");
         return outcome;
      }

      outcome.kind = row.kind;

      if (row.kind == KindOptOut)
      {
         outcome.ok = true;
         outcome.heading = _T("Stop these digests?");
         outcome.detail = _T("You will no longer get the list of mail held for you. Mail will still be held, and the Held mail page of the webmail will still show it.");
         return outcome;
      }

      if (row.kind == KindRelease)
      {
         QuarantinedMessage message;

         if (!QuarantineStore::GetById(row.quarantine_id, message) ||
             !QuarantineStore::IsRecipient(message, account->GetAddress()))
         {
            outcome.http_status = 410;
            outcome.heading = _T("There is nothing left to release");
            outcome.detail = _T("It has already been released or deleted. The Held mail page of the webmail shows what is still there.");
            return outcome;
         }

         outcome.ok = true;
         outcome.released = 1;
         outcome.heading = _T("Deliver this message to your inbox?");
         outcome.detail = Formatter::Format("From {0}: {1}", message.sender,
            message.subject.IsEmpty() ? String(_T("(no subject)")) : message.subject);
         return outcome;
      }

      int count = 0;
      const std::vector<QuarantinedMessage> all = QuarantineStore::List(1000);

      for (const QuarantinedMessage &message : all)
      {
         if (message.id <= row.quarantine_id && QuarantineStore::IsRecipient(message, account->GetAddress()))
            count++;
      }

      if (count == 0)
      {
         outcome.http_status = 410;
         outcome.heading = _T("There is nothing left to release");
         outcome.detail = _T("Everything the digest listed has already been released or deleted.");
         return outcome;
      }

      outcome.ok = true;
      outcome.released = count;
      outcome.heading = count == 1
         ? String(_T("Deliver the message the digest listed?"))
         : Formatter::Format("Deliver the {0} messages the digest listed?", count);
      outcome.detail = _T("Only what the digest listed is delivered; anything held since then stays held.");
      return outcome;
   }

   QuarantineDigest::Outcome
   QuarantineDigest::Redeem(const AnsiString &token)
   {
      Outcome outcome;

      Token row;

      // One answer for every way a token can fail to name something this person
      // may act on: unknown, another account's, already used, expired, deleted.
      // Telling them apart would tell somebody with a guessed token which of
      // their guesses was close.
      if (!FindToken(token, row) || row.used || row.expired)
      {
         outcome.http_status = 410;
         outcome.heading = _T("This link no longer works");
         outcome.detail = _T("A link in a digest can be followed once, and stops working after a few days. Sign in to the webmail to see what is still being held for you.");
         return outcome;
      }

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(row.account_id);

      if (!account)
      {
         outcome.http_status = 410;
         outcome.heading = _T("This link no longer works");
         outcome.detail = _T("The account this link belonged to no longer exists.");
         return outcome;
      }

      if (row.kind == KindOptOut)
      {
         // Marked used first, then acted on: a second click after a successful
         // opt-out says "this link no longer works", which is true, rather than
         // silently doing it again.
         MarkUsed(row.id);

         if (!SetOptOut(row.account_id, true))
         {
            outcome.http_status = 500;
            outcome.heading = _T("That could not be saved");
            outcome.detail = _T("The digest could not be switched off just now. Try again, or turn it off in the webmail under Held mail.");
            return outcome;
         }

         outcome.ok = true;
         outcome.heading = _T("You will not get any more of these");
         outcome.detail = _T("Mail the server holds as suspected spam is still held, and still listed on the Held mail page of the webmail. You can switch the digest back on there.");
         return outcome;
      }

      // A release. Marked used before anything is delivered, so that a link
      // followed twice - a mail client's link checker, a reader's second click -
      // cannot deliver twice.
      if (!MarkUsed(row.id))
      {
         outcome.http_status = 500;
         outcome.heading = _T("That could not be done");
         outcome.detail = _T("The link could not be redeemed just now. Sign in to the webmail to release the message from the Held mail page.");
         return outcome;
      }

      std::vector<QuarantinedMessage> wanted;

      if (row.kind == KindRelease)
      {
         QuarantinedMessage message;

         if (QuarantineStore::GetById(row.quarantine_id, message) &&
             QuarantineStore::IsRecipient(message, account->GetAddress()))
         {
            wanted.push_back(message);
         }
      }
      else
      {
         // Everything the digest listed, and nothing newer: the token carries the
         // highest entry id the message showed, so a message quarantined since is
         // not released by a reader who never saw it.
         const std::vector<QuarantinedMessage> all = QuarantineStore::List(1000);

         for (const QuarantinedMessage &message : all)
         {
            if (message.id > row.quarantine_id)
               continue;

            if (!QuarantineStore::IsRecipient(message, account->GetAddress()))
               continue;

            wanted.push_back(message);
         }
      }

      if (wanted.empty())
      {
         outcome.http_status = 410;
         outcome.heading = _T("There is nothing left to release");
         outcome.detail = _T("It has already been released or deleted. The Held mail page of the webmail shows what is still there.");
         return outcome;
      }

      for (const QuarantinedMessage &message : wanted)
      {
         String error;

         if (QuarantineStore::ReleaseTo(message.id, account->GetAddress(), error))
            outcome.released++;
         else
            LOG_APPLICATION(_T("QuarantineDigest: a release link could not deliver an entry: ") + error);
      }

      if (outcome.released == 0)
      {
         outcome.http_status = 500;
         outcome.heading = _T("That could not be delivered");
         outcome.detail = _T("The message is still being held. Sign in to the webmail and release it from the Held mail page.");
         return outcome;
      }

      outcome.ok = true;
      outcome.heading = outcome.released == 1 ? _T("On its way to your inbox") : _T("On their way to your inbox");
      outcome.detail = outcome.released == 1
         ? _T("The message has been delivered to you. It may take a moment to appear.")
         : _T("The messages have been delivered to you. They may take a moment to appear.");

      LOG_APPLICATION(Formatter::Format("QuarantineDigest: released {0} held message(s) to {1} through a digest link.",
         outcome.released, account->GetAddress()));

      return outcome;
   }

   std::vector<QuarantinedMessage>
   QuarantineDigest::Held(const String &address, __int64 sinceId, const DateTime &lastSent)
   {
      std::vector<QuarantinedMessage> held;

      const std::vector<QuarantinedMessage> all = QuarantineStore::List(1000);

      const bool haveLastSent = lastSent.GetStatus() == DateTime::valid;

      for (const QuarantinedMessage &message : all)
      {
         if (message.id <= sinceId)
         {
            // Below the position: new only if it was held after the last digest
            // went, which is an identity that started again rather than an entry
            // already shown. An unreadable time is not evidence of that.
            if (!haveLastSent)
               continue;

            const DateTime created = Time::GetDateFromSystemDate(message.created);

            if (created.GetStatus() != DateTime::valid || !(lastSent < created))
               continue;
         }

         if (!QuarantineStore::IsRecipient(message, address))
            continue;

         held.push_back(message);
      }

      return held;
   }

   String
   QuarantineDigest::SenderFor(std::shared_ptr<const Domain> domain)
   {
      if (!domain)
         return _T("");

      String postmaster = domain->GetPostmaster();
      postmaster.Trim();

      if (!postmaster.IsEmpty() && postmaster.Find(_T("@")) > 0)
         return postmaster;

      return _T("postmaster@") + domain->GetName();
   }

   AnsiString
   QuarantineDigest::BuildMessage(const String &toAddress, const String &fromAddress,
                                  const std::vector<QuarantinedMessage> &held,
                                  const std::vector<AnsiString> &releaseTokens,
                                  const AnsiString &releaseAllToken,
                                  const AnsiString &optOutToken,
                                  const String &baseUrl)
   {
      const AnsiString base = baseUrl;
      const AnsiString releasePath = base + "/held/release?t=";
      const AnsiString optOutUrl = base + "/held/stop?t=" + optOutToken;

      AnsiString subject;
      subject.Format("%d message%s held for you", (int) held.size(), held.size() == 1 ? "" : "s");

      AnsiString body;
      body += "Your mail server is holding ";
      body += held.size() == 1 ? AnsiString("one message") : AnsiString(StringParser::IntToString((int) held.size())) + " messages";
      body += " that it judged to be spam. Nothing below has been delivered to you.\r\n";
      body += "\r\n";
      body += "If one of them is wanted, follow the link under it and confirm, and it will be\r\n";
      body += "delivered. Each link works once and stops working after a few days. Nothing\r\n";
      body += "here asks for your password, and every link points at your own server.\r\n";
      body += "\r\n";

      for (size_t i = 0; i < held.size(); i++)
      {
         const QuarantinedMessage &message = held[i];

         AnsiString index;
         index.Format("%d. ", (int) i + 1);

         body += index;
         body += BodySafe_(message.subject.IsEmpty() ? String(_T("(no subject)")) : message.subject, 160);
         body += "\r\n";
         body += "   From:    " + BodySafe_(message.sender, 160) + "\r\n";
         body += "   Held:    " + BodySafe_(message.created, 40) + "\r\n";
         body += "   Because: " + BodySafe_(message.reason, 200) + "\r\n";

         if (i < releaseTokens.size() && !releaseTokens[i].IsEmpty())
            body += "   Deliver: " + releasePath + releaseTokens[i] + "\r\n";

         body += "\r\n";
      }

      if (!releaseAllToken.IsEmpty() && held.size() > 1)
      {
         body += "To have all of them delivered:\r\n";
         body += "   " + releasePath + releaseAllToken + "\r\n";
         body += "\r\n";
      }

      body += "--\r\n";
      body += "You are getting this because your mail server holds suspected spam for\r\n";
      body += "review rather than refusing it. To stop these messages:\r\n";
      body += "   " + optOutUrl + "\r\n";
      body += "Mail will still be held, and the Held mail page of the webmail will still\r\n";
      body += "show it.\r\n";

      AnsiString content;
      content += "From: <" + HeaderSafe_(fromAddress) + ">\r\n";
      content += "To: <" + HeaderSafe_(toAddress) + ">\r\n";
      content += "Subject: " + subject + "\r\n";
      content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";

      // RFC 3834: this is generated mail. Auto-Submitted stops another server's
      // vacation responder answering it, and Precedence: bulk keeps it out of
      // some older ones. List-Unsubscribe names the same one-shot link the
      // footer does, so a client that offers an unsubscribe button offers the
      // real one rather than teaching people to ignore the footer.
      content += "Auto-Submitted: auto-generated\r\n";
      content += "Precedence: bulk\r\n";
      content += "X-Auto-Response-Suppress: All\r\n";

      if (!optOutToken.IsEmpty())
      {
         content += "List-Unsubscribe: <" + optOutUrl + ">\r\n";

         // RFC 8058: a client that offers an unsubscribe button POSTs to the link
         // itself, which /held/stop answers without the confirmation page a
         // browser gets - the POST is the confirmation.
         content += "List-Unsubscribe-Post: List-Unsubscribe=One-Click\r\n";
      }

      content += "MIME-Version: 1.0\r\n";
      content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      content += "Content-Transfer-Encoding: 8bit\r\n";
      content += "X-hMailServer-Quarantine-Digest: " + HeaderSafe_(toAddress) + "\r\n";
      content += "\r\n";
      content += body;

      return content;
   }

   bool
   QuarantineDigest::SendFor(std::shared_ptr<const Account> account, std::shared_ptr<const Domain> domain,
                             const std::vector<QuarantinedMessage> &held, __int64 &out_highestId)
   {
      out_highestId = 0;

      if (!account || held.empty())
         return false;

      const String baseUrl = BaseUrl();

      if (baseUrl.IsEmpty())
         return false;

      const String fromAddress = SenderFor(domain);

      if (fromAddress.IsEmpty())
         return false;

      // A token per entry, plus one for the lot and one to stop the digest. If
      // any of them cannot be made the digest is not sent at all: a message
      // whose links do nothing teaches the reader that the links do nothing.
      std::vector<AnsiString> releaseTokens;

      for (const QuarantinedMessage &message : held)
      {
         const AnsiString token = IssueToken(account->GetID(), KindRelease, message.id);

         if (token.IsEmpty())
            return false;

         releaseTokens.push_back(token);

         if (message.id > out_highestId)
            out_highestId = message.id;
      }

      AnsiString releaseAllToken;

      if (held.size() > 1)
      {
         releaseAllToken = IssueToken(account->GetID(), KindReleaseAll, out_highestId);

         if (releaseAllToken.IsEmpty())
            return false;
      }

      const AnsiString optOutToken = IssueToken(account->GetID(), KindOptOut, 0);

      if (optOutToken.IsEmpty())
         return false;

      const AnsiString content = BuildMessage(account->GetAddress(), fromAddress, held,
                                              releaseTokens, releaseAllToken, optOutToken, baseUrl);

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);

      // The null sender. The digest is generated mail addressed to one mailbox on
      // this server, and a bounce of it - the mailbox is full, say - must not go
      // back to the postmaster as though a person had written to them, nor loop.
      message->SetFromAddress(_T(""));

      const String fileName = MessageStore::WorkingFile(message);

      if (!FileUtilities::WriteToFile(fileName, content))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6749, "QuarantineDigest::SendFor",
            "The quarantine digest for " + account->GetAddress() + " could not be written to the queue and has NOT been sent.");
         return false;
      }

      message->SetSize(MessageStore::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(account->GetAddress(), message->GetRecipients(), recipientOk);

      if (message->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(fileName);
         return false;
      }

      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6750, "QuarantineDigest::SendFor",
            "The quarantine digest for " + account->GetAddress() + " could not be saved and has NOT been sent.");

         return false;
      }

      Application::Instance()->SubmitPendingEmail();

      LOG_APPLICATION(Formatter::Format("QuarantineDigest: sent a digest of {0} held message(s) to {1}.",
         (int) held.size(), account->GetAddress()));

      return true;
   }
}
