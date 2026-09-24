// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "MailImporter.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Util/Time.h"
#include "../Util/Utilities.h"
#include "../Util/FileUtilities.h"

#include "../Persistence/PersistentMessage.h"

#include "Parsing/AddresslistParser.h"
#include "../../SMTP/RecipientParser.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"
#include "../BO/Account.h"
#include "../Mime/Mime.h"
#include "../Sieve/SieveParser.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../SQL/SQLCommand.h"
#include "DiskSpace.h"
#include "Strings/Formatter.h"
#include "../../IMAP/IMAPCommandAppend.h"
#include "../../IMAP/IMAPSpecialUse.h"

#include "MessageUtilities.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   MailImporter::MailImporter(void)
   {
   }

   MailImporter::~MailImporter(void)
   {
   }

   bool
   MailImporter::Import (const String &originalFullPath, int iAccountID, const String &sIMAPFolder)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Imports a message located in the data directory into the queue or into an account.
   // If the message is located into an account, the ID of that account must be supplied.
   // hMailServer could determine this automatically, but we're lazy.
   //---------------------------------------------------------------------------()
   {
      // Check so that the file really exists.
      if (!FileUtilities::Exists(originalFullPath))
         return false;

      // The message must be located in the data directory.
      String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      // Retrieve the ID of the message.
      __int64 messageID;
      bool isPartialFileName = false;
      if (!PersistentMessage::GetMessageID(originalFullPath, messageID, isPartialFileName))
         return false;

      // Check so that this message isn't already referenced in the database.
      if (messageID > 0)
      {
         // The file apparently already exists. But if the full path is stored in the database,
         // we should replace that with the partial file name.
         if (isPartialFileName)
            return true;
         else
         {
            bool result = ReplaceMessagePath_(messageID, originalFullPath);
            
            if (iAccountID > 0 && result)
            {
               MessagesContainer::Instance()->UncacheAccount(iAccountID);
            }

            return result;

         }
      }

      // Not referenced in the database, and under the public folder tree. Which
      // public IMAP folder a file there belongs to is not part of its path on disk -
      // every public folder's messages share the one #Public directory - so there is
      // no way to tell, and the code below would guess: relocate the file and, given
      // an account id, file it into that account's inbox while the file itself stayed
      // behind under #Public. Refusing is the only honest answer; the synchronizer
      // reports the file as skipped, which is what it already does for anything it
      // cannot place. Upstream #601.
      String publicFolderPath = FileUtilities::Combine(dataDirectory, IMAPConfiguration::GetPublicFolderDiskName());

      if (originalFullPath.StartsWith(publicFolderPath + FileUtilities::PathSeparator))
         return false;

      String newFullPath = originalFullPath;

      // Construct a partial file name.
      String partialFileName;
      if (!PersistentMessage::GetPartialFilename(newFullPath, partialFileName))
      {
         // It was not possible to construct a partial file name from the full path.
         String movedFile;
         if (!MoveToNamedSubdirectory_(originalFullPath, partialFileName, newFullPath))
            return false;
      }

      if (iAccountID > 0)
      {
         // Make sure we're really placing the file into an account.
         int domainSlashPos = originalFullPath.Find(FileUtilities::PathSeparator, dataDirectory.GetLength()+1);

         if (domainSlashPos < 0)
            return false;

         int accountSlashPos = originalFullPath.Find(FileUtilities::PathSeparator, domainSlashPos+1);

         if (accountSlashPos < 0)
            return false;
      }
      else
      {
         // Make sure the file is placed into the data directory root.
         int invalidSlash = originalFullPath.Find(FileUtilities::PathSeparator, dataDirectory.GetLength()+1);
         
         if (invalidSlash > 0)
         {
            // The message doesn't appear to be placed into the data directory.
            return false;
         }
      }

      std::shared_ptr<Message> pMessage = std::shared_ptr<Message> (new Message);
      pMessage->SetPartialFileName(partialFileName);

      // Set message size
      pMessage->SetSize(FileUtilities::FileSize(newFullPath));

      std::shared_ptr<MessageData> pMsgData = std::shared_ptr<MessageData>(new MessageData());
      if (!pMsgData->LoadFromMessage(newFullPath, pMessage))
         return false;

      // Check what we should set the internal date to.
      String sReceivedHeader = GetInternalDate_(pMsgData);
      pMessage->SetCreateTime(sReceivedHeader);
   
      // Parse out the From-address
      std::shared_ptr<AddresslistParser> pFromParser = std::shared_ptr<AddresslistParser>(new AddresslistParser());
      std::vector<std::shared_ptr<Address> > vecAddresses = pFromParser->ParseList(pMsgData->GetFrom());
      auto iterElement = vecAddresses.begin();

      if (iterElement != vecAddresses.end())
      {
         String sAddress = (*iterElement)->sMailboxName + "@" + (*iterElement)->sDomainName;
         pMessage->SetFromAddress(sAddress);
      }

      if (iAccountID == 0)
      {
         // Since this message is supposed to be delivered, we need
         // to parse the sender and recipients
         std::shared_ptr<AddresslistParser> pToParser = std::shared_ptr<AddresslistParser>(new AddresslistParser());
         std::vector<std::shared_ptr<Address> > vecAddresses = pToParser->ParseList(pMsgData->GetTo());
         auto iterElement = vecAddresses.begin();

         RecipientParser recipientParser;
         while (iterElement != vecAddresses.end())
         {
            String sAddress = (*iterElement)->sMailboxName + "@" + (*iterElement)->sDomainName;
            bool recipientOK = false;
            recipientParser.CreateMessageRecipientList(sAddress, pMessage->GetRecipients(), recipientOK);
            iterElement++;
         }

         // Since this message is supposed to be delivered, we need
         // to parse the sender and recipients
         pToParser = std::shared_ptr<AddresslistParser>(new AddresslistParser());
         vecAddresses = pToParser->ParseList(pMsgData->GetCC());
         iterElement = vecAddresses.begin();

         while (iterElement != vecAddresses.end())
         {
            String sAddress = (*iterElement)->sMailboxName + "@" + (*iterElement)->sDomainName;
            bool recipientOK = false;
            recipientParser.CreateMessageRecipientList(sAddress, pMessage->GetRecipients(), recipientOK);
            iterElement++;
         }

         pMessage->SetState(Message::Delivering);

         // Remove all non-local recipients to prevent duplicate email from being sent.
         pMessage->GetRecipients()->RemoveNonLocalAccounts();

         if (pMessage->GetRecipients()->GetCount() == 0)
            return false;
      }
      else
      {
         pMessage->SetAccountID(iAccountID);
         pMessage->SetState(Message::Delivered);

         // Check if we should move it to an IMAP folder
         if (!sIMAPFolder.IsEmpty())
         {
            __int64 iResultAccount = 0;
            __int64 iResultFolder = 0;
            MessageUtilities::MoveToIMAPFolder(pMessage, iAccountID, sIMAPFolder, true, true, iResultAccount, iResultFolder);
         }
         else
         {
            __int64 inboxID = CacheContainer::Instance()->GetInboxIDCache().GetUserInboxFolder(iAccountID);
            if (inboxID == 0)
               return false;

            pMessage->SetFolderID(inboxID);
         }
      }

      // Add the object to the database.
      if (!PersistentMessage::SaveObject(pMessage))
         return false;

      // A session with the folder open holds its message list in the cache and
      // would not see the import until something else refreshed it - the Import
      // Tool runs while clients are connected. Told to refresh, it does, on its
      // next command, exactly as it learns of a delivery.
      if (pMessage->GetFolderID() > 0)
         MessagesContainer::Instance()->SetFolderNeedsRefresh(pMessage->GetFolderID());

      return true;
   }

   String
   MailImporter::JoinPath(const std::vector<String> &folderPath)
   {
      const String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      String path;
      for (size_t i = 0; i < folderPath.size(); i++)
      {
         if (i > 0)
            path += delimiter;
         path += folderPath[i];
      }
      return path;
   }

   __int64
   MailImporter::FolderFor(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                           int designation, String &problem)
   {
      bool permanent = true;
      return FolderFor(owner, folderPath, designation, problem, permanent);
   }

   __int64
   MailImporter::FolderFor(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                           int designation, String &problem, bool &permanent)
   {
      permanent = true;

      if (!owner || folderPath.empty())
      {
         problem = _T("there is no account or folder to file the message into");
         return 0;
      }

      // The check IMAP's CREATE makes, which the rule engine's
      // MoveToIMAPFolder does not: a name no client could have made is not made
      // here either.
      if (!IMAPFolder::IsValidFolderName(folderPath, false))
      {
         problem = _T("a folder of that name cannot be made in this account");
         return 0;
      }

      // From here a failure is the account's folders or the database, not the
      // name: another attempt may make the folder.
      permanent = false;

      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(owner->GetID());
      if (!tree)
      {
         problem = _T("the account's folders could not be read");
         return 0;
      }

      std::shared_ptr<IMAPFolder> folder = tree->GetFolderByFullPath(folderPath);
      if (!folder)
      {
         tree->CreatePath(tree, folderPath, true);
         folder = tree->GetFolderByFullPath(folderPath);

         if (!folder)
         {
            problem = _T("the folder could not be made in this account");
            return 0;
         }

         // The designation the source gave the folder, on a folder made here,
         // when no folder of the account holds it by its own designation - one
         // \Sent is what a client can file into; two are none.
         const int wanted = designation & IMAPSpecialUse::DesignationMask;
         if (wanted != IMAPSpecialUse::DesignationNone &&
             (IMAPSpecialUse::GetExplicitDesignations(tree, folder->GetID()) & wanted) == 0)
            folder->StoreSpecialUseFlags(wanted);
      }

      return folder->GetID();
   }

   AnsiString
   MailImporter::MessageKey(const String &fileName)
   {
      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      MimeHeader mime;
      if (!header.IsEmpty())
         mime.Load(header.GetBuffer(), header.GetLength(), false);

      unsigned __int64 size = 0;
      FileUtilities::FileSize64(fileName, size);

      AnsiString sizeText;
      sizeText.Format("%I64u", size);

      const char *messageId = header.IsEmpty() ? nullptr : mime.GetRawFieldValue("Message-ID");
      AnsiString id = messageId ? AnsiString(messageId) : AnsiString();
      id.Trim();
      if (!id.IsEmpty())
         return "id:" + id + "|" + sizeText;

      const char *date = header.IsEmpty() ? nullptr : mime.GetRawFieldValue("Date");
      const char *subject = header.IsEmpty() ? nullptr : mime.GetRawFieldValue("Subject");
      return "hd:" + AnsiString(date ? date : "") + "|" + AnsiString(subject ? subject : "") + "|" + sizeText;
   }

   bool
   MailImporter::LoadWrittenKeys(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                                 const String &since, WrittenKeys &written)
   {
      if (!owner)
         return false;

      std::set<AnsiString> &keys = written.byFolder[JoinPath(folderPath)];

      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(owner->GetID());
      std::shared_ptr<IMAPFolder> folder = tree ? tree->GetFolderByFullPath(folderPath) : std::shared_ptr<IMAPFolder>();
      if (!folder)
         return true;

      SQLCommand command("select messagefilename from hm_messages where messageaccountid = @ACCOUNTID and messagefolderid = @FOLDERID and messagesavedate >= @SINCE");
      command.AddParameter("@ACCOUNTID", owner->GetID());
      command.AddParameter("@FOLDERID", folder->GetID());
      command.AddParameter("@SINCE", since);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         // The account's own directory is where the file is only for a message
         // that names the account (PersistentMessage::GetFileName).
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message(false));
         message->SetAccountID(owner->GetID());
         message->SetFolderID(folder->GetID());
         message->SetPartialFileName(recordset->GetStringValue("messagefilename"));

         const AnsiString key = MessageKey(PersistentMessage::GetFileName(owner, message));
         keys.insert(key);
         written.anywhere.insert(key);

         recordset->MoveNext();
      }

      return true;
   }

   MailImporter::ImportOutcome
   MailImporter::ImportMessage(std::shared_ptr<const Account> owner, const std::vector<String> &folderPath,
                               int designation, const String &messageFile, short flags,
                               const std::vector<String> &keywords, const String &internalDate,
                               WrittenKeys *written, bool acrossFolders, String &problem)
   {
      if (!owner || folderPath.empty())
      {
         problem = _T("there is no account or folder to file the message into");
         return ImportFailed;
      }

      unsigned __int64 size = 0;
      if (!FileUtilities::Exists(messageFile) || !FileUtilities::FileSize64(messageFile, size))
      {
         problem = _T("the message file is not where it was written");
         return ImportFailed;
      }

      // APPEND's ceilings: the account's own limit, else its domain's, else the
      // server's, and 2 GB whatever they say.
      const __int64 limit = IMAPCommandAppend::GetEffectiveAppendLimitBytes(owner);
      if (limit > 0 && (__int64) size > limit)
      {
         problem = Formatter::Format(_T("the message is {0} bytes, over the {1} this account may receive"), (__int64) size, limit);
         return ImportTooLarge;
      }

      // APPEND's [UNAVAILABLE]: a volume short of room takes nothing more.
      if (!DiskSpace::DataDirectoryHasRoomForMail())
      {
         problem = _T("the data directory's volume is short of room, so nothing more is stored until there is");
         return ImportNoDiskSpace;
      }

      const String folderKey = JoinPath(folderPath);
      AnsiString key;
      if (written)
      {
         key = MessageKey(messageFile);

         const std::set<AnsiString> &here = written->byFolder[folderKey];
         if (here.find(key) != here.end())
         {
            problem = _T("the job copied this message into this folder already");
            return ImportDuplicate;
         }

         if (acrossFolders && written->anywhere.find(key) != written->anywhere.end())
         {
            problem = _T("the job copied this message into another folder already");
            return ImportDuplicateElsewhere;
         }
      }

      // The quota, as APPEND asks it: filling a mailbox past its limit would
      // leave its owner unable to receive the next message, and the limit is the
      // administrator's to raise.
      if (!owner->SpaceAvailable((__int64) size))
      {
         problem = _T("the account has no room left under its quota");
         return ImportOverQuota;
      }

      bool permanent = true;
      const __int64 folderId = FolderFor(owner, folderPath, designation, problem, permanent);
      if (folderId == 0)
         return permanent ? ImportFolderRefused : ImportFailed;

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      message->SetAccountID(owner->GetID());
      message->SetFolderID(folderId);
      message->SetState(Message::Delivered);
      message->SetSize((int) size);

      // Into the account's own directory, where an APPEND of it would have
      // written it.
      const String destination = PersistentMessage::GetFileName(owner, message);
      const String directory = FileUtilities::GetFilePath(destination);
      if (!FileUtilities::Exists(directory))
         FileUtilities::CreateDirectory(directory);

      if (destination.CompareNoCase(messageFile) != 0 && !FileUtilities::Move(messageFile, destination))
      {
         problem = _T("the message file could not be moved into the account's directory");
         return ImportFailed;
      }

      // The five flags a client may set. \Recent is the server's own, and
      // SaveObject sets it on a new row as it does for a delivery.
      const short settable = (short) (Message::FlagSeen | Message::FlagDeleted | Message::FlagFlagged |
                                      Message::FlagAnswered | Message::FlagDraft);
      message->SetFlags((short) (flags & settable));

      // Keywords as IMAPCommandAppend keeps them: an atom that is not a system
      // flag and that a flag may be called.
      std::vector<String> kept;
      for (const String &keyword : keywords)
      {
         if (keyword.IsEmpty() || keyword.GetAt(0) == '\\' || !SieveParser::IsValidFlagName(keyword))
            continue;
         kept.push_back(keyword);
      }
      message->SetKeywordList(kept);

      MimeHeader header;
      AnsiString headerText = PersistentMessage::LoadHeader(destination, false);
      if (!headerText.IsEmpty())
         header.Load(headerText.GetBuffer(), headerText.GetLength(), false);

      // The sender, for the row, as Import takes it.
      if (!headerText.IsEmpty())
      {
         AddresslistParser fromParser;
         std::vector<std::shared_ptr<Address> > senders = fromParser.ParseList(header.GetUnicodeFieldValue("From"));
         if (!senders.empty())
            message->SetFromAddress(senders[0]->sMailboxName + _T("@") + senders[0]->sDomainName);
      }

      // The source's internal date, which is what a client shows as the date
      // received: a mailbox migrated today must not read as having arrived
      // today. Without one, the message's own Date header.
      String createTime;
      if (!internalDate.IsEmpty())
         createTime = Time::GetInternalDateFromIMAPInternalDate(internalDate);

      if (createTime.IsEmpty() && !headerText.IsEmpty())
      {
         const String dateHeader = header.GetUnicodeFieldValue("Date");
         if (!dateHeader.IsEmpty())
         {
            DateTime dtTime = Time::GetDateTimeFromMimeHeader(dateHeader);
            if (dtTime.GetYear() > 1980 && dtTime.GetYear() < 2100)
               createTime = GetInternalDate_DatePlusTimeZone(dtTime);
         }
      }

      if (createTime.IsEmpty())
         createTime = Time::GetCurrentDateTime();

      message->SetCreateTime(createTime);

      if (!PersistentMessage::SaveObject(message))
      {
         // Moved, and not saved: the file is removed rather than left in the
         // account's directory with no row naming it.
         FileUtilities::DeleteFile(destination);
         problem = _T("the message could not be saved in the database");
         return ImportFailed;
      }

      if (written)
      {
         written->byFolder[folderKey].insert(key);
         written->anywhere.insert(key);
      }

      // A session with the folder open sees it on its next command, and an
      // IDLE session at once, as it sees a delivery.
      MessagesContainer::Instance()->SetFolderNeedsRefresh(folderId);

      if (std::shared_ptr<NotificationServer> notifications = Application::Instance()->GetNotificationServer())
      {
         std::shared_ptr<ChangeNotification> added =
            std::shared_ptr<ChangeNotification>(new ChangeNotification(owner->GetID(), folderId, ChangeNotification::NotificationMessageAdded));
         notifications->SendNotification(added);
      }

      return ImportDone;
   }

   bool
   MailImporter::ReplaceMessagePath_(__int64 messageID, const String &messagePath)
   {
      String newFullPath = messagePath;

      // Construct a partial file name.
      String partialFileName;
      if (!PersistentMessage::GetPartialFilename(newFullPath, partialFileName))
      {
         // It was not possible to construct a partial file name from the full path.
         String movedFile;
         if (!MoveToNamedSubdirectory_(messagePath, partialFileName, newFullPath))
            return false;
      }
      
      // We have a partial file name. Update the database with it.
      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(message, messageID))
         return false;

      message->SetPartialFileName(partialFileName);

      bool result = PersistentMessage::SaveObject(message);

      return result;
   }

   bool 
   MailImporter::MoveToNamedSubdirectory_(const String &sourceFile, String &resultFile, String &newFullPath)
   {
      // The file must be located in the data directory. Make sure this is the case.
      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      if (!sourceFile.StartsWith(dataDirectory))
         return false;

      // Create a new file name.
      resultFile = Message::GenerateFileName();

      String guidFolder = resultFile.Mid(1,2);
      
      // The file could be located in an incorrect folder, not following the name standard
      // for sub folders in hMailServer. Retrieve the correct folder name so that we can
      // move the message into the correct location.
      
      String currentCorrectDirectory; 
      if (!GetRootLevelDirectory_(sourceFile, currentCorrectDirectory))
         return false;

      String destinationDirectory = FileUtilities::Combine(currentCorrectDirectory, guidFolder);
      if (!FileUtilities::Exists(destinationDirectory))
         FileUtilities::CreateDirectory(destinationDirectory);

      String destinationFile = FileUtilities::Combine(destinationDirectory, resultFile);

      if (FileUtilities::Move(sourceFile, destinationFile))
      {
         newFullPath = destinationFile;
         return true;
      }
      
      return false;
   }


   bool
   MailImporter::GetRootLevelDirectory_(const String &fullPath, String &rootLevel)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Takes an input parameter such as C:\DataDir\Account\Sub1\Sub2\Test.eml and
   // returns the root of that hierarcy, such as C:\Datadir\Account in this case.
   //---------------------------------------------------------------------------()
   {
      // The file must be located in the data directory. Make sure this is the case.
      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();

      if (!fullPath.StartsWith(dataDirectory))
         return false;

      int currentTrimPosition = dataDirectory.GetLength() + 1;

      const String publicFolderName = IMAPConfiguration::GetPublicFolderDiskName();

      // Is the file located in the public folder?
      if (fullPath.FindNoCase(publicFolderName, currentTrimPosition) == currentTrimPosition)
      {
         // The file is located in the public folder.
         currentTrimPosition += publicFolderName.GetLength() + 1;
      }
      else
      {
         // The file is either located in the queue folder or in an account folder.
         int slashPosition = fullPath.Find(FileUtilities::PathSeparator, currentTrimPosition);
         if (slashPosition < 0)
            return false;

         int accountSlashPosition = fullPath.Find(FileUtilities::PathSeparator, slashPosition+1);

         if (accountSlashPosition > 0)
         {
            // The file is locate din the queue folder.
            currentTrimPosition = accountSlashPosition+1;
         }
      }

      rootLevel = fullPath.Mid(0, currentTrimPosition);
      return true;
   }

   String 
   MailImporter::GetInternalDate_(std::shared_ptr<MessageData> pMsgData)
   {

      String sReceivedHeader = pMsgData->GetFieldValue("Received");
      if (!sReceivedHeader.IsEmpty())
      {
         DateTime dtTime = Utilities::GetDateTimeFromReceivedHeader(sReceivedHeader);
         
         if (dtTime.GetYear() > 1980 && dtTime.GetYear() < 2040)
         {
            return GetInternalDate_DatePlusTimeZone(dtTime);
         }
      }        

      // We could not determine the date by looking at the Received header, check
      // the Date header insetad.
      String sDateHeader = pMsgData->GetFieldValue("Date");
      if (!sDateHeader.IsEmpty())
      {
         DateTime dtTime = Time::GetDateTimeFromMimeHeader(sDateHeader);
         
         if (dtTime.GetYear() > 1980 && dtTime.GetYear() < 2040)
         {
            return GetInternalDate_DatePlusTimeZone(dtTime);
         }
      }

      // We didn't find it in the Date header either. Use the current date.
      String sCurrentTime = Time::GetCurrentDateTime();
      return sCurrentTime;
      
   }

   String 
   MailImporter::GetInternalDate_DatePlusTimeZone(DateTime dtTime)
   {
      long minutes = Time::GetUTCRelationMinutes();

      DateTimeSpan dtSpan;
      dtSpan.SetDateTimeSpan(0, 0, minutes, 0);
      dtTime = dtTime + dtSpan;     

      String sDate = Time::GetTimeStampFromDateTime(dtTime);

      return sDate;
   }

#ifdef _DEBUG
   void
   MailImporterTester::Test()
   {
      MailImporter::Import("C:\\Program Files\\hMailServer\\Data\\{625F8A16-39BC-4282-B820-F1C270A94805}.eml", 128, "");
   }

#endif
}