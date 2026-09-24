// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "AccountRecovery.h"
#include "ConfidentialMessages.h"
#include "Crypt.h"
#include "PasswordBreachCheck.h"
#include "PasswordHistory.h"
#include "PasswordPolicy.h"
#include "PasswordValidator.h"
#include "RestApiServer.h"
#include "Time.h"
#include "Unicode.h"
#include "Utilities.h"
#include "AlertManager.h"
#include "FileUtilities.h"
#include "Strings/Formatter.h"
#include "Hashing/HashCreator.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../TCPIP/IPAddress.h"
#include "../../SMTP/RecipientParser.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <ctime>
#include <map>
#include <mutex>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int TokenBytes = 32;

      // A request for a reset link, counted twice over. Per ADDRESS: one a
      // minute and six an hour, which is what stops a link being mailed at
      // somebody over and over. Per CLIENT: twenty an hour and no minute rule
      // at all, because a client address is very often a shared one - an office
      // NAT, a mail gateway, a webmail front end on loopback - and a minute
      // rule there would let one person's forgotten password stop everybody
      // else's. Both are in memory and both are charged for an address that has
      // no mailbox exactly as for one that has, so the shape of the refusal
      // does not answer the question the route refuses to.
      const time_t ResendSeconds = 60;
      const time_t HourSeconds = 60 * 60;
      const int RequestsPerAddressPerHour = 6;
      const int RequestsPerClientPerHour = 20;
      const size_t MapCeiling = 10000;

      // How long a recovery code is, in characters, and the alphabet it is drawn
      // from - the same one an app password uses: Crockford-style base32 without
      // the letters that are read as digits. Ten characters of thirty symbols is
      // a little over 49 bits, which is not guessable at the rate a sign-in is
      // counted at.
      const int CodeCharacters = 10;

      struct Request
      {
         time_t last = 0;
         time_t hour = 0;
         int count = 0;
      };

      std::mutex state_mutex;
      std::map<AnsiString, Request> by_address;
      std::map<AnsiString, Request> by_client;

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
      }

      String Lower(const String &text)
      {
         String lower = text;
         lower.Trim();
         lower.ToLower();
         return lower;
      }

      AnsiString Hex(const unsigned char *data, int length)
      {
         static const char *digits = "0123456789abcdef";
         AnsiString out;
         for (int i = 0; i < length; i++)
         {
            out += digits[data[i] >> 4];
            out += digits[data[i] & 15];
         }
         return out;
      }

      AnsiString RandomHex(int bytes)
      {
         std::vector<unsigned char> buffer((size_t) bytes);
         if (RAND_bytes(buffer.data(), bytes) != 1)
            return AnsiString();
         return Hex(buffer.data(), bytes);
      }

      AnsiString Sha256(const AnsiString &text)
      {
         HashCreator hasher(HashCreator::SHA256);
         return hasher.GenerateHashNoSalt(text, HashCreator::hex);
      }

      bool IsTokenText(const AnsiString &token)
      {
         if (token.GetLength() != TokenBytes * 2)
            return false;
         for (int i = 0; i < token.GetLength(); i++)
         {
            const char c = token[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }
         return true;
      }

      // Nothing that ends or folds a header line.
      AnsiString HeaderSafe(const String &text)
      {
         AnsiString bytes = Utf8(text);
         AnsiString safe;
         for (int i = 0; i < bytes.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) bytes[i];
            if (c >= 0x20 && c != 0x7f)
               safe += (char) c;
         }
         return safe;
      }

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      bool Execute(const SQLCommand &command)
      {
         return Application::Instance()->GetDBManager()->Execute(command);
      }

      std::shared_ptr<DALRecordset> Open(const SQLCommand &command)
      {
         return Application::Instance()->GetDBManager()->OpenRecordset(command);
      }

      // Called with state_mutex held.
      void Prune(time_t now)
      {
         for (auto it = by_address.begin(); it != by_address.end();)
            it = now - it->second.last > (time_t) HourSeconds ? by_address.erase(it) : std::next(it);
         for (auto it = by_client.begin(); it != by_client.end();)
            it = now - it->second.last > (time_t) HourSeconds ? by_client.erase(it) : std::next(it);
         if (by_address.size() > MapCeiling)
            by_address.clear();
         if (by_client.size() > MapCeiling)
            by_client.clear();
      }

      // One counter's verdict, and the counter moved on. Called with the mutex
      // held. 0 lets the request through, 1 is too soon, 2 is too many.
      int Charge(std::map<AnsiString, Request> &counters, const AnsiString &key, time_t now,
                 bool oneAMinute, int perHour)
      {
         Request &request = counters[key];
         if (oneAMinute && request.last != 0 && now - request.last < ResendSeconds)
            return 1;
         if (now - request.hour >= (time_t) HourSeconds)
         {
            request.hour = now;
            request.count = 0;
         }
         if (request.count >= perHour)
         {
            request.last = now;
            return 2;
         }
         request.last = now;
         request.count++;
         return 0;
      }

      // What Charge would answer, without moving the counter and without
      // creating one. Called with the mutex held. It exists so that Set can
      // refuse before it writes a row whose confirmation it could not then
      // send - an address stored with no way to prove it is an address that
      // counts for nothing until somebody asks again.
      int Peek(std::map<AnsiString, Request> &counters, const AnsiString &key, time_t now,
               bool oneAMinute, int perHour)
      {
         auto found = counters.find(key);
         if (found == counters.end())
            return 0;

         const Request &request = found->second;
         if (oneAMinute && request.last != 0 && now - request.last < ResendSeconds)
            return 1;
         if (now - request.hour >= (time_t) HourSeconds)
            return 0;
         if (request.count >= perHour)
            return 2;
         return 0;
      }

      // How long a counter says to wait, given a verdict Charge or Peek gave.
      // Called with the mutex held.
      int WaitFor(std::map<AnsiString, Request> &counters, const AnsiString &key, time_t now, int verdict)
      {
         if (verdict == 0)
            return 0;

         const Request &request = counters[key];
         const int wait = verdict == 1 ? (int) (ResendSeconds - (now - request.last))
                                       : (int) (HourSeconds - (now - request.hour));
         return wait < 1 ? 1 : wait;
      }

      // The budget for a verification message - one this server SENDS because an
      // account asked it to, at an address the account named. Counted twice over,
      // and the second half is not decoration: per DESTINATION, so that one
      // address cannot be written to over and over, and per ACCOUNT, so that
      // naming a fresh address each time is not a way round the first. Both are
      // one a minute and six an hour, which nobody setting their own recovery
      // address will ever meet.
      //
      // The destination shares its counter with the password-reset link, because
      // the two are the same capability. The account's own is kept under a key of
      // its own, so that a person proving an address and a stranger asking for a
      // reset of that person's mailbox do not spend each other's budget.
      //
      // 0 when the message may go; otherwise the seconds to wait, with the
      // sentence in problem. charge moves both counters, and only when both would
      // have let the message through; a peek moves neither.
      int MessageBudget(const String &mailbox, const String &destination, bool charge, String &problem)
      {
         const time_t now = (time_t) Now();
         const AnsiString account = AnsiString("acct:") + Utf8(Lower(mailbox));
         const AnsiString sentTo = Utf8(Lower(destination));

         std::lock_guard<std::mutex> guard(state_mutex);
         Prune(now);

         int accountVerdict = Peek(by_address, account, now, true, RequestsPerAddressPerHour);
         int sentToVerdict = Peek(by_address, sentTo, now, true, RequestsPerAddressPerHour);

         if (charge && accountVerdict == 0 && sentToVerdict == 0)
         {
            accountVerdict = Charge(by_address, account, now, true, RequestsPerAddressPerHour);
            sentToVerdict = Charge(by_address, sentTo, now, true, RequestsPerAddressPerHour);
         }

         const int accountWait = WaitFor(by_address, account, now, accountVerdict);
         const int sentToWait = WaitFor(by_address, sentTo, now, sentToVerdict);
         const int wait = accountWait > sentToWait ? accountWait : sentToWait;
         if (wait == 0)
            return 0;

         problem = (accountVerdict == 1 || sentToVerdict == 1)
            ? String(_T("a recovery message went out less than a minute ago; try again shortly"))
            : String(_T("as many recovery messages have gone out in the last hour as may; try again later"));
         return wait;
      }

      // A peer that is this machine itself, which on a normal installation means
      // the webmail or the reverse proxy in front of it rather than a person.
      // The v4-mapped form counted as loopback too, as HttpsClient does it.
      bool IsLoopback(const IPAddress &peer)
      {
         const boost::asio::ip::address address = peer.GetAddress();
         if (address.is_v6() && address.to_v6().is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).is_loopback();
         return address.is_loopback();
      }

      // The postmaster of the account's domain, or postmaster@<domain>: the
      // address a recovery message comes from, as the quarantine digest and the
      // send-as code come from it, so that a reply nobody should send at least
      // reaches somebody who could answer it.
      String SenderFor(std::shared_ptr<const Account> account)
      {
         if (!account)
            return String();
         const String domainName = StringParser::ExtractDomain(account->GetAddress());
         std::shared_ptr<const Domain> domain = domainName.IsEmpty()
            ? std::shared_ptr<const Domain>()
            : CacheContainer::Instance()->GetDomain(domainName);
         String from = domain ? domain->GetPostmaster() : String();
         from.Trim();
         if (from.IsEmpty() || from.Find(_T("@")) <= 0)
            from = _T("postmaster@") + domainName;
         return from;
      }

      // One message into the delivery queue, from the null sender: generated
      // mail whose bounce must not go back to the postmaster as though a person
      // had written it, and must not loop. Exactly the shape
      // QuarantineDigest::SendFor and ConfidentialMessages::SendCode use.
      bool Queue(const String &to, const String &from, const AnsiString &subject, const AnsiString &body,
                 const char *where)
      {
         AnsiString content;
         content += "From: <" + HeaderSafe(from) + ">\r\n";
         content += "To: <" + HeaderSafe(to) + ">\r\n";
         content += "Subject: " + subject + "\r\n";
         content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
         content += "Message-ID: " + HeaderSafe(Utilities::GenerateMessageID()) + "\r\n";

         // RFC 3834: generated mail. Auto-Submitted stops another server's
         // vacation responder answering it, and X-Auto-Response-Suppress does
         // the same for the Microsoft clients that read it.
         content += "Auto-Submitted: auto-generated\r\n";
         content += "X-Auto-Response-Suppress: All\r\n";
         content += "MIME-Version: 1.0\r\n";
         content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
         content += "Content-Transfer-Encoding: 8bit\r\n";
         content += "\r\n";
         content += body;

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         message->SetState(Message::Delivering);
         message->SetFromAddress(_T(""));

         const String fileName = MessageStore::WorkingFile(message);
         if (!FileUtilities::WriteToFile(fileName, content))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7000, where,
               "A recovery message could not be written to the queue for " + to + ", and has NOT been sent.");
            return false;
         }

         message->SetSize(MessageStore::FileSize(fileName));

         bool recipientOk = false;
         RecipientParser recipientParser;
         recipientParser.CreateMessageRecipientList(to, message->GetRecipients(), recipientOk);

         if (message->GetRecipients()->GetCount() == 0 || !PersistentMessage::SaveObject(message))
         {
            FileUtilities::DeleteFile(fileName);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7001, where,
               "A recovery message could not be queued for " + to + ", and has NOT been sent.");
            return false;
         }

         Application::Instance()->SubmitPendingEmail();
         return true;
      }

      // A new one-shot link for an account, of one kind. The clear token is
      // answered; only its digest is stored. Whatever the account held of that
      // kind is removed first, so a second request replaces the first rather
      // than leaving two live links behind.
      AnsiString IssueToken(__int64 accountId, int kind, const String &address, __int64 seconds)
      {
         SQLCommand remove("delete from hm_recoverytokens where rtaccountid = @ACCOUNTID and rtkind = @KIND");
         remove.AddParameter("@ACCOUNTID", accountId);
         remove.AddParameter("@KIND", (int) kind);
         Execute(remove);

         const AnsiString token = RandomHex(TokenBytes);
         if (token.IsEmpty())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7002, "AccountRecovery::IssueToken",
               "No entropy was available for an account recovery link, so none was made and nothing has been sent.");
            return AnsiString();
         }

         SQLStatement statement;
         statement.SetTable("hm_recoverytokens");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("rtid");
         statement.AddColumnInt64("rtaccountid", accountId);
         statement.AddColumn("rtkind", (long) kind);
         statement.AddColumn("rttoken", String(Sha256(token)));
         statement.AddColumn("rtaddress", address);
         statement.AddColumnInt64("rtexpires", Now() + seconds);
         statement.AddColumnInt64("rtused", 0);
         statement.AddColumnInt64("rtcreated", Now());

         if (!Application::Instance()->GetDBManager()->Execute(statement))
            return AnsiString();

         return token;
      }

      int Clamp(int value, int low, int high, int fallback)
      {
         if (value <= 0)
            return fallback;
         if (value < low)
            return low;
         return value > high ? high : value;
      }
   }

   bool
   AccountRecovery::Enabled()
   {
      return Configuration::Instance()->GetAccountRecoveryEnabled();
   }

   int
   AccountRecovery::ResetLinkMinutes()
   {
      return Clamp(Configuration::Instance()->GetRecoveryResetLinkMinutes(), 5, 1440, 30);
   }

   int
   AccountRecovery::VerifyLinkHours()
   {
      return Clamp(Configuration::Instance()->GetRecoveryVerifyLinkHours(), 1, 168, 24);
   }

   String
   AccountRecovery::BaseUrl()
   {
      return ConfidentialMessages::BaseUrl();
   }

   String
   AccountRecovery::Mask(const String &address)
   {
      const String lowered = Lower(address);
      const int at = lowered.Find(_T("@"));
      if (at <= 0)
         return String();

      String masked;
      masked += lowered[0];
      for (int i = 1; i < at; i++)
         masked += _T('.');
      masked += _T('@');

      // Each label of the domain keeps its first character; the last label - the
      // top-level one - is left whole, because "com" and "co.uk" tell nobody
      // anything and hiding them makes the address unrecognisable to its owner.
      const String domain = lowered.Mid(at + 1);
      std::vector<String> labels = StringParser::SplitString(domain, _T("."));
      for (size_t i = 0; i < labels.size(); i++)
      {
         if (i > 0)
            masked += _T('.');
         const String &label = labels[i];
         if (label.IsEmpty())
            continue;
         if (i + 1 == labels.size())
         {
            masked += label;
            continue;
         }
         masked += label[0];
         for (int c = 1; c < label.GetLength(); c++)
            masked += _T('.');
      }

      return masked;
   }

   bool
   AccountRecovery::Get(__int64 accountId, Address &address)
   {
      Address empty;
      address = empty;

      SQLCommand command("select acraddress, acrverified, acrcreated, acrupdated from hm_accountrecovery where acraccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset)
         return false;
      if (recordset->IsEOF())
         return true;

      address.exists = true;
      address.address = recordset->GetStringValue("acraddress");
      address.verified = recordset->GetLongValue("acrverified") != 0;
      address.created = recordset->GetInt64Value("acrcreated");
      address.updated = recordset->GetInt64Value("acrupdated");
      return true;
   }

   bool
   AccountRecovery::Set(std::shared_ptr<const Account> account, const String &wanted, String &problem, int *retryAfter)
   {
      problem = String();
      if (retryAfter)
         *retryAfter = 0;

      if (!account)
      {
         problem = _T("internal error");
         return false;
      }

      if (!Enabled())
      {
         problem = _T("this server does not offer a recovery address");
         return false;
      }

      // Checked before the row is written rather than after: an address stored
      // with no way to send its confirmation is an address that can never be
      // proved, and the caller would have been told the whole thing failed
      // while the row sat there.
      if (BaseUrl().IsEmpty())
      {
         problem = _T("this server has no public address configured, so no link can be sent; an administrator sets confidential_base_url");
         return false;
      }

      const String address = Lower(wanted);

      if (address.IsEmpty() || !StringParser::IsValidEmailAddress(address))
      {
         problem = _T("that is not an e-mail address");
         return false;
      }

      if (address.GetLength() > 254)
      {
         problem = _T("the address is at most 254 characters");
         return false;
      }

      // The account's own address is not a recovery address: the whole point is
      // a mailbox somebody can still read when they cannot open this one.
      if (Lower(account->GetAddress()) == address)
      {
         problem = _T("a recovery address is another mailbox, not this one");
         return false;
      }

      // Asked of the budget before the row is written, and not charged here:
      // SendVerification below charges it. A row written and a confirmation
      // refused would leave an address that counts for nothing until somebody
      // asks again, which is the state this check exists to avoid.
      {
         String tooSoon;
         const int wait = MessageBudget(account->GetAddress(), address, false, tooSoon);
         if (wait > 0)
         {
            problem = tooSoon;
            if (retryAfter)
               *retryAfter = wait;
            return false;
         }
      }

      Address stored;
      if (!Get(account->GetID(), stored))
      {
         problem = _T("the recovery address could not be read");
         return false;
      }

      SQLStatement statement;
      statement.SetTable("hm_accountrecovery");

      if (stored.exists)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         String where;
         where.Format(_T("acraccountid = %I64d"), account->GetID());
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("acrid");
         statement.AddColumnInt64("acraccountid", account->GetID());
         statement.AddColumnInt64("acrcreated", Now());
      }

      statement.AddColumn("acraddress", address);

      // Always unverified: a change re-proves. Otherwise the proof of the old
      // address would carry over to a new one nobody has answered from.
      statement.AddColumn("acrverified", 0L);
      statement.AddColumnInt64("acrupdated", Now());

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         problem = _T("the recovery address could not be saved");
         return false;
      }

      // Whatever links the old address had are dead the moment it is replaced.
      SQLCommand remove("delete from hm_recoverytokens where rtaccountid = @ACCOUNTID");
      remove.AddParameter("@ACCOUNTID", account->GetID());
      Execute(remove);

      Address fresh;
      fresh.exists = true;
      fresh.address = address;
      fresh.verified = false;

      return SendVerification(account, fresh, problem, retryAfter);
   }

   bool
   AccountRecovery::SendVerification(std::shared_ptr<const Account> account, const Address &stored, String &problem,
                                     int *retryAfter)
   {
      problem = String();
      if (retryAfter)
         *retryAfter = 0;

      if (!account || !stored.exists || stored.address.IsEmpty())
      {
         problem = _T("there is no recovery address to prove");
         return false;
      }

      const String base = BaseUrl();
      if (base.IsEmpty())
      {
         problem = _T("this server has no public address configured, so no link can be sent; an administrator sets confidential_base_url");
         return false;
      }

      // Charged before the link is made, and against the address the message
      // would go to. This is the one gate between "an account may name any
      // address" and "an account may post mail at any address as often as it
      // likes": the route above is behind a credential, but a credential the
      // account holder has, or a stolen one, is not a reason to leave the
      // server's own outbound mail unmetered.
      {
         String tooSoon;
         const int wait = MessageBudget(account->GetAddress(), stored.address, true, tooSoon);
         if (wait > 0)
         {
            problem = tooSoon;
            if (retryAfter)
               *retryAfter = wait;

            LOG_APPLICATION("AccountRecovery: a verification message for " + account->GetAddress() +
               " was refused by the per-address budget; none was sent.");
            return false;
         }
      }

      const int hours = VerifyLinkHours();
      const AnsiString token = IssueToken(account->GetID(), KindVerifyAddress, stored.address, (__int64) hours * 3600);
      if (token.IsEmpty())
      {
         problem = _T("the verification link could not be made");
         return false;
      }

      const String from = SenderFor(account);

      AnsiString subject = "Confirm this address for " + HeaderSafe(account->GetAddress());

      AnsiString body;
      body += "The mailbox " + HeaderSafe(account->GetAddress()) + " has named this address as the one to\r\n";
      body += "use if its password is ever forgotten.\r\n";
      body += "\r\n";
      body += "To confirm it, open this link and press the button on the page:\r\n";
      body += "\r\n";
      body += "   " + HeaderSafe(base) + "/recovery/verify?token=" + token + "\r\n";
      body += "\r\n";
      AnsiString life;
      life.Format("The link works once and stops working in %d hour%hs.\r\n", hours, hours == 1 ? "" : "s");
      body += life;
      body += "\r\n";
      body += "If this was not you, do nothing: the address is not used for anything until\r\n";
      body += "it is confirmed, and nobody can confirm it without this message.\r\n";
      body += "\r\n";
      body += "Nothing here asks for a password, and the link points at the mail server\r\n";
      body += "itself.\r\n";

      if (!Queue(stored.address, from, subject, body, "AccountRecovery::SendVerification"))
      {
         problem = _T("the verification message could not be sent; try again in a minute");
         return false;
      }

      LOG_APPLICATION("AccountRecovery: a verification message for a recovery address was sent for " + account->GetAddress() + ".");
      return true;
   }

   bool
   AccountRecovery::Clear(__int64 accountId)
   {
      SQLCommand tokens("delete from hm_recoverytokens where rtaccountid = @ACCOUNTID");
      tokens.AddParameter("@ACCOUNTID", accountId);
      Execute(tokens);

      SQLCommand command("delete from hm_accountrecovery where acraccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);
      return Execute(command);
   }

   bool
   AccountRecovery::FindToken(const AnsiString &token, Token &found)
   {
      Token empty;
      found = empty;

      if (!IsTokenText(token))
         return false;

      SQLCommand command("select rtid, rtaccountid, rtkind, rtaddress, rtexpires, rtused, rtcreated from hm_recoverytokens where rttoken = @TOKEN");
      command.AddParameter("@TOKEN", String(Sha256(token)));

      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset || recordset->IsEOF())
         return false;

      found.id = recordset->GetInt64Value("rtid");
      found.account_id = recordset->GetInt64Value("rtaccountid");
      found.kind = (int) recordset->GetLongValue("rtkind");
      found.address = recordset->GetStringValue("rtaddress");
      found.expires = recordset->GetInt64Value("rtexpires");
      found.used = recordset->GetInt64Value("rtused");
      found.created = recordset->GetInt64Value("rtcreated");
      return true;
   }

   bool
   AccountRecovery::VerifyAddress(const AnsiString &token, String &address, String &problem)
   {
      address = String();
      problem = String();

      Token found;
      if (!FindToken(token, found) || found.kind != KindVerifyAddress)
      {
         problem = _T("this link does not work. It may already have been used.");
         return false;
      }

      // Spent first, whatever happens next: a link works once, and a failure
      // after this point must not leave one that can be tried again.
      SQLCommand spend("delete from hm_recoverytokens where rtid = @ID");
      spend.AddParameter("@ID", found.id);
      Execute(spend);

      if (found.used != 0 || found.expires <= Now())
      {
         problem = _T("this link has expired. Ask for another from the webmail's Security page.");
         return false;
      }

      Address stored;
      if (!Get(found.account_id, stored) || !stored.exists || Lower(stored.address) != Lower(found.address))
      {
         problem = _T("this link is for an address that is no longer the recovery address.");
         return false;
      }

      SQLCommand command("update hm_accountrecovery set acrverified = 1, acrupdated = @NOW where acraccountid = @ACCOUNTID");
      command.AddParameter("@NOW", Now());
      command.AddParameter("@ACCOUNTID", found.account_id);
      if (!Execute(command))
      {
         problem = _T("the address could not be confirmed; try the link again in a minute.");
         return false;
      }

      address = stored.address;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(found.account_id);
      LOG_APPLICATION("AccountRecovery: the recovery address of " +
         (account ? account->GetAddress() : String(_T("an account"))) + " was confirmed.");
      return true;
   }

   AccountRecovery::ResetOutcome
   AccountRecovery::BeginReset(const String &mailbox, const IPAddress &peer)
   {
      if (!Enabled())
         return ResetUnavailable;

      const String base = BaseUrl();
      if (base.IsEmpty())
         return ResetUnavailable;

      const String wanted = Lower(mailbox);
      if (wanted.IsEmpty() || wanted.GetLength() > 254)
         return ResetNothingToDo;

      // Both counters are charged before anything is looked up, and for a name
      // that is no mailbox exactly as for one that is: the work done, and so
      // the time taken, must not depend on whether the address exists.
      {
         const time_t now = (time_t) Now();
         std::lock_guard<std::mutex> guard(state_mutex);
         Prune(now);

         const int address = Charge(by_address, Utf8(wanted), now, true, RequestsPerAddressPerHour);

         // The per-client counter is skipped for a peer that is this machine
         // itself. The REST listener reads no forwarded-for header - nothing
         // here does - so on the ordinary way of publishing a webmail, with TLS
         // terminated in front of hMailServer, every request arrives from the
         // proxy and twenty an hour would be a ceiling on the whole server that
         // the twenty-first person would meet as the same silent 202. The
         // per-address half, which is the half that protects a person's mailbox
         // from being mailed at, is charged either way.
         //
         // A reverse proxy on another host still shares one bucket, and
         // AccountRecovery.md says so, because that is an operator's business to
         // know rather than something this file can detect.
         const int client = IsLoopback(peer)
            ? 0
            : Charge(by_client, AnsiString(peer.ToString()), now, false, RequestsPerClientPerHour);
         const int worst = address > client ? address : client;
         if (worst == 1)
            return ResetTooSoon;
         if (worst == 2)
            return ResetTooMany;
      }

      std::shared_ptr<const Account> account = PasswordValidator::LookupAccount(wanted);
      if (!account || !account->GetActive())
         return ResetNothingToDo;

      // A directory-linked account has no password here to reset: the directory
      // holds it, and this server only ever asks the directory whether one is
      // right. The same silent answer, because saying so would tell a stranger
      // which accounts are linked.
      if (account->GetIsAD())
         return ResetNothingToDo;

      Address stored;
      if (!Get(account->GetID(), stored) || !stored.exists || !stored.verified || stored.address.IsEmpty())
         return ResetNothingToDo;

      const int minutes = ResetLinkMinutes();
      const AnsiString token = IssueToken(account->GetID(), KindPasswordReset, stored.address, (__int64) minutes * 60);
      if (token.IsEmpty())
         return ResetFailed;

      const String from = SenderFor(account);

      AnsiString subject = "Reset the password for " + HeaderSafe(account->GetAddress());

      AnsiString body;
      body += "Somebody asked to reset the password of the mailbox " + HeaderSafe(account->GetAddress()) + ".\r\n";
      body += "\r\n";
      body += "If it was you, open this link and choose a new password:\r\n";
      body += "\r\n";
      body += "   " + HeaderSafe(base) + "/portal/reset?token=" + token + "\r\n";
      body += "\r\n";
      AnsiString life;
      life.Format("The link works once and stops working in %d minute%hs.\r\n", minutes, minutes == 1 ? "" : "s");
      body += life;
      body += "\r\n";
      body += "If it was not you, do nothing. The password has not changed, and it cannot\r\n";
      body += "change without this link. This message went to the recovery address of that\r\n";
      body += "mailbox and nowhere else, so nobody else was told the request was made.\r\n";
      body += "\r\n";
      body += "Nothing here asks for your old password, and the link points at the mail\r\n";
      body += "server itself.\r\n";

      if (!Queue(stored.address, from, subject, body, "AccountRecovery::BeginReset"))
         return ResetFailed;

      LOG_APPLICATION("AccountRecovery: a password-reset link was sent to the recovery address of " +
         account->GetAddress() + ", asked for from " + String(peer.ToString()) + ".");
      return ResetSent;
   }

   AccountRecovery::CompleteOutcome
   AccountRecovery::CompleteReset(const AnsiString &token, const String &newPassword,
                                  String &problem, int &status, String &mailbox)
   {
      problem = String();
      status = 400;
      mailbox = String();

      if (!Enabled())
      {
         problem = _T("this server does not offer a self-service password reset");
         status = 403;
         return CompleteRefused;
      }

      Token found;
      if (!FindToken(token, found) || found.kind != KindPasswordReset || found.used != 0 || found.expires <= Now())
      {
         problem = _T("this link does not work any more. Ask for another from the sign-in page.");
         status = 410;
         return CompleteBadToken;
      }

      if (newPassword.IsEmpty())
      {
         problem = _T("choose a new password");
         status = 400;
         return CompleteRefused;
      }

      // The two properties BeginReset refuses to issue a link for, asked again
      // here: a link lives half an hour, and an administrator who disables an
      // account in the middle of that - which is exactly what an administrator
      // does when a compromise is reported - must stop the reset rather than
      // watch it complete and raise an alert saying it did. A hash written for
      // an account that has since been linked to a directory is a hash nothing
      // will ever consult.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(found.account_id);
      if (!account || account->GetID() == 0 || !account->GetActive() || account->GetIsAD())
      {
         problem = _T("this link does not work any more. Ask for another from the sign-in page.");
         status = 410;
         return CompleteBadToken;
      }

      mailbox = account->GetAddress();

      // The address the link went to has to still be the account's proved
      // recovery address. Someone who changes it after asking for a link, or an
      // administrator who clears it, ends every link that was in flight.
      Address stored;
      if (!Get(account->GetID(), stored) || !stored.exists || !stored.verified ||
          Lower(stored.address) != Lower(found.address))
      {
         problem = _T("this link does not work any more. Ask for another from the sign-in page.");
         status = 410;
         return CompleteBadToken;
      }

      // The same rules the webmail's own password form applies, in the same
      // order: the policy, then the reuse history, then the breach corpus, and
      // only then the history record. A password chosen here must not be one an
      // administrator could not have set.
      String policyFailure;
      if (!PasswordPolicy::IsAcceptable(account->GetAddress(), newPassword, policyFailure))
      {
         problem = policyFailure;
         status = 400;
         return CompleteRefused;
      }

      if (PasswordHistory::IsReuse(account, newPassword))
      {
         problem = _T("this password has been used recently on this account; choose one that has not");
         status = 409;
         return CompleteRefused;
      }

      {
         String breachRefusal;
         AnsiString breachReason;
         int breachStatus = 0;
         if (!PasswordBreachCheck::IsAcceptable(account->GetAddress(), newPassword, breachRefusal, breachReason, breachStatus))
         {
            problem = breachRefusal;
            status = breachStatus;
            return CompleteRefused;
         }
      }

      // The link is spent here, before the password is written: a reset that
      // fails on the write must not leave a link somebody can try again, and a
      // reset that succeeds must not leave one at all.
      SQLCommand spend("delete from hm_recoverytokens where rtid = @ID");
      spend.AddParameter("@ID", found.id);
      Execute(spend);

      std::shared_ptr<Account> mutableAccount = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(mutableAccount, account->GetID()) || mutableAccount->GetID() == 0)
      {
         problem = _T("the account could not be read; try again in a minute");
         status = 500;
         return CompleteFailed;
      }

      PasswordHistory::Record(mutableAccount);

      int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      mutableAccount->SetPassword(Crypt::Instance()->EnCrypt(newPassword, (Crypt::EncryptionType) preferredHashAlgorithm));
      mutableAccount->SetPasswordEncryption(preferredHashAlgorithm);
      mutableAccount->SetPasswordChanged(Time::GetCurrentDateTime());

      String saveError;
      if (!PersistentAccount::SaveObject(mutableAccount, saveError, PersistenceModeNormal))
      {
         problem = saveError.IsEmpty() ? String(_T("the account could not be saved")) : saveError;
         status = 500;
         return CompleteFailed;
      }

      // Every browser signed in to this account is signed out. A password is
      // reset most urgently when somebody else may have had it, and a session
      // they already hold would outlive the change.
      RestApiServer::RevokeSessionsForAccount(account->GetID());

      SendPasswordChanged(account, stored.address, _T("through the recovery address"));

      LOG_APPLICATION("AccountRecovery: the password of " + account->GetAddress() +
         " was reset through its recovery address, and every browser session of the account was ended.");

      // The administrator hears about it. A self-service reset is the one
      // change that turns a mailbox somebody could read into a mailbox
      // somebody else can, so it is an occurrence rather than a state, seeded
      // on, with no cool-down: two in an hour are two alerts.
      AlertManager::Instance()->RaiseOccurrence(AlertManager::ConditionPasswordReset, ErrorManager::Medium,
         Formatter::Format("The password of {0} was reset by its own recovery address.", account->GetAddress()),
         Formatter::Format("A reset link sent to the confirmed recovery address of {0} was used to choose a new password. "
                           "Every browser session of the account was ended, and both the mailbox and the recovery address were told. "
                           "If the account holder did not do this, the recovery address is the mailbox to look at: it is exactly as "
                           "strong as whoever can read it. Clear it on the account's page to stop this happening again.",
                           account->GetAddress()));

      status = 200;
      return CompleteDone;
   }

   void
   AccountRecovery::SendPasswordChanged(std::shared_ptr<const Account> account, const String &recoveryAddress,
                                        const String &how)
   {
      if (!account)
         return;

      const String from = SenderFor(account);

      AnsiString subject = "The password for " + HeaderSafe(account->GetAddress()) + " was changed";

      AnsiString body;
      body += "The password of the mailbox " + HeaderSafe(account->GetAddress()) + " was changed ";
      body += HeaderSafe(how) + ".\r\n";
      body += "\r\n";
      body += "Everything that was signed in to the mailbox in a browser has been signed out.\r\n";
      body += "\r\n";
      body += "If this was you, there is nothing to do. If it was not, tell whoever runs this\r\n";
      body += "mail server at once: whoever can read the recovery address can do this again.\r\n";

      Queue(account->GetAddress(), from, subject, body, "AccountRecovery::SendPasswordChanged");

      if (!recoveryAddress.IsEmpty() && Lower(recoveryAddress) != Lower(account->GetAddress()))
         Queue(recoveryAddress, from, subject, body, "AccountRecovery::SendPasswordChanged");
   }

   bool
   AccountRecovery::GetCodes(__int64 accountId, CodeState &state)
   {
      CodeState empty;
      state = empty;

      SQLCommand command("select rkused, rkcreated from hm_recoverycodes where rkaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         state.total++;
         if (recordset->GetInt64Value("rkused") == 0)
            state.unused++;
         const __int64 created = recordset->GetInt64Value("rkcreated");
         if (created > state.created)
            state.created = created;
         recordset->MoveNext();
      }

      return true;
   }

   bool
   AccountRecovery::Generate(__int64 accountId, std::vector<String> &codes)
   {
      codes.clear();

      // Crockford-style base32 without I, L, O, U, 0 or 1 - the alphabet an app
      // password is drawn from, for the same reason: these are read off paper
      // and typed back, and a character that can be read as another is a code
      // that does not work for a reason nobody can see.
      static const wchar_t alphabet[] = L"23456789ABCDEFGHJKMNPQRSTVWXYZ";
      const int alphabetSize = 30;

      Crypt::EncryptionType type = (Crypt::EncryptionType) IniFileSettings::Instance()->GetPreferredHashAlgorithm();

      // ETNone would store the clear text, and the preferred algorithm is a
      // configured integer, so it is checked rather than trusted - exactly as
      // AppPassword::SetPassword checks it.
      if (type != Crypt::ETPBKDF2 && type != Crypt::ETArgon2id && type != Crypt::ETScrypt &&
          type != Crypt::ETSHA256 && type != Crypt::ETMD5)
      {
         type = Crypt::ETArgon2id;
      }

      std::vector<String> made;

      for (int index = 0; index < CodesPerAccount; index++)
      {
         String code;
         for (int character = 0; character < CodeCharacters; character++)
         {
            unsigned char value = 0;
            if (RAND_bytes(&value, 1) != 1)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::High, 7003, "AccountRecovery::Generate",
                  "The random number generator failed, so no recovery codes were made. Nothing has been stored.");
               return false;
            }

            // 256 is not a multiple of 30, so a plain modulo would make the
            // first sixteen symbols slightly likelier than the rest. Rejection
            // sampling costs nothing here.
            while (value >= 240)
            {
               if (RAND_bytes(&value, 1) != 1)
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::High, 7003, "AccountRecovery::Generate",
                     "The random number generator failed, so no recovery codes were made. Nothing has been stored.");
                  return false;
               }
            }

            if (character > 0 && character % 5 == 0)
               code += _T("-");

            code += alphabet[value % alphabetSize];
         }

         made.push_back(code);
      }

      // The ten new rows are written FIRST and the old set goes only once all
      // ten are there. The identity column is increasing, so the highest id the
      // account holds now is the line between the two sets: everything at or
      // below it is the old set, everything above it is the new one.
      //
      // The point is what an insert that fails halfway leaves behind. Deleting
      // first and inserting after would leave the account with the old codes
      // gone and some number of new ones stored that nobody has ever seen -
      // working credentials the account holder could not possibly have - while
      // the route answered "nothing has changed". This way a failure rolls back
      // to the set the account already had, which is what that sentence says.
      // A max() over no rows is one row holding NULL rather than no rows at all,
      // and GetIsNull is how that is read - the shape AlertManager and AuditTrail
      // settled on after SQL Server Compact refused the scalar-subquery form.
      __int64 lastOfOldSet = 0;
      {
         SQLCommand highest("select max(rkid) as rkhighest from hm_recoverycodes where rkaccountid = @ACCOUNTID");
         highest.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Open(highest);
         if (!recordset)
            return false;
         if (!recordset->IsEOF() && !recordset->GetIsNull("rkhighest"))
            lastOfOldSet = recordset->GetInt64Value("rkhighest");
      }

      const __int64 now = Now();

      auto undo = [accountId, lastOfOldSet]()
      {
         SQLCommand remove("delete from hm_recoverycodes where rkaccountid = @ACCOUNTID and rkid > @AFTER");
         remove.AddParameter("@ACCOUNTID", accountId);
         remove.AddParameter("@AFTER", lastOfOldSet);
         Execute(remove);
      };

      for (const String &code : made)
      {
         SQLStatement statement;
         statement.SetTable("hm_recoverycodes");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("rkid");
         statement.AddColumnInt64("rkaccountid", accountId);
         statement.AddColumn("rkhash", Crypt::Instance()->EnCrypt(code, type));
         statement.AddColumn("rkencryption", (long) type);
         statement.AddColumnInt64("rkused", 0);
         statement.AddColumnInt64("rkcreated", now);

         if (!Application::Instance()->GetDBManager()->Execute(statement))
         {
            undo();
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7004, "AccountRecovery::Generate",
               "A recovery code could not be written, so the whole set was rolled back and the account keeps the codes it had.");
            return false;
         }
      }

      SQLCommand remove("delete from hm_recoverycodes where rkaccountid = @ACCOUNTID and rkid <= @UPTO");
      remove.AddParameter("@ACCOUNTID", accountId);
      remove.AddParameter("@UPTO", lastOfOldSet);
      if (lastOfOldSet != 0 && !Execute(remove))
      {
         undo();
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7005, "AccountRecovery::Generate",
            "The previous set of recovery codes could not be removed, so the new set was rolled back and the account keeps the codes it had.");
         return false;
      }

      codes = made;
      return true;
   }

   __int64
   AccountRecovery::CheckCode(__int64 accountId, const String &code)
   {
      String wanted = code;
      wanted.Trim();
      wanted.ToUpper();
      if (wanted.IsEmpty() || wanted.GetLength() > 64)
         return 0;

      // A code written without its separator is the same code. Nothing else is
      // forgiven: the alphabet has no characters a reader could confuse.
      String stripped;
      for (int i = 0; i < wanted.GetLength(); i++)
      {
         if (wanted[i] != '-' && wanted[i] != ' ')
            stripped += wanted[i];
      }

      String spelled;
      for (int i = 0; i < stripped.GetLength(); i++)
      {
         if (i > 0 && i % 5 == 0)
            spelled += _T("-");
         spelled += stripped[i];
      }

      SQLCommand command("select rkid, rkhash, rkencryption from hm_recoverycodes where rkaccountid = @ACCOUNTID and rkused = 0");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset)
         return 0;

      __int64 matched = 0;

      while (!recordset->IsEOF())
      {
         const __int64 id = recordset->GetInt64Value("rkid");
         const String hash = recordset->GetStringValue("rkhash");
         const Crypt::EncryptionType type = (Crypt::EncryptionType) recordset->GetLongValue("rkencryption");

         // Every row is compared, with no early exit, so the work done is a
         // function of how many codes are left rather than of how much of a
         // guess happened to be right.
         if (!hash.IsEmpty() && Crypt::Instance()->Validate(spelled, hash, type) && matched == 0)
            matched = id;

         recordset->MoveNext();
      }

      return matched;
   }

   bool
   AccountRecovery::SpendCode(__int64 id)
   {
      if (id == 0)
         return false;

      SQLCommand command("update hm_recoverycodes set rkused = @NOW where rkid = @ID and rkused = 0");
      command.AddParameter("@NOW", Now());
      command.AddParameter("@ID", id);
      return Execute(command);
   }

   bool
   AccountRecovery::ConsumeCode(__int64 accountId, const String &code, int &remaining)
   {
      remaining = 0;

      const __int64 id = CheckCode(accountId, code);
      if (id == 0 || !SpendCode(id))
         return false;

      CodeState state;
      if (GetCodes(accountId, state))
         remaining = state.unused;

      return true;
   }

   bool
   AccountRecovery::Sweep()
   {
      // Expired links, and nothing else. Rows whose account is gone are taken by
      // the foreign key's ON DELETE CASCADE in all seven schema scripts, so a
      // sweep for them here would be a second implementation of something the
      // database already does.
      //
      // A link that has been used is not swept because there is none to sweep: a
      // spent token is DELETED where it is spent (VerifyAddress, CompleteReset)
      // rather than marked, which is why rtused is always 0 on a row that is
      // still here. The column is kept for the row a future hand might mark
      // instead, and the clause below would then catch it.
      SQLCommand tokens("delete from hm_recoverytokens where rtexpires <= @NOW or rtused <> 0");
      tokens.AddParameter("@NOW", Now());
      return Execute(tokens);
   }

   bool
   AccountRecovery::ClearCodes(__int64 accountId)
   {
      SQLCommand command("delete from hm_recoverycodes where rkaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);
      return Execute(command);
   }
}
