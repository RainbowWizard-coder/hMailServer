// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "RecoverableStore.h"
#include "DiscoveryHolds.h"
#include "FileUtilities.h"
#include "GUIDCreator.h"
#include "ZeroAccess.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../Application/EffectiveSetting.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"

#include <boost/filesystem.hpp>
#include <ctime>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      thread_local int scopes = 0;
      thread_local int moves = 0;

      String Root()
      {
         return FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Recoverable"));
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

      // The folder's path as IMAP names it, from its parents.
      String PathOf(std::shared_ptr<IMAPFolders> folders, __int64 folderId)
      {
         const String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();
         String path;
         __int64 id = folderId;
         for (int depth = 0; depth < 50 && id > 0 && folders; depth++)
         {
            std::shared_ptr<IMAPFolder> folder = folders->GetItemByDBIDRecursive(id);
            if (!folder)
               break;
            path = path.IsEmpty() ? folder->GetFolderName() : folder->GetFolderName() + delimiter + path;
            id = folder->GetParentFolderID();
         }
         return path;
      }
   }

   RecoverableStore::Scope::Scope()
   {
      scopes++;
   }

   RecoverableStore::Scope::~Scope()
   {
      scopes--;
   }

   RecoverableStore::MoveScope::MoveScope()
   {
      moves++;
   }

   RecoverableStore::MoveScope::~MoveScope()
   {
      moves--;
   }

   int
   RecoverableStore::Days(__int64 accountId)
   {
      std::shared_ptr<const Account> account;
      if (accountId > 0)
         account = CacheContainer::Instance()->GetAccount(accountId);

      // The server's value is clamped to 0-365 by the resolver as it always
      // was here; a domain's or an account's is refused outside it when set.
      const int days = EffectiveSetting::ResolveInt(_T("recover_deleted_days"), std::shared_ptr<const Domain>(), account);
      return days < 0 ? 0 : (days > 365 ? 365 : days);
   }

   bool
   RecoverableStore::Keep(std::shared_ptr<Message> message, bool sealPlaintext)
   {
      if (!message || message->GetAccountID() <= 0 || message->GetFolderID() <= 0)
         return true;

      // Two reasons to keep a copy, asked in this order so that an account the
      // store is on for pays nothing more: a person deleting mail inside the
      // recovery window, as always; and a legal hold on the account, for any
      // delete but the original of a move (docs/Discovery.md).
      const bool recoverable = scopes > 0 && Days(message->GetAccountID()) > 0;
      const bool held = moves <= 0 && DiscoveryHolds::IsAccountHeld(message->GetAccountID());

      if (!recoverable && !held)
         return true;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(message->GetAccountID());
      if (!account)
         return !held;

      const String source = PersistentMessage::GetFileName(account, message);
      if (!FileUtilities::Exists(source))
         return true;

      String guid = GUIDCreator::GetGUID();
      guid.Replace(_T("{"), _T(""));
      guid.Replace(_T("}"), _T(""));
      String relative;
      relative.Format(_T("%I64d"), message->GetAccountID());
      relative = FileUtilities::Combine(relative, guid + _T(".eml"));
      const String target = FileUtilities::Combine(Root(), relative);

      if (!FileUtilities::Copy(source, target, true))
      {
         if (held)
         {
            // The delete is refused rather than made without the copy: that is
            // what a hold is.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7333, "RecoverableStore::Keep",
               "The message " + source + " of " + account->GetAddress() + ", an account under a legal hold, could not be kept at " + target +
               ", so it has not been deleted. It stays where it is until its copy can be made or the hold is released.");
            return false;
         }

         LOG_APPLICATION("RecoverableStore: the deleted message " + source + " could not be kept at " + target + "; it is deleted without a copy.");
         return true;
      }

      // A zero-access mailbox's message caught before its seal: the copy is
      // sealed as the mailbox would have held it, and a copy that cannot be is
      // not kept in the clear (docs/ZeroAccess.md, docs/Discovery.md).
      if (sealPlaintext && account->GetZeroAccess())
      {
         AnsiString problem;
         if (!ZeroAccess::SealFile(account, target, problem))
         {
            // SealFile leaves the file whole on failure; the plaintext copy is
            // overwritten before it goes, as a failed delivery seal's is.
            ZeroAccess::Scrub(target);
            FileUtilities::DeleteFile(target);

            if (held)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7333, "RecoverableStore::Keep",
                  "The message " + source + " of " + account->GetAddress() + ", a zero-access mailbox under a legal hold, could not be sealed to its key at " + target +
                  " (" + String(problem) + "), so no copy of it was kept in the clear and it has not been dropped.");
               return false;
            }

            LOG_APPLICATION("RecoverableStore: the message " + source + " of the zero-access mailbox " + account->GetAddress() + " could not be sealed at " + target + " (" + String(problem) + "); no copy was kept.");
            return true;
         }
      }

      AnsiString header = PersistentMessage::LoadHeader(source, false);
      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(message->GetAccountID());

      SQLStatement statement(SQLStatement::STInsert, "hm_recoverable");
      statement.AddColumnInt64("rcaccountid", message->GetAccountID());
      statement.AddColumnInt64("rcfolderid", message->GetFolderID());
      statement.AddColumn("rcfolder", Clip(PathOf(folders, message->GetFolderID()), 255));
      statement.AddColumn("rcfile", relative);
      statement.AddColumn("rcsize", (int) message->GetSize());
      statement.AddColumn("rcsubject", Clip(mimeHeader.GetUnicodeFieldValue("Subject"), 255));
      statement.AddColumn("rcfrom", Clip(mimeHeader.GetUnicodeFieldValue("From"), 255));
      statement.AddColumn("rcflags", (int) message->GetFlags());
      statement.AddColumnInt64("rcdeleted", (__int64) time(nullptr));
      statement.SetIdentityColumn("rcid");

      __int64 made = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &made) || made <= 0)
      {
         FileUtilities::DeleteFile(target);

         if (held)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7333, "RecoverableStore::Keep",
               "The message " + source + " of " + account->GetAddress() + ", an account under a legal hold, could not be recorded among its deleted items, so it has not been deleted. It stays where it is until it can be or the hold is released.");
            return false;
         }

         return true;
      }

      if (held && !recoverable)
         LOG_DEBUG("RecoverableStore: a message of " + account->GetAddress() + " was deleted and is kept among its deleted items, because the account is under a legal hold.");

      return true;
   }

   String
   RecoverableStore::KeptFile(const Entry &entry)
   {
      return FileUtilities::Combine(Root(), entry.file);
   }

   bool
   RecoverableStore::KeptFileOf(__int64 accountId, __int64 id, String &path)
   {
      SQLCommand command("select rcfile from hm_recoverable where rcaccountid = @ACCOUNTID and rcid = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      path = FileUtilities::Combine(Root(), recordset->GetStringValue("rcfile"));
      return true;
   }

   bool
   RecoverableStore::List(__int64 accountId, std::vector<Entry> &entries)
   {
      entries.clear();
      SQLCommand command("select rcid, rcfolderid, rcfolder, rcsubject, rcfrom, rcsize, rcflags, rcdeleted, rcfile from hm_recoverable where rcaccountid = @ACCOUNTID order by rcid desc");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Entry entry;
         entry.id = recordset->GetInt64Value("rcid");
         entry.folder_id = recordset->GetInt64Value("rcfolderid");
         entry.folder = recordset->GetStringValue("rcfolder");
         entry.subject = recordset->GetStringValue("rcsubject");
         entry.from = recordset->GetStringValue("rcfrom");
         entry.size = (int) recordset->GetLongValue("rcsize");
         entry.flags = (int) recordset->GetLongValue("rcflags");
         entry.deleted = recordset->GetInt64Value("rcdeleted");
         entry.file = recordset->GetStringValue("rcfile");
         entries.push_back(entry);
         recordset->MoveNext();
      }
      return true;
   }

   bool
   RecoverableStore::Restore(std::shared_ptr<const Account> account, __int64 id, __int64 &folderId, __int64 &messageId, String &problem)
   {
      SQLCommand command("select rcfolderid, rcfolder, rcfile, rcflags from hm_recoverable where rcaccountid = @ACCOUNTID and rcid = @ID");
      command.AddParameter("@ACCOUNTID", account->GetID());
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
      {
         problem = _T("not found");
         return false;
      }

      const __int64 originalFolder = recordset->GetInt64Value("rcfolderid");
      const String path = recordset->GetStringValue("rcfolder");
      const String relative = recordset->GetStringValue("rcfile");
      const short flags = (short) recordset->GetLongValue("rcflags");
      const String kept = FileUtilities::Combine(Root(), relative);

      if (!FileUtilities::Exists(kept))
      {
         Purge(account->GetID(), id);
         problem = _T("the kept copy is gone");
         return false;
      }

      std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
      std::shared_ptr<IMAPFolder> folder = folders ? folders->GetItemByDBIDRecursive(originalFolder) : std::shared_ptr<IMAPFolder>();
      if (!folder && folders && !path.IsEmpty())
         folder = folders->GetFolderByFullPath(path);
      if (!folder && folders)
         folder = folders->GetFolderByName(_T("INBOX"));
      if (!folder)
      {
         problem = _T("there is no folder to put it back in");
         return false;
      }

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetAccountID(account->GetID());
      message->SetFolderID(folder->GetID());
      const String fileName = PersistentMessage::GetFileName(account, message);
      const String directory = FileUtilities::GetFilePath(fileName);
      if (!FileUtilities::Exists(directory) && !FileUtilities::CreateDirectory(directory))
      {
         problem = _T("the account's directory could not be made");
         return false;
      }
      if (!FileUtilities::Copy(kept, fileName, true))
      {
         problem = _T("the message could not be written back");
         return false;
      }

      message->SetSize((int) FileUtilities::FileSize(fileName));
      message->SetFlags(flags);
      message->SetFlagDeleted(false);
      message->SetFlagRecent(true);
      message->SetState(Message::Delivered);
      if (!PersistentMessage::SaveObject(message))
      {
         FileUtilities::DeleteFile(fileName);
         problem = _T("the message could not be stored");
         return false;
      }

      MessagesContainer::Instance()->SetFolderNeedsRefresh(folder->GetID());
      std::vector<__int64> added;
      added.push_back(message->GetID());
      std::shared_ptr<ChangeNotification> notification =
         std::shared_ptr<ChangeNotification>(new ChangeNotification(account->GetID(), folder->GetID(), ChangeNotification::NotificationMessageAdded, added));
      Application::Instance()->GetNotificationServer()->SendNotification(notification);

      Purge(account->GetID(), id);

      folderId = folder->GetID();
      messageId = message->GetID();
      return true;
   }

   bool
   RecoverableStore::Purge(__int64 accountId, __int64 id)
   {
      SQLCommand command("select rcfile from hm_recoverable where rcaccountid = @ACCOUNTID and rcid = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      const String kept = FileUtilities::Combine(Root(), recordset->GetStringValue("rcfile"));

      SQLCommand remove("delete from hm_recoverable where rcaccountid = @ACCOUNTID and rcid = @ID");
      remove.AddParameter("@ACCOUNTID", accountId);
      remove.AddParameter("@ID", id);
      if (!Application::Instance()->GetDBManager()->Execute(remove))
         return false;

      if (FileUtilities::Exists(kept))
         FileUtilities::DeleteFile(kept);
      return true;
   }

   int
   RecoverableStore::Sweep()
   {
      // The window is each account's own (recover_deleted_days has levels), so
      // the rows are read from the shortest window anywhere - the server's, or
      // the shortest a domain or an account holds - and each is ended against
      // its own account's. With no window of a domain's or an account's own
      // this is the server's window for every row, as it always was.
      const __int64 now = (__int64) time(nullptr);
      const __int64 cutoff = now - (__int64) EffectiveSetting::SmallestAnywhere(_T("recover_deleted_days")) * 24 * 60 * 60;

      // No recordset is the database not answering, and a sweep that cannot
      // read the rows must not act on the files: nothing below runs.
      SQLCommand command("select rcid, rcaccountid, rcdeleted from hm_recoverable where rcdeleted <= @CUTOFF");
      command.AddParameter("@CUTOFF", cutoff);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;

      std::map<__int64, __int64> accountCutoffs;
      std::map<__int64, bool> accountsHeld;
      std::vector<std::pair<__int64, __int64> > expired;
      while (!recordset->IsEOF())
      {
         const __int64 accountId = recordset->GetInt64Value("rcaccountid");

         auto known = accountCutoffs.find(accountId);
         if (known == accountCutoffs.end())
            known = accountCutoffs.insert(std::make_pair(accountId, now - (__int64) Days(accountId) * 24 * 60 * 60)).first;

         // A held account's copies are what its hold keeps, however old they
         // are (docs/Discovery.md): they end at the first sweep after the last
         // hold on the account is released.
         auto held = accountsHeld.find(accountId);
         if (held == accountsHeld.end())
            held = accountsHeld.insert(std::make_pair(accountId, DiscoveryHolds::IsAccountHeld(accountId))).first;

         if (!held->second && recordset->GetInt64Value("rcdeleted") <= known->second)
            expired.push_back(std::make_pair(accountId, recordset->GetInt64Value("rcid")));

         recordset->MoveNext();
      }

      int ended = 0;
      for (const auto &entry : expired)
      {
         if (Purge(entry.first, entry.second))
            ended++;
      }

      // A deleted account's rows went with it; its directory is removed here. Only
      // a real directory named by a number is ever looked at, and a symlink never
      // - and "gone" is what the database said when asked for that account, never
      // a cache lookup that came back empty because the database was away: an
      // outage must not delete every account's recoverable items.
      const String root = Root();
      if (FileUtilities::DirectoryExists(root))
      {
         boost::system::error_code error_code;
         const boost::filesystem::path rootPath(root.c_str());
         std::vector<boost::filesystem::path> gone;
         for (boost::filesystem::directory_iterator entry(rootPath, error_code);
              entry != boost::filesystem::directory_iterator(); entry.increment(error_code))
         {
            if (error_code)
               break;
            const boost::filesystem::path current = entry->path();
            if (boost::filesystem::is_symlink(current, error_code) || !boost::filesystem::is_directory(current, error_code))
               continue;
            const std::string name = current.filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || name.size() > 18)
               continue;
            const __int64 accountId = std::stoll(name);
            if (accountId <= 0)
               continue;
            SQLCommand exists("select accountid from hm_accounts where accountid = @ID");
            exists.AddParameter("@ID", accountId);
            std::shared_ptr<DALRecordset> answer = Application::Instance()->GetDBManager()->OpenRecordset(exists);
            if (answer && answer->IsEOF())
               gone.push_back(current);
         }
         for (const boost::filesystem::path &directory : gone)
            boost::filesystem::remove_all(directory, error_code);
      }

      return ended;
   }
}
