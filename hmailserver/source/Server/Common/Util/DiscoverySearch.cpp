// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See DiscoverySearch.h: the runner, the search and the export.

#include "StdAfx.h"
#include "DiscoverySearch.h"
#include "DiscoveryStore.h"
#include "DiscoveryHolds.h"
#include "RecoverableStore.h"
#include "PersonExport.h"
#include "ZipWriter.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "FileUtilities.h"
#include "Time.h"
#include "Encoding/ModifiedUTF7.h"
#include "SealedMessage.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/Attachments.h"
#include "../BO/Attachment.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentMessageIndex.h"
#include "../Persistence/PersistentArchiveIndex.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
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
      // Rows read from hm_messages and hm_archiveindex at a time.
      const int PageRows = 500;

      // How often the search's row is told how far it has got.
      const int ProgressEvery = 200;

      // ZipWriter refuses an archive past 4 GB (2^32 bytes); an export is
      // refused before anything is written when its messages twice over - once
      // as EML, once in an mbox - with room for the headers, SHA256SUMS and the
      // manifest would pass this.
      const unsigned __int64 ZipCeiling = 4000000000ULL;

      // The words, the custodians and the length of a query: the query is
      // recorded whole on the audit trail, whose detail is 2,000 characters.
      const size_t MaxCustodians = 100;
      const size_t MaxWords = 50;
      const int MaxWordLength = 200;

      // One runner at a time: the flag is up while a runner is queued or at
      // work. The generation changes when the maintenance queue the runner ran
      // on is taken away (DiscoverySearch::Forget), so that a runner the queue
      // never started, or one cut off by the stop, does not keep every later
      // search queued - and so that a runner still finishing on the old queue
      // neither takes another job nor puts down a flag that is no longer its.
      std::atomic<bool> runner_running(false);
      std::atomic<unsigned int> runner_generation(0);

      // The runner's hold on the flag, put down on every way out of RunQueue -
      // an exception, an interruption - while it is still this runner's.
      class RunnerFlag
      {
      public:
         explicit RunnerFlag(unsigned int generation) :
            generation_(generation),
            held_(true)
         {
         }

         ~RunnerFlag()
         {
            PutDown();
         }

         bool Current() const
         {
            return runner_generation.load() == generation_;
         }

         void PutDown()
         {
            if (held_ && Current())
               runner_running = false;

            held_ = false;
         }

         // Up again for another turn, unless another runner took it in the
         // moment it was down, or the queue it ran on has gone.
         bool TakeUp()
         {
            if (!Current())
               return false;

            bool expected = false;
            if (!runner_running.compare_exchange_strong(expected, true))
               return false;

            held_ = true;
            return true;
         }

      private:
         const unsigned int generation_;
         bool held_;
      };

      class DiscoveryRunTask : public Task
      {
      public:
         explicit DiscoveryRunTask(unsigned int generation) :
            Task(_T("Discovery runner")),
            generation_(generation)
         {
         }

      protected:
         void DoWork() override
         {
            // Queued before the maintenance queue it was queued on was taken
            // away (DiscoverySearch::Forget): the flag it was queued under is
            // gone, and a runner of the new queue may be at work already.
            if (runner_generation.load() != generation_)
               return;

            DiscoverySearch::RunQueue();
         }

      private:
         const unsigned int generation_;
      };

      AnsiString Utf8(const String &text)
      {
         AnsiString out;
         Unicode::WideToMultiByte(text, out);
         return out;
      }

      String Wide(const std::string &utf8)
      {
         String out;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), out);
         return out;
      }

      // A JSON string, quoted, from UTF-8.
      AnsiString QuotedUtf8(const AnsiString &utf8)
      {
         static const char digits[] = "0123456789abcdef";

         AnsiString out = "\"";
         const char *bytes = utf8.c_str();
         const int length = utf8.GetLength();

         for (int i = 0; i < length; i++)
         {
            const unsigned char c = (unsigned char) bytes[i];
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  out += "\\u00";
                  out += digits[c >> 4];
                  out += digits[c & 15];
               }
               else
               {
                  out += (char) c;
               }
            }
         }

         out += "\"";
         return out;
      }

      AnsiString Quoted(const String &text)
      {
         return QuotedUtf8(Utf8(text));
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString Bool(bool value)
      {
         return value ? AnsiString("true") : AnsiString("false");
      }

      String Lower(const String &value)
      {
         String lowered = value;
         lowered.ToLower();
         return lowered;
      }

      bool IsDay(const String &text)
      {
         if (text.GetLength() != 10)
            return false;

         for (int i = 0; i < 10; i++)
         {
            const wchar_t c = text[i];
            if (i == 4 || i == 7)
            {
               if (c != '-')
                  return false;
            }
            else if (c < '0' || c > '9')
            {
               return false;
            }
         }

         return true;
      }

      String DayFromEpoch(__int64 seconds)
      {
         struct tm parts = {};
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         if (::gmtime_r(&value, &parts) == nullptr)
            return String();
#else
         __time64_t value = seconds;
         if (_gmtime64_s(&parts, &value) != 0)
            return String();
#endif
         String day;
         day.Format(_T("%04d-%02d-%02d"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
         return day;
      }

      // The day a message is dated: its Date field read as UTC, or - when it
      // has none that reads as a date - the day the server stored it.
      String DayOf(const String &dateField, const String &storedDay)
      {
         if (!dateField.IsEmpty())
         {
            DateTime when = Time::GetDateTimeFromMimeHeader(dateField);
            if (when.GetYear() >= 1971 && when.GetYear() <= 9999 && when.GetMonth() >= 1 && when.GetDay() >= 1)
            {
               String day;
               day.Format(_T("%04d-%02d-%02d"), when.GetYear(), when.GetMonth(), when.GetDay());
               return day;
            }
         }

         String fallback = storedDay.GetLength() >= 10 ? storedDay.Mid(0, 10) : storedDay;
         return IsDay(fallback) ? fallback : String();
      }

      // "image/png" out of "image/png; name=\"logo.png\"", lower-cased.
      String MediaType(const AnsiString &declared)
      {
         AnsiString type = declared;
         const int semicolon = type.Find(";");
         if (semicolon >= 0)
            type = type.Mid(0, semicolon);
         type.Trim();
         type.ToLower();

         String wide;
         Unicode::MultiByteToWide(type, wide);
         return wide;
      }

      bool TypeMatches(const String &type, const String &wanted)
      {
         if (wanted.IsEmpty())
            return true;

         // "image/" and "image/*" are every image; anything else is exact.
         String prefix = wanted;
         if (prefix.EndsWith(_T("/*")))
            prefix = prefix.Mid(0, prefix.GetLength() - 1);

         if (prefix.EndsWith(_T("/")))
            return type.GetLength() >= prefix.GetLength() && type.Mid(0, prefix.GetLength()) == prefix;

         return type == wanted;
      }

      String DecodeFolderName(const String &stored)
      {
         for (int i = 0; i < stored.GetLength(); i++)
         {
            if (stored[i] > 127)
               return stored;
         }

         return ModifiedUTF7::Decode(AnsiString(stored));
      }

      // The words of a query as a person writes them: separated by white
      // space, with "a phrase in quotes" kept as one.
      std::vector<String> SplitWords(const String &text)
      {
         std::vector<String> words;
         String current;
         bool quoted = false;

         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text[i];

            if (c == '"')
            {
               if (!current.IsEmpty())
                  words.push_back(current);
               current = String();
               quoted = !quoted;
               continue;
            }

            if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
               if (!current.IsEmpty())
                  words.push_back(current);
               current = String();
               continue;
            }

            current += c;
         }

         if (!current.IsEmpty())
            words.push_back(current);

         return words;
      }

      String JoinWords(const std::vector<String> &words)
      {
         String joined;
         for (size_t i = 0; i < words.size(); i++)
         {
            if (i > 0)
               joined += _T(" ");

            if (words[i].Find(_T(" ")) >= 0 || words[i].Find(_T("\t")) >= 0)
               joined += _T("\"") + words[i] + _T("\"");
            else
               joined += words[i];
         }
         return joined;
      }

      // A row-limited SELECT for each backend: TOP (n) on SQL Server and Compact,
      // which requires the parentheses, LIMIT n on MySQL and PostgreSQL - the
      // same knowledge AuditTrail and PersistentBounce apply.
      String SelectWithLimit(const String &columns, const String &from, const String &where, const String &orderBy, int rows)
      {
         DatabaseSettings::SQLDBType type = IniFileSettings::Instance()->GetDatabaseType();

         String statement;

         switch (type)
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            statement.Format(_T("select top (%d) %s from %s"), rows, columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            break;
         default:
            statement.Format(_T("select %s from %s"), columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            statement += Formatter::Format(" limit {0}", rows);
            break;
         }

         return statement;
      }

      AnsiString Hex(const unsigned char *digest, unsigned int length)
      {
         static const char digits[] = "0123456789abcdef";
         AnsiString hex;
         for (unsigned int i = 0; i < length; i++)
         {
            hex += digits[digest[i] >> 4];
            hex += digits[digest[i] & 15];
         }
         return hex;
      }

      // The SHA-256 of bytes in memory, over the buffer itself: an mbox part
      // is up to 256 MB, and a hasher that copied its input first would hold
      // it twice.
      AnsiString Sha256Hex(const AnsiString &bytes)
      {
         EVP_MD_CTX *context = EVP_MD_CTX_new();
         if (!context)
            return "";

         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;

         const bool good = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                           EVP_DigestUpdate(context, bytes.c_str(), (size_t) bytes.GetLength()) == 1 &&
                           EVP_DigestFinal_ex(context, digest, &length) == 1;
         EVP_MD_CTX_free(context);

         return good ? Hex(digest, length) : AnsiString();
      }

      // A file's SHA-256, read a megabyte at a time: an export may be 4 GB.
      AnsiString FileSha256(const String &fileName)
      {
         boost::filesystem::path path(fileName.begin(), fileName.end());
         boost::filesystem::ifstream in(path, std::ios::in | std::ios::binary);
         if (!in)
            return "";

         EVP_MD_CTX *context = EVP_MD_CTX_new();
         if (!context)
            return "";

         bool good = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
         std::vector<char> buffer(1024 * 1024);

         while (good && in)
         {
            in.read(&buffer[0], (std::streamsize) buffer.size());
            const std::streamsize got = in.gcount();
            if (got > 0)
               good = EVP_DigestUpdate(context, &buffer[0], (size_t) got) == 1;
         }

         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;
         good = good && EVP_DigestFinal_ex(context, digest, &length) == 1;
         EVP_MD_CTX_free(context);

         if (!good)
            return "";

         return Hex(digest, length);
      }

      // A path segment inside the zip: what an unpacker's file system refuses
      // becomes '_', and a name that is only dots - which would climb out of
      // the archive when it is unpacked - becomes "_". PersonExport's rule.
      String Segment(const String &name)
      {
         String result = name;
         const wchar_t *forbidden = L"\\/:*?\"<>|";
         for (const wchar_t *c = forbidden; *c != 0; ++c)
            result.Replace(*c, L'_');

         for (int i = 0; i < result.GetLength(); i++)
         {
            if (result[i] < 0x20)
               result.SetAt(i, L'_');
         }

         result.Trim();

         bool allDots = !result.IsEmpty();
         for (int i = 0; i < result.GetLength(); i++)
         {
            if (result[i] != L'.')
            {
               allDots = false;
               break;
            }
         }

         if (result.IsEmpty() || allDots)
            result = _T("_");

         return result;
      }

      String FolderEntryPath(const String &folder)
      {
         std::vector<String> parts = StringParser::SplitString(folder, _T("/"));
         String path;
         for (const String &part : parts)
         {
            if (part.IsEmpty())
               continue;
            if (!path.IsEmpty())
               path += _T("/");
            path += Segment(part);
         }
         return path.IsEmpty() ? String(_T("_")) : path;
      }

      // The Message-ID as a file name: the characters an address or an id is
      // made of, everything else '_', at most 100.
      String MessageIdForFile(const String &messageId)
      {
         String id = messageId;
         id.Trim();
         if (id.StartsWith(_T("<")))
            id = id.Mid(1);
         if (id.EndsWith(_T(">")))
            id = id.Mid(0, id.GetLength() - 1);

         String safe;
         for (int i = 0; i < id.GetLength() && safe.GetLength() < 100; i++)
         {
            const wchar_t c = id[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '.' || c == '-' || c == '_' || c == '@' || c == '+' || c == '=';
            safe += plain ? c : L'_';
         }

         return safe.IsEmpty() ? String(_T("no-message-id")) : safe;
      }

      // The envelope sender an mbox From_ line needs: the address in the From
      // field, or MAILER-DAEMON when there is none.
      AnsiString EnvelopeFrom(const String &from)
      {
         String address = from;
         const int open = address.Find(_T("<"));
         const int close = address.Find(_T(">"));
         if (open >= 0 && close > open)
            address = address.Mid(open + 1, close - open - 1);

         address.Trim();
         if (address.IsEmpty() || address.Find(_T("@")) < 0 || address.Find(_T(" ")) >= 0)
            return "MAILER-DAEMON";

         return Utf8(address);
      }

      // What the full-text index can prove for the query's words: which of the
      // messages asked about cannot contain them. The filter IMAP SEARCH and
      // the webmail's search apply, and it fails open the same way - anything
      // it cannot answer for is read. Asked a page of an account's messages at
      // a time (Resolve), never of the account's whole index: that took
      // minutes on SQL Server Compact with a large mailbox and held the
      // database's one connection throughout (PersistentMessageIndex::
      // GetMessagesThatCannotContain).
      class IndexPrune
      {
      public:
         explicit IndexPrune(const std::vector<String> &words)
         {
            if (words.empty() || !IniFileSettings::Instance()->GetIndexerFullTextEnabled())
               return;

            String text;
            for (const String &word : words)
               text += word + _T(" ");

            PersistentMessageIndex::CreateQueryNeedles(text, IniFileSettings::Instance()->GetIndexerFullTextMinTokenLength(), needles_);
         }

         // The index asked about these messages - a page's, which is what
         // CannotContain answers for until the next page is asked about - in
         // pieces of PersistentMessageIndex::MaxMessagesPerLookup; what it
         // could not answer is read.
         void Resolve(const std::vector<__int64> &messageIds)
         {
            excluded_.clear();

            if (!needles_.empty() && !messageIds.empty())
               PersistentMessageIndex::GetMessagesThatCannotContain(messageIds, needles_, excluded_);
         }

         bool CannotContain(__int64 messageId) const
         {
            return excluded_.find(messageId) != excluded_.end();
         }

      private:
         std::vector<String> needles_;
         std::set<__int64> excluded_;
      };

      // One thing a search may match: a message in a folder, a deleted item, an
      // archived copy.
      struct Candidate
      {
         const char *kind = "mailbox";      // mailbox, recoverable, archive
         String custodian;                  // the address it is filed under; empty for an Inbound copy
         __int64 account_id = 0;
         __int64 id = 0;                    // hm_messages, hm_recoverable or hm_archiveindex
         String folder;
         String file;
         __int64 size = 0;
         String stored_day;                 // the day it was stored, when its Date field says none
         bool headers_only = false;         // only its headers can be read: a zero-access mailbox's
         bool zero_access = false;          // from a zero-access mailbox, which an export withholds
         bool held = false;
         bool prunable = false;             // the index may rule it out
      };

      struct ArchivePlace
      {
         String domain;
         String mailbox;
      };

      class Searcher
      {
      private:
         const DiscoveryStore::Search &search_;
         const DiscoverySearch::Query &query_;
         std::ostream &out_;

      public:
         Searcher(const DiscoveryStore::Search &search, const DiscoverySearch::Query &query, std::ostream &out) :
            search_(search),
            query_(query),
            out_(out),
            scanned(0),
            matched(0),
            unsearched_count(0),
            truncated(false)
         {
         }

         bool Run(String &error)
         {
            std::vector<std::shared_ptr<const Account> > accounts;
            std::vector<ArchivePlace> places;

            if (!Resolve_(accounts, places, error))
               return false;

            if (query_.mailboxes)
            {
               for (std::shared_ptr<const Account> account : accounts)
               {
                  if (truncated)
                     break;

                  if (!SearchAccount_(account, error))
                     return false;
               }
            }

            if (query_.archive && !truncated)
            {
               if (!SearchArchive_(places, error))
                  return false;
            }

            return true;
         }

         int scanned;
         int matched;
         int unsearched_count;
         bool truncated;
         std::vector<AnsiString> unsearched;

      private:

         bool AccountsOf_(const String &where, __int64 domainId, bool everyone, std::vector<std::shared_ptr<const Account> > &accounts, std::set<__int64> &seen, String &error)
         {
            std::shared_ptr<DALRecordset> recordset;

            if (everyone)
            {
               SQLCommand command(_T("select accountid from hm_accounts order by accountaddress"));
               recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
            }
            else
            {
               SQLCommand command(_T("select accountid from hm_accounts where accountdomainid = @DOMAINID order by accountaddress"));
               command.AddParameter("@DOMAINID", domainId);
               recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
            }

            if (!recordset)
            {
               error = Formatter::Format("The accounts of {0} could not be read from the database.", where);
               return false;
            }

            while (!recordset->IsEOF())
            {
               const __int64 id = recordset->GetInt64Value("accountid");
               if (seen.insert(id).second)
               {
                  std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(id);
                  if (account)
                     accounts.push_back(account);
               }
               recordset->MoveNext();
            }

            return true;
         }

         bool Resolve_(std::vector<std::shared_ptr<const Account> > &accounts, std::vector<ArchivePlace> &places, String &error)
         {
            std::set<__int64> seen;
            bool everyone = false;

            for (const String &custodian : query_.custodians)
            {
               if (custodian == _T("*"))
               {
                  everyone = true;
                  continue;
               }

               if (custodian.Find(_T("@")) >= 0)
               {
                  std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(custodian);
                  if (account)
                  {
                     if (seen.insert(account->GetID()).second)
                        accounts.push_back(account);
                  }
                  else if (query_.mailboxes)
                  {
                     Custodian_(custodian, _T("There is no account of this address on the server now, so no mailbox was searched for it; the archive is searched for it by its address."));
                  }

                  ArchivePlace place;
                  place.domain = StringParser::ExtractDomain(custodian);
                  place.mailbox = StringParser::ExtractAddress(custodian);
                  places.push_back(place);
                  continue;
               }

               std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(custodian);
               if (domain)
               {
                  if (!AccountsOf_(custodian, domain->GetID(), false, accounts, seen, error))
                     return false;
               }
               else if (query_.mailboxes)
               {
                  Custodian_(custodian, _T("The server hosts no domain of this name now, so no mailbox was searched for it; the archive is searched for it by its name."));
               }

               ArchivePlace place;
               place.domain = custodian;
               places.push_back(place);
            }

            if (everyone)
            {
               accounts.clear();
               seen.clear();
               if (!AccountsOf_(_T("the server"), 0, true, accounts, seen, error))
                  return false;

               // Every copy the archive holds, the Inbound ones included.
               places.clear();
               places.push_back(ArchivePlace());
            }

            return true;
         }

         void ReadFolders_(__int64 accountId, std::map<__int64, std::pair<__int64, String> > &folders)
         {
            SQLCommand command(_T("select folderid, folderparentid, foldername from hm_imapfolders where folderaccountid = @ACCOUNTID"));
            command.AddParameter("@ACCOUNTID", accountId);

            std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
            if (!recordset)
               return;

            while (!recordset->IsEOF())
            {
               folders[recordset->GetInt64Value("folderid")] =
                  std::make_pair(recordset->GetInt64Value("folderparentid"), DecodeFolderName(recordset->GetStringValue("foldername")));
               recordset->MoveNext();
            }
         }

         static String FolderPath_(const std::map<__int64, std::pair<__int64, String> > &folders, __int64 folderId)
         {
            String path;
            __int64 id = folderId;

            for (int depth = 0; depth < 50 && id > 0; depth++)
            {
               auto folder = folders.find(id);
               if (folder == folders.end())
                  break;

               path = path.IsEmpty() ? folder->second.second : folder->second.second + _T("/") + path;
               id = folder->second.first;
            }

            return path.IsEmpty() ? String(_T("(a folder that is gone)")) : path;
         }

         bool SearchAccount_(std::shared_ptr<const Account> account, String &error)
         {
            const __int64 accountId = account->GetID();
            const String custodian = Lower(account->GetAddress());
            const bool zeroAccess = account->GetZeroAccess();
            const bool held = DiscoveryHolds::IsAccountHeld(accountId);

            std::map<__int64, std::pair<__int64, String> > folders;
            ReadFolders_(accountId, folders);

            // A zero-access mailbox's index holds the terms of what it was
            // given to index - nothing, since its bodies are encrypted - so it
            // is matched on its headers and never pruned.
            const std::vector<String> noWords;
            IndexPrune prune(zeroAccess ? noWords : query_.words);

            __int64 after = 0;

            for (;;)
            {
               SQLCommand command(SelectWithLimit(_T("messageid, messagefolderid, messagefilename, messagecreatetime, messagesize"),
                                                  _T("hm_messages"),
                                                  _T("messageaccountid = @ACCOUNTID and messagetype = @TYPE and messageid > @AFTER"),
                                                  _T("messageid"), PageRows));
               command.AddParameter("@ACCOUNTID", accountId);
               command.AddParameter("@TYPE", (int) Message::Delivered);
               command.AddParameter("@AFTER", after);

               std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
               if (!recordset)
               {
                  error = Formatter::Format("The messages of {0} could not be read from the database.", custodian);
                  return false;
               }

               int rows = 0;

               // The page first, then the index asked about it in one go,
               // then each message considered.
               std::vector<Candidate> page;
               std::vector<__int64> pageIds;

               while (!recordset->IsEOF())
               {
                  rows++;

                  Candidate candidate;
                  candidate.kind = "mailbox";
                  candidate.custodian = custodian;
                  candidate.account_id = accountId;
                  candidate.id = recordset->GetInt64Value("messageid");
                  after = candidate.id;

                  const __int64 folderId = recordset->GetInt64Value("messagefolderid");

                  // A message exists here only to resolve the file's path; it
                  // must not be given a freshly generated file name.
                  std::shared_ptr<Message> message(new Message(false));
                  message->SetPartialFileName(recordset->GetStringValue("messagefilename"));
                  message->SetAccountID(accountId);
                  message->SetFolderID(folderId);

                  candidate.file = PersistentMessage::GetFileName(account, message);
                  candidate.folder = FolderPath_(folders, folderId);
                  candidate.size = (__int64) recordset->GetLongValue("messagesize");
                  candidate.stored_day = recordset->GetStringValue("messagecreatetime");
                  candidate.headers_only = zeroAccess;
                  candidate.zero_access = zeroAccess;
                  candidate.held = held;
                  candidate.prunable = true;

                  page.push_back(candidate);
                  pageIds.push_back(candidate.id);

                  recordset->MoveNext();
               }

               recordset.reset();

               if (!query_.words.empty())
                  prune.Resolve(pageIds);

               for (const Candidate &candidate : page)
               {
                  Consider_(candidate, &prune);

                  if (truncated)
                     return true;
               }

               if (rows < PageRows)
                  break;
            }

            // The account's deleted items: what a hold kept, and what
            // RecoverDeletedDays keeps.
            std::vector<RecoverableStore::Entry> entries;
            if (!RecoverableStore::List(accountId, entries))
            {
               error = Formatter::Format("The deleted items of {0} could not be read from the database.", custodian);
               return false;
            }

            for (const RecoverableStore::Entry &entry : entries)
            {
               Candidate candidate;
               candidate.kind = "recoverable";
               candidate.custodian = custodian;
               candidate.account_id = accountId;
               candidate.id = entry.id;
               candidate.folder = String(_T("Recoverable Items/")) + (entry.folder.IsEmpty() ? String(_T("(unknown folder)")) : entry.folder);
               candidate.file = RecoverableStore::KeptFile(entry);
               candidate.size = (__int64) entry.size;
               candidate.stored_day = DayFromEpoch(entry.deleted);
               candidate.headers_only = zeroAccess;
               candidate.zero_access = zeroAccess;
               candidate.held = held;

               Consider_(candidate, nullptr);

               if (truncated)
                  return true;
            }

            return true;
         }

         bool SearchArchive_(const std::vector<ArchivePlace> &places, String &error)
         {
            std::set<__int64> seen;

            for (const ArchivePlace &place : places)
            {
               __int64 after = 0;

               for (;;)
               {
                  std::vector<PersistentArchiveIndex::Entry> entries;
                  if (!PersistentArchiveIndex::ListAfter(place.domain, place.mailbox, after, PageRows, entries))
                  {
                     error = _T("The archive index could not be read from the database.");
                     return false;
                  }

                  for (const PersistentArchiveIndex::Entry &entry : entries)
                  {
                     after = entry.id;

                     // A domain and one of its addresses both named: each
                     // copy once.
                     if (!seen.insert(entry.id).second)
                        continue;

                     Candidate candidate;
                     candidate.kind = "archive";
                     candidate.custodian = entry.mailbox.IsEmpty() ? String() : Lower(entry.mailbox + _T("@") + entry.domain);
                     candidate.id = entry.id;
                     candidate.folder = entry.direction == PersistentArchiveIndex::DirectionSent ? String(_T("Archive/Sent"))
                                      : entry.direction == PersistentArchiveIndex::DirectionReceived ? String(_T("Archive/Received"))
                                      : String(_T("Archive/Inbound"));
                     candidate.file = entry.path;
                     candidate.size = entry.size;
                     candidate.stored_day = entry.time;
                     candidate.held = entry.hold || DiscoveryHolds::IsArchiveEntryHeld(entry.id, entry.domain, entry.mailbox);

                     Consider_(candidate, nullptr);

                     if (truncated)
                        return true;
                  }

                  if ((int) entries.size() < PageRows)
                     break;
               }
            }

            return true;
         }

         // Whether a message labelled encrypted really is: its body read as the
         // search would read it, and asked of SealedMessage. One too large to
         // read is taken at its label, since its body would not be searched
         // either way and it is listed as not searched for it.
         bool HoldsCiphertext_(const Candidate &candidate)
         {
            unsigned __int64 fileSize = 0;
            MessageStore::FileSize64(candidate.file, fileSize);
            if (fileSize > (unsigned __int64) DiscoverySearch::MaxMessageBytes)
               return true;

            std::shared_ptr<MessageData> data = std::make_shared<MessageData>();
            std::shared_ptr<Message> shell(new Message(false));
            if (!data->LoadFromMessage(candidate.file, shell))
               return true;

            return SealedMessage::IsOpaque(data);
         }

         void Consider_(const Candidate &candidate, const IndexPrune *prune)
         {
            // A search may read for hours; a service stop or a reinitialise
            // must be able to end it between two messages.
            boost::this_thread::interruption_point();

            scanned++;
            if (scanned % ProgressEvery == 0)
               DiscoveryStore::RecordSearchProgress(search_.id, scanned, matched, unsearched_count);

            if (!MessageStore::Exists(candidate.file))
            {
               Unsearched_(candidate, _T("Its file is not on the disk."));
               return;
            }

            // The head of the file, for the date and the fields every hit names.
            AnsiString header = PersistentMessage::LoadHeader(candidate.file, false);
            MimeHeader mime;
            if (!header.IsEmpty())
               mime.Load(header.c_str(), header.GetLength(), true);

            const String subject = mime.GetUnicodeFieldValue("Subject");
            const String from = mime.GetUnicodeFieldValue("From");
            const String to = mime.GetUnicodeFieldValue("To");
            const String cc = mime.GetUnicodeFieldValue("Cc");
            const char *rawId = mime.GetRawFieldValue("Message-ID");
            const char *rawDate = mime.GetRawFieldValue("Date");
            const String messageId = rawId ? String(rawId) : String();
            const String day = DayOf(rawDate ? String(rawDate) : String(), candidate.stored_day);

            if (!query_.from.IsEmpty() || !query_.to.IsEmpty())
            {
               if (day.IsEmpty())
               {
                  Unsearched_(candidate, _T("Its date could not be read, so it could not be judged against the dates asked for."));
                  return;
               }

               if (!query_.from.IsEmpty() && day.Compare(query_.from) < 0)
                  return;

               if (!query_.to.IsEmpty() && day.Compare(query_.to) > 0)
                  return;
            }

            std::vector<String> attachmentNames;

            // A message encrypted end to end - S/MIME or PGP/MIME, in any
            // mailbox, the archive or the deleted items - has no body this
            // server can read, as a zero-access mailbox's has none: its
            // top-level Content-Type says so (SealedMessage). Matched on its
            // headers alone as well, and its hit says so; but where a
            // zero-access mailbox's miss is a miss, as decided, such a message
            // whose headers lack a word is listed as not searched, since its
            // body might hold it - never dropped as a non-match.
            //
            // The label only says where to look. "multipart/encrypted" over
            // ordinary text is one header any sender can write, and taking it
            // at its word would keep a readable message out of every hit list
            // as "not searched"; so a labelled message is read, and is treated
            // as encrypted only when SealedMessage finds ciphertext in it.
            const char *contentType = mime.GetContentType();
            const bool encrypted = !candidate.headers_only && contentType != nullptr &&
                                   SealedMessage::IsOpaqueContentType(String(contentType)) &&
                                   HoldsCiphertext_(candidate);

            if (candidate.headers_only || encrypted)
            {
               if (query_.attachment)
               {
                  Unsearched_(candidate, encrypted
                     ? String(_T("It is encrypted end to end (S/MIME or PGP/MIME), so only its headers can be read, and an attachment criterion needs the body."))
                     : String(_T("A zero-access mailbox: its messages are encrypted to a key this server does not have, so only their headers can be read, and an attachment criterion needs the body.")));
                  return;
               }

               if (!query_.words.empty())
               {
                  String headerText;
                  Unicode::MultiByteToWide(header, headerText);
                  String text = subject + _T("\n") + from + _T("\n") + to + _T("\n") + cc + _T("\n") + headerText;
                  text.ToLower();

                  for (const String &word : query_.words)
                  {
                     if (text.Find(word) < 0)
                     {
                        if (encrypted)
                           Unsearched_(candidate, _T("It is encrypted end to end (S/MIME or PGP/MIME), so only its headers could be read, and they do not hold every word asked for; its body might."));
                        return;
                     }
                  }
               }

               Hit_(candidate, messageId, day, from, to, subject, attachmentNames, false, true);
               return;
            }

            // The index proves a message cannot contain a word, and no other
            // criterion can make up for a missing word.
            if (!query_.words.empty() && prune && candidate.prunable && prune->CannotContain(candidate.id))
               return;

            const bool needBody = !query_.words.empty() || query_.attachment;

            unsigned __int64 fileSize = 0;
            MessageStore::FileSize64(candidate.file, fileSize);

            if (fileSize > (unsigned __int64) DiscoverySearch::MaxMessageBytes)
            {
               if (needBody)
               {
                  Unsearched_(candidate, _T("It is larger than 80 MB, which is as much of one message as this server reads, so its body was not searched."));
                  return;
               }

               Hit_(candidate, messageId, day, from, to, subject, attachmentNames, false, false);
               return;
            }

            MessageData data;
            std::shared_ptr<Message> shell(new Message(false));
            if (!data.LoadFromMessage(candidate.file, shell))
            {
               Unsearched_(candidate, _T("It could not be read as a message."));
               return;
            }

            if (!query_.words.empty())
            {
               // What IMAP SEARCH TEXT reads, each part lower-cased once.
               String headerText = data.GetHeader();
               String body = data.GetBody();
               String html = data.GetHTMLBody();
               String attachmentText = data.GetAttachmentText();
               headerText.ToLower();
               body.ToLower();
               html.ToLower();
               attachmentText.ToLower();

               for (const String &word : query_.words)
               {
                  if (headerText.Find(word) < 0 && body.Find(word) < 0 && html.Find(word) < 0 && attachmentText.Find(word) < 0)
                     return;
               }
            }

            bool attachmentMatched = !query_.attachment;

            std::shared_ptr<Attachments> attachments = data.GetAttachments();
            for (size_t i = 0; attachments && i < attachments->GetCount(); i++)
            {
               std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
               if (!attachment)
                  continue;

               const String name = attachment->GetFileName();
               attachmentNames.push_back(name);

               if (attachmentMatched)
                  continue;

               bool fits = true;

               if (!query_.attachment_name.IsEmpty())
                  fits = Lower(name).Find(query_.attachment_name) >= 0;

               if (fits && !query_.attachment_type.IsEmpty())
                  fits = TypeMatches(MediaType(attachment->GetContentType()), query_.attachment_type);

               if (fits && query_.attachment_min_bytes > 0)
               {
                  AnsiString decoded;
                  const __int64 size = attachment->GetContent(decoded) ? (__int64) decoded.GetLength() : (__int64) attachment->GetSize();
                  fits = size >= query_.attachment_min_bytes;
               }

               attachmentMatched = fits;
            }

            if (!attachmentMatched)
               return;

            Hit_(candidate, messageId, day, from, to, subject, attachmentNames, true, false);
         }

         void Hit_(const Candidate &candidate, const String &messageId, const String &day, const String &from, const String &to,
                   const String &subject, const std::vector<String> &attachmentNames, bool attachmentsRead, bool headersOnly)
         {
            if (matched >= DiscoverySearch::MaxHits)
            {
               truncated = true;
               return;
            }

            matched++;

            AnsiString line = matched > 1 ? AnsiString(",") : AnsiString();
            line += "{\"n\":" + Number(matched);
            line += ",\"kind\":\"" + AnsiString(candidate.kind) + "\"";
            line += ",\"custodian\":" + Quoted(candidate.custodian);
            line += ",\"account_id\":" + Number(candidate.account_id);
            line += ",\"id\":" + Number(candidate.id);
            line += ",\"folder\":" + Quoted(candidate.folder);
            line += ",\"message_id\":" + Quoted(messageId);
            line += ",\"date\":" + Quoted(day);
            line += ",\"from\":" + Quoted(from);
            line += ",\"to\":" + Quoted(to);
            line += ",\"subject\":" + Quoted(subject);
            line += ",\"size\":" + Number(candidate.size);

            if (attachmentsRead)
            {
               line += ",\"attachments\":[";
               for (size_t i = 0; i < attachmentNames.size(); i++)
               {
                  if (i > 0)
                     line += ",";
                  line += Quoted(attachmentNames[i]);
               }
               line += "]";
            }
            else
            {
               line += ",\"attachments\":null";
            }

            line += ",\"headers_only\":" + Bool(headersOnly);
            line += ",\"zero_access\":" + Bool(candidate.zero_access);
            line += ",\"held\":" + Bool(candidate.held);
            line += "}\n";

            out_.write(line.c_str(), line.GetLength());
         }

         void Unsearched_(const Candidate &candidate, const String &reason)
         {
            unsearched_count++;

            if ((int) unsearched.size() >= DiscoverySearch::MaxUnsearchedListed)
               return;

            AnsiString entry = "{\"kind\":\"" + AnsiString(candidate.kind) + "\"";
            entry += ",\"custodian\":" + Quoted(candidate.custodian);
            entry += ",\"id\":" + Number(candidate.id);
            entry += ",\"folder\":" + Quoted(candidate.folder);
            entry += ",\"reason\":" + Quoted(reason);
            entry += "}";
            unsearched.push_back(entry);
         }

         // A custodian the search could not read a mailbox of at all.
         void Custodian_(const String &custodian, const String &reason)
         {
            Candidate candidate;
            candidate.kind = "custodian";
            candidate.custodian = custodian;
            Unsearched_(candidate, reason);
         }
      };

      // One hit of a result file, as the export reads it back.
      struct HitRecord
      {
         __int64 n = 0;
         String kind;
         String custodian;
         __int64 account_id = 0;
         __int64 id = 0;
         String folder;
         String message_id;
         String date;
         String from;
         String to;
         String subject;
         __int64 size = 0;
         bool headers_only = false;
         bool zero_access = false;
      };

      bool ReadHit(const AnsiString &line, HitRecord &hit)
      {
         JsonValue value;
         std::string parseError;
         if (!JsonValue::Parse(std::string(line.c_str(), line.GetLength()), value, parseError) || !value.IsObject())
            return false;

         hit.n = value.GetInt64("n");
         hit.kind = Wide(value.GetString("kind"));
         hit.custodian = Wide(value.GetString("custodian"));
         hit.account_id = value.GetInt64("account_id");
         hit.id = value.GetInt64("id");
         hit.folder = Wide(value.GetString("folder"));
         hit.message_id = Wide(value.GetString("message_id"));
         hit.date = Wide(value.GetString("date"));
         hit.from = Wide(value.GetString("from"));
         hit.to = Wide(value.GetString("to"));
         hit.subject = Wide(value.GetString("subject"));
         hit.size = value.GetInt64("size");
         hit.headers_only = value.GetBool("headers_only");
         hit.zero_access = value.GetBool("zero_access");
         return true;
      }

      class Exporter
      {
      public:
         Exporter(const DiscoveryStore::Case &theCase, const DiscoveryStore::Search &search, DiscoveryStore::Export &result) :
            case_(theCase),
            search_(search),
            result_(result)
         {
         }

         bool Run(String &error)
         {
            const String directory = DiscoveryStore::CaseDirectory(case_.id);
            const String resultsPath = FileUtilities::Combine(directory, search_.result_file);

            std::vector<AnsiString> lines;
            std::vector<AnsiString> unsearched;
            int total = 0;
            int unsearchedListed = 0;
            if (search_.result_file.IsEmpty() ||
                !DiscoverySearch::ReadResults(resultsPath, 0, INT_MAX, lines, total, 0, INT_MAX, unsearched, unsearchedListed))
            {
               error = _T("The search's results could not be read.");
               return false;
            }

            std::vector<HitRecord> hits;
            unsigned __int64 bytes = 0;
            for (const AnsiString &line : lines)
            {
               HitRecord hit;
               if (!ReadHit(line, hit))
               {
                  error = _T("The search's results file holds a line that is not a hit.");
                  return false;
               }

               if (!hit.headers_only && hit.size > 0)
                  bytes += (unsigned __int64) hit.size;

               hits.push_back(hit);
            }

            // Each message goes in twice - as EML and in its folder's mbox - and
            // ZipWriter writes the 32-bit format, so the ceiling is known before
            // a byte is written.
            if (bytes * 2 + (unsigned __int64) hits.size() * 1024 + 64 * 1024 * 1024 > ZipCeiling)
            {
               error = Formatter::Format("The export would be larger than 4 GB - {0} message(s), {1} bytes, written twice over as EML and in an mbox - which is as large as the zip format this server writes can be. Narrow the search, with a shorter date range or fewer custodians, and export each part.",
                  (int) hits.size(), (__int64) bytes);
               return false;
            }

            // The zip goes under the data directory, on the volume the message
            // store is on, and nothing removes an export: one that would leave
            // less than a gigabyte free there is refused before it is started,
            // so that exporting cannot fill the disk the mail is delivered to.
            {
               const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();
               const unsigned __int64 needed = bytes * 2 + (unsigned __int64) hits.size() * 1024 + 64ULL * 1024 * 1024;
               const unsigned __int64 keepFree = 1024ULL * 1024 * 1024;

               boost::system::error_code spaceError;
               boost::filesystem::space_info volume = boost::filesystem::space(boost::filesystem::path(dataDirectory.begin(), dataDirectory.end()), spaceError);

               if (!spaceError && (unsigned __int64) volume.available < needed + keepFree)
               {
                  error = Formatter::Format("The export needs about {0} bytes, and the data directory's volume has {1} bytes free: less than that and a gigabyte to spare for the mail delivered there. Free space there, remove exports no longer needed from {2}, or narrow the search.",
                     needed, (unsigned __int64) volume.available, FileUtilities::Combine(dataDirectory, _T("Discovery")));
                  return false;
               }
            }

            if (!FileUtilities::DirectoryExists(directory) && !FileUtilities::CreateDirectory(directory))
            {
               error = _T("The case's directory could not be made: ") + directory;
               return false;
            }

            String zipName;
            zipName.Format(_T("export-%I64d.zip"), result_.id);
            const String zipPath = FileUtilities::Combine(directory, zipName);

            if (FileUtilities::Exists(zipPath))
               FileUtilities::DeleteFile(zipPath);

            ZipWriter zip;
            if (!zip.Open(zipPath))
            {
               error = zip.Error();
               return false;
            }

            bool written = false;

            try
            {
               written = Write_(zip, hits, unsearched, error);
            }
            catch (const boost::thread_interrupted &)
            {
               // Stopped between two messages: nothing half-written is left
               // under the export's name.
               zip.Close();
               FileUtilities::DeleteFile(zipPath);
               throw;
            }

            if (!written)
            {
               zip.Close();
               FileUtilities::DeleteFile(zipPath);
               return false;
            }

            if (!zip.Close())
            {
               error = zip.Error();
               FileUtilities::DeleteFile(zipPath);
               return false;
            }

            unsigned __int64 size = 0;
            FileUtilities::FileSize64(zipPath, size);

            const AnsiString digest = FileSha256(zipPath);
            if (digest.IsEmpty())
            {
               error = _T("The export was written but could not be read back for its SHA-256.");
               FileUtilities::DeleteFile(zipPath);
               return false;
            }

            result_.file = zipName;
            result_.size = (__int64) size;
            Unicode::MultiByteToWide(digest, result_.sha256);
            return true;
         }

      private:

         // Where one hit's bytes are now, or the reason it is withheld.
         bool Locate_(const HitRecord &hit, String &path, bool &held, String &reason)
         {
            held = false;

            if (hit.zero_access)
            {
               reason = _T("A zero-access mailbox: its messages are encrypted to a key this server does not have, and its administrative export is refused by design (docs/ZeroAccess.md). Only its headers were searched.");
               return false;
            }

            if (hit.kind == _T("archive"))
            {
               PersistentArchiveIndex::Entry entry;
               if (!PersistentArchiveIndex::Get(hit.id, entry))
               {
                  reason = _T("The archive no longer has this copy: it was swept or erased since the search ran.");
                  return false;
               }

               path = entry.path;
               held = entry.hold || DiscoveryHolds::IsArchiveEntryHeld(entry.id, entry.domain, entry.mailbox);
               return true;
            }

            held = DiscoveryHolds::IsAccountHeld(hit.account_id);

            // Asked now, not as the search found it: a custodian who switched
            // zero access on since the search ran has a mailbox whose
            // administrative export is refused from that moment, so this one
            // withholds it too.
            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(hit.account_id);
            if (account && account->GetZeroAccess())
            {
               reason = _T("A zero-access mailbox now: its owner switched zero access on since the search ran, and from then its administrative export is refused by design (docs/ZeroAccess.md).");
               return false;
            }

            if (hit.kind == _T("recoverable"))
            {
               if (!RecoverableStore::KeptFileOf(hit.account_id, hit.id, path))
               {
                  reason = _T("The deleted item is no longer kept: it was put back, deleted for good or swept since the search ran.");
                  return false;
               }

               return true;
            }

            if (!account)
            {
               reason = _T("The account is no longer on the server.");
               return false;
            }

            SQLCommand command(_T("select messagefilename, messagefolderid from hm_messages where messageid = @MESSAGEID and messageaccountid = @ACCOUNTID"));
            command.AddParameter("@MESSAGEID", hit.id);
            command.AddParameter("@ACCOUNTID", hit.account_id);

            std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
            if (!recordset || recordset->IsEOF())
            {
               reason = _T("The message is no longer in the mailbox: it was deleted or moved since the search ran. A held account's deleted message is among its deleted items, which a search run again finds.");
               return false;
            }

            std::shared_ptr<Message> message(new Message(false));
            message->SetPartialFileName(recordset->GetStringValue("messagefilename"));
            message->SetAccountID(hit.account_id);
            message->SetFolderID(recordset->GetInt64Value("messagefolderid"));

            path = PersistentMessage::GetFileName(account, message);
            return true;
         }

         AnsiString ItemJson_(const HitRecord &hit, bool held) const
         {
            AnsiString json = "\"kind\":" + Quoted(hit.kind);
            json += ",\"custodian\":" + Quoted(hit.custodian);
            json += ",\"folder\":" + Quoted(hit.folder);
            json += ",\"id\":" + Number(hit.id);
            json += ",\"message_id\":" + Quoted(hit.message_id);
            json += ",\"date\":" + Quoted(hit.date);
            json += ",\"from\":" + Quoted(hit.from);
            json += ",\"to\":" + Quoted(hit.to);
            json += ",\"subject\":" + Quoted(hit.subject);
            json += ",\"held\":" + Bool(held);
            return json;
         }

         // One part of a folder's mbox into the zip, and its line of SHA256SUMS:
         // <folder>.mbox, and <folder>-part2.mbox and on for a folder whose
         // mbox would pass Write_'s ceiling.
         bool AddMbox_(ZipWriter &zip, const String &group, int part, const AnsiString &mbox, AnsiString &sums, String &error)
         {
            String name = group;
            if (part > 1)
               name += Formatter::Format("-part{0}", part);
            name += _T(".mbox");

            const AnsiString entry = Utf8(name);
            if (!zip.Add(entry, mbox))
            {
               error = zip.Error();
               return false;
            }

            sums += Sha256Hex(mbox) + "  " + entry + "\n";
            return true;
         }

         bool Write_(ZipWriter &zip, const std::vector<HitRecord> &hits, const std::vector<AnsiString> &unsearched, String &error)
         {
            // One mbox per custodian folder, as PersonExport writes one per
            // folder: ZipWriter takes an entry whole, so the mbox being built
            // is held in memory, with its deflated copy beside it while it is
            // added. A folder's mbox is written in parts of at most
            // MaxMboxBytes, so that a folder of two gigabytes does not ask the
            // mail server's process for three times that at once.
            const int MaxMboxBytes = 256 * 1024 * 1024;

            std::vector<std::pair<String, std::vector<size_t> > > groups;
            std::map<String, size_t> groupIndex;

            for (size_t i = 0; i < hits.size(); i++)
            {
               const String custodianDirectory = Segment(hits[i].custodian.IsEmpty() ? String(_T("inbound")) : hits[i].custodian);
               const String key = custodianDirectory + _T("/") + FolderEntryPath(hits[i].folder);

               auto found = groupIndex.find(key);
               if (found == groupIndex.end())
               {
                  groupIndex[key] = groups.size();
                  groups.push_back(std::make_pair(key, std::vector<size_t>()));
                  found = groupIndex.find(key);
               }

               groups[found->second].second.push_back(i);
            }

            AnsiString items;
            AnsiString sums;
            int written = 0;
            int withheld = 0;

            for (const auto &group : groups)
            {
               AnsiString mbox;
               int part = 1;

               for (size_t index : group.second)
               {
                  // An export may run for hours; a service stop or a
                  // reinitialise must be able to end it between two messages.
                  boost::this_thread::interruption_point();

                  const HitRecord &hit = hits[index];

                  String path;
                  String reason;
                  bool held = false;
                  AnsiString bytes;

                  bool readable = Locate_(hit, path, held, reason);
                  if (readable && !MessageStore::ReadAll(path, bytes))
                  {
                     readable = false;
                     reason = _T("Its file could not be read.");
                  }

                  if (!items.IsEmpty())
                     items += ",\n";

                  if (!readable)
                  {
                     withheld++;
                     items += "    {" + ItemJson_(hit, held) + ",\"withheld\":true,\"reason\":" + Quoted(reason) + "}";
                     continue;
                  }

                  String fileName;
                  fileName.Format(_T("%I64d-%s.eml"), hit.n, MessageIdForFile(hit.message_id).c_str());
                  const AnsiString entry = Utf8(group.first + _T("/") + fileName);

                  if (!zip.Add(entry, bytes))
                  {
                     error = zip.Error();
                     return false;
                  }

                  const AnsiString digest = Sha256Hex(bytes);
                  sums += digest + "  " + entry + "\n";
                  written++;

                  items += "    {\"path\":" + QuotedUtf8(entry) + ",\"sha256\":\"" + digest + "\",\"size\":" + Number((__int64) bytes.GetLength()) +
                           "," + ItemJson_(hit, held) + ",\"withheld\":false}";

                  // The part so far goes into the zip before this message
                  // would take it past the ceiling; a single message larger
                  // than the ceiling is a part of its own.
                  if (!mbox.IsEmpty() && (__int64) mbox.GetLength() + (__int64) bytes.GetLength() > (__int64) MaxMboxBytes)
                  {
                     if (!AddMbox_(zip, group.first, part, mbox, sums, error))
                        return false;

                     mbox = AnsiString();
                     part++;
                  }

                  PersonExport::AppendMbox(mbox, bytes, EnvelopeFrom(hit.from), Utf8(hit.date));
               }

               if (!mbox.IsEmpty() && !AddMbox_(zip, group.first, part, mbox, sums, error))
                  return false;
            }

            if (!zip.Add("SHA256SUMS", sums))
            {
               error = zip.Error();
               return false;
            }

            const AnsiString sumsDigest = Sha256Hex(sums);

            // What the search could not read, as the results file lists it -
            // up to the first 10,000 (DiscoverySearch::MaxUnsearchedListed),
            // search.unsearched being the whole count - so that the examiner
            // is told which messages were never searched, not only how many.
            AnsiString unsearchedList;
            for (size_t i = 0; i < unsearched.size(); i++)
               unsearchedList += (i > 0 ? AnsiString(",\n") : AnsiString()) + "    " + unsearched[i];

            AnsiString manifest = "{\n";
            manifest += "  \"format\":\"hMailServer eDiscovery export 1\",\n";
            manifest += "  \"case\":{\"id\":" + Number(case_.id) + ",\"name\":" + Quoted(case_.name) + ",\"description\":" + Quoted(case_.description) + "},\n";
            manifest += "  \"search\":{\"id\":" + Number(search_.id) + ",\"query\":" + (search_.query.IsEmpty() ? AnsiString("null") : search_.query) +
                        ",\"ran_by\":" + Quoted(search_.ran_by) + ",\"ran\":" + Number(search_.ran) + ",\"finished\":" + Number(search_.finished) +
                        ",\"matched\":" + Number(search_.matched) + ",\"truncated\":" + Bool(search_.truncated) + ",\"unsearched\":" + Number(search_.unsearched) + "},\n";
            manifest += "  \"export\":{\"id\":" + Number(result_.id) + ",\"requested_by\":" + Quoted(result_.requested_by) + ",\"requested\":" + Number(result_.requested) +
                        ",\"written\":" + Number((__int64) std::time(nullptr)) + "},\n";
            manifest += "  \"files\":" + Number(written) + ",\n";
            manifest += "  \"withheld\":" + Number(withheld) + ",\n";
            manifest += "  \"sums_file\":\"SHA256SUMS\",\n";
            manifest += "  \"sums_sha256\":\"" + sumsDigest + "\",\n";
            manifest += "  \"unsearched_list\":[\n" + unsearchedList + (unsearchedList.IsEmpty() ? AnsiString() : AnsiString("\n")) + "  ],\n";
            manifest += "  \"items\":[\n" + items + (items.IsEmpty() ? AnsiString() : AnsiString("\n")) + "  ]\n}\n";

            if (!zip.Add("manifest.json", manifest))
            {
               error = zip.Error();
               return false;
            }

            result_.items = written;
            result_.withheld = withheld;
            return true;
         }

         const DiscoveryStore::Case &case_;
         const DiscoveryStore::Search &search_;
         DiscoveryStore::Export &result_;
      };
   }

   // ---------------------------------------------------------------- the query

   bool
   DiscoverySearch::ParseQuery(const AnsiString &json, Query &query, String &problem)
   {
      query = Query();

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(json.c_str(), json.GetLength()), document, parseError) || !document.IsObject())
      {
         problem = _T("The query must be a JSON object.");
         return false;
      }

      for (const auto &member : document.Members())
      {
         const std::string &key = member.first;
         const JsonValue &value = member.second;

         if (key == "custodians")
         {
            if (!value.IsArray())
            {
               problem = _T("custodians must be an array of addresses, domains, or \"*\" for every mailbox and the whole archive.");
               return false;
            }

            for (const JsonValue &item : value.Items())
            {
               if (!item.IsString())
               {
                  problem = _T("Each custodian must be a string: an address, a domain, or \"*\".");
                  return false;
               }

               String custodian = Wide(item.AsString());
               custodian.Trim();
               custodian.ToLower();

               if (custodian != _T("*"))
               {
                  const bool address = custodian.Find(_T("@")) >= 0;
                  if (address ? !StringParser::IsValidEmailAddress(custodian) : !StringParser::IsValidDomainName(custodian))
                  {
                     problem = Formatter::Format("{0} is neither an address, a domain nor \"*\".", custodian);
                     return false;
                  }
               }

               if (std::find(query.custodians.begin(), query.custodians.end(), custodian) == query.custodians.end())
                  query.custodians.push_back(custodian);
            }

            if (query.custodians.size() > MaxCustodians)
            {
               problem = _T("A search names at most 100 custodians; a domain names every account in it, and \"*\" every account on the server.");
               return false;
            }

            continue;
         }

         if (key == "from" || key == "to")
         {
            if (value.IsNull())
               continue;

            if (!value.IsString())
            {
               problem = Formatter::Format("{0} must be a date, YYYY-MM-DD.", String(Wide(key)));
               return false;
            }

            String day = Wide(value.AsString());
            day.Trim();

            if (!day.IsEmpty() && !IsDay(day))
            {
               problem = Formatter::Format("{0} must be a date, YYYY-MM-DD.", String(Wide(key)));
               return false;
            }

            if (key == "from")
               query.from = day;
            else
               query.to = day;

            continue;
         }

         if (key == "words")
         {
            if (value.IsNull())
               continue;

            if (!value.IsString())
            {
               problem = _T("words must be a string: the words that must all be found, a \"quoted phrase\" as one.");
               return false;
            }

            std::vector<String> words = SplitWords(Wide(value.AsString()));
            for (String word : words)
            {
               word.ToLower();

               if (word.GetLength() > MaxWordLength)
               {
                  problem = _T("A word of a search is at most 200 characters.");
                  return false;
               }

               if (std::find(query.words.begin(), query.words.end(), word) == query.words.end())
                  query.words.push_back(word);
            }

            if (query.words.size() > MaxWords)
            {
               problem = _T("A search asks for at most 50 words.");
               return false;
            }

            continue;
         }

         if (key == "attachment")
         {
            if (value.IsNull())
               continue;

            if (!value.IsObject())
            {
               problem = _T("attachment must be an object with name, type and min_kb, each optional: {} is any attachment.");
               return false;
            }

            query.attachment = true;

            for (const auto &part : value.Members())
            {
               if (part.first == "name" || part.first == "type")
               {
                  if (part.second.IsNull())
                     continue;

                  if (!part.second.IsString())
                  {
                     problem = Formatter::Format("attachment.{0} must be a string.", String(Wide(part.first)));
                     return false;
                  }

                  String text = Wide(part.second.AsString());
                  text.Trim();
                  text.ToLower();

                  if (part.first == "name")
                  {
                     query.attachment_name = text;
                  }
                  else
                  {
                     if (!text.IsEmpty() && text.Find(_T("/")) <= 0)
                     {
                        problem = _T("attachment.type must be a media type, such as application/pdf, or a family of them, such as image/.");
                        return false;
                     }

                     query.attachment_type = text;
                  }

                  continue;
               }

               if (part.first == "min_kb")
               {
                  if (part.second.IsNull())
                     continue;

                  const double kilobytes = part.second.IsNumber() ? part.second.AsNumber() : -1.0;
                  if (kilobytes < 0.0 || kilobytes > 4000000.0 || kilobytes != (double) (__int64) kilobytes)
                  {
                     problem = _T("attachment.min_kb must be a whole number of kilobytes, 0 or more.");
                     return false;
                  }

                  query.attachment_min_bytes = (__int64) kilobytes * 1024;
                  continue;
               }

               problem = Formatter::Format("attachment.{0} is not part of an attachment criterion: the parts are name, type and min_kb.", String(Wide(part.first)));
               return false;
            }

            continue;
         }

         if (key == "scope")
         {
            if (value.IsNull())
               continue;

            const std::string scope = value.IsString() ? value.AsString() : std::string();
            if (scope == "mailboxes")
            {
               query.mailboxes = true;
               query.archive = false;
            }
            else if (scope == "archive")
            {
               query.mailboxes = false;
               query.archive = true;
            }
            else if (scope == "both")
            {
               query.mailboxes = true;
               query.archive = true;
            }
            else
            {
               problem = _T("scope must be mailboxes, archive or both.");
               return false;
            }

            continue;
         }

         problem = Formatter::Format("{0} is not part of a discovery query: the parts are custodians, from, to, words, attachment and scope.", String(Wide(key)));
         return false;
      }

      if (query.custodians.empty())
      {
         problem = _T("Name at least one custodian: an address, a domain, or \"*\" for every mailbox and the whole archive.");
         return false;
      }

      if (!query.from.IsEmpty() && !query.to.IsEmpty() && query.from.Compare(query.to) > 0)
      {
         problem = _T("from is after to.");
         return false;
      }

      return true;
   }

   AnsiString
   DiscoverySearch::QueryJson(const Query &query)
   {
      AnsiString json = "{\"custodians\":[";
      for (size_t i = 0; i < query.custodians.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += Quoted(query.custodians[i]);
      }
      json += "]";

      json += ",\"from\":" + Quoted(query.from);
      json += ",\"to\":" + Quoted(query.to);
      json += ",\"words\":" + Quoted(JoinWords(query.words));

      if (query.attachment)
      {
         json += ",\"attachment\":{\"name\":" + Quoted(query.attachment_name);
         json += ",\"type\":" + Quoted(query.attachment_type);
         json += ",\"min_kb\":" + Number(query.attachment_min_bytes / 1024) + "}";
      }
      else
      {
         json += ",\"attachment\":null";
      }

      const char *scope = query.mailboxes && query.archive ? "both" : (query.mailboxes ? "mailboxes" : "archive");
      json += ",\"scope\":\"" + AnsiString(scope) + "\"}";
      return json;
   }

   // ---------------------------------------------------------------- the runner

   void
   DiscoverySearch::Kick()
   {
      bool expected = false;
      if (!runner_running.compare_exchange_strong(expected, true))
         return;

      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetMaintenanceWorkQueue();
      if (!queue)
      {
         runner_running = false;
         return;
      }

      // A task the queue refuses while it stops, or drops unstarted when it is
      // stopped, leaves the flag up; Forget, called when the queue is taken
      // away, puts it down.
      std::shared_ptr<Task> task(new DiscoveryRunTask(runner_generation.load()));
      queue->AddTask(task);
   }

   void
   DiscoverySearch::Forget()
   {
      runner_generation++;
      runner_running = false;
   }

   void
   DiscoverySearch::RunQueue()
   {
      RunnerFlag flag(runner_generation.load());

      // A runner starts only when no other runs in this process, so a row
      // still marked running is one nobody is running: the server stopped, or
      // the maintenance queue was taken away, while it ran. It is failed with
      // the sentence rather than left running for ever - on every start of the
      // runner, not only the first in the process.
      DiscoveryStore::FailInterrupted();

      for (;;)
      {
         bool isSearch = false;
         __int64 id = 0;

         while (flag.Current() && DiscoveryStore::OldestQueued(isSearch, id))
         {
            // False only when the job's row could not be read or marked
            // running; asking again would find the same row, so the runner
            // stops and the next request starts it.
            if (!(isSearch ? RunSearch_(id) : RunExport_(id)))
               return;
         }

         flag.PutDown();

         // A job queued after the last look and before the flag went down
         // found the runner still running and did not start one.
         if (!DiscoveryStore::OldestQueued(isSearch, id))
            return;

         if (!flag.TakeUp())
            return;
      }
   }

   bool
   DiscoverySearch::RunSearch_(__int64 searchId)
   {
      DiscoveryStore::Search search;
      if (!DiscoveryStore::GetSearch(searchId, search))
         return false;

      if (search.state != DiscoveryStore::JobQueued)
         return true;

      if (!DiscoveryStore::MarkSearchRunning(searchId))
         return false;

      search.state = DiscoveryStore::JobRunning;

      Query query;
      String problem;
      if (!ParseQuery(search.query, query, problem))
      {
         search.state = DiscoveryStore::JobFailed;
         search.error = problem;
         DiscoveryStore::FinishSearch(search);
         return true;
      }

      const String directory = DiscoveryStore::CaseDirectory(search.case_id);
      String fileName;
      fileName.Format(_T("search-%I64d.json"), search.id);
      const String path = FileUtilities::Combine(directory, fileName);

      bool opened = FileUtilities::DirectoryExists(directory) || FileUtilities::CreateDirectory(directory);

      boost::filesystem::path filePath(path.begin(), path.end());
      boost::filesystem::ofstream out;
      if (opened)
      {
         out.open(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
         opened = out.is_open();
      }

      if (!opened)
      {
         search.state = DiscoveryStore::JobFailed;
         search.error = _T("The results file could not be written: ") + path;
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7334, "DiscoverySearch::RunSearch_",
            Formatter::Format("Discovery search {0} of case {1} could not write its results file {2}.", search.id, search.case_id, path));
         DiscoveryStore::FinishSearch(search);
         return true;
      }

      AnsiString head = "{\"case\":" + Number(search.case_id) + ",\"search\":" + Number(search.id) + ",\"query\":" + search.query +
                        ",\"ran_by\":" + Quoted(search.ran_by) + ",\"ran\":" + Number(search.ran) + ",\"hits\":[\n";
      out.write(head.c_str(), head.GetLength());

      Searcher searcher(search, query, out);
      String error;
      bool succeeded = false;

      try
      {
         succeeded = searcher.Run(error);
      }
      catch (const boost::thread_interrupted &)
      {
         // The service is stopping, or the maintenance queue going: the task's
         // to handle. The row stays running, and the runner's next start fails
         // it with the sentence (DiscoveryStore::FailInterrupted).
         throw;
      }
      catch (...)
      {
         succeeded = false;
         error = _T("The search stopped on an unexpected error.");
      }

      AnsiString tail = "]\n,\"matched\":" + Number(searcher.matched) + ",\"truncated\":" + Bool(searcher.truncated) +
                        ",\"scanned\":" + Number(searcher.scanned) + ",\"unsearched_count\":" + Number(searcher.unsearched_count) + ",\"unsearched\":[\n";
      for (size_t i = 0; i < searcher.unsearched.size(); i++)
         tail += (i > 0 ? AnsiString(",") : AnsiString()) + searcher.unsearched[i] + "\n";
      tail += "]}\n";
      out.write(tail.c_str(), tail.GetLength());
      out.close();

      const bool written = !out.fail();

      search.scanned = searcher.scanned;
      search.matched = searcher.matched;
      search.unsearched = searcher.unsearched_count;
      search.truncated = searcher.truncated;
      search.result_file = fileName;

      if (!succeeded || !written)
      {
         search.state = DiscoveryStore::JobFailed;
         search.error = succeeded ? String(_T("The results file could not be written: ")) + path : error;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7330, "DiscoverySearch::RunSearch_",
            Formatter::Format("Discovery search {0} of case {1} failed: {2}", search.id, search.case_id, search.error));
      }
      else
      {
         search.state = DiscoveryStore::JobDone;
         search.error = String();
      }

      DiscoveryStore::FinishSearch(search);

      LOG_APPLICATION(Formatter::Format("Discovery: search {0} of case {1} {2}: {3} message(s) read, {4} matched.",
         search.id, search.case_id, String(search.state == DiscoveryStore::JobDone ? _T("finished") : _T("failed")), search.scanned, search.matched));

      return true;
   }

   bool
   DiscoverySearch::RunExport_(__int64 exportId)
   {
      DiscoveryStore::Export result;
      if (!DiscoveryStore::GetExport(exportId, result))
         return false;

      if (result.state != DiscoveryStore::JobQueued)
         return true;

      if (!DiscoveryStore::MarkExportRunning(exportId))
         return false;

      result.state = DiscoveryStore::JobRunning;

      DiscoveryStore::Search search;
      DiscoveryStore::Case theCase;
      String error;
      bool succeeded = false;

      if (!DiscoveryStore::GetSearch(result.search_id, search) || !DiscoveryStore::GetCase(result.case_id, theCase))
      {
         error = _T("The search or the case this export belongs to could not be read.");
      }
      else
      {
         Exporter exporter(theCase, search, result);

         try
         {
            succeeded = exporter.Run(error);
         }
         catch (const boost::thread_interrupted &)
         {
            // As the search's: the row stays running until the runner's next
            // start fails it. The exporter has removed its half-written zip.
            throw;
         }
         catch (...)
         {
            succeeded = false;
            error = _T("The export stopped on an unexpected error.");
         }
      }

      if (succeeded)
      {
         result.state = DiscoveryStore::JobDone;
         result.error = String();
      }
      else
      {
         result.state = DiscoveryStore::JobFailed;
         result.error = error;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7331, "DiscoverySearch::RunExport_",
            Formatter::Format("Discovery export {0} of case {1} failed: {2}", result.id, result.case_id, error));
      }

      DiscoveryStore::FinishExport(result);

      LOG_APPLICATION(Formatter::Format("Discovery: export {0} of case {1} {2}: {3} message(s) written, {4} withheld.",
         result.id, result.case_id, String(succeeded ? _T("finished") : _T("failed")), result.items, result.withheld));

      return true;
   }

   // ---------------------------------------------------------------- the results

   bool
   DiscoverySearch::ReadResults(const String &path, int offset, int limit, std::vector<AnsiString> &hits, int &total,
                                int unsearchedOffset, int unsearchedLimit, std::vector<AnsiString> &unsearched, int &unsearchedListed)
   {
      hits.clear();
      unsearched.clear();
      total = 0;
      unsearchedListed = 0;

      boost::filesystem::path filePath(path.begin(), path.end());
      boost::filesystem::ifstream in(filePath, std::ios::in | std::ios::binary);
      if (!in)
         return false;

      // The file's shape, which RunSearch_ writes: a head line ending in
      // "hits":[, a hit per line, a line "]", a line of counts ending in
      // "unsearched":[, an entry per line, and "]}". A hit or an entry after the
      // first begins with its comma.
      enum Section { Head, Hits, Counts, Unsearched, Done };
      Section section = Head;
      std::string line;

      while (std::getline(in, line))
      {
         if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

         switch (section)
         {
         case Head:
            section = Hits;
            break;

         case Hits:
            if (line == "]")
            {
               section = Counts;
               break;
            }

            if (total >= offset && (int) hits.size() < limit)
               hits.push_back(AnsiString(line.size() > 0 && line[0] == ',' ? line.substr(1).c_str() : line.c_str()));

            total++;
            break;

         case Counts:
            section = Unsearched;
            break;

         case Unsearched:
            if (line == "]}")
            {
               section = Done;
               break;
            }

            if (unsearchedListed >= unsearchedOffset && (int) unsearched.size() < unsearchedLimit)
               unsearched.push_back(AnsiString(line.size() > 0 && line[0] == ',' ? line.substr(1).c_str() : line.c_str()));

            unsearchedListed++;
            break;

         case Done:
            break;
         }
      }

      return section != Head;
   }
}
