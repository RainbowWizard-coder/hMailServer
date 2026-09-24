// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SenderAddresses.h"
#include "Unicode.h"
#include "Utilities.h"
#include "FileUtilities.h"
#include "Time.h"
#include "Parsing/StringParser.h"
#include "../AntiSpam/DMARC/DMARC.h"
#include "../Application/Application.h"
#include "../Application/ErrorManager.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../../SMTP/RecipientParser.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <ctime>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const time_t CodeSeconds = 60 * 60;
      const time_t ResendSeconds = 60;
      const int CodeTries = 5;

      struct Pending
      {
         AnsiString code;
         time_t sent = 0;
         int wrong = 0;
      };

      boost::mutex pendingLock;
      std::map<__int64, Pending> pending;

      String Lower(const String &text)
      {
         String lower = text;
         lower.ToLower();
         return lower;
      }

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
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

      SenderAddresses::Entry ReadEntry(std::shared_ptr<DALRecordset> recordset)
      {
         SenderAddresses::Entry entry;
         entry.id = recordset->GetInt64Value("said");
         entry.address = recordset->GetStringValue("saaddress");
         entry.name = recordset->GetStringValue("saname");
         entry.verified = recordset->GetLongValue("saverified") == 1;
         entry.created = recordset->GetStringValue("sacreated");
         return entry;
      }
   }

   bool
   SenderAddresses::List(__int64 accountId, std::vector<Entry> &entries)
   {
      entries.clear();
      SQLCommand command("select said, saaddress, saname, saverified, sacreated from hm_senderaddresses where saaccountid = @ACCOUNTID order by saaddress");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         entries.push_back(ReadEntry(recordset));
         recordset->MoveNext();
      }
      return true;
   }

   bool
   SenderAddresses::Find(__int64 accountId, __int64 id, Entry &entry)
   {
      SQLCommand command("select said, saaddress, saname, saverified, sacreated from hm_senderaddresses where saaccountid = @ACCOUNTID and said = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      entry = ReadEntry(recordset);
      return true;
   }

   int
   SenderAddresses::Count(__int64 accountId)
   {
      SQLCommand command("select count(*) as c from hm_senderaddresses where saaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return 0;
      return (int) recordset->GetLongValue("c");
   }

   bool
   SenderAddresses::IsVerified(__int64 accountId, const String &address)
   {
      // SQL Compact refuses a comparison with a parameter longer than the column.
      const String wanted = Lower(address);
      if (accountId <= 0 || wanted.IsEmpty() || wanted.GetLength() > 255)
         return false;

      SQLCommand command("select said from hm_senderaddresses where saaccountid = @ACCOUNTID and saaddress = @ADDRESS and saverified = 1");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ADDRESS", wanted);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      return recordset && !recordset->IsEOF();
   }

   bool
   SenderAddresses::Add(__int64 accountId, const String &address, const String &name, __int64 &id)
   {
      SQLStatement statement(SQLStatement::STInsert, "hm_senderaddresses");
      statement.AddColumnInt64("saaccountid", accountId);
      statement.AddColumn("saaddress", Lower(address));
      statement.AddColumn("saname", name);
      statement.AddColumn("saverified", 0);
      statement.AddColumn("sacreated", Time::GetCurrentDateTime());
      statement.SetIdentityColumn("said");

      __int64 made = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &made) || made <= 0)
         return false;

      id = made;
      return true;
   }

   bool
   SenderAddresses::MarkVerified(__int64 accountId, __int64 id)
   {
      SQLCommand command("update hm_senderaddresses set saverified = 1 where saaccountid = @ACCOUNTID and said = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   SenderAddresses::Delete(__int64 accountId, __int64 id)
   {
      {
         boost::lock_guard<boost::mutex> guard(pendingLock);
         pending.erase(id);
      }

      SQLCommand command("delete from hm_senderaddresses where saaccountid = @ACCOUNTID and said = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   AnsiString
   SenderAddresses::NewCode(__int64 id, int &waitSeconds)
   {
      waitSeconds = 0;
      const time_t now = time(nullptr);

      boost::lock_guard<boost::mutex> guard(pendingLock);

      // The map holds no more than the codes of the last hour.
      for (auto it = pending.begin(); it != pending.end();)
      {
         if (now - it->second.sent > CodeSeconds)
            it = pending.erase(it);
         else
            ++it;
      }

      auto found = pending.find(id);
      if (found != pending.end() && now - found->second.sent < ResendSeconds)
      {
         waitSeconds = (int) (ResendSeconds - (now - found->second.sent));
         return "";
      }

      unsigned char bytes[4];
      if (RAND_bytes(bytes, sizeof(bytes)) != 1)
         return "";
      const unsigned int value = ((unsigned int) bytes[0] << 24 | (unsigned int) bytes[1] << 16 | (unsigned int) bytes[2] << 8 | bytes[3]) % 1000000;

      Pending entry;
      entry.code.Format("%06u", value);
      entry.sent = now;
      pending[id] = entry;
      return entry.code;
   }

   SenderAddresses::CodeOutcome
   SenderAddresses::CheckCode(__int64 id, const AnsiString &code, int &attemptsLeft)
   {
      attemptsLeft = 0;
      const time_t now = time(nullptr);

      boost::lock_guard<boost::mutex> guard(pendingLock);
      auto found = pending.find(id);
      if (found == pending.end() || now - found->second.sent > CodeSeconds)
      {
         if (found != pending.end())
            pending.erase(found);
         return CodeExpired;
      }

      if (found->second.wrong >= CodeTries)
         return CodeLocked;

      AnsiString typed = code;
      typed.Trim();
      if (typed.GetLength() == found->second.code.GetLength() &&
          CRYPTO_memcmp(typed.c_str(), found->second.code.c_str(), (size_t) typed.GetLength()) == 0)
      {
         pending.erase(found);
         return CodeRight;
      }

      found->second.wrong++;
      attemptsLeft = CodeTries - found->second.wrong;
      return attemptsLeft > 0 ? CodeWrong : CodeLocked;
   }

   bool
   SenderAddresses::SendCode(std::shared_ptr<const Account> account, const Entry &entry, const AnsiString &code)
   {
      String accountAddress = account->GetAddress();
      const String domainName = StringParser::ExtractDomain(accountAddress);
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);

      String from = domain ? domain->GetPostmaster() : String();
      from.Trim();
      if (from.IsEmpty() || from.Find(_T("@")) <= 0)
         from = _T("postmaster@") + domainName;

      AnsiString content;
      content += "From: <" + HeaderSafe(from) + ">\r\n";
      content += "To: <" + HeaderSafe(entry.address) + ">\r\n";
      content += "Subject: Your code to send mail as " + HeaderSafe(entry.address) + "\r\n";
      content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
      content += "Message-ID: " + HeaderSafe(Utilities::GenerateMessageID()) + "\r\n";
      content += "Auto-Submitted: auto-generated\r\n";
      content += "X-Auto-Response-Suppress: All\r\n";
      content += "MIME-Version: 1.0\r\n";
      content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      content += "Content-Transfer-Encoding: 8bit\r\n";
      content += "\r\n";
      content += HeaderSafe(accountAddress) + " asked to send mail as " + HeaderSafe(entry.address) + " from the mail server of " + HeaderSafe(domainName) + ".\r\n";
      content += "\r\n";
      content += "The code is: " + code + "\r\n";
      content += "\r\n";
      content += "It works for an hour. If you did not ask for this, ignore this message:\r\n";
      content += "nothing can be sent as this address from there without the code.\r\n";

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);

      // The null sender: generated mail, whose bounce must go nowhere.
      message->SetFromAddress(_T(""));

      const String fileName = MessageStore::WorkingFile(message);
      if (!FileUtilities::WriteToFile(fileName, content))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6780, "SenderAddresses::SendCode",
            "The code for " + accountAddress + " to send as " + entry.address + " could not be written to the queue.");
         return false;
      }

      message->SetSize(MessageStore::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(entry.address, message->GetRecipients(), recipientOk);

      if (message->GetRecipients()->GetCount() == 0 || !PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6781, "SenderAddresses::SendCode",
            "The code for " + accountAddress + " to send as " + entry.address + " could not be queued.");
         return false;
      }

      Application::Instance()->SubmitPendingEmail();
      LOG_APPLICATION("SenderAddresses: sent " + accountAddress + " a code to prove " + entry.address + ".");
      return true;
   }

   AnsiString
   SenderAddresses::PolicyOf(const String &address)
   {
      const String domain = StringParser::ExtractDomain(address);
      if (domain.IsEmpty())
         return "";

      DMARC dmarc;
      DMARC::Evaluation evaluation;
      std::vector<AnsiString> noDkim;
      dmarc.Verify(domain, domain, true, noDkim, &evaluation);
      if (!evaluation.policy_found)
         return "";

      // A subdomain found its policy at the organizational domain: sp= governs
      // it, falling back to p= when sp= is absent.
      String word = evaluation.p;
      if (evaluation.policy_domain.CompareNoCase(domain) != 0 && !evaluation.sp.IsEmpty())
         word = evaluation.sp;
      word.ToLower();
      if (word != _T("reject") && word != _T("quarantine") && word != _T("none"))
         return "";
      return AnsiString(word);
   }
}
