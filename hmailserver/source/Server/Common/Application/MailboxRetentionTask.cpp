// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "MailboxRetentionTask.h"

#include "Application.h"
#include "../BO/Domains.h"
#include "../BO/Domain.h"
#include "../BO/Accounts.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolders.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Messages.h"
#include "../BO/Message.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../Util/MessageUtilities.h"
#include "../Util/RecoverableStore.h"
#include "../Util/ConfidentialMessages.h"
#include "../Util/DomainReports.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/IMAPSpecialUse.h"
#include "../../IMAP/MessagesContainer.h"

#include <map>
#include <set>
#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The policy that applies to an account: its own when it has one, else the
      // domain's. 0 means none; -1 on the account means keep forever whatever the
      // domain says.
      int EffectiveRetentionDays_(int domainDays, int accountDays)
      {
         if (accountDays < 0)
            return 0;

         if (accountDays > 0)
            return accountDays;

         return domainDays > 0 ? domainDays : 0;
      }

      // "YYYY-MM-DD HH:MM:SS", local time, days ago - the format and the clock the
      // stored messagecreatetime uses (Time::GetCurrentDateTime), so the comparison
      // is between like and like.
      bool CutoffText_(int days, String &cutoff)
      {
         const std::time_t cutoffTime = std::time(nullptr) - (static_cast<std::time_t>(days) * 24 * 60 * 60);

         struct tm cutoffTm;
         if (localtime_s(&cutoffTm, &cutoffTime) != 0)
            return false;

         char text[32];
         if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &cutoffTm) == 0)
            return false;

         cutoff = text;
         return true;
      }

      // The delivered messages of an account older than the cutoff, by folder -
      // every folder, or one. The rows are read and the cutoff applied here rather
      // than in the WHERE clause; see the class comment. Only delivered messages: a
      // message still in the queue is not in a mailbox yet.
      void ExpiredByFolder_(std::shared_ptr<const Account> account, const String &cutoff, __int64 onlyFolderID,
                            std::map<__int64, std::set<__int64> > &expiredByFolder)
      {
         SQLCommand command("select messageid, messagefolderid, messagecreatetime from hm_messages where messageaccountid = @ACCOUNTID and messagetype = @TYPE");
         command.AddParameter("@ACCOUNTID", account->GetID());
         command.AddParameter("@TYPE", (int) Message::Delivered);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return;

         while (!recordset->IsEOF())
         {
            const __int64 folderID = recordset->GetInt64Value("messagefolderid");
            const String created = recordset->GetStringValue("messagecreatetime");

            // An unreadable stamp is left alone: deleting the only copy of a message on
            // the strength of a date that cannot be read is the wrong way to resolve it.
            if ((onlyFolderID == 0 || folderID == onlyFolderID) && created.GetLength() >= 19 && created.Compare(cutoff) < 0)
               expiredByFolder[folderID].insert(recordset->GetInt64Value("messageid"));

            recordset->MoveNext();
         }
      }

      // Through the folder's live collection, so the deletion is the one an
      // EXPUNGE performs: the file goes, the quota moves, the QRESYNC record is
      // written, and the collection every IMAP session shares is the one updated.
      // Refreshed first, because the sweep may be the first thing to touch this
      // folder since the service started and the collection is loaded lazily.
      int DeleteFromFolder_(std::shared_ptr<const Account> account, __int64 folderID, const std::set<__int64> &messageIDs)
      {
         std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(account->GetID(), folderID);
         if (!messages)
            return 0;

         messages->Refresh(false);

         RecoverableStore::Scope recoverable;   // a person deleting mail: kept for recovery
         const std::vector<__int64> deleted = messages->DeleteMessagesById(messageIDs);
         if (deleted.empty())
            return 0;

         // A session with the folder selected is told the way it is told about any
         // other expunge; its view decides when it may say so.
         std::shared_ptr<ChangeNotification> notification = std::shared_ptr<ChangeNotification>(
            new ChangeNotification(account->GetID(), folderID, ChangeNotification::NotificationMessageDeleted, deleted));
         Application::Instance()->GetNotificationServer()->SendNotification(notification);

         return (int) deleted.size();
      }

      int SweepAccount_(std::shared_ptr<const Account> account, int days)
      {
         String cutoff;
         if (!CutoffText_(days, cutoff))
            return 0;

         std::map<__int64, std::set<__int64> > expiredByFolder;
         ExpiredByFolder_(account, cutoff, 0, expiredByFolder);

         int deletedTotal = 0;
         for (const auto &folderEntry : expiredByFolder)
            deletedTotal += DeleteFromFolder_(account, folderEntry.first, folderEntry.second);

         if (deletedTotal > 0)
         {
            String message;
            message.Format(_T("Message retention: removed %d message(s) older than %d day(s) from %s."), deletedTotal, days, account->GetAddress().c_str());
            LOG_APPLICATION(message);
         }

         return deletedTotal;
      }

      // ---- The reader's own choices ------------------------------------------
      //
      // Three preferences the webmail keeps with the account (hm_accountprefs):
      // retention_trash_days, retention_junk_days and retention_archive_days - "empty
      // Trash after 30 days", "archive Inbox mail older than a year" - as the
      // reader's own settings beside the administrator's policy, run by the same
      // sweep. A value that is not a whole number of days from 1 to 3650 is off,
      // which is what an empty or absent row means too: nothing here guesses.

      int ReaderDays_(__int64 accountID, const String &name)
      {
         SQLCommand command("select prefvalue from hm_accountprefs where prefaccountid = @ACCOUNTID and prefname = @NAME");
         command.AddParameter("@ACCOUNTID", accountID);
         command.AddParameter("@NAME", name);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;

         String value = recordset->GetStringValue("prefvalue");
         value.Trim();

         if (value.IsEmpty() || value.GetLength() > 4)
            return 0;

         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value[i] < '0' || value[i] > '9')
               return 0;
         }

         const int days = _ttoi(value);
         return days >= 1 && days <= 3650 ? days : 0;
      }

      // The folder that carries a special-use designation - by its stored
      // attribute, or by the names clients have always used (Trash, Deleted Items,
      // Junk, Spam, Archive) - and, when asked, the top-level folder of that name
      // made if there is none: what the webmail's own "archive" does.
      std::shared_ptr<IMAPFolder> DesignatedFolder_(__int64 accountID, int designation, const String *createName)
      {
         std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(accountID);
         if (!folders)
            return std::shared_ptr<IMAPFolder>();

         for (int attempt = 0; attempt < 2; attempt++)
         {
            std::map<__int64, int> designations;
            IMAPSpecialUse::Resolve(folders, designations);

            for (std::map<__int64, int>::const_iterator it = designations.begin(); it != designations.end(); ++it)
            {
               if ((it->second & designation) == 0)
                  continue;

               std::shared_ptr<IMAPFolder> folder = folders->GetItemByDBIDRecursive(it->first);
               if (folder)
                  return folder;
            }

            if (!createName || attempt == 1)
               break;

            std::vector<String> path;
            path.push_back(*createName);
            folders->CreatePath(folders, path, true);
         }

         return std::shared_ptr<IMAPFolder>();
      }

      int EmptyOlderThan_(std::shared_ptr<const Account> account, int designation, int days, const TCHAR *what)
      {
         if (days <= 0)
            return 0;

         std::shared_ptr<IMAPFolder> folder = DesignatedFolder_(account->GetID(), designation, nullptr);
         if (!folder)
            return 0;

         String cutoff;
         if (!CutoffText_(days, cutoff))
            return 0;

         std::map<__int64, std::set<__int64> > expired;
         ExpiredByFolder_(account, cutoff, folder->GetID(), expired);
         if (expired.empty())
            return 0;

         const int deleted = DeleteFromFolder_(account, folder->GetID(), expired[folder->GetID()]);

         if (deleted > 0)
         {
            String message;
            message.Format(_T("Message retention: removed %d message(s) older than %d day(s) from the %s folder of %s, as the account holder chose."),
               deleted, days, what, account->GetAddress().c_str());
            LOG_APPLICATION(message);
         }

         return deleted;
      }

      // Inbox mail older than the choice goes to the Archive folder the way the
      // webmail's own archive moves it: copied there, then removed here, so it is a
      // new row with a new UID in the destination and every session on either
      // folder is told. A copy that could not be followed by the removal leaves the
      // copy, which is the safe way round.
      int ArchiveOlderThan_(std::shared_ptr<const Account> account, int days)
      {
         if (days <= 0)
            return 0;

         std::shared_ptr<IMAPFolders> folders = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());
         if (!folders)
            return 0;

         std::shared_ptr<IMAPFolder> inbox = folders->GetFolderByName(_T("INBOX"));
         if (!inbox)
            return 0;

         String cutoff;
         if (!CutoffText_(days, cutoff))
            return 0;

         std::map<__int64, std::set<__int64> > expired;
         ExpiredByFolder_(account, cutoff, inbox->GetID(), expired);
         if (expired.empty())
            return 0;

         const String archiveName = _T("Archive");
         std::shared_ptr<IMAPFolder> archive = DesignatedFolder_(account->GetID(), IMAPSpecialUse::DesignationArchive, &archiveName);
         if (!archive || archive->GetID() == inbox->GetID())
            return 0;

         std::set<__int64> copied;
         for (const __int64 messageID : expired[inbox->GetID()])
         {
            std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
            if (!PersistentMessage::ReadObject(message, messageID) || message->GetID() == 0)
               continue;

            __int64 newMessageID = 0;
            if (MessageUtilities::CopyToIMAPFolder(message, (int) archive->GetID(), newMessageID))
               copied.insert(messageID);
         }

         if (copied.empty())
            return 0;

         const int archived = DeleteFromFolder_(account, inbox->GetID(), copied);

         if (archived > 0)
         {
            String message;
            message.Format(_T("Message retention: archived %d message(s) older than %d day(s) from the Inbox of %s, as the account holder chose."),
               archived, days, account->GetAddress().c_str());
            LOG_APPLICATION(message);
         }

         return archived;
      }

      int SweepReaderChoices_(std::shared_ptr<const Account> account, int &archived)
      {
         int deleted = 0;

         deleted += EmptyOlderThan_(account, IMAPSpecialUse::DesignationTrash, ReaderDays_(account->GetID(), _T("retention_trash_days")), _T("Trash"));
         deleted += EmptyOlderThan_(account, IMAPSpecialUse::DesignationJunk, ReaderDays_(account->GetID(), _T("retention_junk_days")), _T("Junk"));
         archived += ArchiveOlderThan_(account, ReaderDays_(account->GetID(), _T("retention_archive_days")));

         return deleted;
      }
   }

   MailboxRetentionTask::MailboxRetentionTask(void)
   {

   }

   MailboxRetentionTask::~MailboxRetentionTask(void)
   {

   }

   void
   MailboxRetentionTask::DoWork()
   {
      Sweep();
   }

   int
   MailboxRetentionTask::Sweep()
   {
      int archived = 0;
      return Sweep(archived);
   }

   int
   MailboxRetentionTask::Sweep(int &archived)
   {
      int deleted = 0;
      archived = 0;

      // Read afresh rather than from the object cache: the sweep runs a few times a
      // day, and a policy changed since the cache was filled is exactly the change it
      // must not miss.
      Domains domains;
      domains.Refresh();

      for (int i = 0; i < domains.GetCount(); i++)
      {
         std::shared_ptr<Domain> domain = domains.GetItem(i);
         if (!domain)
            continue;

         std::shared_ptr<Accounts> accounts = domain->GetAccounts();
         if (!accounts)
            continue;

         accounts->Refresh();

         for (int j = 0; j < accounts->GetCount(); j++)
         {
            std::shared_ptr<Account> account = accounts->GetItem(j);
            if (!account)
               continue;

            // The administrator's policy first, then the account holder's own
            // choices - which reach only the folders they name.
            const int days = EffectiveRetentionDays_(domain->GetMessageRetentionDays(), account->GetMessageRetentionDays());
            if (days > 0)
               deleted += SweepAccount_(account, days);

            deleted += SweepReaderChoices_(account, archived);
         }
      }

      // And the deleted items whose recovery window has ended, and the kept
      // copies of messages that have expired (the minute task sweeps those too;
      // this is the sweep an administrator can run on demand, and the one that
      // removes the directories of accounts that no longer exist).
      RecoverableStore::Sweep();
      ConfidentialMessages::Sweep(true);

      // And the DMARC aggregate and TLS-RPT reports read more than
      // DomainReportRetentionDays ago.
      DomainReports::Sweep();

      return deleted;
   }
}
