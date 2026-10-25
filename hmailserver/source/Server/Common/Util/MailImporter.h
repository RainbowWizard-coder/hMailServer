// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2006 Martin Knafve / hMailServer.com.

#pragma once

#include "Time.h"

#include <map>
#include <set>
#include <vector>

namespace HM
{
   class MessageData;
   class Message;
   class Account;

   class MailImporter
   {
   public:
      MailImporter(void);
      ~MailImporter(void);

      static bool Import (const String &originalFullPath, int iAccountID, const String &sIMAPFolder);
      // Imports a message from file to the database.

      //------------------------------------------------------------------------
      // THE IMPORT ENTRY: one message file into one folder of one account, with
      // the flags, keywords and internal date its source gave it, as an IMAP
      // APPEND files one. A migration job (Application/MigrationJob.h) copies
      // every message through it, and it is the seam any other importer of a
      // mailbox reuses - a PST reader among them - rather than Import above,
      // which takes no flags, no keywords and no date, and sends its folder
      // through the rule engine's MoveToIMAPFolder (%YEAR% substituted, no
      // folder-name check).
      //
      // What it asks, in APPEND's order: the size - the 2 GB ceiling and the
      // account's own max_message_size_kb, IMAPCommandAppend's
      // GetEffectiveAppendLimitBytes - then the room on the data directory's
      // volume, then the duplicate check, then the account's quota. The folder is
      // a list of names, checked with IMAPFolder::IsValidFolderName and made when
      // it is missing (subscribed, and given the designation when it names one
      // and no other folder of the account holds it explicitly). The row takes
      // the folder's next UID through PersistentMessage::SaveObject, the sender
      // and the date are read from the header where the caller gave none, and
      // the folder's sessions are told.
      //------------------------------------------------------------------------

      enum ImportOutcome
      {
         ImportDone = 0,
         ImportDuplicate = 1,           // the same job wrote this message into this folder already
         ImportDuplicateElsewhere = 2,  // ... into another folder, and the caller asked across folders
         ImportTooLarge = 3,            // over the account's limit or the 2 GB ceiling
         ImportOverQuota = 4,           // the account's quota has no room for it
         ImportNoDiskSpace = 5,         // the data directory's volume is short of room, as APPEND refuses
         ImportFolderRefused = 6,       // a folder of that name can never be made in this account
         ImportFailed = 7               // the file could not be placed, the row not saved, or the folder
                                        // not made just now (its tree unreadable, the database away)
      };

      // What one import job has written, for its duplicate check: the key of
      // each message (MessageKey) by the folder it went into, and across every
      // folder. The job keeps one, the import adds to it, and after an
      // interruption the job builds it back with LoadWrittenKeys.
      struct WrittenKeys
      {
         std::map<String, std::set<AnsiString> > byFolder;   // the folder's names joined by the hierarchy delimiter
         std::set<AnsiString> anywhere;
      };

      // messageFile is anywhere under the data directory - the queue directory,
      // as a fetch spools - and is moved into the account's own directory, so the
      // file is gone from there on ImportDone. On any other outcome the file is
      // left where it was, for the caller to remove, and nothing was saved.
      // internalDate is the IMAP INTERNALDATE ("17-Jul-1996 02:44:25 -0700");
      // empty, the Date header's. written may be null (no duplicate check);
      // acrossFolders checks it against every folder, not only this one.
      // archived is a message from an archive - a PST - rather than one being
      // sent to the account now: only the 2 GB ceiling is asked of its size,
      // not the account's max_message_size_kb.
      static ImportOutcome ImportMessage(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                                         int designation, const String &messageFile, short flags,
                                         const std::vector<String> &keywords, const String &internalDate,
                                         WrittenKeys *written, bool acrossFolders, String &problem, bool archived = false);

      // The account's folder at a path, made when it is missing, as ImportMessage
      // makes it; 0 with the reason when it cannot be. permanent then says
      // whether it never can be - a name the account refuses - or could not be
      // just now, the account's folders unreadable or the database away.
      static __int64 FolderFor(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                               int designation, String &problem);
      static __int64 FolderFor(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                               int designation, String &problem, bool &permanent);

      // What makes two copies of a message the same message: its Message-ID and
      // its size together - two different messages a sender gave one Message-ID
      // are rarely the same size - and without a Message-ID its Date, its
      // Subject and its size.
      static AnsiString MessageKey(const String &fileName);

      // The keys of what is in one folder of the account and was saved at or
      // after since (the server's local time, as hm_messages keeps
      // messagesavedate): a job's own writes since it first started - and what
      // was delivered beside them, which only ever suppresses a second copy of a
      // message the folder holds already. A folder that does not exist has none.
      static bool LoadWrittenKeys(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                                  const String &since, WrittenKeys &written);

      // The names joined by the hierarchy delimiter: WrittenKeys' key.
      static String JoinPath(const std::vector<String> &folderPath);

   private:

      static bool ReplaceMessagePath_(__int64 messageID, const String &messagePath);
      static String GetInternalDate_(std::shared_ptr<MessageData> pMessageData);
      static String GetInternalDate_DatePlusTimeZone(DateTime dtTime);
      static bool MoveToNamedSubdirectory_(const String &sourceFile, String &resultFile, String &newFullPath);
      static bool GetRootLevelDirectory_(const String &fullPath, String &rootLevel);
   };

#ifdef _DEBUG
   class MailImporterTester
   {
   public :
      MailImporterTester () {};
      ~MailImporterTester () {};      

      void Test();
   };

#endif 
}