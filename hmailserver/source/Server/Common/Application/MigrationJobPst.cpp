// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A migration job whose source is an Outlook data file (docs/Migration.md,
// "Outlook data files (PST)"): a migration of kind pst, a job per file, each
// into one account.
//
// THE FILE is named, not given as a path: it is looked for in the target
// account's domain's subdirectory of pst_import_directory and nowhere else
// (Util/PstImportPath.h), resolved there as the file system resolves it, and
// the file the job then opens is checked again by the final path of its own
// handle - a handle whose final path cannot be told, or is on another machine,
// is refused as a handle outside the directory is. A file another program holds
// without sharing - Outlook, while it runs - pauses the job with that said, and
// a file that is gone, that the reader refuses (an OST, an ANSI or a protected
// file, a damaged header, a password missing or wrong), that is not a PST, or
// whose store this account has already been given by a finished job, fails it
// with the reason, which the report keeps as its refusal.
//
// THE READING is this server's own PST reader (Common/Pst), in a PROCESS OF ITS
// OWN (Pst/PstReaderProcess.h): the store, the folders under the IPM subtree,
// and each message read whole and rebuilt as Internet mail (Pst/PstMime.h),
// deterministically, by this program started as the reader with the handle the
// job opened and checked, and nothing else of the service's. What a file can
// make the reader do is bounded as it was when the reader ran in the service:
// by the limits each structure is read with, and by the WORK BUDGET
// (Pst/PstFormat.h, PstBudget) - every page, block, heap allocation, table cell,
// subnode, attachment and embedded message read is counted, per message
// (4,000,000 reads, 512 MB decoded) and for the whole file in one run
// (10,000,000 reads and one more for every 4 bytes of the file; 1 GB decoded and
// 64 times the file's size), carried from one reader to the next. A message
// past its budget is left out as too_large; a file past its own fails the job,
// what was copied kept, and a retry goes on from the marks with a budget of its
// own. A pause, a cancel or a service stop kills the reader, and the job's
// thread, waiting for it, writes its marks at once. What neither bound can rule
// out - a defect in the reader that ends its process - now ends the reader and
// not the service: the job starts another and asks for the message again, and a
// message the reader ends on twice is left out as reader_failed, named, and the
// job goes on from the next with a new reader. The store and the folder list
// ended on twice fail the file; a folder's list, that folder.
//
// THE MARKERS stay, as the guard of what still runs in the service - the filing
// of each message, and a service that ends for any other reason while a message
// is in hand: before the store is opened and its folders listed, and before each
// folder's list of messages and each message, the job writes where it is into its
// report or into the folder's row; after, it writes that it got past. A run that
// finds such a marker still standing knows the service ended while it was there,
// and takes it once more; a second time, it leaves that message out
// (reader_stopped) - or that folder, or the file - and says so, rather than
// trying again for ever.
//
// THE FILING is the import entry every importer of a mailbox shares,
// MailImporter::ImportMessage, as archived mail: the 2 GB ceiling (and not the
// account's max_message_size, which is about new mail), the volume's room, the
// quota, the folder made (subscribed) when it is missing. A message it cannot
// file for now - the quota full, the volume short - pauses the job with the
// reason and the usual back-off, and it goes on from that message.
//
// THE FOLDERS. Each PST folder is filed at its own path below the account's
// top level, a top-level "Inbox" into the INBOX; the special folders the store
// names (Sent Items, Deleted Items, Drafts, Junk E-mail - Pst/PstStore.h) into
// the account's own folder of that use where it has one, and otherwise into a
// folder of the PST's name that is given the use. An account folder that holds
// its use only by its name (IMAPSpecialUse's table) is given it for good before
// anything is made, so that no folder the import makes can take it by name; and
// an ordinary PST folder whose path is a special folder's - the account's, or
// the one the PST's own is filed into - is filed beside it as "<name> (PST)",
// never into it. A folder's target is decided the first time the job sees it
// and kept in its row. Notes and journal entries are filed as messages, each
// folder at its own path; contacts, calendars and tasks are counted by class
// here and imported by the contacts and calendars phases
// (MigrationJobPstItems.cpp).
//
// THE MARKS. A folder's row (hm_migrationfolders) holds, in its "UIDVALIDITY",
// the store's record key and the folder's NID - what a folder is matched by
// after a restart - and in its last UID the NID of the last message dealt
// with; a folder's messages are taken in ascending NID order, so the mark says
// exactly where to go on. It is written after every message. The job's own row
// and report are written every 50 messages and at each folder's end; a job that
// ran before reads back what it filed since it first started, and the import's
// duplicate check keeps anything from being filed twice.

#include "StdAfx.h"
#include "MigrationJob.h"
#include "MigrationManager.h"

