// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ConfidentialMessages.h"
#include "Unicode.h"
#include "Utilities.h"
#include "FileUtilities.h"
#include "Time.h"
#include "Hashing/HashCreator.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../../SMTP/RecipientParser.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <boost/filesystem.hpp>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int TokenBytes = 32;
      const __int64 KeepRowsSeconds = 30 * 24 * 60 * 60;
      const time_t CodeSeconds = 10 * 60;
      const time_t ResendSeconds = 60;
      const time_t HourSeconds = 60 * 60;
      const time_t DaySeconds = 24 * 60 * 60;
      const int CodeTries = 5;
      const int PasscodeTries = 10;
      const time_t PasscodePauseSeconds = 15 * 60;
      const time_t GrantSeconds = 30 * 60;
      const size_t MapCeiling = 10000;

      const char *Columns = "cfid, cfaccountid, cftoken, cfsubject, cfsender, cfrecipients, cffile, cfpasscode, cfcreated, cfexpires, cfopened, cfopenedby, cfrevoked";

      // One address asked about for one message. Made for every address asked,
      // recipient or not: a stranger's has a code nothing matches, and the same
      // resend limit, the same tries and the same lock, so the page's answers
      // do not say which addresses the message went to. Kept a day for the
      // per-address count; the code itself lapses after CodeSeconds.
      struct Code
      {
         AnsiString code;      // empty for an address the message did not go to
         time_t first = 0;     // when the address was first asked about
         time_t sent = 0;      // when the last code was issued
         int issued = 0;       // codes issued since first
         int wrong = 0;        // wrong answers to the current code
      };

      // One message's budget for codes, whoever asks: how many this hour, and
      // how many wrong answers in all. Only a token that names a row ever gets
      // one, so the map holds at most one entry per live message.
      struct Budget
      {
         int codes = 0;
         time_t hour = 0;
         int wrong = 0;
         time_t last = 0;
      };

      struct Guesses
      {
         int wrong = 0;
         time_t since = 0;
      };

      struct GrantRow
      {
         AnsiString token;
         String by;
         time_t expires = 0;
      };

      std::mutex state_mutex;
      std::map<AnsiString, Code> codes;          // token|address
      std::map<AnsiString, Budget> budgets;      // token
      std::map<AnsiString, Guesses> guesses;     // token
      std::map<AnsiString, GrantRow> grants;     // SHA-256 of the grant

      String Root()
      {
         return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Confidential"));
      }

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

      bool Same(const AnsiString &left, const AnsiString &right)
      {
         if (left.IsEmpty() || left.GetLength() != right.GetLength())
            return false;
         return CRYPTO_memcmp(left.c_str(), right.c_str(), (size_t) left.GetLength()) == 0;
      }

      String Clip(const String &text, int length)
      {
         String clipped = text;
         for (int i = 0; i < clipped.GetLength(); i++)
         {
            if (clipped[i] < 0x20)
               clipped.SetAt(i, _T(' '));
         }
         clipped.Trim();
         return clipped.GetLength() > length ? clipped.Mid(0, length) : clipped;
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

      String HtmlText(const String &text)
      {
         String out;
         for (int i = 0; i < text.GetLength(); i++)
         {
            wchar_t c = text[i];
            if (c == '&') out += _T("&amp;");
            else if (c == '<') out += _T("&lt;");
            else if (c == '>') out += _T("&gt;");
            else if (c == '"') out += _T("&quot;");
            else out += c;
         }
         return out;
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
         static const String months[] = { _T("January"), _T("February"), _T("March"), _T("April"), _T("May"), _T("June"), _T("July"), _T("August"), _T("September"), _T("October"), _T("November"), _T("December") };
         String text;
         text.Format(_T("%d %s %04d, %02d:%02d UTC"), parts.tm_mday, months[parts.tm_mon % 12].c_str(), parts.tm_year + 1900, parts.tm_hour, parts.tm_min);
         return text;
      }

      ConfidentialMessages::Entry ReadEntry(std::shared_ptr<DALRecordset> recordset)
      {
         ConfidentialMessages::Entry entry;
         entry.id = recordset->GetInt64Value("cfid");
         entry.account_id = recordset->GetInt64Value("cfaccountid");
         entry.token = AnsiString(recordset->GetStringValue("cftoken"));
         entry.subject = recordset->GetStringValue("cfsubject");
         entry.sender = recordset->GetStringValue("cfsender");
         const String recipients = recordset->GetStringValue("cfrecipients");
         std::vector<String> parts = StringParser::SplitString(recipients, _T(","));
         for (const String &part : parts)
         {
            String address = Lower(part);
            if (!address.IsEmpty())
               entry.recipients.push_back(address);
         }
         entry.file = recordset->GetStringValue("cffile");
         entry.passcode_hash = AnsiString(recordset->GetStringValue("cfpasscode"));
         entry.created = recordset->GetInt64Value("cfcreated");
         entry.expires = recordset->GetInt64Value("cfexpires");
         entry.opened = recordset->GetInt64Value("cfopened");
         entry.opened_by = recordset->GetStringValue("cfopenedby");
         entry.revoked = recordset->GetLongValue("cfrevoked") != 0;
         return entry;
      }

      // Called with state_mutex held: what has lapsed goes, and a map that has
      // grown past its ceiling anyway is emptied rather than left to grow. The
      // codes map, which anyone with a link can add to (one entry an address,
      // within the message's hourly budget), first sheds the entries whose code
      // has lapsed and only clears if that was not enough.
      void Prune(time_t now)
      {
         for (auto it = codes.begin(); it != codes.end();)
            it = now - it->second.first > DaySeconds ? codes.erase(it) : std::next(it);
         for (auto it = budgets.begin(); it != budgets.end();)
            it = now - it->second.last > (time_t) ConfidentialMessages::MaximumSeconds ? budgets.erase(it) : std::next(it);
         for (auto it = guesses.begin(); it != guesses.end();)
            it = now - it->second.since > PasscodePauseSeconds ? guesses.erase(it) : std::next(it);
         for (auto it = grants.begin(); it != grants.end();)
            it = it->second.expires <= now ? grants.erase(it) : std::next(it);

         if (codes.size() > MapCeiling)
         {
            for (auto it = codes.begin(); it != codes.end();)
               it = now - it->second.sent > CodeSeconds ? codes.erase(it) : std::next(it);
            if (codes.size() > MapCeiling)
               codes.clear();
         }
         if (budgets.size() > MapCeiling)
            budgets.clear();
         if (guesses.size() > MapCeiling)
            guesses.clear();
         if (grants.size() > MapCeiling)
            grants.clear();
      }

      // Whether the account exists, decided by a query that answered: 1 when
      // it does, 0 when the database says it does not, -1 when the database
      // did not answer - which is never taken for "gone".
      int AccountExists(__int64 accountId)
      {
         SQLCommand command("select accountid from hm_accounts where accountid = @ID");
         command.AddParameter("@ID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;
         return recordset->IsEOF() ? 0 : 1;
      }
   }

   bool
   ConfidentialMessages::Entry::IsRecipient(const String &address) const
   {
      const String wanted = Lower(address);
      if (wanted.IsEmpty())
         return false;
      for (const String &recipient : recipients)
      {
         if (recipient == wanted)
            return true;
      }
      return false;
   }

   AnsiString
   ConfidentialMessages::HashPasscode(const String &passcode)
   {
      return HashCreator::GeneratePBKDF2(Utf8(passcode));
   }

   String
   ConfidentialMessages::BaseUrl()
   {
      String url = Configuration::Instance()->GetConfidentialBaseUrl();
      url.Trim();
      if (url.IsEmpty())
         url = Configuration::Instance()->GetQuarantineDigestBaseUrl();
      url.Trim();
      if (url.IsEmpty())
         url = Configuration::Instance()->GetImportOAuthRedirectBaseUrl();
      url.Trim();

      String lowered = url;
      lowered.ToLower();
      if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
         return String();
      while (!url.IsEmpty() && url[url.GetLength() - 1] == '/')
         url = url.Mid(0, url.GetLength() - 1);
      return url;
   }

   String
   ConfidentialMessages::DisplayName(const String &fromHeader)
   {
      String name = fromHeader;
      const int bracket = name.Find(_T("<"));
      if (bracket > 0)
         name = name.Mid(0, bracket);
      else if (bracket == 0)
      {
         name = name.Mid(1);
         const int close = name.Find(_T(">"));
         if (close >= 0)
            name = name.Mid(0, close);
      }
      name.Replace(_T("\""), _T(""));
      name.Trim();
      return name.IsEmpty() ? fromHeader : name;
   }

   bool
   ConfidentialMessages::Create(std::shared_ptr<const Account> account, const String &sourceFile, const String &subject, const String &sender,
                                const std::vector<String> &recipients, __int64 expires, const String &passcode, Entry &made)
   {
      made = Entry();
      made.token = RandomHex(TokenBytes);
      if (made.token.IsEmpty())
         return false;

      String directory;
      directory.Format(_T("%I64d"), account->GetID());
      const String relative = FileUtilities::Combine(directory, String(made.token) + _T(".eml"));
      const String target = FileUtilities::Combine(Root(), relative);

      // The log names the account and the directory, never the file: the token
      // in its name is the link, and a log is shipped to places a link should
      // not go.
      if (!FileUtilities::Copy(sourceFile, target, true))
      {
         LOG_APPLICATION("ConfidentialMessages: the message from " + account->GetAddress() + " could not be kept under " + FileUtilities::Combine(Root(), directory) + ".");
         return false;
      }

      String joined;
      std::set<String> seen;
      for (const String &recipient : recipients)
      {
         const String address = Lower(recipient);
         if (address.IsEmpty() || !seen.insert(address).second)
            continue;
         joined += (joined.IsEmpty() ? String() : String(_T(","))) + address;
         made.recipients.push_back(address);
      }

      made.account_id = account->GetID();
      made.subject = Clip(subject, 255);
      made.sender = Clip(sender, 255);
      made.file = relative;
      made.passcode_hash = passcode.IsEmpty() ? AnsiString() : HashPasscode(passcode);
      if (!passcode.IsEmpty() && made.passcode_hash.IsEmpty())
      {
         FileUtilities::DeleteFile(target);
         return false;
      }
      made.created = (__int64) time(nullptr);
      made.expires = expires;

      SQLStatement statement(SQLStatement::STInsert, "hm_confidential");
      statement.AddColumnInt64("cfaccountid", made.account_id);
      statement.AddColumn("cftoken", String(made.token));
      statement.AddColumn("cfsubject", made.subject);
      statement.AddColumn("cfsender", made.sender);
      statement.AddColumn("cfrecipients", joined, RecipientsMaximum);
      statement.AddColumn("cffile", relative);
      statement.AddColumn("cfpasscode", String(made.passcode_hash));
      statement.AddColumnInt64("cfcreated", made.created);
      statement.AddColumnInt64("cfexpires", expires);
      statement.AddColumnInt64("cfopened", 0);
      statement.AddColumn("cfopenedby", String());
      statement.AddColumn("cfrevoked", (long) 0);
      statement.SetIdentityColumn("cfid");

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
      {
         FileUtilities::DeleteFile(target);
         return false;
      }
      made.id = id;

      String line;
      line.Format(_T("ConfidentialMessages: %s sent a message that expires (row %I64d)."), account->GetAddress().c_str(), id);
      LOG_APPLICATION(line);
      return true;
   }

   void
   ConfidentialMessages::Discard(const Entry &made)
   {
      if (made.id <= 0)
         return;
      SQLCommand remove("delete from hm_confidential where cfid = @ID");
      remove.AddParameter("@ID", made.id);
      Application::Instance()->GetDBManager()->Execute(remove);

      const String path = PathOf(made);
      if (!path.IsEmpty() && FileUtilities::Exists(path))
         FileUtilities::DeleteFile(path);
   }

   std::shared_ptr<Message>
   ConfidentialMessages::CopyKeptToFolder(const String &keptPath, const String &fromAddress, std::shared_ptr<IMAPFolder> folder)
   {
      std::shared_ptr<Message> none;
      if (!folder || keptPath.IsEmpty() || !FileUtilities::Exists(keptPath))
         return none;

      std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(folder->GetAccountID());
      if (!owner)
         return none;

      std::shared_ptr<Message> copy = std::shared_ptr<Message>(new Message());
      copy->SetAccountID(owner->GetID());
      copy->SetFolderID(folder->GetID());
      copy->SetFromAddress(fromAddress);
      copy->SetState(Message::Delivered);

      const String fileName = PersistentMessage::GetFileName(owner, copy);
      if (!FileUtilities::Copy(keptPath, fileName, true))
         return none;

      copy->SetSize((int) FileUtilities::FileSize(fileName));
      return copy;
   }

   bool
   ConfidentialMessages::WriteNotice(std::shared_ptr<Message> message, const String &fileName, const String &fromHeader, const String &toHeader, const String &ccHeader,
                                     const String &subject, const String &senderName, const AnsiString &link, __int64 expires, bool passcode)
   {
      FileUtilities::DeleteFile(fileName);

      const String when = UtcText(expires);
      const String url = String(link);
      String text;
      text += senderName + _T(" sent you a confidential message.\r\n\r\n");
      text += _T("It is kept on their mail server rather than sent, and it can be read there until ") + when + _T(":\r\n\r\n");
      text += url + _T("\r\n\r\n");
      text += passcode
         ? String(_T("To read it, ask for a code sent to this address, or enter the passcode the sender gives you.\r\n"))
         : String(_T("To read it, ask for a code sent to this address.\r\n"));
      text += _T("It cannot be downloaded, forwarded or printed from that page. After ") + when + _T(" it cannot be read at all.\r\n");

      String html;
      html += _T("<p><b>") + HtmlText(senderName) + _T("</b> sent you a confidential message.</p>");
      html += _T("<p>It is kept on their mail server rather than sent, and it can be read there until ") + HtmlText(when) + _T(".</p>");
      html += _T("<p><a href=\"") + HtmlText(url) + _T("\">Read the message</a></p>");
      html += passcode
         ? String(_T("<p style=\"color:#555\">To read it, ask for a code sent to this address, or enter the passcode the sender gives you. It cannot be downloaded, forwarded or printed from that page.</p>"))
         : String(_T("<p style=\"color:#555\">To read it, ask for a code sent to this address. It cannot be downloaded, forwarded or printed from that page.</p>"));

      MessageData data;
      data.LoadFromMessage(fileName, message);
      data.SetCharset(_T("utf-8"));
      data.SetFrom(fromHeader);
      data.SetTo(toHeader);
      if (!ccHeader.IsEmpty())
         data.SetCC(ccHeader);
      data.SetSubject(subject);
      data.SetBody(text);
      data.SetHTMLBody(html);
      data.SetSentTime(Time::GetCurrentMimeDate());
      data.GenerateMessageID();

      String expiresHeader;
      expiresHeader.Format(_T("expires=%I64d"), expires);
      data.SetFieldValue(_T("X-hMailServer-Confidential"), expiresHeader);

      return data.Write(fileName);
   }

   bool
   ConfidentialMessages::FindByToken(const AnsiString &token, Entry &entry)
   {
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_confidential where cftoken = @TOKEN"));
      command.AddParameter("@TOKEN", String(token));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      entry = ReadEntry(recordset);
      return entry.token == token;
   }

   bool
   ConfidentialMessages::List(__int64 accountId, std::vector<Entry> &entries)
   {
      entries.clear();
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_confidential where cfaccountid = @ACCOUNTID order by cfid desc"));
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

   String
   ConfidentialMessages::PathOf(const Entry &entry)
   {
      return entry.file.IsEmpty() ? String() : FileUtilities::Combine(Root(), entry.file);
   }

   int
   ConfidentialMessages::CountLive(__int64 accountId)
   {
      SQLCommand command("select count(*) as livecount from hm_confidential where cfaccountid = @ACCOUNTID and cfexpires > @NOW and cfrevoked = 0 and cffile <> @EMPTY");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NOW", (__int64) time(nullptr));
      command.AddParameter("@EMPTY", String());
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return -1;
      return (int) recordset->GetInt64Value("livecount");
   }

   __int64
   ConfidentialMessages::KeptBytes(__int64 accountId)
   {
      String directory;
      directory.Format(_T("%I64d"), accountId);
      const String path = FileUtilities::Combine(Root(), directory);
      if (!FileUtilities::DirectoryExists(path))
         return 0;

      __int64 total = 0;
      boost::system::error_code error_code;
      for (boost::filesystem::directory_iterator item(boost::filesystem::path(path.c_str()), error_code);
           item != boost::filesystem::directory_iterator(); item.increment(error_code))
      {
         if (error_code)
            break;
         if (!boost::filesystem::is_regular_file(item->path(), error_code))
            continue;
         const uintmax_t size = boost::filesystem::file_size(item->path(), error_code);
         if (!error_code)
            total += (__int64) size;
      }
      return total;
   }

   // The kept copy, deleted; true when it is gone. A delete that fails - the
   // file open in a reader's page, a scanner holding it - is reported once and
   // left for the next sweep, which selects the row again for as long as it
   // still names the file.
   static bool DeleteKept(__int64 id, const String &file, const char *where)
   {
      const String path = FileUtilities::Combine(Root(), file);
      if (!FileUtilities::Exists(path) || FileUtilities::DeleteFile(path))
         return true;

      String text;
      text.Format(_T("The kept copy of the message that expires with row %I64d could not be deleted; it will be tried again on the next sweep."), id);
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6784, where, text);
      return false;
   }

   bool
   ConfidentialMessages::Revoke(__int64 accountId, __int64 id)
   {
      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_confidential where cfid = @ID and cfaccountid = @ACCOUNTID"));
      command.AddParameter("@ID", id);
      command.AddParameter("@ACCOUNTID", accountId);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      const Entry entry = ReadEntry(recordset);

      // The row is what the page refuses on, so it goes first; the file's
      // column is cleared only once the file is gone, so the sweep - which
      // selects every withdrawn row still naming a file - retries a delete
      // that fails here.
      SQLCommand update("update hm_confidential set cfrevoked = 1 where cfid = @ID and cfaccountid = @ACCOUNTID");
      update.AddParameter("@ID", id);
      update.AddParameter("@ACCOUNTID", accountId);
      if (!Application::Instance()->GetDBManager()->Execute(update))
         return false;

      if (!entry.file.IsEmpty() && DeleteKept(entry.id, entry.file, "ConfidentialMessages::Revoke"))
      {
         SQLCommand clear("update hm_confidential set cffile = @EMPTY where cfid = @ID");
         clear.AddParameter("@EMPTY", String());
         clear.AddParameter("@ID", id);
         Application::Instance()->GetDBManager()->Execute(clear);
      }

      std::lock_guard<std::mutex> guard(state_mutex);
      for (auto it = grants.begin(); it != grants.end();)
         it = it->second.token == entry.token ? grants.erase(it) : std::next(it);
      return true;
   }

   void
   ConfidentialMessages::MarkOpened(const Entry &entry, const String &by)
   {
      if (entry.opened != 0)
         return;
      SQLCommand command("update hm_confidential set cfopened = @NOW, cfopenedby = @BY where cfid = @ID and cfopened = 0");
      command.AddParameter("@NOW", (__int64) time(nullptr));
      command.AddParameter("@BY", Clip(by, 255));
      command.AddParameter("@ID", entry.id);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   ConfidentialMessages::CodeResult
   ConfidentialMessages::SendCode(const Entry &entry, const String &address)
   {
      const String to = Lower(address);
      const bool recipient = StringParser::IsValidEmailAddress(to) && entry.IsRecipient(to);

      // Every limit is applied, and every entry made, before the address is
      // acted on, and the same way whether or not it is a recipient's: the
      // answer must not depend on that.
      const time_t now = time(nullptr);
      const AnsiString key = entry.token + "|" + Utf8(to);
      AnsiString code;
      {
         std::lock_guard<std::mutex> guard(state_mutex);
         Prune(now);

         Budget &budget = budgets[entry.token];
         budget.last = now;
         if (budget.wrong >= WrongCodesMaximum)
            return CodeClosed;
         if (now - budget.hour >= HourSeconds)
         {
            budget.hour = now;
            budget.codes = 0;
         }
         if (budget.codes >= CodesPerHour)
            return CodeTooMany;

         auto found = codes.find(key);
         if (found != codes.end() && now - found->second.sent < ResendSeconds)
            return CodeTooSoon;
         if (found != codes.end() && found->second.issued >= CodesPerAddressPerDay)
            return CodeTooMany;

         if (recipient)
         {
            unsigned char random[4];
            if (RAND_bytes(random, sizeof(random)) != 1)
               return CodeFailed;
            const unsigned int number = ((unsigned int) random[0] << 24 | (unsigned int) random[1] << 16 | (unsigned int) random[2] << 8 | random[3]) % 1000000u;
            code.Format("%06u", number);
         }

         budget.codes++;
         Code &pending = codes[key];
         if (pending.first == 0)
            pending.first = now;
         pending.issued++;
         pending.code = code;
         pending.sent = now;
         pending.wrong = 0;
      }

      // An address the message did not go to: an entry like any other, and no
      // mail. What the caller shows is the same page either way.
      if (!recipient)
         return CodeSent;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(entry.account_id);
      const String domainName = account ? StringParser::ExtractDomain(account->GetAddress()) : String();
      std::shared_ptr<const Domain> domain = domainName.IsEmpty() ? std::shared_ptr<const Domain>() : CacheContainer::Instance()->GetDomain(domainName);
      String from = domain ? domain->GetPostmaster() : String();
      from.Trim();
      if (from.IsEmpty() || from.Find(_T("@")) <= 0)
         from = _T("postmaster@") + domainName;

      AnsiString content;
      content += "From: <" + HeaderSafe(from) + ">\r\n";
      content += "To: <" + HeaderSafe(to) + ">\r\n";
      content += "Subject: Your code to read a confidential message\r\n";
      content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
      content += "Message-ID: " + HeaderSafe(Utilities::GenerateMessageID()) + "\r\n";
      content += "Auto-Submitted: auto-generated\r\n";
      content += "X-Auto-Response-Suppress: All\r\n";
      content += "MIME-Version: 1.0\r\n";
      content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      content += "Content-Transfer-Encoding: 8bit\r\n";
      content += "\r\n";
      content += "Somebody asked to read the confidential message \"" + HeaderSafe(entry.subject) + "\" from " + HeaderSafe(entry.sender) + ".\r\n";
      content += "\r\n";
      content += "The code is: " + code + "\r\n";
      content += "\r\n";
      content += "It works for ten minutes. If it was not you, ignore this message: nobody can read it without the code.\r\n";

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetState(Message::Delivering);
      message->SetFromAddress(_T(""));

      const String fileName = PersistentMessage::GetFileName(message);
      if (!FileUtilities::WriteToFile(fileName, content))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6782, "ConfidentialMessages::SendCode",
            "The code to read a confidential message could not be written to the queue for " + to + ".");
         return CodeFailed;
      }

      message->SetSize(FileUtilities::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(to, message->GetRecipients(), recipientOk);

      if (message->GetRecipients()->GetCount() == 0 || !PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6783, "ConfidentialMessages::SendCode",
            "The code to read a confidential message could not be queued for " + to + ".");
         return CodeFailed;
      }

      Application::Instance()->SubmitPendingEmail();
      return CodeSent;
   }

   ConfidentialMessages::CheckResult
   ConfidentialMessages::CheckCode(const Entry &entry, const String &address, const AnsiString &code)
   {
      const time_t now = time(nullptr);
      const AnsiString key = entry.token + "|" + Utf8(Lower(address));

      std::lock_guard<std::mutex> guard(state_mutex);
      Prune(now);

      // Once the message's wrong answers are spent, no code opens it - a
      // stranger's entry and a recipient's alike.
      Budget &budget = budgets[entry.token];
      budget.last = now;
      if (budget.wrong >= WrongCodesMaximum)
         return CheckClosed;

      // The entry stays for its daily count; a lapsed code is one nothing
      // matches, and a stranger's entry never held one.
      auto found = codes.find(key);
      if (found == codes.end() || now - found->second.sent > CodeSeconds)
         return CheckLocked;

      if (found->second.wrong >= CodeTries)
         return CheckLocked;

      if (Same(code, found->second.code))
      {
         found->second.code = AnsiString();
         return CheckRight;
      }

      found->second.wrong++;
      budget.wrong++;
      if (budget.wrong >= WrongCodesMaximum)
         return CheckClosed;
      return found->second.wrong >= CodeTries ? CheckLocked : CheckWrong;
   }

   ConfidentialMessages::CheckResult
   ConfidentialMessages::CheckPasscode(const Entry &entry, const String &passcode)
   {
      const time_t now = time(nullptr);

      {
         std::lock_guard<std::mutex> guard(state_mutex);
         Prune(now);
         Guesses &tries = guesses[entry.token];
         if (tries.wrong >= PasscodeTries)
            return CheckLocked;
      }

      // The derivation is slow by design, and done with nothing held: a page
      // checking one message's passcode must not stall every other page.
      const bool right = !entry.passcode_hash.IsEmpty() && HashCreator::ValidatePBKDF2(Utf8(passcode), entry.passcode_hash);

      std::lock_guard<std::mutex> guard(state_mutex);
      if (right)
      {
         guesses.erase(entry.token);
         return CheckRight;
      }

      Guesses &tries = guesses[entry.token];
      if (tries.wrong == 0)
         tries.since = now;
      tries.wrong++;
      return tries.wrong >= PasscodeTries ? CheckLocked : CheckWrong;
   }

   AnsiString
   ConfidentialMessages::Grant(const Entry &entry, const String &by)
   {
      const AnsiString grant = RandomHex(TokenBytes);
      if (grant.IsEmpty())
         return grant;

      const time_t now = time(nullptr);
      std::lock_guard<std::mutex> guard(state_mutex);
      Prune(now);
      GrantRow &row = grants[Sha256(grant)];
      row.token = entry.token;
      row.by = by;
      row.expires = now + GrantSeconds;
      return grant;
   }

   bool
   ConfidentialMessages::Granted(const Entry &entry, const AnsiString &grant, String &by)
   {
      if (grant.GetLength() != TokenBytes * 2)
         return false;

      const time_t now = time(nullptr);
      std::lock_guard<std::mutex> guard(state_mutex);
      auto found = grants.find(Sha256(grant));
      if (found == grants.end())
         return false;
      if (found->second.expires <= now)
      {
         grants.erase(found);
         return false;
      }
      if (found->second.token != entry.token)
         return false;
      by = found->second.by;
      return true;
   }

   int
   ConfidentialMessages::Sweep(bool orphanDirectories)
   {
      const __int64 now = (__int64) time(nullptr);

      struct Lapsed
      {
         __int64 id;
         String file;
         bool remove_row;
      };
      std::vector<Lapsed> lapsed;

      // No recordset is the database not answering, and a sweep that cannot
      // read the rows must not act on the files: nothing below runs.
      SQLCommand command("select cfid, cffile, cfexpires, cfrevoked from hm_confidential where cfexpires <= @NOW or cfrevoked = 1");
      command.AddParameter("@NOW", now);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;
      while (!recordset->IsEOF())
      {
         Lapsed item;
         item.id = recordset->GetInt64Value("cfid");
         item.file = recordset->GetStringValue("cffile");
         item.remove_row = recordset->GetInt64Value("cfexpires") + KeepRowsSeconds <= now;
         if (!item.file.IsEmpty() || item.remove_row)
            lapsed.push_back(item);
         recordset->MoveNext();
      }

      int ended = 0;
      for (const Lapsed &item : lapsed)
      {
         // The column is cleared only once the file is gone: a row that still
         // names its file is selected again next minute, so a delete that
         // fails now is tried again then, for as long as it takes.
         if (!item.file.IsEmpty() && DeleteKept(item.id, item.file, "ConfidentialMessages::Sweep"))
         {
            SQLCommand update("update hm_confidential set cffile = @EMPTY where cfid = @ID");
            update.AddParameter("@EMPTY", String());
            update.AddParameter("@ID", item.id);
            Application::Instance()->GetDBManager()->Execute(update);
            ended++;
         }

         if (item.remove_row && (item.file.IsEmpty() || !FileUtilities::Exists(FileUtilities::Combine(Root(), item.file))))
         {
            SQLCommand remove("delete from hm_confidential where cfid = @ID");
            remove.AddParameter("@ID", item.id);
            Application::Instance()->GetDBManager()->Execute(remove);
         }
      }

      // A deleted account's rows went with it; its directory is removed here,
      // from the retention task only. Only a real directory named by a number
      // is ever looked at, and a symlink never - and "gone" is what the
      // database said when asked, never a lookup that came back empty because
      // the database was away: an outage must not delete every account's copies.
      const String root = Root();
      if (orphanDirectories && FileUtilities::DirectoryExists(root))
      {
         boost::system::error_code error_code;
         const boost::filesystem::path rootPath(root.c_str());
         std::vector<boost::filesystem::path> gone;
         for (boost::filesystem::directory_iterator item(rootPath, error_code);
              item != boost::filesystem::directory_iterator(); item.increment(error_code))
         {
            if (error_code)
               break;
            const boost::filesystem::path current = item->path();
            if (boost::filesystem::is_symlink(current, error_code) || !boost::filesystem::is_directory(current, error_code))
               continue;
            const std::string name = current.filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || name.size() > 18)
               continue;
            const __int64 accountId = std::stoll(name);
            if (accountId > 0 && AccountExists(accountId) == 0)
               gone.push_back(current);
         }
         for (const boost::filesystem::path &directory : gone)
            boost::filesystem::remove_all(directory, error_code);
      }

      if (ended > 0)
      {
         String text;
         text.Format(_T("ConfidentialMessages: %d kept message(s) deleted, expired or withdrawn."), ended);
         LOG_APPLICATION(text);
      }
      return ended;
   }
}
