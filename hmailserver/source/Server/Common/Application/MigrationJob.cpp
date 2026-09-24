// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See MigrationJob.h.

#include "StdAfx.h"
#include "MigrationJob.h"
#include "MigrationManager.h"

#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Util/AddressBookStore.h"
#include "../Util/CalendarStore.h"
#include "../Util/ContactStore.h"
#include "../Util/DavSupport.h"
#include "../Util/FileUtilities.h"
#include "../Util/ICalendar.h"
#include "../Util/Imip.h"
#include "../Util/JsonDocument.h"
#include "../Util/MailImporter.h"
#include "../Util/MigrationImapClient.h"
#include "../Util/MigrationSources.h"
#include "../Util/ReaderOAuth.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../Util/VariantDateTime.h"
#include "../Util/VCard.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Strings/Formatter.h"
#include "../../IMAP/IMAPCommandAppend.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/IMAPSpecialUse.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The resume mark is written after this many messages dealt with.
      const int MarkEvery = 50;

      // How often a message the source refuses to give is asked for before it
      // is skipped.
      const int MaxFetchAttempts = 3;

      // A contacts or calendars phase that keeps meeting a busy source is
      // skipped after this many attempts in a row, rather than failing a job
      // whose mail is copied.
      const int MaxDavAttempts = 6;

      // The largest card or event taken: what this server's own CardDAV and
      // CalDAV take (CardDavServer's and CalDavServer's max-resource-size).
      const int MaxDavItemBytes = 1024 * 1024;

      // Under one of the prefixes NAMESPACE gave for other users' and shared
      // mailboxes, or the namespace's own node ("Other Users" of "Other Users/").
      bool UnderPrefix(const AnsiString &name, const std::vector<AnsiString> &prefixes)
      {
         for (const AnsiString &prefix : prefixes)
         {
            if (prefix.IsEmpty())
               continue;

            if (name.GetLength() >= prefix.GetLength() && name.Mid(0, prefix.GetLength()).CompareNoCase(prefix) == 0)
               return true;

            AnsiString node = prefix;
            const char last = node.GetAt(node.GetLength() - 1);
            if (!((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9')))
               node = node.Mid(0, node.GetLength() - 1);
            if (!node.IsEmpty() && name.CompareNoCase(node) == 0)
               return true;
         }

         return false;
      }

      // The largest message copied, as the fetcher's (IMAPClientConnection's
      // MaxMessageBytes): a larger one is skipped and counted.
      const __int64 MaxMessageBytes = (__int64) 512 * 1024 * 1024;

      // The errors a report keeps, newest last.
      const size_t MaxErrorsKept = 50;

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(value, bytes);
         return bytes;
      }

      String Wide(const AnsiString &utf8)
      {
         String text;
         Unicode::MultiByteToWide(utf8, text);
         return text;
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      // An epoch as the server's local time, as hm_messages keeps its dates.
      String LocalTime(__int64 epoch)
      {
         time_t moment = (time_t) epoch;
         DateTime local(moment);
         return Time::GetTimeStampFromDateTime(local);
      }

      __int64 NowMilliseconds()
      {
         return (__int64) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      // One folder of the source, and what becomes of it.
      struct Planned
      {
         Planned() : designation(0), all(false) { }

         MigrationImapClient::Mailbox mailbox;
         int designation;
         bool all;
         AnsiString target;
      };

      bool HasAttribute(const AnsiString &attributes, const char *attribute)
      {
         std::vector<AnsiString> words = StringParser::SplitString(attributes, " ");
         for (AnsiString word : words)
         {
            word.Trim();
            if (word.CompareNoCase(attribute) == 0)
               return true;
         }
         return false;
      }

      // The special use of a source folder: the attributes its LIST line
      // carries, else - a server that sends none - the name table this server
      // knows its own top-level folders by.
      int DesignationOf(const MigrationImapClient::Mailbox &mailbox)
      {
         int designation = IMAPSpecialUse::DesignationNone;

         std::vector<AnsiString> words = StringParser::SplitString(mailbox.attributes, " ");
         for (AnsiString word : words)
         {
            word.Trim();
            if (word.IsEmpty())
               continue;

            int one = IMAPSpecialUse::DesignationNone;
            if (IMAPSpecialUse::ParseAttributeList(String(word), one))
               designation |= one;
         }

         if (designation == IMAPSpecialUse::DesignationNone)
         {
            const bool topLevel = mailbox.delimiter.IsEmpty() || mailbox.name.Find(mailbox.delimiter) < 0;
            if (topLevel)
               designation = IMAPSpecialUse::DesignationOfName(String(mailbox.name));
         }

         return designation;
      }

      const char *AttributeWord(int designation)
      {
         if (designation & IMAPSpecialUse::DesignationSent) return "\\Sent";
         if (designation & IMAPSpecialUse::DesignationDrafts) return "\\Drafts";
         if (designation & IMAPSpecialUse::DesignationJunk) return "\\Junk";
         if (designation & IMAPSpecialUse::DesignationTrash) return "\\Trash";
         if (designation & IMAPSpecialUse::DesignationArchive) return "\\Archive";
         if (designation & IMAPSpecialUse::DesignationAll) return "\\All";
         return "";
      }

      // A folder of the account's own tree as a path in this server's delimiter.
      AnsiString PathOf(std::shared_ptr<IMAPFolders> tree, std::shared_ptr<IMAPFolder> folder, const String &delimiter)
      {
         std::vector<String> names;
         std::shared_ptr<IMAPFolder> walk = folder;

         for (int guard = 0; walk && guard <= IMAPFolder::MaxFolderDepth; guard++)
         {
            names.insert(names.begin(), walk->GetFolderName());

            const __int64 parentId = walk->GetParentFolderID();
            if (parentId < 0)
            {
               String path;
               for (size_t i = 0; i < names.size(); i++)
                  path += (i == 0 ? String() : delimiter) + names[i];
               return Utf8(path);
            }

            walk = tree->GetItemByDBIDRecursive(parentId);
         }

         // A cycle, or a tree read half way through a change: no path.
         return "";
      }

      // The account's own folders of each special use, by designation bit.
      std::map<int, AnsiString> SpecialFolders(std::shared_ptr<IMAPFolders> tree, const String &delimiter)
      {
         std::map<int, AnsiString> result;

         std::map<__int64, int> designations;
         IMAPSpecialUse::Resolve(tree, designations);

         for (const std::pair<const __int64, int> &entry : designations)
         {
            std::shared_ptr<IMAPFolder> folder = tree->GetItemByDBIDRecursive(entry.first);
            if (!folder)
               continue;

            const AnsiString path = PathOf(tree, folder, delimiter);
            if (path.IsEmpty())
               continue;

            for (int bit = 1; bit <= (int) IMAPSpecialUse::DesignationTrash; bit <<= 1)
            {
               if ((entry.second & bit) != 0)
                  result[bit] = path;
            }
         }

         return result;
      }

      AnsiString TargetPath(const Planned &planned, const std::map<int, AnsiString> &special, const AnsiString &localDelimiter)
      {
         if (planned.mailbox.name.CompareNoCase("INBOX") == 0)
            return "INBOX";

         const int uses[] =
         {
            IMAPSpecialUse::DesignationSent, IMAPSpecialUse::DesignationDrafts, IMAPSpecialUse::DesignationJunk,
            IMAPSpecialUse::DesignationTrash, IMAPSpecialUse::DesignationArchive
         };

         for (int use : uses)
         {
            if ((planned.designation & use) == 0)
               continue;

            std::map<int, AnsiString>::const_iterator found = special.find(use);
            if (found != special.end())
               return found->second;
         }

         // Mail kept under no other folder - Gmail's archived mail, which is in
         // All Mail alone - goes where the account keeps its archive.
         if (planned.all)
         {
            std::map<int, AnsiString>::const_iterator found = special.find(IMAPSpecialUse::DesignationArchive);
            if (found != special.end())
               return found->second;
         }

         AnsiString path = planned.mailbox.name;
         if (!planned.mailbox.delimiter.IsEmpty() && planned.mailbox.delimiter != localDelimiter)
            path.Replace(planned.mailbox.delimiter, localDelimiter);

         return path;
      }

      void ParseFlags(const AnsiString &flags, short &bits, std::vector<String> &keywords)
      {
         bits = 0;
         keywords.clear();

         std::vector<AnsiString> words = StringParser::SplitString(flags, " ");
         for (AnsiString word : words)
         {
            word.Trim();
            if (word.IsEmpty())
               continue;

            if (word.CompareNoCase("\\Seen") == 0)
               bits |= Message::FlagSeen;
            else if (word.CompareNoCase("\\Flagged") == 0)
               bits |= Message::FlagFlagged;
            else if (word.CompareNoCase("\\Answered") == 0)
               bits |= Message::FlagAnswered;
            else if (word.CompareNoCase("\\Draft") == 0)
               bits |= Message::FlagDraft;
            else if (word.CompareNoCase("\\Deleted") == 0)
               bits |= Message::FlagDeleted;
            else if (word.GetAt(0) != '\\')
               keywords.push_back(Wide(word));
         }
      }

      void Skip(MigrationJob::FolderTally &tally, MigrationJobRecord &job, const char *reason)
      {
         tally.skipped[reason]++;
         job.skipped++;
      }

      void ImportCards(__int64 accountId, const std::vector<MigrationSources::Collection> &collections, MigrationJob::PhaseTally &tally)
      {
         AddressBookRecord defaultBook;
         if (!AddressBookStore::EnsureDefault(accountId, defaultBook))
         {
            tally.reason = "the account's address books could not be read";
            return;
         }

         bool first = true;
         for (const MigrationSources::Collection &collection : collections)
         {
            // The source's first book, or its "contacts", is the account's
            // default one; every other is a book of the same name here.
            __int64 bookId = defaultBook.id;
            if (!first && collection.segment.CompareNoCase("contacts") != 0)
            {
               String name = Wide(collection.displayName);
               if (!AddressBookStore::IsUsableName(name))
                  name = Wide(collection.segment);

               AddressBookRecord book;
               if (AddressBookStore::IsUsableName(name) &&
                   (AddressBookStore::FindByName(accountId, name, book) || AddressBookStore::CreateBook(accountId, name, book)))
                  bookId = book.id;
            }
            first = false;

            for (const MigrationSources::Item &item : collection.items)
            {
               if (item.data.GetLength() > MaxDavItemBytes)
               {
                  tally.skipped["too_large"]++;
                  continue;
               }

               std::vector<VCardProperty> properties;
               AnsiString problem;
               if (!VCard::Parse(item.data, properties, problem))
               {
                  tally.skipped["not_a_card"]++;
                  continue;
               }

               AnsiString which;
               if (VCard::UsesLegacyEncoding(properties, which))
               {
                  tally.skipped["legacy_encoding"]++;
                  continue;
               }

               if (VCard::IsGroup(properties))
               {
                  tally.skipped["group"]++;
                  continue;
               }

               AnsiString name;
               AnsiString address;
               if (!VCard::ExtractNameAndAddress(properties, name, address))
               {
                  tally.skipped["no_address"]++;
                  continue;
               }

               String wideAddress = Wide(address);
               wideAddress.Trim();
               wideAddress.MakeLower();
               if (!ContactStore::IsValidAddress(wideAddress))
               {
                  tally.skipped["no_address"]++;
                  continue;
               }

               String wideName = Wide(name);
               wideName.Trim();
               if (wideName.GetLength() > ContactStore::MaximumNameLength)
                  wideName = wideName.Mid(0, ContactStore::MaximumNameLength);

               // One address once in an account, across its books - the
               // address book's own rule.
               __int64 existing = 0;
               if (ContactStore::FindByAddress(accountId, wideAddress, existing))
               {
                  tally.skipped["already_there"]++;
                  continue;
               }

               ContactRecord inserted;
               if (!ContactStore::InsertCard(accountId, bookId, wideName, wideAddress, String(), Wide(VCard::UidOf(properties)),
                                             Wide(item.data), inserted))
               {
                  tally.skipped["not_saved"]++;
                  continue;
               }

               tally.copied++;
            }
         }
      }

      void ImportEvents(__int64 accountId, const std::vector<MigrationSources::Collection> &collections, MigrationJob::PhaseTally &tally)
      {
         CalendarRecord defaultCalendar;
         if (!CalendarStore::EnsureDefault(accountId, defaultCalendar))
         {
            tally.reason = "the account's calendars could not be read";
            return;
         }

         bool first = true;
         for (const MigrationSources::Collection &collection : collections)
         {
            __int64 calendarId = defaultCalendar.id;
            const String segment = Wide(collection.segment);
            if (!first && collection.segment.CompareNoCase("calendar") != 0 && CalendarStore::IsUsableCalendarName(segment))
            {
               CalendarRecord calendar;
               if (CalendarStore::FindByName(accountId, segment, calendar) ||
                   CalendarStore::CreateCalendar(accountId, segment, Wide(collection.displayName), String(), calendar))
                  calendarId = calendar.id;
            }
            first = false;

            for (const MigrationSources::Item &item : collection.items)
            {
               if (item.data.GetLength() > MaxDavItemBytes)
               {
                  tally.skipped["too_large"]++;
                  continue;
               }

               ICalComponent tree;
               AnsiString problem;
               if (!ICalendar::Parse(item.data, tree, problem))
               {
                  tally.skipped["not_an_event"]++;
                  continue;
               }

               // What the calendar store takes and nothing else, by the check
               // CalDAV and the webmail make.
               ICalendar::Summary summary;
               if (!ICalendar::Summarize(tree, summary, problem))
               {
                  tally.skipped["not_storable"]++;
                  continue;
               }

               CalendarObjectRecord existing;
               if (CalendarStore::FindByUid(accountId, calendarId, Wide(summary.uid), existing))
               {
                  tally.skipped["already_there"]++;
                  continue;
               }

               CalendarObjectRecord record;
               record.uid = Wide(summary.uid);
               record.component = Wide(summary.component);
               record.data = Wide(item.data);
               record.etag = Dav::Wide(Dav::EtagOf(item.data));
               record.start = summary.start;
               record.end = summary.end;
               record.first = summary.first;
               record.last = summary.last;
               record.uri = Imip::UriFor(summary.uid);

               CalendarObjectRecord inserted;
               __int64 token = 0;
               if (!CalendarStore::Insert(accountId, calendarId, record, inserted, token))
               {
                  tally.skipped["not_saved"]++;
                  continue;
               }

               tally.copied++;
            }
         }
      }

      AnsiString ReasonsJson(const std::map<AnsiString, int> &reasons)
      {
         AnsiString json = "{";
         bool firstReason = true;
         for (const std::pair<const AnsiString, int> &reason : reasons)
         {
            if (!firstReason)
               json += ",";
            json += MigrationJob::JsonString(reason.first) + ":" + Number(reason.second);
            firstReason = false;
         }
         return json + "}";
      }

      AnsiString PhaseJson(const MigrationJob::PhaseTally &tally)
      {
         int skipped = 0;
         for (const std::pair<const AnsiString, int> &reason : tally.skipped)
            skipped += reason.second;

         return "{\"state\":" + MigrationJob::JsonString(tally.state) +
                ",\"copied\":" + Number(tally.copied) +
                ",\"skipped\":" + Number(skipped) +
                ",\"skipped_reasons\":" + ReasonsJson(tally.skipped) +
                ",\"reason\":" + MigrationJob::JsonString(tally.reason) + "}";
      }

      AnsiString FolderJson(const MigrationJob::FolderTally &tally)
      {
         int skipped = 0;
         for (const std::pair<const AnsiString, int> &reason : tally.skipped)
            skipped += reason.second;

         return "{\"source\":" + MigrationJob::JsonString(tally.source) +
                ",\"target\":" + MigrationJob::JsonString(tally.target) +
                ",\"special_use\":" + MigrationJob::JsonString(tally.specialUse) +
                ",\"source_messages\":" + Number(tally.sourceMessages) +
                ",\"copied\":" + Number(tally.copied) +
                ",\"skipped\":" + Number(skipped) +
                ",\"skipped_reasons\":" + ReasonsJson(tally.skipped) +
                ",\"uidvalidity_restarts\":" + Number(tally.uidValidityRestarts) +
                ",\"note\":" + MigrationJob::JsonString(tally.note) + "}";
      }

      void ReadReasons(const JsonValue *object, std::map<AnsiString, int> &reasons)
      {
         if (!object || !object->IsObject())
            return;

         for (const std::pair<std::string, JsonValue> &member : object->Members())
            reasons[AnsiString(member.first.c_str())] = (int) member.second.AsInt64();
      }

      void ReadPhase(const JsonValue *object, MigrationJob::PhaseTally &tally)
      {
         if (!object || !object->IsObject())
            return;

         tally.state = AnsiString(object->GetString("state", "pending").c_str());
         tally.copied = (int) object->GetInt64("copied");
         tally.reason = AnsiString(object->GetString("reason").c_str());
         ReadReasons(object->Get("skipped_reasons"), tally.skipped);
      }
   }

   MigrationJob::MigrationJob(__int64 jobId, std::shared_ptr<std::atomic<int> > signal) :
      job_id_(jobId),
      signal_(signal),
      provider_(nullptr),
      first_run_(true),
      progressed_(false)
   {
   }

   MigrationJob::~MigrationJob()
   {
   }

   const char *
   MigrationJob::StateWord(int state)
   {
      switch (state)
      {
      case MigrationStore::JobQueued:    return "queued";
      case MigrationStore::JobRunning:   return "running";
      case MigrationStore::JobPaused:    return "paused";
      case MigrationStore::JobDone:      return "done";
      case MigrationStore::JobFailed:    return "failed";
      case MigrationStore::JobCancelled: return "cancelled";
      default:                           return "unknown";
      }
   }

   const char *
   MigrationJob::PhaseWord(int phase)
   {
      switch (phase)
      {
      case MigrationStore::PhaseMail:      return "mail";
      case MigrationStore::PhaseContacts:  return "contacts";
      case MigrationStore::PhaseCalendars: return "calendars";
      default:                             return "report";
      }
   }

   AnsiString
   MigrationJob::JsonString(const AnsiString &utf8)
   {
      static const char *hex = "0123456789abcdef";

      std::string out = "\"";
      for (int i = 0; i < utf8.GetLength(); i++)
      {
         const unsigned char c = (unsigned char) utf8.GetAt(i);
         switch (c)
         {
         case '"':  out += "\\\""; break;
         case '\\': out += "\\\\"; break;
         case '\n': out += "\\n"; break;
         case '\r': out += "\\r"; break;
         case '\t': out += "\\t"; break;
         default:
            if (c < 0x20)
            {
               out += "\\u00";
               out += hex[c >> 4];
               out += hex[c & 0x0F];
            }
            else
               out += (char) c;
         }
      }
      out += "\"";

      return AnsiString(out.c_str());
   }

   AnsiString
   MigrationJob::ReportJson(const MigrationJobRecord &job, const AnsiString &target, const Report &report)
   {
      const __int64 end = job.finishedAt > 0 ? job.finishedAt : MigrationStore::Now();

      AnsiString json = "{";
      json += "\"job\":" + Number(job.id);
      json += ",\"source_user\":" + JsonString(Utf8(job.sourceUser));
      json += ",\"target\":" + JsonString(target);
      json += ",\"state\":" + JsonString(StateWord(job.state));
      json += ",\"phase\":" + JsonString(PhaseWord(job.phase));
      json += ",\"started\":" + Number(job.startedAt);
      json += ",\"finished\":" + Number(job.finishedAt);
      json += ",\"duration_seconds\":" + Number(job.startedAt > 0 && end >= job.startedAt ? end - job.startedAt : 0);
      json += ",\"attempts\":" + Number(job.attempts);
      json += ",\"bytes\":" + Number(job.bytesDone);
      json += ",\"messages_total\":" + Number(job.messagesTotal);
      json += ",\"messages_copied\":" + Number(job.messagesDone);
      json += ",\"messages_skipped\":" + Number(job.skipped);
      json += ",\"first_message\":" + JsonString(report.firstMessage);
      json += ",\"last_message\":" + JsonString(report.lastMessage);
      json += ",\"contacts\":" + PhaseJson(report.contacts);
      json += ",\"calendars\":" + PhaseJson(report.calendars);

      json += ",\"errors\":[";
      for (size_t i = 0; i < report.errors.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "{\"at\":" + Number(report.errors[i].at) + ",\"attempt\":" + Number(report.errors[i].attempt) +
                 ",\"error\":" + JsonString(report.errors[i].error) + "}";
      }
      json += "]";

      // The folders last, as many as the column holds; the count of those left
      // out says so rather than the report looking complete.
      AnsiString folders;
      size_t leftOut = 0;
      for (size_t i = 0; i < report.folders.size(); i++)
      {
         const AnsiString one = FolderJson(report.folders[i]);
         if (json.GetLength() + folders.GetLength() + one.GetLength() + 64 > MigrationStore::MaximumReportBytes)
         {
            leftOut = report.folders.size() - i;
            break;
         }
         if (!folders.IsEmpty())
            folders += ",";
         folders += one;
      }

      json += ",\"folders\":[" + folders + "]";
      json += ",\"folders_left_out\":" + Number((__int64) leftOut);
      json += "}";
      return json;
   }

   void
   MigrationJob::ParseReport(const AnsiString &json, Report &report)
   {
      report = Report();
      if (json.IsEmpty())
         return;

      JsonValue root;
      std::string parseError;
      if (!JsonValue::Parse(std::string(json.c_str(), (size_t) json.GetLength()), root, parseError) || !root.IsObject())
         return;

      report.firstMessage = AnsiString(root.GetString("first_message").c_str());
      report.lastMessage = AnsiString(root.GetString("last_message").c_str());
      ReadPhase(root.Get("contacts"), report.contacts);
      ReadPhase(root.Get("calendars"), report.calendars);

      if (const JsonValue *errors = root.Get("errors"))
      {
         for (const JsonValue &error : errors->Items())
         {
            ErrorEntry entry;
            entry.at = error.GetInt64("at");
            entry.attempt = (int) error.GetInt64("attempt");
            entry.error = AnsiString(error.GetString("error").c_str());
            report.errors.push_back(entry);
         }
      }

      if (const JsonValue *folders = root.Get("folders"))
      {
         for (const JsonValue &folder : folders->Items())
         {
            FolderTally tally;
            tally.source = AnsiString(folder.GetString("source").c_str());
            tally.target = AnsiString(folder.GetString("target").c_str());
            tally.specialUse = AnsiString(folder.GetString("special_use").c_str());
            tally.sourceMessages = folder.GetInt64("source_messages");
            tally.copied = (int) folder.GetInt64("copied");
            tally.uidValidityRestarts = (int) folder.GetInt64("uidvalidity_restarts");
            tally.note = AnsiString(folder.GetString("note").c_str());
            ReadReasons(folder.Get("skipped_reasons"), tally.skipped);
            report.folders.push_back(tally);
         }
      }
   }

   bool
   MigrationJob::StopRequested_() const
   {
      return signal_ && signal_->load() != MigrationManager::SignalNone;
   }

   bool
   MigrationJob::WaitUntil_(__int64 milliseconds)
   {
      // A throttle a pause or a stop does not have to wait out.
      while (NowMilliseconds() < milliseconds)
      {
         if (StopRequested_())
            return false;

         const __int64 left = milliseconds - NowMilliseconds();
         std::this_thread::sleep_for(std::chrono::milliseconds(left > 100 ? 100 : (left > 0 ? left : 0)));
      }

      return !StopRequested_();
   }

   void
   MigrationJob::Record_(const String &what)
   {
      ErrorEntry entry;
      entry.at = MigrationStore::Now();
      entry.attempt = job_.attempts;
      entry.error = Utf8(what);
      report_.errors.push_back(entry);
      if (report_.errors.size() > MaxErrorsKept)
         report_.errors.erase(report_.errors.begin());

      LOG_APPLICATION(Formatter::Format(_T("Migration {0}: job {1} ({2}): {3}"), job_.migrationId, job_id_, job_.sourceUser, what));
   }

   MigrationJob::FolderTally &
   MigrationJob::Tally_(const AnsiString &source)
   {
      for (FolderTally &tally : report_.folders)
      {
         if (tally.source == source)
            return tally;
      }

      FolderTally tally;
      tally.source = source;
      report_.folders.push_back(tally);
      return report_.folders.back();
   }

   void
   MigrationJob::Save_()
   {
      job_.report = Wide(ReportJson(job_, owner_ ? Utf8(owner_->GetAddress()) : AnsiString(), report_));

      if (!MigrationStore::UpdateJob(job_))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7240, "MigrationJob::Save_",
            Formatter::Format(_T("Migration job {0}: its progress and resume state could not be written to the database. It goes on, and an interruption before the next write repeats the work since the last one, without copying anything twice."), job_id_));
      }
   }

   void
   MigrationJob::Finish_(int state, const String &error)
   {
      job_.state = state;
      job_.nextAttempt = 0;
      if (state == MigrationStore::JobDone || state == MigrationStore::JobFailed || state == MigrationStore::JobCancelled)
         job_.finishedAt = MigrationStore::Now();
      if (state == MigrationStore::JobDone)
         job_.phase = MigrationStore::PhaseReport;
      if (!error.IsEmpty())
         job_.lastError = error;

      Save_();

      LOG_APPLICATION(Formatter::Format(_T("Migration {0}: job {1} ({2}) is {3}{4}"), job_.migrationId, job_id_, job_.sourceUser,
         String(AnsiString(StateWord(state))), error.IsEmpty() ? String(_T(".")) : String(_T(": ")) + error));
   }

   void
   MigrationJob::Retry_(const String &error)
   {
      job_.attempts++;

      ErrorEntry entry;
      entry.at = MigrationStore::Now();
      entry.attempt = job_.attempts;
      entry.error = Utf8(error);
      report_.errors.push_back(entry);
      if (report_.errors.size() > MaxErrorsKept)
         report_.errors.erase(report_.errors.begin());

      if (job_.attempts >= MigrationManager::MaxAttempts)
      {
         Finish_(MigrationStore::JobFailed, Formatter::Format(_T("Gave up after {0} attempts. The last: {1}"), job_.attempts, error));
         return;
      }

      const int minutes = MigrationManager::BackoffMinutes(job_.attempts);
      job_.state = MigrationStore::JobPaused;
      job_.nextAttempt = MigrationStore::Now() + (__int64) minutes * 60;
      job_.lastError = error;
      Save_();

      const String which = Formatter::Format(_T("Migration {0}: job {1} ({2})"), job_.migrationId, job_id_, job_.sourceUser);
      LOG_APPLICATION(Formatter::Format(_T("{0} paused after attempt {1}, and tries again in {2} minute(s): {3}"),
         which, job_.attempts, minutes, error));
   }

   bool
   MigrationJob::AccessToken_(const AnsiString &scope, String &token, AnsiString &granted, bool &transient, String &problem)
   {
      transient = false;

      const ReaderOAuth::Provider *oauth = ReaderOAuth::Find(AnsiString(provider_->oauthProvider));
      if (!oauth)
      {
         problem = _T("the migration names no sign-in provider this server knows");
         return false;
      }

      AnsiString rotated;
      if (!ReaderOAuth::RefreshAccessToken(*oauth, Utf8(job_.sourceSecret), scope, token, granted, rotated, transient, problem))
      {
         // transient is the provider's trouble - no answer, a 429, a 5xx - which
         // passes; anything else is the grant's, which only the mailbox's owner
         // signing in again mends.
         return false;
      }

      // A provider that rotates the refresh token (Microsoft does) has handed
      // over the one that works from now on.
      if (!rotated.IsEmpty())
      {
         job_.sourceSecret = Wide(rotated);
         MigrationStore::SaveSecret(job_id_, job_.sourceSecret, false);
      }

      return true;
   }

   MigrationJob::Outcome
   MigrationJob::RunMail_(String &problem)
   {
      const AnsiString user = Utf8(job_.sourceUser);
      const bool signsIn = !AnsiString(provider_->oauthProvider).IsEmpty();

      // A job that signs in gets its token before it connects: a provider that
      // cannot issue one now is a retry, a grant it refuses fails the job, and
      // in neither case has the source been spoken to.
      String token;
      if (signsIn)
      {
         AnsiString granted;
         bool transient = false;
         if (!AccessToken_(provider_->mailScope, token, granted, transient, problem))
            return transient ? OutcomeRetry : OutcomeFailed;
      }

      MigrationImapClient client([this]() { return StopRequested_(); });

      if (!client.Connect(Utf8(migration_.host), migration_.port, (ConnectionSecurity) migration_.security, 60, problem))
         return client.Stopped() ? OutcomeStopped : OutcomeRetry;

      // Only the credentials refused fails the job at once; a source unable to
      // log anybody on just now is tried again (MigrationImapClient::Login).
      bool refused = false;
      const bool loggedOn = signsIn ? client.LoginXOAuth2(user, Utf8(token), refused, problem)
                                    : client.Login(user, Utf8(job_.sourceSecret), refused, problem);
      if (!loggedOn)
         return client.Stopped() ? OutcomeStopped : (refused ? OutcomeFailed : OutcomeRetry);

      // Other users' mailboxes and the shared ones, as the source names them:
      // somebody else's mail, which their owners' own jobs copy.
      std::vector<AnsiString> othersPrefixes;
      if (!client.Namespace(othersPrefixes, problem))
         return client.Stopped() ? OutcomeStopped : OutcomeRetry;

      std::vector<MigrationImapClient::Mailbox> mailboxes;
      if (!client.List(mailboxes, problem))
         return client.Stopped() ? OutcomeStopped : OutcomeRetry;

      std::shared_ptr<IMAPFolders> tree = IMAPFolderContainer::Instance()->GetFoldersForAccount(owner_->GetID());
      if (!tree)
      {
         problem = _T("The target account's folders could not be read.");
         return OutcomeRetry;
      }

      const String delimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();
      const AnsiString localDelimiter = Utf8(delimiter);
      const std::map<int, AnsiString> special = SpecialFolders(tree, delimiter);

      // The plan: the INBOX first, then the rest in the order the source listed
      // them, \All last; the views of mail kept elsewhere, and other people's
      // mailboxes, left out and said.
      std::vector<Planned> plan;
      for (const MigrationImapClient::Mailbox &mailbox : mailboxes)
      {
         if (!mailbox.selectable)
            continue;

         if (UnderPrefix(mailbox.name, othersPrefixes))
         {
            FolderTally &tally = Tally_(mailbox.name);
            tally.note = "another user's mailbox or a shared one; not copied, since it is not this person's own";
            continue;
         }

         Planned planned;
         planned.mailbox = mailbox;
         planned.designation = DesignationOf(mailbox);

         if ((planned.designation & IMAPSpecialUse::DesignationFlagged) != 0 || HasAttribute(mailbox.attributes, "\\Important"))
         {
            FolderTally &tally = Tally_(mailbox.name);
            tally.specialUse = HasAttribute(mailbox.attributes, "\\Important") ? AnsiString("\\Important") : AnsiString("\\Flagged");
            tally.note = "a view of messages kept in the other folders; not copied, so nothing arrives twice";
            continue;
         }

         planned.all = (planned.designation & IMAPSpecialUse::DesignationAll) != 0;
         planned.target = TargetPath(planned, special, localDelimiter);
         plan.push_back(planned);
      }

      std::stable_partition(plan.begin(), plan.end(), [](const Planned &p) { return p.mailbox.name.CompareNoCase("INBOX") == 0; });
      std::stable_partition(plan.begin(), plan.end(), [](const Planned &p) { return !p.all; });

      // The resume state: a row per folder, made the first time the folder is
      // seen and kept; its target is decided once, so a folder does not move
      // between two attempts because the account gained a \Sent in between.
      std::vector<MigrationFolderRecord> rows;
      if (!MigrationStore::ListFolders(job_id_, rows))
      {
         problem = _T("The job's resume state could not be read.");
         return OutcomeRetry;
      }

      std::vector<MigrationFolderRecord> planRows;
      for (const Planned &planned : plan)
      {
         const String source = Wide(planned.mailbox.name);
         MigrationFolderRecord row;
         bool found = false;
         for (const MigrationFolderRecord &existing : rows)
         {
            if (existing.folder == source)
            {
               row = existing;
               found = true;
               break;
            }
         }

         if (!found)
         {
            row.jobId = job_id_;
            row.folder = source;
            row.targetFolder = Wide(planned.target);
            if (!MigrationStore::SaveFolder(row))
            {
               problem = _T("The job's resume state could not be written.");
               return OutcomeRetry;
            }
         }

         planRows.push_back(row);

         FolderTally &tally = Tally_(planned.mailbox.name);
         tally.target = Utf8(row.targetFolder);
         tally.specialUse = AttributeWord(planned.designation);
      }

      // The totals, for the progress bar and the report: what each folder holds.
      __int64 total = 0;
      int done = 0;
      for (size_t i = 0; i < plan.size(); i++)
      {
         __int64 messages = 0;
         if (!client.Status(plan[i].mailbox.name, messages, problem))
            return client.Stopped() ? OutcomeStopped : OutcomeRetry;

         Tally_(plan[i].mailbox.name).sourceMessages = messages;
         total += messages;
         if (planRows[i].done)
            done++;
      }

      job_.foldersTotal = (int) plan.size();
      job_.foldersDone = done;
      job_.messagesTotal = (int) total;
      Save_();

      // A folder the source names with a special use and that the account has
      // no folder of is made here with that use; one mapped to the account's own
      // is there already.
      const int ownUses = IMAPSpecialUse::DesignationSent | IMAPSpecialUse::DesignationDrafts | IMAPSpecialUse::DesignationJunk |
                          IMAPSpecialUse::DesignationTrash | IMAPSpecialUse::DesignationArchive;

      // What the job has already written, after an interruption, for every
      // folder of the plan - one finished before included, since the check
      // across folders (Gmail's labels, a \All folder) has to know what went
      // into it - and the folders here, made where they are missing.
      const String since = LocalTime(job_.startedAt);
      std::vector<std::vector<String> > paths(plan.size());
      for (size_t i = 0; i < plan.size(); i++)
      {
         MigrationFolderRecord &row = planRows[i];
         paths[i] = StringParser::SplitString(row.targetFolder, delimiter);

         if (!first_run_ && !MailImporter::LoadWrittenKeys(owner_, paths[i], since, written_))
         {
            problem = _T("What the job had already copied could not be read.");
            return OutcomeRetry;
         }

         if (row.done)
            continue;

         String why;
         bool permanent = true;
         if (MailImporter::FolderFor(owner_, paths[i], plan[i].designation & ownUses, why, permanent) == 0)
         {
            // The account's folders unreadable or the database away: another
            // attempt makes the folder. A name the account never takes: the
            // folder is not copied, and the report and the log say so.
            if (!permanent)
            {
               problem = Formatter::Format(_T("The folder {0} could not be made in the target account: {1}"), row.targetFolder, why);
               return OutcomeRetry;
            }

            Tally_(plan[i].mailbox.name).note = Utf8(why);
            Record_(Formatter::Format(_T("the folder {0} was not copied: {1}"), Wide(plan[i].mailbox.name), why));
            row.done = true;
            MigrationStore::SaveFolder(row);
            job_.foldersDone++;
         }
      }

      // The largest message taken: the account's own limit under APPEND's
      // ceiling, and the fetcher's. A larger one is read past, not written.
      __int64 largest = IMAPCommandAppend::GetEffectiveAppendLimitBytes(owner_);
      if (largest <= 0 || largest > MaxMessageBytes)
         largest = MaxMessageBytes;

      const bool acrossFolders = migration_.kind == MigrationStore::KindGoogle;
      const __int64 interval = migration_.messagesPerMinute > 0 ? (__int64) (60000 / migration_.messagesPerMinute) : 0;
      __int64 nextFetch = 0;

      for (size_t i = 0; i < plan.size(); i++)
      {
         const Planned &planned = plan[i];
         MigrationFolderRecord &row = planRows[i];
         if (row.done)
            continue;

         FolderTally &tally = Tally_(planned.mailbox.name);
         const std::vector<String> &path = paths[i];

         __int64 uidValidity = 0;
         __int64 exists = 0;
         bool refusedFolder = false;
         bool transientRefusal = false;
         if (!client.Examine(planned.mailbox.name, uidValidity, exists, refusedFolder, transientRefusal, problem))
         {
            if (client.Stopped())
               return OutcomeStopped;

            // A lost connection, or a refusal time mends (UNAVAILABLE, INUSE,
            // LIMIT...): tried again, the folder kept open.
            if (!refusedFolder || transientRefusal)
               return OutcomeRetry;

            // A folder the source says it will never open: one folder not
            // copied, said in the report and the log; the rest go on.
            tally.note = Utf8(problem);
            Record_(Formatter::Format(_T("the folder {0} was not copied: {1}"), Wide(planned.mailbox.name), problem));
            row.done = true;
            MigrationStore::SaveFolder(row);
            job_.foldersDone++;
            Save_();
            continue;
         }

         const String validity = String(Number(uidValidity));
         if (!row.uidValidity.IsEmpty() && row.uidValidity != validity)
         {
            // The source renumbered the folder: its UIDs name other messages
            // now, so the folder starts again from its first, and the duplicate
            // check keeps what was copied from arriving twice.
            tally.uidValidityRestarts++;
            row.lastUid = 0;
            if (first_run_ && !MailImporter::LoadWrittenKeys(owner_, path, since, written_))
            {
               problem = _T("What the job had already copied could not be read.");
               return OutcomeRetry;
            }
         }

         row.uidValidity = validity;
         if (!MigrationStore::SaveFolder(row))
         {
            problem = _T("The job's resume state could not be written.");
            return OutcomeRetry;
         }

         std::vector<unsigned int> uids;
         if (!client.UidsFrom((unsigned int) (row.lastUid + 1), uids, problem))
            return client.Stopped() ? OutcomeStopped : OutcomeRetry;

         const bool anywhere = acrossFolders || planned.all;
         const int designation = planned.designation & ownUses;
         int sinceMark = 0;
         bool folderRefused = false;

         for (unsigned int uid : uids)
         {
            if (interval > 0)
            {
               if (!WaitUntil_(nextFetch))
               {
                  MigrationStore::SaveFolder(row);
                  Save_();
                  return OutcomeStopped;
               }
               nextFetch = NowMilliseconds() + interval;
            }

            if (StopRequested_())
            {
               MigrationStore::SaveFolder(row);
               Save_();
               return OutcomeStopped;
            }

            // Spooled where a fetched message waits - the queue directory, on
            // the data directory's own volume - and moved into the account's
            // directory by the import, as the fetcher's mirror files one.
            std::shared_ptr<Message> spool = std::shared_ptr<Message>(new Message());
            const String spoolFile = MessageStore::WorkingFile(spool);

            // A message the source will not give - a NO to its FETCH on a
            // connection still in step, a damaged message - is asked for again,
            // then left: one message must not hold back the rest of the mailbox.
            MigrationImapClient::Fetched fetched;
            bool gone = false;
            bool fetchRefused = false;
            bool fetchedOk = false;
            for (int attempt = 1; attempt <= MaxFetchAttempts; attempt++)
            {
               if (attempt > 1 && !WaitUntil_(NowMilliseconds() + 1000))
               {
                  FileUtilities::DeleteFile(spoolFile);
                  MigrationStore::SaveFolder(row);
                  Save_();
                  return OutcomeStopped;
               }

               fetchedOk = client.FetchToFile(uid, spoolFile, largest, fetched, gone, fetchRefused, problem);
               if (fetchedOk || !fetchRefused)
                  break;

               FileUtilities::DeleteFile(spoolFile);
            }

            if (!fetchedOk && !fetchRefused)
            {
               FileUtilities::DeleteFile(spoolFile);
               MigrationStore::SaveFolder(row);
               Save_();
               return client.Stopped() ? OutcomeStopped : OutcomeRetry;
            }

            const __int64 previousUid = row.lastUid;
            row.lastUid = uid;

            if (!fetchedOk)
            {
               FileUtilities::DeleteFile(spoolFile);
               Skip(tally, job_, "refused");
               Record_(Formatter::Format(_T("the message with UID {0} in {1} was not copied: {2}"), uid, Wide(planned.mailbox.name), problem));
            }
            else if (gone)
            {
               Skip(tally, job_, "gone");
            }
            else if (fetched.tooLarge)
            {
               Skip(tally, job_, "too_large");
            }
            else if (!fetched.body || fetched.bytes == 0)
            {
               FileUtilities::DeleteFile(spoolFile);
               Skip(tally, job_, "empty");
            }
            else
            {
               short flags = 0;
               std::vector<String> keywords;
               ParseFlags(fetched.flags, flags, keywords);

               String why;
               const MailImporter::ImportOutcome imported =
                  MailImporter::ImportMessage(owner_, path, designation, spoolFile, flags, keywords, Wide(fetched.internalDate),
                                              &written_, anywhere, why);

               if (imported == MailImporter::ImportDone)
               {
                  tally.copied++;
                  row.messages++;
                  job_.messagesDone++;
                  job_.bytesDone += fetched.bytes;

                  const AnsiString when = Utf8(Time::GetInternalDateFromIMAPInternalDate(Wide(fetched.internalDate)));
                  if (!when.IsEmpty())
                  {
                     if (report_.firstMessage.IsEmpty() || when < report_.firstMessage)
                        report_.firstMessage = when;
                     if (report_.lastMessage.IsEmpty() || when > report_.lastMessage)
                        report_.lastMessage = when;
                  }

                  // A run that copies something has made progress: its attempts
                  // are counted afresh, so the ceiling counts attempts in a row
                  // that achieved nothing.
                  if (!progressed_)
                  {
                     progressed_ = true;
                     job_.attempts = 0;
                  }
               }
               else
               {
                  FileUtilities::DeleteFile(spoolFile);

                  if (imported == MailImporter::ImportDuplicate)
                  {
                     Skip(tally, job_, "duplicate");
                  }
                  else if (imported == MailImporter::ImportDuplicateElsewhere)
                  {
                     Skip(tally, job_, "another_folder");
                     if (tally.note.IsEmpty())
                        tally.note = "a message in several folders is copied once, into the first folder it was met in";
                  }
                  else if (imported == MailImporter::ImportTooLarge)
                  {
                     Skip(tally, job_, "too_large");
                  }
                  else if (imported == MailImporter::ImportFolderRefused)
                  {
                     // The folder was there when the job began and cannot be
                     // made now.
                     tally.note = Utf8(why);
                     Record_(Formatter::Format(_T("the folder {0} was not copied on: {1}"), Wide(planned.mailbox.name), why));
                     folderRefused = true;
                  }
                  else
                  {
                     // Not copied, so not dealt with: the mark stays before it and
                     // the next attempt fetches it again - after a quota raised, a
                     // volume cleared, a database back.
                     row.lastUid = previousUid;
                     MigrationStore::SaveFolder(row);

                     if (imported == MailImporter::ImportFailed)
                     {
                        ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7241, "MigrationJob::RunMail_",
                           Formatter::Format(_T("Migration job {0}: a message of the folder {1} could not be filed into {2}: {3}. The job pauses and tries again."),
                              job_id_, Wide(planned.mailbox.name), owner_->GetAddress(), why));
                     }

                     problem = Formatter::Format(_T("A message of {0} could not be filed: {1}"), Wide(planned.mailbox.name), why);
                     Save_();
                     return OutcomeRetry;
                  }
               }
            }

            if (folderRefused)
               break;

            // The mark, after every fifty messages dealt with - copied or
            // skipped - so that a long run of skips is not fetched again after
            // an interruption.
            if (++sinceMark >= MarkEvery)
            {
               sinceMark = 0;
               if (!MigrationStore::SaveFolder(row))
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7240, "MigrationJob::RunMail_",
                     Formatter::Format(_T("Migration job {0}: the resume mark of the folder {1} could not be written. The job goes on; an interruption before the next mark repeats the work since the last one, without copying anything twice."),
                        job_id_, Wide(planned.mailbox.name)));
               Save_();
            }
         }

         if (!row.done)
         {
            row.done = true;
            MigrationStore::SaveFolder(row);
            job_.foldersDone++;
            Save_();
         }
      }

      client.Logout();
      return OutcomeDone;
   }

   MigrationJob::Outcome
   MigrationJob::DavAgain_(PhaseTally &tally, const String &why, String &problem)
   {
      // Tried again, until the attempts in a row reach the phase's own ceiling:
      // then the phase is skipped with the last reason, and the job - whose
      // mail is copied - is done rather than failed.
      if (job_.attempts + 1 >= MaxDavAttempts)
      {
         tally.state = "skipped";
         tally.reason = Utf8(Formatter::Format(_T("gave up after {0} attempts: {1}"), job_.attempts + 1, why));
         return OutcomeDone;
      }

      problem = why;
      return OutcomeRetry;
   }

   MigrationJob::Outcome
   MigrationJob::RunDav_(bool calendars, String &problem)
   {
      PhaseTally &tally = calendars ? report_.calendars : report_.contacts;
      tally = PhaseTally();

      std::vector<MigrationSources::Collection> collections;
      String skipReason;
      bool transient = false;
      bool read = false;

      const char *scope = calendars ? provider_->calendarsScope : provider_->contactsScope;
      const std::function<bool()> stop = [this]() { return StopRequested_(); };

      if (provider_->graph)
      {
         String token;
         AnsiString granted;
         String tokenProblem;
         bool tokenTransient = false;
         if (!AccessToken_(scope, token, granted, tokenTransient, tokenProblem))
         {
            if (tokenTransient)
               return DavAgain_(tally, tokenProblem, problem);

            // Consent for the one resource and not the other is the case a
            // skipped phase is for.
            tally.state = "skipped";
            tally.reason = Utf8(_T("the sign-in does not reach them: ") + tokenProblem);
            return OutcomeDone;
         }

         read = calendars ? MigrationSources::ReadGraphEvents(Utf8(token), collections, skipReason, problem, transient, stop)
                          : MigrationSources::ReadGraphContacts(Utf8(token), collections, skipReason, problem, transient, stop);
      }
      else
      {
         MigrationSources::Credentials credentials;

         if (AnsiString(provider_->oauthProvider).IsEmpty())
         {
            credentials.user = Utf8(job_.sourceUser);
            credentials.password = Utf8(job_.sourceSecret);
         }
         else
         {
            String token;
            AnsiString granted;
            String tokenProblem;
            bool tokenTransient = false;
            if (!AccessToken_(scope, token, granted, tokenTransient, tokenProblem))
            {
               if (tokenTransient)
                  return DavAgain_(tally, tokenProblem, problem);

               tally.state = "skipped";
               tally.reason = Utf8(_T("the sign-in does not reach them: ") + tokenProblem);
               return OutcomeDone;
            }

            // Google says what the token carries; a person may untick a scope
            // on the consent screen, and the phase is refused with the reason.
            if (!granted.IsEmpty() && granted.Find(scope) < 0)
            {
               tally.state = "skipped";
               tally.reason = "the sign-in did not grant " + AnsiString(scope) + "; the job's owner can sign in again with it ticked";
               return OutcomeDone;
            }

            credentials.bearer = Utf8(token);
         }

         const AnsiString start = MigrationSources::DavStart(*provider_, Utf8(migration_.host), Utf8(migration_.davUrl),
                                                              Utf8(job_.sourceUser), calendars);
         read = MigrationSources::ReadDav(start, calendars, credentials, collections, skipReason, problem, transient, stop);
      }

      if (StopRequested_())
         return OutcomeStopped;

      if (!read)
      {
         if (!skipReason.IsEmpty())
         {
            tally.state = "skipped";
            tally.reason = Utf8(skipReason);
            return OutcomeDone;
         }

         if (transient)
            return DavAgain_(tally, problem, problem);

         tally.state = "skipped";
         tally.reason = Utf8(problem);
         return OutcomeDone;
      }

      if (calendars)
         ImportEvents(owner_->GetID(), collections, tally);
      else
         ImportCards(owner_->GetID(), collections, tally);

      tally.state = tally.reason.IsEmpty() ? "copied" : "skipped";
      return OutcomeDone;
   }

   void
   MigrationJob::Run()
   {
      if (!MigrationStore::GetJob(job_id_, job_) || !MigrationStore::GetMigration(job_.migrationId, migration_))
         return;

      ParseReport(Utf8(job_.report), report_);

      provider_ = MigrationSources::ByKind(migration_.kind);
      if (!provider_)
      {
         Finish_(MigrationStore::JobFailed, _T("The migration names a kind of source this server does not know."));
         return;
      }

      owner_ = CacheContainer::Instance()->GetAccount(job_.targetAccountId);
      if (!owner_)
      {
         Finish_(MigrationStore::JobFailed, _T("The target account no longer exists."));
         return;
      }

      first_run_ = job_.startedAt == 0;
      if (first_run_)
         job_.startedAt = MigrationStore::Now();

      job_.state = MigrationStore::JobRunning;
      job_.nextAttempt = 0;
      job_.finishedAt = 0;
      Save_();

      const String which = Formatter::Format(_T("Migration {0}: job {1}"), job_.migrationId, job_id_);
      LOG_APPLICATION(Formatter::Format(_T("{0} copies {1} into {2}, from its {3} phase{4}."),
         which, job_.sourceUser, owner_->GetAddress(), String(AnsiString(PhaseWord(job_.phase))),
         first_run_ ? String(_T("")) : String(_T(", resuming from its marks"))));

      String problem;
      for (;;)
      {
         Outcome outcome = OutcomeDone;

         if (job_.phase == MigrationStore::PhaseMail)
            outcome = RunMail_(problem);
         else if (job_.phase == MigrationStore::PhaseContacts)
            outcome = RunDav_(false, problem);
         else if (job_.phase == MigrationStore::PhaseCalendars)
            outcome = RunDav_(true, problem);
         else
            break;

         if (outcome == OutcomeDone)
         {
            job_.phase++;
            Save_();
            continue;
         }

         if (outcome == OutcomeStopped)
         {
            const int signal = signal_ ? signal_->load() : MigrationManager::SignalStop;
            if (signal == MigrationManager::SignalCancel)
            {
               Finish_(MigrationStore::JobCancelled, String());
            }
            else if (signal == MigrationManager::SignalTargetGone)
            {
               Finish_(MigrationStore::JobCancelled, _T("The target account was deleted."));
            }
            else if (signal == MigrationManager::SignalPause)
            {
               job_.state = MigrationStore::JobPaused;
               job_.nextAttempt = 0;
               Save_();
               LOG_APPLICATION(Formatter::Format(_T("Migration {0}: job {1} ({2}) paused."), job_.migrationId, job_id_, job_.sourceUser));
            }
            else
            {
               // A service stop: the row says running, and the next start
               // queues it again from its marks.
               job_.state = MigrationStore::JobRunning;
               Save_();
            }
            return;
         }

         if (outcome == OutcomeRetry)
         {
            Retry_(problem);
            return;
         }

         Finish_(MigrationStore::JobFailed, problem);
         return;
      }

      // Done: the errors met on the way stay in the report, and the row's
      // last error, which says what is wrong now, is cleared.
      job_.lastError = String();
      Finish_(MigrationStore::JobDone, String());
   }

   void
   MigrationJobTester::Test()
   {
      typedef MigrationJob J;

      Expect(J::JsonString("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "A JSON string escapes the quote, the backslash and the controls.");
      Expect(AnsiString(J::StateWord(MigrationStore::JobCancelled)) == "cancelled" && AnsiString(J::PhaseWord(MigrationStore::PhaseCalendars)) == "calendars",
             "The words of the states and phases.");

      // The report survives its own JSON: what a resumed job reads back is what
      // it wrote.
      MigrationJobRecord job;
      job.id = 7;
      job.sourceUser = _T("ann@source.test");
      job.state = MigrationStore::JobRunning;
      job.startedAt = 1790000000;
      job.messagesDone = 3;

      J::Report report;
      J::FolderTally inbox;
      inbox.source = "INBOX";
      inbox.target = "INBOX";
      inbox.sourceMessages = 4;
      inbox.copied = 3;
      inbox.skipped["duplicate"] = 1;
      report.folders.push_back(inbox);
      report.contacts.state = "skipped";
      report.contacts.reason = "the source has no CardDAV there (HTTP 404)";
      J::ErrorEntry error;
      error.at = 1790000100;
      error.attempt = 1;
      error.error = "The server closed the connection.";
      report.errors.push_back(error);
      report.firstMessage = "2020-01-01 10:00:00";

      const AnsiString json = J::ReportJson(job, "ann@example.test", report);
      J::Report back;
      J::ParseReport(json, back);
      Expect(back.folders.size() == 1 && back.folders[0].source == "INBOX" && back.folders[0].copied == 3 &&
             back.folders[0].skipped["duplicate"] == 1 && back.folders[0].sourceMessages == 4, "The folders read back.");
      Expect(back.contacts.state == "skipped" && back.contacts.reason == report.contacts.reason, "A skipped phase and its reason read back.");
      Expect(back.errors.size() == 1 && back.errors[0].attempt == 1 && back.errors[0].error == error.error, "The errors retried read back.");
      Expect(back.firstMessage == report.firstMessage, "The first message's date reads back.");

      // A report too large for the column leaves folders out, and says so.
      J::Report large;
      for (int i = 0; i < 2000; i++)
      {
         J::FolderTally folder;
         folder.source.Format("Folder number %d with a long name to fill the column", i);
         folder.target = folder.source;
         large.folders.push_back(folder);
      }
      const AnsiString big = J::ReportJson(job, "ann@example.test", large);
      Expect(big.GetLength() <= MigrationStore::MaximumReportBytes && big.Find("\"folders_left_out\":0") < 0,
             "A report is kept within the column, and the folders left out are counted.");

      Expect(MigrationManager::BackoffMinutes(1) == 1 && MigrationManager::BackoffMinutes(2) == 5 &&
             MigrationManager::BackoffMinutes(3) == 15 && MigrationManager::BackoffMinutes(4) == 60 &&
             MigrationManager::BackoffMinutes(20) == 60, "The back-off: 1, 5, 15, then 60 minutes.");
   }
}