#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../Persistence/MessageStore.h"
#include "../Pst/PstReaderProcess.h"
#include "../Pst/PstSource.h"
#include "../Pst/PstStore.h"
#include "../Util/Encoding/ModifiedUTF7.h"
#include "../Util/FileUtilities.h"
#include "../Util/MailImporter.h"
#include "../Util/MigrationStore.h"
#include "../Util/PstImportPath.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Strings/Formatter.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/IMAPSpecialUse.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The job's row and report are written after this many messages.
      const int PstMarkEvery = 50;

      // The warnings one run writes into the report's errors and the log; the
      // rest are counted per folder.
      const int PstMaximumRecordedWarnings = 50;

      // The classes of what is not mail the report names, in each of its two
      // counts; the rest are counted together.
      const size_t PstMaximumClasses = 20;

      // 1981-01-01 00:00:00 UTC: an internal date before it is not one.
      const __int64 EarliestInternalDate = 347155200;

      AnsiString Utf8Of(const String &value)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(value, bytes);
         return bytes;
      }

      String WideOf(const std::string &utf8)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(utf8), text);
         return text;
      }

      // Text from the file as the report and the log keep it: no control
      // characters, and at most maximum bytes, cut at a character.
      std::string ReportText(const std::string &utf8, size_t maximum = 200)
      {
         std::string clean = PstFormat::CutUtf8(utf8, maximum);
         for (size_t i = 0; i < clean.size(); i++)
         {
            const unsigned char c = (unsigned char) clean[i];
            if (c < 0x20 || c == 0x7F)
               clean[i] = ' ';
         }
         return clean;
      }

      AnsiString NumberOf(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      __int64 MillisecondsNow()
      {
         return (__int64) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      String LocalTimeOf(__int64 epoch)
      {
         time_t moment = (time_t) epoch;
         DateTime local(moment);
         return Time::GetTimeStampFromDateTime(local);
      }

      String NodeName(uint32_t nid)
      {
         return WideOf("0x" + PstFormat::Hex32(nid));
      }

      int DesignationOfRole(int role)
      {
         switch (role)
         {
         case PstFolderInfo::RoleSent:   return IMAPSpecialUse::DesignationSent;
         case PstFolderInfo::RoleTrash:  return IMAPSpecialUse::DesignationTrash;
         case PstFolderInfo::RoleDrafts: return IMAPSpecialUse::DesignationDrafts;
         case PstFolderInfo::RoleJunk:   return IMAPSpecialUse::DesignationJunk;
         default:                        return IMAPSpecialUse::DesignationNone;
         }
      }

      const char *UseWord(int designation)
      {
         if (designation & IMAPSpecialUse::DesignationSent) return "\\Sent";
         if (designation & IMAPSpecialUse::DesignationDrafts) return "\\Drafts";
         if (designation & IMAPSpecialUse::DesignationJunk) return "\\Junk";
         if (designation & IMAPSpecialUse::DesignationTrash) return "\\Trash";
         return "";
      }

      // A folder of the account's own tree as its list of names, or empty for
      // a tree read half way through a change.
      std::vector<String> NamesOf(std::shared_ptr<IMAPFolders> tree, std::shared_ptr<IMAPFolder> folder)
      {
         std::vector<String> names;
         std::shared_ptr<IMAPFolder> walk = folder;

         for (int guard = 0; walk && guard <= IMAPFolder::MaxFolderDepth; guard++)
         {
            names.insert(names.begin(), walk->GetFolderName());
            const __int64 parentId = walk->GetParentFolderID();
            if (parentId < 0)
               return names;
            walk = tree->GetItemByDBIDRecursive(parentId);
         }

         return std::vector<String>();
      }

      // The four uses a PST's special folders have.
      const int PstUses = IMAPSpecialUse::DesignationSent | IMAPSpecialUse::DesignationTrash |
                          IMAPSpecialUse::DesignationDrafts | IMAPSpecialUse::DesignationJunk;

      // The account's own folders of each special use, as the IMAP job finds
      // them: by the use a folder holds, else by the name table. guessed gets
      // each folder that holds one of the four uses by its name alone, with
      // those uses, so that the job can give them to it for good.
      std::map<int, std::vector<String> > SpecialTargets(std::shared_ptr<IMAPFolders> tree,
                                                         std::map<__int64, std::pair<std::shared_ptr<IMAPFolder>, int> > &guessed)
      {
         std::map<int, std::vector<String> > result;
         guessed.clear();

         std::map<__int64, int> designations;
         IMAPSpecialUse::Resolve(tree, designations);

         for (const std::pair<const __int64, int> &entry : designations)
         {
            std::shared_ptr<IMAPFolder> folder = tree->GetItemByDBIDRecursive(entry.first);
            if (!folder)
               continue;

            const std::vector<String> names = NamesOf(tree, folder);
            if (names.empty())
               continue;

            for (int bit = 1; bit <= (int) IMAPSpecialUse::DesignationTrash; bit <<= 1)
            {
               if ((entry.second & bit) != 0)
                  result[bit] = names;
            }

            const int byName = entry.second & PstUses & ~(folder->GetSpecialUseFlags() & IMAPSpecialUse::DesignationMask);
            if (byName != 0)
               guessed[entry.first] = std::make_pair(folder, byName);
         }

         return result;
      }

      String Joined(const std::vector<String> &names, const String &delimiter)
      {
         String path;
         for (size_t i = 0; i < names.size(); i++)
            path += (i == 0 ? String() : delimiter) + names[i];
         return path;
      }

      AnsiString DisplayPath(const std::vector<std::string> &names)
      {
         std::string path;
         for (size_t i = 0; i < names.size(); i++)
            path += (i == 0 ? std::string() : std::string(" / ")) + names[i];
         return AnsiString(ReportText(path, 1024));
      }

      void SkipMessage(MigrationJob::FolderTally &tally, MigrationJobRecord &job, const char *reason)
      {
         tally.skipped[reason]++;
         job.skipped++;
      }

      // A class of what is not mail, counted: at most PstMaximumClasses by
      // name, the rest together.
      void CountClass(std::map<AnsiString, int> &counts, const std::string &kind, int count)
      {
         AnsiString key = kind.empty() ? AnsiString("(none)") : AnsiString(ReportText(kind, 100));
         if (counts.find(key) == counts.end() && counts.size() >= PstMaximumClasses)
            key = "(other classes)";
         counts[key] += count;
      }

      // A folder's row carries, after its key, where the reader was when the
      // last run wrote it: "/l.N" while the folder's list of messages was being
      // read for the Nth time, "/<nid>.N" while that message was.
      void ParseMarker(const String &uidValidity, const String &key, bool &list, uint32_t &nid, int &attempt)
      {
         list = false;
         nid = 0;
         attempt = 0;

         if (uidValidity.GetLength() <= key.GetLength() + 1 || uidValidity.Left(key.GetLength()) != key ||
             uidValidity[key.GetLength()] != L'/')
            return;

         const String marker = uidValidity.Mid(key.GetLength() + 1);
         const int dot = marker.Find(_T("."));
         if (dot <= 0)
            return;

         const String what = marker.Left(dot);
         attempt = _ttoi(marker.Mid(dot + 1));
         if (what == _T("l"))
            list = true;
         else
            nid = (uint32_t) wcstoul(what.c_str(), nullptr, 16);
      }

      std::string Atom(const std::string &text)
      {
         std::string out;
         for (size_t i = 0; i < text.size(); i++)
         {
            const unsigned char c = (unsigned char) text[i];
            if ((c & 0xC0) == 0x80)
               continue;
            if (c <= 0x20 || c >= 0x7F || strchr("(){%*\"\\]", (int) c) != nullptr)
               out += '_';
            else
               out += (char) c;
         }
         return out;
      }
   }

   String
   MigrationJob::PstFolderName(const std::string &utf8, const String &delimiter, bool topLevel)
   {
      // No name the folder table can hold is longer than 255 characters, and
      // each is at least one: the rest is not looked at, so that a name of
      // 64 KB costs what one of 256 characters does. 1,024 bytes of UTF-8 are
      // at least 256 UTF-16 units whatever they hold.
      std::string head = utf8;
      if (head.size() > 1024)
      {
         size_t cut = 1024;
         while (cut > 0 && ((unsigned char) head[cut] & 0xC0) == 0x80)
            cut--;
         head = head.substr(0, cut);
      }

      String name = WideOf(head);
      if (name.GetLength() > 256)
      {
         int keep = 256;
         if (name[keep - 1] >= 0xD800 && name[keep - 1] <= 0xDBFF)
            keep--;
         name = name.Left(keep);
      }

      // Nothing a folder name cannot hold: a control character, or the
      // hierarchy delimiter, which would make one folder two.
      String cleaned;
      for (int i = 0; i < name.GetLength(); i++)
      {
         const wchar_t c = name[i];
         if (c < 0x20 || c == 0x7F || delimiter.Find(c) >= 0)
            cleaned += L'_';
         else
            cleaned += c;
      }
      name = cleaned;

      if (name.IsEmpty())
         name = _T("Unnamed folder");

      // A top-level name starting with # is a namespace's (IMAPFolder::IsValidFolderName).
      if (topLevel && name[0] == L'#')
         name = _T("_") + name;

      // The folder table holds the encoded form, and it is that which may be
      // 255 characters: cut a character at a time, never between the two
      // halves of a surrogate pair - at most 256 times, the name being that
      // short already.
      AnsiString encoded = ModifiedUTF7::Encode(name);
      while (encoded.GetLength() > 255 && name.GetLength() > 1)
      {
         int cut = name.GetLength() - 1;
         if (cut > 0 && name[cut - 1] >= 0xD800 && name[cut - 1] <= 0xDBFF)
            cut--;
         name = name.Left(cut);
         encoded = ModifiedUTF7::Encode(name);
      }

      return String(encoded);
   }

   std::vector<String>
   MigrationJob::PstTargetPath(const std::vector<std::string> &names, const String &delimiter, std::vector<AnsiString> &renames)
   {
      std::vector<String> path;
      for (size_t i = 0; i < names.size(); i++)
      {
         if ((int) path.size() >= (int) IMAPFolder::MaxFolderDepth)
         {
            renames.push_back(DisplayPath(names) + " -> filed into its ancestor " + NumberOf((__int64) IMAPFolder::MaxFolderDepth) +
                              " levels down, the deepest a folder here may be");
            break;
         }

         String name = PstFolderName(names[i], delimiter, i == 0);
         if (i == 0 && name.CompareNoCase(_T("INBOX")) == 0)
         {
            path.push_back(_T("INBOX"));
            continue;
         }

         const AnsiString decoded = Utf8Of(ModifiedUTF7::Decode(AnsiString(name)));
         if (decoded != AnsiString(names[i]))
            renames.push_back(AnsiString(ReportText(names[i])) + " -> " + decoded);

         path.push_back(name);
      }
      return path;
   }

   String
   MigrationJob::PstKeyword(const std::string &utf8)
   {
      return WideOf(Atom(utf8));
   }

   String
   MigrationJob::PstInternalDate(unsigned __int64 fileTime, __int64 now)
   {
      static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

      int64_t seconds = 0;
      if (!PstFormat::FileTimeToUnix((uint64_t) fileTime, seconds) || seconds < EarliestInternalDate || seconds > now + 86400)
         return String();

      // In this server's time, with its offset, as MailImporter's
      // GetInternalDate_DatePlusTimeZone gives a Date header's: the import
      // keeps the date and time and not the zone (Time::
      // GetInternalDateFromIMAPInternalDate), and FETCH gives them back with
      // the server's offset, so a UTC time written as it is would come back
      // off by that offset.
      const int offset = Time::GetUTCRelationMinutes();
      seconds += (int64_t) offset * 60;

      int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, weekday = 0;
      PstFormat::UnixToCivil(seconds, year, month, day, hour, minute, second, weekday);

      const int magnitude = offset < 0 ? -offset : offset;
      char text[64];
      snprintf(text, sizeof(text), "%02d-%s-%04d %02d:%02d:%02d %c%02d%02d", day, months[(month - 1) % 12], year, hour, minute, second,
               offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
      return WideOf(text);
   }

   MigrationJob::Outcome
   MigrationJob::RunPst_(String &problem)
   {
      report_.isPst = true;
      report_.pst.file = AnsiString(ReportText(std::string(Utf8Of(job_.sourceUser).c_str()), 255));
      report_.pst.refusal.Empty();

      // ------------------------------------------------------------ the file

      const String domain = StringParser::ExtractDomain(owner_->GetAddress());
      String file;
      String directory;
      bool missing = false;
      if (!PstImportPath::Resolve(job_.sourceUser, domain, file, directory, problem, missing))
      {
         if (missing)
            problem += _T(" What the job copied stays; put the file back and retry the job to go on from where it stopped.");
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }

      // Named as the job names it, below the domain's directory: these
      // sentences reach a domain's administrator, who is not told where the
      // directory is on this machine.
      const String shown = PstImportPath::RelativeName(file, directory);

      std::shared_ptr<PstFileSource> source = std::make_shared<PstFileSource>();
      const PstFileSource::OpenResult opened = source->Open(file);
      if (opened == PstFileSource::OpenInUse)
      {
         problem = _T("The file is open in Outlook or another program that does not share it; close it, and the job tries again.");
         return OutcomeRetry;
      }

      if (opened != PstFileSource::OpenDone)
      {
         problem = opened == PstFileSource::OpenDenied
            ? Formatter::Format(_T("The file {0} cannot be read by the account the service runs as; give that account read access to it."), shown)
            : Formatter::Format(_T("The file {0} could not be opened, or is empty."), shown);
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }

      // The file the handle is on is the one that was checked: a name swapped
      // in between for a link out of the directory is refused, not read - and
      // so is a handle whose final path cannot be told, or that is on another
      // machine (Comparable gives nothing for a share), as Resolve refuses both.
      const String finalPath = PstImportPath::Comparable(source->FinalPath());
      if (finalPath.IsEmpty() || !PstImportPath::IsInside(finalPath, directory))
      {
         problem = _T("The file changed between the check and the read; name it again.");
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }

      report_.pst.size = (__int64) source->Size();

      // ------------------------------------------- the store and its folders

      // Twice begun and not finished is a reader that ended the service twice
      // on this file's store or its folders: the job fails, rather than doing
      // it a third time. Counted before the store is opened, since opening it
      // reads the file as listing its folders does.
      if (report_.pst.listAttempts >= 2)
      {
         problem = _T("The reader stopped twice while reading this file's store or its list of folders, and the file is not read again; repair it with Outlook's Inbox Repair Tool (scanpst.exe), or export its folders to a new PST in Outlook.");
         report_.pst.refusal = Utf8Of(problem);
         report_.pst.listAttempts = 0;
         return OutcomeFailed;
      }

      report_.pst.listAttempts++;
      Save_();

      // The reader, in a process of its own, given this file's handle; the stop
      // that a pause, a cancel or the service's own stop sets ends it
      // (Pst/PstReaderProcess.h). What reading the file may cost in this run is
      // the reader's to count, and each reader started after one ended goes on
      // from what the last had counted. The test hook, when an administrator
      // has put one beside the file, is said in the error log.
      PstReaderProcess::TestHook hook;
      String hookSaid;
      if (PstReaderProcess::ReadTestHook(file, directory, hook, hookSaid))
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7553, "MigrationJob::RunPst_", Formatter::Format(_T("Migration job {0}: {1}"), job_id_, hookSaid));

      PstReaderProcess store(source, [this]() { return StopRequested_(); }, Formatter::Format(_T("Migration job {0} ({1})"), job_id_, shown),
                             [this]() { report_.pst.readerRestarts++; }, hook);

      // A way out of the store or the list that is not the reader ending the
      // service: the guard's count goes back to nothing, and is saved with the
      // outcome.
      auto listEnded = [&]()
      {
         report_.pst.listAttempts = 0;
      };

      try
      {
         store.Open(std::string(Utf8Of(job_.sourceSecret).c_str()));
      }
      catch (PstStopped &)
      {
         listEnded();
         Save_();
         return OutcomeStopped;
      }
      catch (PstRefusal &refusal)
      {
         listEnded();
         problem = WideOf(refusal.what());
         report_.pst.refusal = AnsiString(refusal.what());
         return OutcomeFailed;
      }
      catch (PstError &error)
      {
         listEnded();
         problem = WideOf(std::string("The file could not be read as an Outlook data file: ") + error.what());
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }
      catch (std::bad_alloc &)
      {
         listEnded();
         problem = _T("The server ran short of memory opening the file.");
         return OutcomeRetry;
      }
      catch (PstReaderFailed &)
      {
         listEnded();
         problem = _T("The PST reader's process ended twice while opening this file's store, and the file is not read again; repair it with Outlook's Inbox Repair Tool (scanpst.exe), or export its folders to a new PST in Outlook.");
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }
      catch (PstReaderUnavailable &unavailable)
      {
         listEnded();
         problem = WideOf(unavailable.what());
         return OutcomeRetry;
      }
      catch (std::exception &error)
      {
         listEnded();
         problem = WideOf(std::string("The file could not be read as an Outlook data file: ") + error.what());
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }

      const std::string recordKey = PstFormat::Hex(store.GetRecordKey());
      report_.pst.storeName = AnsiString(ReportText(store.GetDisplayName()));
      report_.pst.recordKey = AnsiString(recordKey);
      report_.pst.version = store.GetVersion();
      report_.pst.crypt = store.GetCryptMethod();

      // The same store into the same account again would file every message a
      // second time - the duplicate check knows only what this job wrote - so a
      // store a finished job already gave this account is refused, naming it.
      std::vector<MigrationJobRecord> finished;
      if (!MigrationStore::ListDoneJobsOfAccount(job_.targetAccountId, finished))
      {
         listEnded();
         problem = _T("The account's finished migration jobs could not be read.");
         return OutcomeRetry;
      }

      const String recordKeyField = WideOf("\"record_key\":\"" + recordKey + "\"");
      for (const MigrationJobRecord &other : finished)
      {
         if (other.id != job_id_ && other.report.Find(recordKeyField.c_str()) >= 0)
         {
            listEnded();
            problem = Formatter::Format(_T("This file's store was imported into this account already, by job {0} of migration {1}, and importing it again would file every message twice; to import it again, delete that migration first."),
               other.id, other.migrationId);
            report_.pst.refusal = Utf8Of(problem);
            return OutcomeFailed;
         }
      }

      std::vector<PstFolderInfo> folders;
      std::vector<std::string> unread;
      try
      {
         store.ListFolders(folders, unread);
      }
      catch (PstStopped &)
      {
         listEnded();
         Save_();
         return OutcomeStopped;
      }
      catch (PstError &error)
      {
         listEnded();
         problem = WideOf(std::string("The file's folders could not be read: ") + error.what());
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }
      catch (std::bad_alloc &)
      {
         listEnded();
         problem = _T("The server ran short of memory reading the file's folders.");
         return OutcomeRetry;
      }
      catch (PstReaderFailed &)
      {
         listEnded();
         problem = _T("The PST reader's process ended twice while reading this file's list of folders, and the file is not read again; repair it with Outlook's Inbox Repair Tool (scanpst.exe), or export its folders to a new PST in Outlook.");
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }
      catch (PstReaderUnavailable &unavailable)
      {
         listEnded();
         problem = WideOf(unavailable.what());
         return OutcomeRetry;
      }
      catch (std::exception &error)
      {
         listEnded();
         problem = WideOf(std::string("The file's folders could not be read: ") + error.what());
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      }

      // The folders that could not be read, named - the first twenty, and the
      // rest counted, so that a badly damaged file cannot grow the report past
      // what its column holds.
      listEnded();
      report_.pst.notRead.clear();
      for (size_t i = 0; i < unread.size(); i++)
      {
         if (i == 20)
         {
            report_.pst.notRead.push_back("and " + NumberOf((__int64) (unread.size() - 20)) + " more");
            break;
         }
         report_.pst.notRead.push_back(AnsiString(ReportText(unread[i])));
         Record_(WideOf(ReportText(unread[i], 500)));
      }

      // ---------------------------------------------------------- the plan

      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(owner_->GetID());
      if (!tree)
      {
         problem = _T("The target account's folders could not be read.");
         return OutcomeRetry;
      }

      const String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();
      std::map<__int64, std::pair<std::shared_ptr<IMAPFolder>, int> > guessed;
      const std::map<int, std::vector<String> > special = SpecialTargets(tree, guessed);

      // An account folder that holds a use only by its name keeps it for good
      // before the import makes anything, so that no folder the import makes
      // - an ordinary "Sent" of the file, listed before the account's "Sent
      // Items" - can take it by name (IMAPSpecialUse::Resolve).
      bool anyMail = false;
      for (const PstFolderInfo &folder : folders)
         anyMail = anyMail || folder.mail || PstStore::IsMessageLikeFolderClass(folder.containerClass);
      if (anyMail)
      {
         for (const auto &entry : guessed)
         {
            std::shared_ptr<IMAPFolder> folder = entry.second.first;
            const int kept = folder->GetSpecialUseFlags() & IMAPSpecialUse::DesignationMask;
            if (!folder->StoreSpecialUseFlags(kept | entry.second.second))
            {
               problem = _T("The special use of one of the target account's folders could not be written.");
               return OutcomeRetry;
            }
         }
      }

      // The paths an ordinary folder of the file may not be filed into: the
      // account's folders of the four uses a PST's special folders have (an
      // ordinary "Archive" of the file still goes into the account's), and
      // those the file's own special folders are made at where the account has
      // none of that use.
      std::vector<String> reserved;
      for (const std::pair<const int, std::vector<String> > &entry : special)
      {
         if ((entry.first & PstUses) != 0)
            reserved.push_back(Joined(entry.second, delimiter));
      }
      for (const PstFolderInfo &folder : folders)
      {
         const int designation = DesignationOfRole(folder.role);
         if (!folder.mail || designation == IMAPSpecialUse::DesignationNone || special.find(designation) != special.end() ||
             folder.path.size() != 1)
            continue;
         std::vector<AnsiString> ignored;
         reserved.push_back(Joined(PstTargetPath(std::vector<std::string>(1, folder.path.back()), delimiter, ignored), delimiter));
      }

      auto isReserved = [&](const std::vector<String> &target) -> bool
      {
         const String joined = Joined(target, delimiter);
         for (const String &path : reserved)
         {
            if (path.CompareNoCase(joined) == 0)
               return true;
         }
         return false;
      };

      std::vector<MigrationFolderRecord> rows;
      if (!MigrationStore::ListFolders(job_id_, rows))
      {
         problem = _T("The job's resume state could not be read.");
         return OutcomeRetry;
      }

      struct Planned
      {
         Planned() : nid(0), designation(0) { }

         uint32_t nid;
         AnsiString source;
         String key;
         std::vector<String> target;
         int designation;          // the use to give the folder when this job makes it
         MigrationFolderRecord row;
      };

      std::vector<Planned> plan;
      std::map<uint32_t, std::vector<String> > targets;
      std::map<AnsiString, int> notMailFolders;
      std::set<AnsiString> tallied;
      __int64 total = 0;
      int done = 0;

      for (const PstFolderInfo &folder : folders)
      {
         const AnsiString source = DisplayPath(folder.path);
         std::vector<AnsiString> renames;

         // Where it goes: the account's own folder of its use, where the store
         // names it one and the account has one; else below its parent's
         // target, or at the top.
         const int designation = DesignationOfRole(folder.role);
         std::map<int, std::vector<String> >::const_iterator own = designation != 0 ? special.find(designation) : special.end();
         std::map<uint32_t, std::vector<String> >::const_iterator parent = targets.find(folder.parentNid);

         std::vector<String> target;
         int give = IMAPSpecialUse::DesignationNone;
         if (own != special.end())
         {
            target = own->second;
         }
         else if (folder.path.size() <= 1 || parent == targets.end())
         {
            std::vector<std::string> last;
            last.push_back(folder.path.empty() ? std::string() : folder.path.back());
            target = PstTargetPath(last, delimiter, renames);
            give = designation;
         }
         else
         {
            target = parent->second;
            if ((int) target.size() >= (int) IMAPFolder::MaxFolderDepth)
               renames.push_back(source + " -> filed into its ancestor " + NumberOf((__int64) IMAPFolder::MaxFolderDepth) + " levels down, the deepest a folder here may be");
            else
            {
               const String name = PstFolderName(folder.path.back(), delimiter, false);
               const AnsiString decoded = Utf8Of(ModifiedUTF7::Decode(AnsiString(name)));
               if (decoded != AnsiString(folder.path.back()))
                  renames.push_back(AnsiString(ReportText(folder.path.back())) + " -> " + decoded);
               target.push_back(name);
            }
            give = designation;
         }

         // An ordinary folder whose path is a special folder's is filed beside
         // it, "<name> (PST)", never into it: an ordinary "Sent Items" of the
         // file does not pour into the account's \Sent.
         if (designation == IMAPSpecialUse::DesignationNone && !target.empty() && !folder.path.empty() && isReserved(target))
         {
            const std::string renamed = folder.path.back() + " (PST)";
            target.back() = PstFolderName(renamed, delimiter, target.size() == 1);
            renames.push_back(AnsiString(ReportText(folder.path.back())) + " -> " + AnsiString(ReportText(renamed)) +
                              ", beside the special folder of that name");
         }

         targets[folder.nid] = target;

         // Two sibling folders of one name are one line of the report, their
         // counts added.
         FolderTally &tally = Tally_(source);
         if (tallied.insert(source).second)
            tally.sourceMessages = folder.contentCount;
         else
            tally.sourceMessages += folder.contentCount;

         if (!folder.mail && !PstStore::IsMessageLikeFolderClass(folder.containerClass))
         {
            // Contacts, a calendar, tasks: counted here and not made as a
            // folder; the contacts and calendars phases import them
            // (MigrationJobPstItems.cpp). Notes and a journal are filed as
            // messages, each folder at its own path. Counted afresh at every
            // list pass, apart from what the messages' own classes add.
            const std::string kind = folder.containerClass.empty() ? std::string("(none)") : ReportText(folder.containerClass, 100);
            CountClass(notMailFolders, folder.containerClass, folder.contentCount);
            tally.note = AnsiString("not mail (") + AnsiString(kind) + "): " + NumberOf(tally.sourceMessages) + " item(s) counted and not imported";
            continue;
         }

         Planned planned;
         planned.nid = folder.nid;
         planned.source = source;
         planned.key = WideOf(recordKey + ":" + PstFormat::Hex32(folder.nid));
         planned.designation = give;

         // Its row, matched by the store and the folder's NID; made the first
         // time the folder is seen, and its target kept from then on.
         bool found = false;
         for (const MigrationFolderRecord &existing : rows)
         {
            if (existing.uidValidity == planned.key ||
                (existing.uidValidity.GetLength() > planned.key.GetLength() &&
                 existing.uidValidity.Left(planned.key.GetLength()) == planned.key &&
                 existing.uidValidity[planned.key.GetLength()] == L'/'))
            {
               planned.row = existing;
               found = true;
               break;
            }
         }

         const String joined = Joined(target, delimiter);
         if (!found)
         {
            planned.row.jobId = job_id_;
            // Shown, never looked up (rows are found by their key): a
            // character a three-byte MySQL column cannot hold is U+FFFD.
            planned.row.folder = WideOf(PstFormat::BmpOnly(std::string(source.c_str())));
            planned.row.targetFolder = joined;
            planned.row.uidValidity = planned.key;
            if (!MigrationStore::SaveFolder(planned.row))
            {
               problem = _T("The job's resume state could not be written.");
               return OutcomeRetry;
            }
         }

         // The target the row keeps. Worked out again it is the same list of
         // names unless the account has gained a folder of the folder's use
         // since, and then the row's is kept; split from the row's text only
         // then, because an encoded name may hold a "/" of its own
         // (ModifiedUTF7) where "/" is the delimiter. The row holds at most
         // MaximumFolderLength characters: what is worked out is compared as
         // the row would hold it, and a row's text that long may be cut, so it
         // is never split.
         if (!found || joined.Left(MigrationStore::MaximumFolderLength) == planned.row.targetFolder ||
             planned.row.targetFolder.GetLength() >= MigrationStore::MaximumFolderLength)
            planned.target = target;
         else
            planned.target = StringParser::SplitString(planned.row.targetFolder, delimiter);

         // A child is filed below where its parent really goes.
         targets[folder.nid] = planned.target;

         tally.target = Utf8Of(planned.row.targetFolder);
         tally.specialUse = UseWord(designation);
         if (!renames.empty() && tally.note.IsEmpty())
         {
            AnsiString note = "renamed: ";
            for (size_t i = 0; i < renames.size(); i++)
               note += (i > 0 ? "; " : "") + renames[i];
            tally.note = note;
         }

         total += folder.contentCount;
         if (planned.row.done)
            done++;
         plan.push_back(planned);
      }

      report_.pst.notMailFolders = notMailFolders;
      job_.foldersTotal = (int) plan.size();
      job_.foldersDone = done;
      job_.messagesTotal = (int) total;
      Save_();

      // What the job filed before an interruption, for the duplicate check,
      // and the folders made, subscribed, where they are missing.
      const String since = LocalTimeOf(job_.startedAt);
      for (Planned &planned : plan)
      {
         if (!first_run_ && !MailImporter::LoadWrittenKeys(owner_, planned.target, since, written_))
         {
            problem = _T("What the job had already copied could not be read.");
            return OutcomeRetry;
         }

         if (planned.row.done)
            continue;

         String why;
         bool permanent = true;
         if (MailImporter::FolderFor(owner_, planned.target, planned.designation, why, permanent) == 0)
         {
            if (!permanent)
            {
               problem = Formatter::Format(_T("The folder {0} could not be made in the target account: {1}"), planned.row.targetFolder, why);
               return OutcomeRetry;
            }

            Tally_(planned.source).note = Utf8Of(why);
            Record_(Formatter::Format(_T("the folder {0} was not copied: {1}"), WideOf(std::string(planned.source.c_str())), why));
            planned.row.done = true;
            planned.row.uidValidity = planned.key;
            MigrationStore::SaveFolder(planned.row);
            job_.foldersDone++;
         }
      }

      // --------------------------------------------------------- the messages

      const __int64 interval = migration_.messagesPerMinute > 0 ? (__int64) (60000 / migration_.messagesPerMinute) : 0;
      __int64 nextMessage = 0;
      int recordedWarnings = 0;

      // The work budget of the whole file spent: the file is not read further
      // in this run, and the job fails with what it copied kept.
      auto spent = [&](MigrationFolderRecord &row, const String &key) -> Outcome
      {
         row.uidValidity = key;
         MigrationStore::SaveFolder(row);
         problem = _T("Reading this file has cost more work than any PST Outlook writes needs for its size: it is damaged, or built to exhaust the reader, and it is not read further. What the job copied stays; retry the job to go on from where it stopped, or repair the file with Outlook's Inbox Repair Tool (scanpst.exe).");
         report_.pst.refusal = Utf8Of(problem);
         return OutcomeFailed;
      };

      for (Planned &planned : plan)
      {
         MigrationFolderRecord &row = planned.row;
         if (row.done)
            continue;

         FolderTally &tally = Tally_(planned.source);
         const String folderName = WideOf(std::string(planned.source.c_str()));

         bool markedList = false;
         uint32_t markedNid = 0;
         int markedAttempt = 0;
         ParseMarker(row.uidValidity, planned.key, markedList, markedNid, markedAttempt);

         // The folder's list of messages, guarded as the list of folders is.
         const int listAttempt = markedList ? markedAttempt + 1 : 1;
         if (listAttempt > 2)
         {
            tally.note = "the reader stopped twice while reading this folder's list of messages; the folder was not copied";
            Record_(Formatter::Format(_T("the folder {0} was not copied: the reader stopped twice while reading its list of messages"), folderName));
            row.done = true;
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            job_.foldersDone++;
            Save_();
            continue;
         }

         row.uidValidity = planned.key + Formatter::Format(_T("/l.{0}"), listAttempt);
         MigrationStore::SaveFolder(row);

         std::vector<uint32_t> messages;
         std::string listProblem;
         try
         {
            store.ListMessages(planned.nid, messages);
         }
         catch (PstStopped &)
         {
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            Save_();
            return OutcomeStopped;
         }
         catch (PstError &error)
         {
            if (store.FileExhausted())
               return spent(row, planned.key);
            listProblem = error.what();
         }
         catch (std::bad_alloc &)
         {
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            problem = _T("The server ran short of memory reading a folder's list of messages.");
            return OutcomeRetry;
         }
         catch (PstReaderFailed &)
         {
            listProblem = "the PST reader's process ended twice while reading it";
         }
         catch (PstReaderUnavailable &unavailable)
         {
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            Save_();
            problem = WideOf(unavailable.what());
            return OutcomeRetry;
         }
         catch (std::exception &error)
         {
            listProblem = error.what();
         }

         if (!listProblem.empty())
         {
            tally.note = AnsiString("the folder's list of messages could not be read: ") + AnsiString(ReportText(listProblem, 500));
            Record_(Formatter::Format(_T("the folder {0} was not copied: its list of messages could not be read: {1}"), folderName, WideOf(ReportText(listProblem, 500))));
            row.done = true;
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            job_.foldersDone++;
            Save_();
            continue;
         }

         row.uidValidity = planned.key;

         int sinceMark = 0;
         bool folderRefused = false;

         for (uint32_t nid : messages)
         {
            if ((__int64) nid <= row.lastUid)
               continue;

            if (interval > 0)
            {
               if (!WaitUntil_(nextMessage))
               {
                  MigrationStore::SaveFolder(row);
                  Save_();
                  return OutcomeStopped;
               }
               nextMessage = MillisecondsNow() + interval;
            }

            if (StopRequested_())
            {
               MigrationStore::SaveFolder(row);
               Save_();
               return OutcomeStopped;
            }

            // The guard: the message the last run was reading or filing when
            // the service ended is taken once more, and a second time left out.
            const int attempt = (!markedList && nid == markedNid) ? markedAttempt + 1 : 1;
            markedNid = 0;
            if (attempt > 2)
            {
               SkipMessage(tally, job_, "reader_stopped");
               Record_(Formatter::Format(_T("the message {0} in {1} was not imported: the service stopped twice while this message was being read or filed, and it is left out so that it cannot stop the job again"),
                  NodeName(nid), folderName));
               row.lastUid = nid;
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);
               continue;
            }

            row.uidValidity = planned.key + _T("/") + WideOf(PstFormat::Hex32(nid)) + Formatter::Format(_T(".{0}"), attempt);
            if (!MigrationStore::SaveFolder(row))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7370, "MigrationJob::RunPst_",
                  Formatter::Format(_T("Migration job {0}: the resume mark of the folder {1} could not be written. The job goes on; an interruption before the next mark repeats the work since the last one, without copying anything twice."),
                     job_id_, folderName));
            }

            // Read, whole, and rebuilt as Internet mail, in the reader's
            // process; one that cannot be is that message's failure. A note or
            // a journal entry is taken as a message there, under the same
            // budget and the same failures (PstStore::AsMessage). A message the
            // reader ends on twice is that message's failure too.
            PstMailRead mail;
            PstMessage &message = mail.message;
            const char *skipReason = nullptr;
            std::string why;
            bool shortOfMemory = false;
            try
            {
               store.ReadMail(nid, mail);
            }
            catch (PstStopped &)
            {
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);
               Save_();
               return OutcomeStopped;
            }
            catch (PstTooLarge &error)
            {
               if (store.FileExhausted())
                  return spent(row, planned.key);
               skipReason = "too_large";
               why = error.what();
            }
            catch (PstError &error)
            {
               if (store.FileExhausted())
                  return spent(row, planned.key);
               skipReason = "unreadable";
               why = error.what();
            }
            catch (std::bad_alloc &)
            {
               shortOfMemory = true;
            }
            catch (PstReaderFailed &error)
            {
               skipReason = "reader_failed";
               why = error.what();
            }
            catch (PstReaderUnavailable &unavailable)
            {
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);
               Save_();
               problem = WideOf(unavailable.what());
               return OutcomeRetry;
            }
            catch (std::exception &error)
            {
               skipReason = "unreadable";
               why = error.what();
            }

            if (!skipReason && !shortOfMemory && mail.outcome == PstMailRead::NotMail)
            {
               // An appointment, a contact, a task in a mail folder: counted by
               // class, not imported. A note or a journal entry was taken as a
               // message by the reader.
               skipReason = "not_mail";
               CountClass(report_.pst.notMail, message.messageClass, 1);
            }

            std::string mime;
            std::vector<PstWarning> warnings;
            if (!skipReason && !shortOfMemory)
            {
               if (mail.outcome == PstMailRead::NotBuiltShortOfMemory)
                  shortOfMemory = true;
               else if (mail.outcome == PstMailRead::NotBuilt)
               {
                  skipReason = "unreadable";
                  why = "it could not be written as Internet mail: " + mail.why;
               }
               else
               {
                  mime.swap(mail.mime);
                  warnings.swap(mail.warnings);
               }
            }

            // Memory that ran short is the server's, not the message's: the
            // mark stays before it, and the job pauses and reads it again.
            if (shortOfMemory)
            {
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);
               Save_();
               problem = Formatter::Format(_T("The server ran short of memory reading the message {0} of {1}; the job goes on from it."), NodeName(nid), folderName);
               return OutcomeRetry;
            }

            if (skipReason)
            {
               SkipMessage(tally, job_, skipReason);
               if (strcmp(skipReason, "not_mail") != 0)
                  Record_(Formatter::Format(_T("the message {0} in {1} was not imported: {2}"), NodeName(nid), folderName, WideOf(ReportText(why, 500))));
               row.lastUid = nid;
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);

               // A file that ends the reader on message after message is not
               // read on by starting two readers for each of the rest.
               if (strcmp(skipReason, "reader_failed") == 0 && store.FailuresInARow() >= PstReaderProcess::MaximumFailuresInARow)
               {
                  problem = Formatter::Format(_T("The PST reader's process ended on {0} messages in a row, each read twice, and the file is not read further: it is damaged throughout, or built to end the reader. What the job copied stays; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and retry the job to go on from where it stopped."),
                     PstReaderProcess::MaximumFailuresInARow);
                  report_.pst.refusal = Utf8Of(problem);
                  Save_();
                  return OutcomeFailed;
               }
               continue;
            }

            // Its flags: read, unsent, flagged for follow-up, replied to; its
            // categories and a forward as keywords.
            short flags = 0;
            if (message.messageFlags & 0x01)
               flags |= Message::FlagSeen;
            if (message.messageFlags & 0x08)
               flags |= Message::FlagDraft;
            if (message.flagStatus == 2)
               flags |= Message::FlagFlagged;
            if (message.lastVerb == 102 || message.lastVerb == 103)
               flags |= Message::FlagAnswered;

            std::vector<String> keywords;
            if (message.lastVerb == 104)
               keywords.push_back(_T("$Forwarded"));

            int keywordLength = message.lastVerb == 104 ? 10 : 0;
            for (const std::string &category : message.categories)
            {
               const String keyword = PstKeyword(category);
               if (keyword.IsEmpty())
                  continue;

               if (keywordLength + (keywordLength > 0 ? 1 : 0) + keyword.GetLength() > Message::KeywordsMaximumLength)
               {
                  warnings.push_back(PstWarning("keywords_left_out", "the category \"" + ReportText(category) + "\" did not fit in the keywords a message may carry"));
                  continue;
               }

               keywordLength += (keywordLength > 0 ? 1 : 0) + keyword.GetLength();
               keywords.push_back(keyword);
            }

            // Its internal date: when it was delivered, else sent, else made.
            const __int64 now = MigrationStore::Now();
            String internalDate = PstInternalDate(message.deliveryTime, now);
            if (internalDate.IsEmpty() && message.deliveryTime != 0)
               warnings.push_back(PstWarning("delivery_time_out_of_range", "its delivery time is before 1981 or in the future; the next date it has is used"));
            if (internalDate.IsEmpty())
            {
               internalDate = PstInternalDate(message.submitTime, now);
               if (internalDate.IsEmpty() && message.submitTime != 0)
                  warnings.push_back(PstWarning("submit_time_out_of_range", "its sent time is before 1981 or in the future; the next date it has is used"));
            }
            if (internalDate.IsEmpty())
               internalDate = PstInternalDate(message.creationTime, now);
            if (internalDate.IsEmpty())
               warnings.push_back(PstWarning("date_missing", "it has no date between 1981 and now; it is dated as its Date header, else as imported"));

            // Written where a fetched message waits, and moved into the account
            // by the import.
            std::shared_ptr<Message> spool = std::shared_ptr<Message>(new Message());
            const String spoolFile = MessageStore::WorkingFile(spool);
            const __int64 bytes = (__int64) mime.size();
            if (!MessageStore::Backend().WriteAtomically(spoolFile, AnsiString(mime)))
            {
               FileUtilities::DeleteFile(spoolFile);
               row.uidValidity = planned.key;
               MigrationStore::SaveFolder(row);
               Save_();
               problem = Formatter::Format(_T("A message of {0} could not be written to the queue directory to be filed."), folderName);
               return OutcomeRetry;
            }
            mime.clear();

            // Archived mail: the 2 GB ceiling, and not the account's
            // max_message_size, which is about what may be sent to it now.
            String importProblem;
            const MailImporter::ImportOutcome imported =
               MailImporter::ImportMessage(owner_, planned.target, planned.designation, spoolFile, flags, keywords, internalDate,
                                           &written_, false, importProblem, true);

            if (imported == MailImporter::ImportDone)
            {
               tally.copied++;
               row.messages++;
               job_.messagesDone++;
               job_.bytesDone += bytes;

               const AnsiString when = Utf8Of(Time::GetInternalDateFromIMAPInternalDate(internalDate));
               if (!when.IsEmpty())
               {
                  if (report_.firstMessage.IsEmpty() || when < report_.firstMessage)
                     report_.firstMessage = when;
                  if (report_.lastMessage.IsEmpty() || when > report_.lastMessage)
                     report_.lastMessage = when;
               }

               for (const PstWarning &warning : warnings)
               {
                  tally.warnings[AnsiString(warning.word)]++;
                  if (recordedWarnings < PstMaximumRecordedWarnings)
                  {
                     recordedWarnings++;
                     Record_(Formatter::Format(_T("the message {0} in {1} (\"{2}\") was imported without something: {3}"),
                        NodeName(nid), folderName, WideOf(ReportText(message.subject)), WideOf(ReportText(warning.detail, 500))));
                  }
               }

               if (!progressed_)
               {
                  progressed_ = true;
                  job_.attempts = 0;
               }
            }
            else
            {
               FileUtilities::DeleteFile(spoolFile);

               if (imported == MailImporter::ImportDuplicate || imported == MailImporter::ImportDuplicateElsewhere)
               {
                  // Counted, not named: a job run again after an interruption
                  // meets every message it filed since its last mark.
                  SkipMessage(tally, job_, "duplicate");
               }
               else if (imported == MailImporter::ImportTooLarge)
               {
                  SkipMessage(tally, job_, "too_large");
                  Record_(Formatter::Format(_T("the message {0} in {1} (\"{2}\") was not imported: {3}"),
                     NodeName(nid), folderName, WideOf(ReportText(message.subject)), importProblem));
               }
               else if (imported == MailImporter::ImportFolderRefused)
               {
                  tally.note = Utf8Of(importProblem);
                  Record_(Formatter::Format(_T("the folder {0} was not copied on: {1}"), folderName, importProblem));
                  folderRefused = true;
               }
               else
               {
                  // Not filed, so not dealt with: the mark stays before it, and
                  // the next attempt reads it again - after a quota raised, a
                  // volume cleared, a database back.
                  row.uidValidity = planned.key;
                  MigrationStore::SaveFolder(row);

                  if (imported == MailImporter::ImportFailed)
                  {
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7371, "MigrationJob::RunPst_",
                        Formatter::Format(_T("Migration job {0}: a message of the PST folder {1} could not be filed into {2}: {3}. The job pauses and tries again."),
                           job_id_, folderName, owner_->GetAddress(), importProblem));
                  }

                  problem = Formatter::Format(_T("A message of {0} could not be filed into {1}: {2}. The job goes on from it once that is mended."),
                     folderName, owner_->GetAddress(), importProblem);
                  Save_();
                  return OutcomeRetry;
               }
            }

            if (folderRefused)
            {
               row.uidValidity = planned.key;
               break;
            }

            row.lastUid = nid;
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);

            if (++sinceMark >= PstMarkEvery)
            {
               sinceMark = 0;
               Save_();
            }
         }

         row.done = true;
         row.uidValidity = planned.key;
         MigrationStore::SaveFolder(row);
         job_.foldersDone++;
         Save_();
      }

      return OutcomeDone;
   }
}
