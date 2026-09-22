// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MailboxRestore.h"

#include "Application.h"
#include "Backup.h"
#include "BackupManager.h"
#include "BackupRestorer.h"
#include "MessageIndexer.h"

#include "../BO/Account.h"
#include "../BO/IMAPFolders.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Messages.h"
#include "../BO/Message.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../Util/FileUtilities.h"
#include "../Util/GUIDCreator.h"
#include "../Util/RecoverableStore.h"
#include "../Util/StorePath.h"
#include "../Util/Time.h"
#include "../Util/Utilities.h"
#include "../Mime/Mime.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/MessagesContainer.h"

#include <boost/filesystem.hpp>

#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The registry. Process-wide and under its own lock for the reason
      // BackupManager's run slot is: the manager is destroyed and recreated on
      // every reinitialization, and a job that was queued a moment before one
      // finishes after it.
      boost::mutex jobs_mutex;
      std::map<int, MailboxRestore::Outcome> jobs;
      int next_job = 1;
      const size_t JOBS_KEPT = 50;

      // How many of the files the archive did not hold are named in the log.
      const size_t MISSING_NAMED_LIMIT = 10;

      String Lower(const String &value)
      {
         String copy = value;
         copy.MakeLower();
         return copy;
      }

      String JoinPath(const std::vector<String> &path)
      {
         return StringParser::JoinVector(path, Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter());
      }

      // A value as a JSON string: the quotes, the backslash and the control
      // characters escaped, everything else as it is. The wide text stays wide;
      // whoever sends it encodes it.
      String JsonQuote(const String &value)
      {
         String out = _T("\"");

         for (int i = 0; i < value.GetLength(); i++)
         {
            wchar_t character = value.SafeGetAt((unsigned int) i);

            switch (character)
            {
            case L'"':  out += _T("\\\""); break;
            case L'\\': out += _T("\\\\"); break;
            case L'\n': out += _T("\\n"); break;
            case L'\r': out += _T("\\r"); break;
            case L'\t': out += _T("\\t"); break;
            default:
               if (character < 0x20)
               {
                  String escaped;
                  escaped.Format(_T("\\u%04x"), (unsigned int) character);
                  out += escaped;
               }
               else
                  out += character;
               break;
            }
         }

         out += _T("\"");
         return out;
      }

      std::vector<String> SplitPath(const String &path)
      {
         return StringParser::SplitString(path, Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter());
      }

      bool SamePath(const std::vector<String> &left, const std::vector<String> &right)
      {
         if (left.size() != right.size())
            return false;

         for (size_t i = 0; i < left.size(); i++)
         {
            if (left[i].CompareNoCase(right[i]) != 0)
               return false;
         }

         return true;
      }

      // The backup destination as a normalised path, or empty when none is set.
      boost::filesystem::path DestinationPath()
      {
         String destination = Configuration::Instance()->GetBackupDestination();

         while (destination.EndsWith(FileUtilities::PathSeparator))
            destination = destination.Left(destination.GetLength() - 1);

         if (destination.IsEmpty())
            return boost::filesystem::path();

         return boost::filesystem::path(destination.c_str()).lexically_normal();
      }

      // Whether child lies inside parent, both normalised: the same directory
      // or one below it. Compared without regard to case on Windows, where the
      // file system does not regard it either.
      bool IsInside(const boost::filesystem::path &parent, const boost::filesystem::path &child)
      {
         String parentText(parent.wstring());
         String childText(child.wstring());

         while (parentText.EndsWith(FileUtilities::PathSeparator))
            parentText = parentText.Left(parentText.GetLength() - 1);

         if (childText.GetLength() <= parentText.GetLength() + 1)
            return false;

         String head = childText.Left(parentText.GetLength());
         String separator = childText.Mid(parentText.GetLength(), 1);

#ifdef _WIN32
         return head.CompareNoCase(parentText) == 0 && separator == FileUtilities::PathSeparator;
#else
         return head == parentText && separator == FileUtilities::PathSeparator;
#endif
      }
   }

   size_t
   ArchivedMailbox::MessageCount() const
   {
      size_t count = 0;

      for (size_t i = 0; i < folders.size(); i++)
         count += folders[i].messages.size();

      return count;
   }

   MailboxRestore::MailboxRestore(const Request &request) :
      request_(request)
   {
   }

   MailboxRestore::~MailboxRestore()
   {
      // The passphrase this job was given, overwritten rather than left in the
      // freed block. See Utilities::SecureWipe.
      Utilities::SecureWipe(request_.passphrase);
   }

   String
   MailboxRestore::ModeName(Mode mode)
   {
      return mode == ModeReplace ? _T("replace") : _T("merge");
   }

   bool
   MailboxRestore::ParseMode(const String &name, Mode &mode)
   {
      if (name.CompareNoCase(_T("merge")) == 0)
      {
         mode = ModeMerge;
         return true;
      }

      if (name.CompareNoCase(_T("replace")) == 0)
      {
         mode = ModeReplace;
         return true;
      }

      return false;
   }

   bool
   MailboxRestore::ResolveArchive(const String &named, String &archiveFile, bool &notFound, String &failureReason)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The one rule about where an archive may come from: inside the backup
   // destination, which is the directory this server writes its own archives to
   // and the one the listing reads. A request may name an archive by the bare
   // name the listing answers, or by a path; a path that leads anywhere else is
   // refused, whatever it points at. The whole restore takes any path, from an
   // administrator who has already chosen to replace the server; this one is
   // reached from a mailbox picker, and a picker must not be a way to read files
   // the server was never told about.
   //---------------------------------------------------------------------------()
   {
      notFound = false;

      String trimmed = named;
      trimmed.Trim();

      if (trimmed.IsEmpty())
      {
         failureReason = "No archive was named. Name one from the backup listing, or by its path inside the backup destination.";
         return false;
      }

      boost::filesystem::path destination = DestinationPath();

      if (destination.empty())
      {
         failureReason = "No backup destination is set on this server, so there is no directory to take an archive from.";
         return false;
      }

      boost::filesystem::path candidate;

      // A bare name is a name inside the destination; anything with a
      // separator in it is a path and is judged as one.
      if (trimmed.Find(_T("\\")) < 0 && trimmed.Find(_T("/")) < 0)
      {
         if (trimmed == _T(".") || trimmed == _T(".."))
         {
            failureReason = "The archive name " + trimmed + " is not a file.";
            return false;
         }

         candidate = destination / boost::filesystem::path(trimmed.c_str());
      }
      else
      {
         candidate = boost::filesystem::path(trimmed.c_str());

         if (!candidate.is_absolute())
         {
            failureReason = "The archive path " + trimmed + " is not inside the backup destination " + String(destination.wstring()) + ". Name the archive as the backup listing does, or by its full path there.";
            return false;
         }

         candidate = candidate.lexically_normal();

         if (!IsInside(destination, candidate))
         {
            failureReason = "The archive path " + trimmed + " is not inside the backup destination " + String(destination.wstring()) + ". A mailbox is restored from an archive this server wrote there, and from nowhere else.";
            return false;
         }
      }

      archiveFile = String(candidate.wstring());

      boost::system::error_code error;
      bool isRegularFile = boost::filesystem::is_regular_file(candidate, error);

      if (error || !isRegularFile)
      {
         notFound = true;
         failureReason = "There is no archive " + archiveFile + " in the backup destination.";
         return false;
      }

      return true;
   }

   void
   MailboxRestore::ParseFolders_(XNode *foldersNode, const std::vector<String> &parentPath, ArchivedMailbox &mailbox)
   {
      if (!foldersNode)
         return;

      for (int i = 0; i < foldersNode->GetChildCount(); i++)
      {
         XNode *folderNode = foldersNode->GetChild(i);

         if (!folderNode || folderNode->name != _T("Folder"))
            continue;

         ArchivedFolder folder;
         folder.path = parentPath;
         folder.path.push_back(folderNode->GetAttrValue(_T("Name")));
         folder.subscribed = folderNode->GetAttrValue(_T("Subscribed")) == _T("1");
         folder.special_use = _ttoi(folderNode->GetAttrValue(_T("SpecialUse")));

         XNode *messagesNode = folderNode->GetChild(_T("Messages"));

         if (messagesNode)
         {
            for (int m = 0; m < messagesNode->GetChildCount(); m++)
            {
               XNode *messageNode = messagesNode->GetChild(m);

               if (!messageNode || messageNode->name != _T("Message"))
                  continue;

               ArchivedMessage message;
               message.id = _ttoi64(messageNode->GetAttrValue(_T("ID")));
               message.filename = messageNode->GetAttrValue(_T("Filename"));
               message.from_address = messageNode->GetAttrValue(_T("FromAddress"));
               message.create_time = messageNode->GetAttrValue(_T("CreateTime"));
               message.keywords = messageNode->GetAttrValue(_T("Keywords"));
               message.size = _ttoi(messageNode->GetAttrValue(_T("Size")));
               message.flags = _ttoi(messageNode->GetAttrValue(_T("Flags")));
               message.uid = (unsigned int) _ttoi(messageNode->GetAttrValue(_T("UID")));

               // A row with no file name has nothing to restore from and cannot
               // be named by the identity rule; the whole restore writes such a
               // row and it is then a message that cannot be fetched.
               if (message.filename.IsEmpty())
                  continue;

               folder.messages.push_back(message);
            }
         }

         mailbox.folders.push_back(folder);

         ParseFolders_(folderNode->GetChild(_T("Folders")), folder.path, mailbox);
      }
   }

   bool
   MailboxRestore::ParseMailbox_(XNode *backupNode, const String &address, ArchivedMailbox &mailbox)
   {
      mailbox.folders.clear();
      mailbox.address.Empty();
      mailbox.domain_name = StringParser::ExtractDomain(address);

      XNode *domainsNode = backupNode ? backupNode->GetChild(_T("Domains")) : 0;

      if (!domainsNode)
         return false;

      for (int d = 0; d < domainsNode->GetChildCount(); d++)
      {
         XNode *domainNode = domainsNode->GetChild(d);

         if (!domainNode || domainNode->name != _T("Domain"))
            continue;

         if (domainNode->GetAttrValue(_T("Name")).CompareNoCase(mailbox.domain_name) != 0)
            continue;

         XNode *accountsNode = domainNode->GetChild(_T("Accounts"));

         if (!accountsNode)
            continue;

         for (int a = 0; a < accountsNode->GetChildCount(); a++)
         {
            XNode *accountNode = accountsNode->GetChild(a);

            if (!accountNode || accountNode->name != _T("Account"))
               continue;

            String archivedAddress = accountNode->GetAttrValue(_T("Name"));

            if (archivedAddress.CompareNoCase(address) != 0)
               continue;

            mailbox.address = archivedAddress;

            ParseFolders_(accountNode->GetChild(_T("Folders")), std::vector<String>(), mailbox);

            return true;
         }
      }

      return false;
   }

   bool
   MailboxRestore::ReadMailbox(const String &archiveFile, const String &passphrase, const String &address,
                               unsigned int openTimeoutMilliseconds, bool *openTimedOut,
                               ArchivedMailbox &mailbox, bool &accountNotFound, String &failureReason)
   {
      accountNotFound = false;

      // A local instance, so that the directory the index is extracted to is
      // removed on every exit path.
      BackupRestorer restorer;

      if (!restorer.OpenIndex(archiveFile, passphrase, openTimeoutMilliseconds, failureReason, openTimedOut))
         return false;

      if ((restorer.GetArchiveContents() & Backup::BODomains) == 0)
      {
         failureReason = "This backup does not contain any domains or accounts, so no mailbox can be read from it. Use a backup that was taken with domains included. Nothing has been changed on this server.";
         return false;
      }

      if ((restorer.GetArchiveContents() & Backup::BOMessages) == 0)
      {
         failureReason = "This backup does not contain any messages, so no mailbox can be restored from it. Use a backup that was taken with messages included. Nothing has been changed on this server.";
         return false;
      }

      if (!ParseMailbox_(restorer.GetBackupNode(), address, mailbox))
      {
         accountNotFound = true;
         failureReason = "The archive " + archiveFile + " has no account " + address + ". Nothing has been changed on this server.";
         return false;
      }

      return true;
   }

   bool
   MailboxRestore::ReadSubjects(const String &archiveFile, const String &passphrase, ArchivedMailbox &mailbox,
                                unsigned int timeoutMilliseconds, String &failureReason)
   {
      if (mailbox.MessageCount() == 0)
         return true;

      if (mailbox.MessageCount() > SUBJECTS_LIMIT)
      {
         failureReason = Formatter::Format("The listing holds {0} messages and subjects are read from the files for at most {1}; ask for one folder to see them.",
            (unsigned int) mailbox.MessageCount(), (unsigned int) SUBJECTS_LIMIT);
         return false;
      }

      BackupRestorer restorer;
      bool timedOut = false;

      if (!restorer.OpenIndex(archiveFile, passphrase, timeoutMilliseconds, failureReason, &timedOut))
         return false;

      String domainDirectory = StoreDirectoryName(mailbox.domain_name);
      String accountDirectory = StoreDirectoryName(StringParser::ExtractAddress(mailbox.address));

      if (!restorer.StageAccountMessageStore(archiveFile, domainDirectory, accountDirectory, timeoutMilliseconds, &timedOut, failureReason))
         return false;

      if (!restorer.GetHasStagedMessageStore())
      {
         failureReason = "The archive holds no message files for this account, so there are no subjects to read.";
         return false;
      }

      String store = restorer.GetStagedMessageStore();

      for (size_t f = 0; f < mailbox.folders.size(); f++)
      {
         std::vector<ArchivedMessage> &messages = mailbox.folders[f].messages;

         for (size_t m = 0; m < messages.size(); m++)
         {
            String file = FileUtilities::Combine(FileUtilities::Combine(store, messages[m].filename.Mid(1, 2)), messages[m].filename);

            if (!FileUtilities::Exists(file))
               continue;

            AnsiString headerText = PersistentMessage::LoadHeader(file, false);

            if (headerText.IsEmpty())
               continue;

            MimeHeader header;
            header.Load(headerText.GetBuffer(), headerText.GetLength(), false);

            messages[m].subject = header.GetUnicodeFieldValue("Subject");
         }
      }

      return true;
   }

   int
   MailboxRestore::RegisterJob_(const Request &request)
   {
      boost::lock_guard<boost::mutex> guard(jobs_mutex);

      Outcome outcome;
      outcome.job = next_job++;
      outcome.state = _T("queued");
      outcome.archive = request.archive_file;
      outcome.address = request.address;
      outcome.mode = request.message_id > 0 ? String(_T("message")) : ModeName(request.mode);
      outcome.message_id = request.message_id;

      jobs[outcome.job] = outcome;

      while (jobs.size() > JOBS_KEPT)
         jobs.erase(jobs.begin());

      return outcome.job;
   }

   void
   MailboxRestore::UpdateJob_(const Outcome &outcome)
   {
      boost::lock_guard<boost::mutex> guard(jobs_mutex);

      jobs[outcome.job] = outcome;
   }

   bool
   MailboxRestore::GetOutcome(int job, Outcome &outcome)
   {
      boost::lock_guard<boost::mutex> guard(jobs_mutex);

      std::map<int, Outcome>::const_iterator found = jobs.find(job);

      if (found == jobs.end())
         return false;

      outcome = found->second;
      return true;
   }

   std::vector<MailboxRestore::Outcome>
   MailboxRestore::ListOutcomes()
   {
      boost::lock_guard<boost::mutex> guard(jobs_mutex);

      std::vector<Outcome> result;

      for (std::map<int, Outcome>::const_iterator it = jobs.begin(); it != jobs.end(); it++)
         result.push_back(it->second);

      return result;
   }

   String
   MailboxRestore::FolderPathText(const std::vector<String> &path)
   {
      return JoinPath(path);
   }

   String
   MailboxRestore::ListingJson(const String &archiveFile, const ArchivedMailbox &mailbox, bool subjects, const String &subjectsNote)
   {
      String json = _T("{\"archive\":") + JsonQuote(archiveFile) + _T(",\"address\":") + JsonQuote(mailbox.address) + _T(",\"folders\":[");

      for (size_t f = 0; f < mailbox.folders.size(); f++)
      {
         const ArchivedFolder &folder = mailbox.folders[f];
         const String folderPath = JoinPath(folder.path);

         if (f > 0)
            json += _T(",");

         json += _T("{\"path\":") + JsonQuote(folderPath) +
                 _T(",\"subscribed\":") + String(folder.subscribed ? _T("true") : _T("false")) +
                 _T(",\"special_use\":") + StringParser::IntToString(folder.special_use) +
                 _T(",\"messages\":[");

         for (size_t m = 0; m < folder.messages.size(); m++)
         {
            const ArchivedMessage &message = folder.messages[m];

            if (m > 0)
               json += _T(",");

            json += _T("{\"id\":") + StringParser::IntToString(message.id) +
                    _T(",\"folder\":") + JsonQuote(folderPath) +
                    _T(",\"subject\":") + JsonQuote(message.subject) +
                    _T(",\"from\":") + JsonQuote(message.from_address) +
                    _T(",\"date\":") + JsonQuote(message.create_time) +
                    _T(",\"size\":") + StringParser::IntToString(message.size) +
                    _T(",\"flags\":") + StringParser::IntToString(message.flags) +
                    _T(",\"keywords\":") + JsonQuote(message.keywords) + _T("}");
         }

         json += _T("]}");
      }

      json += _T("],\"messages\":") + StringParser::IntToString((unsigned int) mailbox.MessageCount()) +
              _T(",\"subjects\":") + String(subjects ? _T("true") : _T("false")) +
              _T(",\"subjects_note\":") + JsonQuote(subjectsNote) + _T("}");

      return json;
   }

   String
   MailboxRestore::OutcomeJson(const Outcome &outcome)
   {
      return _T("{\"job\":") + StringParser::IntToString(outcome.job) +
             _T(",\"state\":") + JsonQuote(outcome.state) +
             _T(",\"archive\":") + JsonQuote(outcome.archive) +
             _T(",\"address\":") + JsonQuote(outcome.address) +
             _T(",\"mode\":") + JsonQuote(outcome.mode) +
             _T(",\"message\":") + StringParser::IntToString(outcome.message_id) +
             _T(",\"folders\":") + StringParser::IntToString(outcome.folders) +
             _T(",\"added\":") + StringParser::IntToString(outcome.added) +
             _T(",\"skipped\":") + StringParser::IntToString(outcome.skipped) +
             _T(",\"failed\":") + StringParser::IntToString(outcome.failed) +
             _T(",\"error\":") + JsonQuote(outcome.error) +
             _T(",\"started\":") + JsonQuote(outcome.started) +
             _T(",\"finished\":") + JsonQuote(outcome.finished) + _T("}");
   }

   std::shared_ptr<MailboxRestore>
   MailboxRestore::Prepare(const Request &requested, unsigned int openTimeoutMilliseconds, int &statusCode, String &failureReason)
   {
      statusCode = 400;

      Request request = requested;
      request.address.Trim();

      if (request.address.IsEmpty() || request.address.Find(_T("@")) <= 0)
      {
         failureReason = "An account address is required: the mailbox to restore, as the archive names it.";
         return std::shared_ptr<MailboxRestore>();
      }

      bool archiveNotFound = false;

      if (!ResolveArchive(request.archive, request.archive_file, archiveNotFound, failureReason))
      {
         statusCode = archiveNotFound ? 404 : 400;
         return std::shared_ptr<MailboxRestore>();
      }

      // The live account, first: a mailbox is restored into the account it
      // belongs to, and an account that no longer exists is not recreated here
      // - that is the whole restore's job, or the account's, and a mailbox
      // without its account would be rows nothing can sign in to.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(request.address);

      if (!account)
      {
         statusCode = 404;
         failureReason = "There is no account " + request.address + " on this server. A mailbox is restored into the account it belongs to, which has to exist; restoring into a different account is not offered.";
         return std::shared_ptr<MailboxRestore>();
      }

      std::vector<String> targetPath;

      if (!request.folder.IsEmpty())
      {
         targetPath = SplitPath(request.folder);

         if (!IMAPFolder::IsValidFolderName(targetPath, false))
         {
            failureReason = "The folder " + request.folder + " is not a valid folder path for an account.";
            return std::shared_ptr<MailboxRestore>();
         }

         if (request.message_id <= 0)
         {
            failureReason = "folder names where one message goes; a whole mailbox is restored into its own folders.";
            return std::shared_ptr<MailboxRestore>();
         }
      }

      std::shared_ptr<MailboxRestore> job(new MailboxRestore(request));

      bool openTimedOut = false;
      bool accountNotFound = false;

      if (!ReadMailbox(request.archive_file, request.passphrase, request.address, openTimeoutMilliseconds, &openTimedOut,
                       job->mailbox_, accountNotFound, failureReason))
      {
         statusCode = openTimedOut ? 504 : (accountNotFound ? 404 : 400);
         return std::shared_ptr<MailboxRestore>();
      }

      if (request.message_id > 0)
      {
         bool found = false;

         for (size_t f = 0; f < job->mailbox_.folders.size() && !found; f++)
         {
            const std::vector<ArchivedMessage> &messages = job->mailbox_.folders[f].messages;

            for (size_t m = 0; m < messages.size(); m++)
            {
               if (messages[m].id == request.message_id)
               {
                  found = true;
                  break;
               }
            }
         }

         if (!found)
         {
            statusCode = 404;
            failureReason = Formatter::Format("The archive holds no message {0} for {1}. The ids are the ones its listing answers. Nothing has been changed on this server.",
               request.message_id, request.address);
            return std::shared_ptr<MailboxRestore>();
         }
      }

      job->outcome_.job = RegisterJob_(request);
      GetOutcome(job->outcome_.job, job->outcome_);

      statusCode = 202;
      return job;
   }

   int
   MailboxRestore::GetJob() const
   {
      return outcome_.job;
   }

   const MailboxRestore::Request &
   MailboxRestore::GetRequest() const
   {
      return request_;
   }

   String
   MailboxRestore::Describe() const
   {
      if (request_.message_id > 0)
      {
         return Formatter::Format("restoring message {0} of {1} from {2} into {3}", request_.message_id, request_.address, request_.archive_file,
            request_.folder.IsEmpty() ? String(_T("the folder it was in")) : request_.folder);
      }

      if (request_.mode == ModeReplace)
         return Formatter::Format("replacing the mailbox of {0} with the one in {1} (every live folder emptied first)", request_.address, request_.archive_file);

      return Formatter::Format("merging the mailbox of {0} from {1}", request_.address, request_.archive_file);
   }

   void
   MailboxRestore::Log_(const String &line) const
   {
      Logger::Instance()->LogBackup(Formatter::Format("Mailbox restore {0}: {1}", outcome_.job, line));
   }

   int
   MailboxRestore::EmptyLiveFolders_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> folders, int &foldersEmptied)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Replace's first half: every message of every live folder, through the
   // folder's shared collection so that the deletion is the one an EXPUNGE
   // performs - the file goes, the size moves, every session with the folder
   // selected is told - and inside a RecoverableStore scope, so that what
   // replace removes can be recovered for RecoverDeletedDays as an emptied
   // folder's messages can. The folders themselves stay.
   //---------------------------------------------------------------------------()
   {
      int removed = 0;

      if (!folders)
         return 0;

      for (int i = 0; i < folders->GetCount(); i++)
      {
         std::shared_ptr<IMAPFolder> folder = folders->GetItem(i);

         if (!folder)
            continue;

         std::shared_ptr<Messages> messages = MessagesContainer::Instance()->GetMessages(account->GetID(), folder->GetID());

         if (messages)
         {
            messages->Refresh(false);

            std::vector<__int64> deleted;

            {
               RecoverableStore::Scope recoverable;
               deleted = messages->DeleteMessages([] (std::shared_ptr<Message>) { return true; });
            }

            if (!deleted.empty())
            {
               foldersEmptied++;
               removed += (int) deleted.size();

               std::shared_ptr<ChangeNotification> notification = std::shared_ptr<ChangeNotification>(
                  new ChangeNotification(account->GetID(), folder->GetID(), ChangeNotification::NotificationMessageDeleted, deleted));
               Application::Instance()->GetNotificationServer()->SendNotification(notification);
            }
         }

         removed += EmptyLiveFolders_(account, folder->GetSubFolders(), foldersEmptied);
      }

      return removed;
   }

   bool
   MailboxRestore::RestoreFolder_(std::shared_ptr<const Account> account, std::shared_ptr<IMAPFolders> tree,
                                  const ArchivedFolder &archived, const std::vector<String> &livePath,
                                  const std::vector<const ArchivedMessage *> &messages, const String &stagedStore,
                                  std::set<String> &liveFileNames)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // One live folder: found or created, then each message given that is not
   // already in the mailbox copied into the account's own directory and given a
   // row. The rows go through PersistentMessage::SaveObject with no UID, so each
   // takes the folder's next UID and mod-sequence and adds to the account's
   // size exactly as a delivery does; the folder's shared collection is
   // refreshed and every session with it selected is told once the folder is
   // done. A message whose file is not in the archive, or whose row could not
   // be written, is counted as failed and the run goes on: one bad file is not
   // a reason to leave the other thousand out.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<IMAPFolder> folder = tree->GetFolderByFullPath(livePath);

      // Whether this run is putting back the archived folder itself, or a
      // folder somebody named for one message. The archived one comes back
      // subscribed as it was; a named one is made subscribed, so that the
      // client the message was restored for can see it.
      const bool archivedFolderItself = SamePath(livePath, archived.path);

      if (!folder)
      {
         tree->CreatePath(tree, livePath, archivedFolderItself ? archived.subscribed : true);
         folder = tree->GetFolderByFullPath(livePath);

         if (!folder)
         {
            outcome_.failed += (int) messages.size();
            Log_("the folder " + JoinPath(livePath) + " could not be created; " + StringParser::IntToString((int) messages.size()) + " message(s) of it not restored.");
            return false;
         }

         // The designation the archive carried, on a folder this run made. A
         // folder that already exists keeps its own.
         if (archived.special_use != 0 && archivedFolderItself)
            folder->StoreSpecialUseFlags(archived.special_use);

         Log_("created the folder " + JoinPath(livePath) + ".");
      }

      outcome_.folders++;

      int addedHere = 0;
      int skippedHere = 0;
      int failedHere = 0;

      for (size_t i = 0; i < messages.size(); i++)
      {
         const ArchivedMessage &archivedMessage = *messages[i];

         if (liveFileNames.find(Lower(archivedMessage.filename)) != liveFileNames.end())
         {
            outcome_.skipped++;
            skippedHere++;
            continue;
         }

         String source = FileUtilities::Combine(FileUtilities::Combine(stagedStore, archivedMessage.filename.Mid(1, 2)), archivedMessage.filename);

         if (stagedStore.IsEmpty() || !FileUtilities::Exists(source))
         {
            outcome_.failed++;
            failedHere++;

            if (missing_named_.size() < MISSING_NAMED_LIMIT)
               missing_named_.push_back(archivedMessage.filename);

            continue;
         }

         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message);
         message->SetState(Message::Delivered);
         message->SetAccountID(account->GetID());
         message->SetFolderID(folder->GetID());
         message->SetPartialFileName(archivedMessage.filename);

         String destination = PersistentMessage::GetFileName(account, message);

         // The executer's name is a GUID, so a file already at that path with
         // no row naming it is an orphan of some earlier run; it is not
         // overwritten - nothing here deletes or rewrites what is there - and
         // the message gets a fresh name instead.
         if (FileUtilities::Exists(destination))
         {
            message->SetPartialFileName(Message::GenerateFileName());
            destination = PersistentMessage::GetFileName(account, message);
         }

         if (!FileUtilities::Copy(source, destination, true))
         {
            outcome_.failed++;
            failedHere++;
            Log_("the file " + archivedMessage.filename + " could not be copied into " + FileUtilities::GetFilePath(destination) + ".");
            continue;
         }

         // The size as the file is, not as the index says: it is what the
         // account's quota is charged.
         long size = FileUtilities::FileSize(destination);

         message->SetSize(size > 0 ? (int) size : archivedMessage.size);
         message->SetFlags((short) archivedMessage.flags);
         message->SetKeywords(archivedMessage.keywords);
         message->SetFromAddress(archivedMessage.from_address);
         message->SetCreateTime(archivedMessage.create_time);
         message->SetNoOfRetries(0);
         message->SetUID(0);

         if (!PersistentMessage::SaveObject(message))
         {
            FileUtilities::DeleteFile(destination);
            outcome_.failed++;
            failedHere++;
            Log_("the row for " + archivedMessage.filename + " could not be written; the copied file has been removed again.");
            continue;
         }

         liveFileNames.insert(Lower(message->GetPartialFileName()));
         outcome_.added++;
         addedHere++;
      }

      if (addedHere > 0)
      {
         // What CopyToIMAPFolder does after a copy: the shared collection every
         // session reads brought up to date, and the sessions told.
         MessagesContainer::Instance()->SetFolderNeedsRefresh(folder->GetID());
         folder->GetMessages()->Refresh(false);

         std::shared_ptr<ChangeNotification> notification = std::shared_ptr<ChangeNotification>(
            new ChangeNotification(account->GetID(), folder->GetID(), ChangeNotification::NotificationMessageAdded));
         Application::Instance()->GetNotificationServer()->SendNotification(notification);
      }

      Log_(Formatter::Format("{0}: {1} added, {2} skipped as still in the mailbox, {3} failed, of {4} in the archive.",
         JoinPath(livePath), addedHere, skippedHere, failedHere, (int) messages.size()));

      return true;
   }

   bool
   MailboxRestore::RunInner_(std::shared_ptr<const Account> account, String &failureReason)
   {
      // PHASE ONE: everything read, nothing changed. The index again, on this
      // thread and with no bound - the queue's thread exists for this - and
      // then the account's files, extracted before replace empties anything.
      BackupRestorer restorer;

      if (!restorer.OpenIndex(request_.archive_file, request_.passphrase, 0, failureReason, nullptr))
         return false;

      if (!ParseMailbox_(restorer.GetBackupNode(), request_.address, mailbox_))
      {
         failureReason = "The archive " + request_.archive_file + " has no account " + request_.address + ". Nothing has been changed on this server.";
         return false;
      }

      String domainDirectory = StoreDirectoryName(mailbox_.domain_name);
      String accountDirectory = StoreDirectoryName(StringParser::ExtractAddress(mailbox_.address));

      if (!restorer.StageAccountMessageStore(request_.archive_file, domainDirectory, accountDirectory, 0, nullptr, failureReason))
         return false;

      // Which archived folders and messages this run puts back.
      std::vector<const ArchivedFolder *> folders;
      std::vector<std::vector<const ArchivedMessage *> > selected;
      std::vector<std::vector<String> > livePaths;

      for (size_t f = 0; f < mailbox_.folders.size(); f++)
      {
         const ArchivedFolder &folder = mailbox_.folders[f];
         std::vector<const ArchivedMessage *> messages;

         for (size_t m = 0; m < folder.messages.size(); m++)
         {
            if (request_.message_id > 0 && folder.messages[m].id != request_.message_id)
               continue;

            messages.push_back(&folder.messages[m]);
         }

         // For a whole mailbox every archived folder is put back, empty ones
         // too: an empty folder is part of how the mailbox was. For one
         // message, only the folder it goes to.
         if (request_.message_id > 0 && messages.empty())
            continue;

         folders.push_back(&folder);
         selected.push_back(messages);
         livePaths.push_back(request_.message_id > 0 && !request_.folder.IsEmpty() ? SplitPath(request_.folder) : folder.path);
      }

      size_t wanted = 0;

      for (size_t i = 0; i < selected.size(); i++)
         wanted += selected[i].size();

      if (wanted > 0 && !restorer.GetHasStagedMessageStore())
      {
         failureReason = Formatter::Format("The archive's index lists {0} message(s) for {1} but its message store holds no files for the account - which is what a backup taken with BackupMessagesDBOnly looks like. A mailbox cannot be restored from message rows alone. Nothing has been changed on this server.",
            (unsigned int) wanted, request_.address);
         return false;
      }

      Log_(Formatter::Format("the archive holds {0} folder(s) and {1} message(s) for {2}; {3} message(s) selected. Files staged in {4}.",
         (unsigned int) mailbox_.folders.size(), (unsigned int) mailbox_.MessageCount(), request_.address, (unsigned int) wanted,
         restorer.GetHasStagedMessageStore() ? restorer.GetStagedMessageStore() : String(_T("(none: the account has no files in the archive)"))));

      // The identity rule's set: every file name a delivered row of the live
      // account carries, whatever folder it is in. One query, not one per
      // message.
      std::set<String> liveFileNames;

      {
         SQLCommand command("select messagefilename from hm_messages where messageaccountid = @ACCOUNTID and messagetype = @TYPE");
         command.AddParameter("@ACCOUNTID", account->GetID());
         command.AddParameter("@TYPE", (int) Message::Delivered);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

         if (!recordset)
         {
            failureReason = "The messages of " + request_.address + " could not be read from the database, so what is already in the mailbox cannot be told from what is not. Nothing has been changed on this server.";
            return false;
         }

         while (!recordset->IsEOF())
         {
            liveFileNames.insert(Lower(FileUtilities::GetFileNameFromFullPath(recordset->GetStringValue("messagefilename"))));
            recordset->MoveNext();
         }
      }

      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(account->GetID());

      if (!tree)
      {
         failureReason = "The folders of " + request_.address + " could not be read. Nothing has been changed on this server.";
         return false;
      }

      // PHASE TWO. Replace empties first - the only deletion this operation
      // makes, and it happens only once everything above has been read.
      if (request_.message_id == 0 && request_.mode == ModeReplace)
      {
         int foldersEmptied = 0;
         int removed = EmptyLiveFolders_(account, tree, foldersEmptied);

         // The files are gone with the rows, so nothing is "still there".
         liveFileNames.clear();

         Log_(Formatter::Format("replace: {0} message(s) removed from {1} folder(s) of the live mailbox before filling it from the archive.", removed, foldersEmptied));
      }

      for (size_t i = 0; i < folders.size(); i++)
         RestoreFolder_(account, tree, *folders[i], livePaths[i], selected[i], restorer.GetStagedMessageStore(), liveFileNames);

      if (!missing_named_.empty())
      {
         Log_(Formatter::Format("message row(s) whose file the archive does not hold{0}: {1}. They were deleted from the message store before or while the backup ran and could not be restored.",
            outcome_.failed > (int) MISSING_NAMED_LIMIT ? String(_T(" (the first ten)")) : String(_T("")),
            StringParser::JoinVector(missing_named_, _T(", "))));
      }

      // The full-text index picks the new rows up on its own pass; nudged so
      // that a search finds a restored message in seconds rather than at the
      // next minute.
      if (outcome_.added > 0)
         MessageIndexer::Instance()->IndexNow();

      return true;
   }

   void
   MailboxRestore::Run()
   {
      outcome_.state = _T("running");
      outcome_.started = Time::GetCurrentDateTime();
      UpdateJob_(outcome_);

      Log_("started: " + Describe() + ".");

      String failureReason;
      bool completed = false;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(request_.address);

      if (!account)
         failureReason = "There is no account " + request_.address + " on this server any more. Nothing has been changed on this server.";
      else
         completed = RunInner_(account, failureReason);

      outcome_.finished = Time::GetCurrentDateTime();

      if (!completed)
      {
         outcome_.state = _T("failed");
         outcome_.error = failureReason;
         UpdateJob_(outcome_);

         // The same report a whole restore's failure makes: the backup log, a
         // critical error, the backup-failed alert and the OnBackupFailed event.
         Application::Instance()->GetBackupManager()->OnBackupFailed(Formatter::Format("Mailbox restore {0} failed: {1}", outcome_.job, failureReason));
         return;
      }

      outcome_.state = _T("done");
      UpdateJob_(outcome_);

      Log_(Formatter::Format("completed: {0} folder(s), {1} message(s) added, {2} skipped as still in the mailbox, {3} failed.",
         outcome_.folders, outcome_.added, outcome_.skipped, outcome_.failed));
   }
}
