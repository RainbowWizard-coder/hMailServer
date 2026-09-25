// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A PST job's contacts and calendars phases (docs/Migration.md, "Outlook data
// files (PST)"): after the mail, the same file is opened again and what it holds
// besides mail goes where a CardDAV or CalDAV client would have put it -
// through ContactStore and CalendarStore, the stores the webmail, CardDAV and
// CalDAV read and write, and past the same checks.
//
// WHICH FOLDERS. Every folder of the phase's kinds under the IPM subtree but
// those Outlook keeps hidden (PR_ATTR_HIDDEN) and, of contacts, the sub-classes
// of IPF.Contact Exchange and Outlook keep for themselves (the recipient cache,
// the GAL and organisational contacts, Skype's lists); a folder under Deleted
// Items is imported as the mail phase imports Deleted Items, but after every
// other, so that a live contact or event always wins its address or UID over a
// deleted one - an event's UID looked for across every calendar of the account
// - and never into the default: one that cannot have a book or calendar of its
// own is left out, and the report says why. The default folders - those the
// root folder names, else the first of each kind outside Deleted Items - come
// first. The name-to-id map is read once, after the store is opened, as a unit
// of its own.
//
// WHERE THEY GO is decided when a folder's first item is stored, not before,
// and kept in the folder's row ("#<id>", "#<id>*" for a calendar this job made:
// the id alone, since that column is three-byte UTF-8 on MySQL and a name may
// not be), so that a later run puts the rest of the folder where its first
// items went: the default folders into the account's default book or calendar; every
// other contacts folder into a book of its name (at most 255 characters), found
// or made, and every other calendar or tasks folder into a calendar of its own,
// named after the file and the folder, found or made - at most
// MaximumCalendarsPerJob made by one job. A folder that cannot have its own -
// the account at its ceiling of books, the job at its ceiling of calendars, a
// name no book may have - goes into the default, and the report says so.
//
// THE CONTACTS PHASE makes each contact (IPM.Contact) a vCard 4.0
// (Pst/PstItems.h) kept as its card, under its name and first e-mail address -
// the address book's own rule of one address once, so a contact with no e-mail
// address cannot be kept (no_address), nor one whose address the account
// already holds as a contact (already_there). An address the account only
// collected from mail it sent, in the book the contact goes into, is adopted as
// CardDAV adopts it: the row takes the PST's card and is kept from then on. A
// distribution list (IPM.DistList) is counted and not imported.
//
// THE CALENDARS PHASE makes an appointment (IPM.Appointment, or a meeting
// request kept in a calendar folder) a VEVENT with its time zone, its recurrence
// and its changed occurrences, and a task (IPM.Task) a VTODO. Each is parsed and
// summarised as CalDAV parses and summarises what a client sends, and one that
// does not pass is counted (not_storable) with the reason, as is one whose
// recurrence cannot be read (recurrence_unreadable) or that has no time
// (no_start); an object whose UID the calendar holds is not stored again.
//
// THE READER is the mail phase's, in a process of its own
// (Pst/PstReaderProcess.h): each phase starts one for the file, and it reads each
// item and writes it as the vCard or iCalendar text this server keeps
// (Pst/PstItems.h) there; the service parses and stores what it is handed. An
// item the reader ends on twice is left out (reader_failed) and named, and the
// phase goes on with a new reader; the store, the folder list and the default
// folders ended on twice skip the phase, a folder's list that folder.
//
// RESUME AND THE GUARD are the mail phase's (MigrationJobPst.cpp): each folder
// has its own row in hm_migrationfolders, keyed by the first eight digits of the
// store's record key, the folder's NID and ":c" (contacts) or ":e" (calendar
// items and tasks); an item
// is read in NID order, the last one dealt with is its row's mark, and before an
// item or a folder's list is read the row says so ("/<nid>.N", "/l.N"), so an
// item the service ended on - storing it, or for any other reason - is let in
// once more there and the second time left out (reader_stopped) and named.
//
// AN ITEM THE STORE REFUSES is held, its count kept in the folder's row, and the
// mark moves on past it: the items after it show whether the store works. One
// of them stored: each held item is tried once more at once, and one refused
// again is its own refusal - a resource name another UID holds, a character a
// MySQL table cannot keep - counted not_saved and named, with no pause. At the
// end of the folder the held items are tried once more too; one stored shows
// the same. Nothing stored after them: their counts go up and the job pauses
// (HM7411), as the mail phase does with a message it could not file, and the
// next run tries them again when it reaches the folder's end. At most
// MaxHeldItems are held; a further refusal with none stored since pauses the job
// with the mark before it. A held item that reaches MaxItemSaveAttempts is
// decided by its count, in its fourth run (three pauses): given up as
// not_saved, or - where its folder's own book
// or calendar was refused - the folder goes into the default (under Deleted
// Items, is left out) and the item is tried there. Every read, of an item after
// the mark or of a held one, is written into the row first with its attempt
// (a second read of one item in a run keeps it), so the reader's guard leaves
// out an item that stopped the service twice, wherever it is read; the row's
// mark lists the held items with their counts, and an item leaves it only in
// the save that writes its outcome, so no crash, stop or pause loses one.
//
// THE BUDGET is the mail phase's too: each phase opens the file with a work
// budget of its own, the same size as the mail phase's for the file's size
// (Pst/PstFormat.h, PstBudget), carried from one reader to the next, and the
// job's stop ends the reader. The
// store opened, the folder list, the root folder's names for the default
// folders, each folder's list of items and each item read - its recurrence and
// its time zones with it - are each a unit of it. A stop - a pause, a cancel,
// the service stopping - kills the reader where it stands and ends the phase
// with the marks put back where the last item left them (PstStopped); an item
// past its unit is
// counted too_large; the whole file's budget spent fails the job with what it
// copied kept, and a retry goes on from the marks. Memory that runs short is
// the server's: the mark stays before the item, and the job tries again.

#include "StdAfx.h"
#include "MigrationJob.h"
#include "MigrationManager.h"

#include "../BO/Account.h"
#include "../Pst/PstItems.h"
#include "../Pst/PstReaderProcess.h"
#include "../Pst/PstSource.h"
#include "../Pst/PstStore.h"
#include "../Util/AddressBookStore.h"
#include "../Util/CalendarStore.h"
#include "../Util/ContactStore.h"
#include "../Util/DavSupport.h"
#include "../Util/ICalendar.h"
#include "../Util/Imip.h"
#include "../Util/MigrationStore.h"
#include "../Util/PstImportPath.h"
#include "../Util/Unicode.h"
#include "../Util/VCard.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Strings/Formatter.h"

#include <cstring>
#include <cwchar>
#include <functional>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The job's report is written after this many items.
      const int ItemsMarkEvery = 50;

      // What this server's own CardDAV and CalDAV take.
      const size_t MaxItemBytes = 1024 * 1024;

      // The calendars one job makes for a file's calendar and tasks folders;
      // the rest go into the account's default calendar. An account's books
      // have a ceiling of their own (AddressBookStore::MaximumBooks).
      const int MaximumCalendarsPerJob = 50;

      // What a book's or a calendar's name may be, as their columns hold it.
      const size_t MaximumTargetNameUnits = 255;

      // How often a held item may be refused, a run each, before its count
      // decides (the header, AN ITEM THE STORE REFUSES): three pauses.
      const int MaxItemSaveAttempts = 4;

      // An item the store refused, held while the items after it show whether
      // the store works: its NID, how often in a row, and whether it was its
      // folder's book or calendar that was refused rather than the item.
      struct HeldItem
      {
         HeldItem() : nid(0), count(0), target(false) { }

         uint32_t nid;
         int count;
         bool target;
      };

      // How many are held at once: two fit in the row's mark beside the guard's
      // (mfuidvalidity is 64 characters; the key is 19, the guard's part 11,
      // each held item's 12).
      const size_t MaxHeldItems = 2;

      // What a folder's book or calendar is, asked for.
      enum ItemTarget
      {
         ItemTargetFound,       // the id
         ItemTargetNotYet,      // asked without making anything, and it is not made yet
         ItemTargetFailed,      // the database could not say, or would not make it
         ItemTargetLeftOut      // a folder under Deleted Items with none of its own: left out
      };

      // The items a run names in the report's errors and the log; the rest
      // are counted.
      const int MaxRecordedItems = 50;

      // Text from the file as the report and the log keep it, as the mail
      // phase keeps it: no control characters, and at most maximum bytes,
      // cut at a character.
      std::string ItemReportText(const std::string &utf8, size_t maximum = 200)
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

      AnsiString ItemUtf8(const String &value)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(value, bytes);
         return bytes;
      }

      String ItemWide(const std::string &utf8)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(utf8), text);
         return text;
      }

      String ItemNodeName(uint32_t nid)
      {
         return ItemWide("0x" + PstFormat::Hex32(nid));
      }

      // The start of a sentence saying the store refused an item.
      std::string refusedWordOf(bool calendars)
      {
         return std::string("the account's ") + (calendars ? "calendar" : "address book") + " refused it";
      }

      // A folder's name as a book's or a calendar's: at most 255 characters,
      // cut at a character, trimmed.
      String TargetName(const std::string &utf8)
      {
         String wide = ItemWide(PstItems::CutToUnits(utf8, MaximumTargetNameUnits));
         wide.Trim();
         return wide;
      }

      // Where a folder's row says its items go: "#<id>", or "#<id>*" for a
      // calendar the job made (a text after a space, as the first rows of this
      // kind had, is ignored). False for a row that says nothing yet.
      bool ParseTarget(const String &recorded, __int64 &id, bool &made)
      {
         id = 0;
         made = false;
         if (recorded.GetLength() < 2 || recorded[0] != L'#')
            return false;

         int at = 1;
         while (at < recorded.GetLength() && recorded[at] >= L'0' && recorded[at] <= L'9' && id < 100000000000000LL)
         {
            id = id * 10 + (recorded[at] - L'0');
            at++;
         }
         if (at < recorded.GetLength() && recorded[at] == L'*')
         {
            made = true;
            at++;
         }
         if (id <= 0 || (at < recorded.GetLength() && recorded[at] != L' '))
         {
            id = 0;
            made = false;
            return false;
         }
         return true;
      }

      // A class and its sub-classes: "IPM.Contact" and "IPM.Contact.x", not
      // "IPM.Contacts" - the rule the reader's process takes items by.
      bool OfClass(const std::string &value, const char *base)
      {
         return PstItems::IsOfClass(value, base);
      }

      // The reader keeps a contact's picture out of a card larger than this.
      static_assert(MaxItemBytes == PstItems::MaximumItemTextBytes, "The reader's ceiling on a card is the address book's.");

      // A folder's path as the mail phase names it in the report, so that
      // both phases count one folder on one line.
      std::string DisplayPath(const std::vector<std::string> &names)
      {
         std::string path;
         for (size_t i = 0; i < names.size(); i++)
            path += (i == 0 ? std::string() : std::string(" / ")) + names[i];
         return ItemReportText(path, 1024);
      }

      // A folder's row carries, after its key, where the reader was: "/l.N"
      // while the folder's list of items was being read for the Nth time,
      // "/<nid>.N" while that item was - as the mail phase's rows do - and, after
      // either or alone, "/s<nid>.N" for each item held after N refusals of the
      // store, "/t<nid>.N" where it was its folder's book or calendar.
      void ParseItemMarker(const String &uidValidity, const String &key, bool &list, uint32_t &nid, int &attempt,
                           std::vector<HeldItem> &held)
      {
         list = false;
         nid = 0;
         attempt = 0;
         held.clear();

         if (uidValidity.GetLength() <= key.GetLength() + 1 || uidValidity.Left(key.GetLength()) != key ||
             uidValidity[key.GetLength()] != L'/')
            return;

         const String marks = uidValidity.Mid(key.GetLength() + 1);
         int from = 0;
         while (from < marks.GetLength())
         {
            int slash = marks.Find(_T("/"), from);
            if (slash < 0)
               slash = marks.GetLength();
            const String mark = marks.Mid(from, slash - from);
            from = slash + 1;

            const int dot = mark.Find(_T("."));
            if (dot <= 0)
               continue;

            const String what = mark.Left(dot);
            const int count = _ttoi(mark.Mid(dot + 1));
            if (what == _T("l"))
            {
               list = true;
               attempt = count;
            }
            else if (what.GetLength() > 1 && (what[0] == L's' || what[0] == L't'))
            {
               const String hex = what.Mid(1);
               HeldItem item;
               item.nid = (uint32_t) wcstoul(hex.c_str(), nullptr, 16);
               item.count = count;
               item.target = what[0] == L't';
               if (item.nid != 0 && held.size() < MaxHeldItems)
                  held.push_back(item);
            }
            else
            {
               nid = (uint32_t) wcstoul(what.c_str(), nullptr, 16);
               attempt = count;
            }
         }
      }

      // One contact into the book: nullptr when it is there now, else the
      // reason word; why says more where there is more, notes what came in
      // without something. target gives the book (make: made if it is not yet
      // there). not_saved is the store's failure, target_failed the book's,
      // folder_left_out a deleted folder with no book of its own - none of them
      // the contact's.
      const char *ImportContact(PstReaderProcess &store, uint32_t nid, __int64 accountId, const std::function<ItemTarget(bool, __int64 &)> &target,
                                const std::string &recordKey, std::string &why, std::vector<std::string> &notes)
      {
         // Read, and made its card - without its picture where the card with
         // it would be larger than a megabyte - in the reader's process.
         PstContactRead contact;
         store.ReadContact(nid, contact);

         if (contact.outcome == PstContactRead::DistributionList)
            return "distribution_list";
         if (contact.outcome != PstContactRead::Card)
            return "not_a_contact";

         for (const std::string &note : contact.notes)
            notes.push_back(note);

         const std::string uid = "pst-" + recordKey + "-" + PstFormat::Hex32(nid);
         std::string card;
         card.swap(contact.card);
         if (contact.photoLeftOut)
            notes.push_back("the contact's picture, since the card with it is larger than a megabyte");
         if (card.size() > MaxItemBytes)
            return "too_large";

         std::vector<VCardProperty> properties;
         AnsiString problem;
         if (!VCard::Parse(AnsiString(card), properties, problem))
         {
            why = std::string(problem.c_str());
            return "not_a_card";
         }

         AnsiString name;
         AnsiString address;
         if (!VCard::ExtractNameAndAddress(properties, name, address))
            return "no_address";

         String wideAddress = ItemWide(std::string(address.c_str()));
         wideAddress.Trim();
         wideAddress.MakeLower();
         if (!ContactStore::IsValidAddress(wideAddress))
            return "no_address";

         // The name the book keeps: at most its column's 255, cut at a character.
         String wideName = ItemWide(PstItems::CutToUnits(std::string(name.c_str()), (size_t) ContactStore::MaximumNameLength));
         wideName.Trim();

         // One address once in an account, across its books.
         __int64 existing = 0;
         if (ContactStore::FindByAddress(accountId, wideAddress, existing))
         {
            ContactRecord record;
            if (!ContactStore::Get(accountId, existing, record))
               return "not_saved";
            if (record.source != ContactStore::SourceCollected)
               return "already_there";

            // An address the account only collected from mail it sent, in the
            // book this contact goes into, is adopted as CardDAV adopts one
            // (CardDavServer.cpp): the row takes the PST's card, under its
            // name, and is kept from then on. One collected into another book
            // is left there, as CardDAV leaves it - asked without making the
            // book, since a book not made yet holds nothing collected.
            __int64 bookId = 0;
            const ItemTarget answer = target(false, bookId);
            if (answer == ItemTargetFailed)
               return "target_failed";
            if (answer == ItemTargetLeftOut)
               return "folder_left_out";
            if (answer == ItemTargetNotYet)
               return "already_there";
            if (record.bookId != bookId && !(record.bookId == 0 && AddressBookStore::IsDefault(accountId, bookId)))
               return "already_there";
            if (!ContactStore::UpdateCard(accountId, existing, wideName, wideAddress, ItemWide(uid), ItemWide(card)) ||
                !ContactStore::SetSource(accountId, existing, ContactStore::SourceManual))
               return "not_saved";
            return nullptr;
         }

         __int64 bookId = 0;
         const ItemTarget answer = target(true, bookId);
         if (answer == ItemTargetLeftOut)
            return "folder_left_out";
         if (answer != ItemTargetFound)
            return "target_failed";

         ContactRecord inserted;
         if (!ContactStore::InsertCard(accountId, bookId, wideName, wideAddress, String(), ItemWide(uid), ItemWide(card), inserted))
            return "not_saved";

         return nullptr;
      }

      // One appointment or task into the calendar, the same way. A deleted
      // folder's copy is looked for across every calendar of the account, in
      // one query, so that it never lands beside the live one.
      const char *ImportCalendarItem(PstReaderProcess &store, uint32_t nid, bool taskFolder, const std::vector<__int64> *accountCalendars,
                                     __int64 accountId, const std::function<ItemTarget(bool, __int64 &)> &target,
                                     std::string &why, std::vector<std::string> &notes)
      {
         // Read, and written as a VTODO or a VEVENT - under the file's budget,
         // its recurrence and its zones with it - in the reader's process, with
         // "pst-<record key>-<nid>" as the UID of one that has none.
         PstCalendarRead item;
         store.ReadCalendarItem(nid, taskFolder, item);
         if (item.outcome == PstCalendarRead::NotAnItem)
            return "not_an_item";

         for (const std::string &note : item.notes)
            notes.push_back(note);

         if (item.outcome == PstCalendarRead::Refused)
         {
            why = item.why;
            if (item.word == "no_start")
               return "no_start";
            if (item.word == "too_large")
               return "too_large";
            return "recurrence_unreadable";
         }

         std::string text;
         text.swap(item.text);

         if (text.size() > MaxItemBytes)
            return "too_large";

         // What the calendar store takes and nothing else, by the check CalDAV
         // and the webmail make.
         const AnsiString data(text);
         ICalComponent tree;
         AnsiString problem;
         ICalendar::Summary summary;
         if (!ICalendar::Parse(data, tree, problem) || !ICalendar::Summarize(tree, summary, problem))
         {
            why = std::string(problem.c_str());
            return "not_storable";
         }

         const String uid = ItemWide(std::string(summary.uid.c_str()));
         const bool deletedFolder = accountCalendars != nullptr;
         CalendarObjectRecord existing;
         if (deletedFolder)
         {
            // Found, not there, or the database could not say - the last the
            // store's failure, not a reason to store a second copy.
            bool failed = false;
            if (accountCalendars->empty())
               return "not_saved";
            if (CalendarStore::FindByUidAmong(accountId, *accountCalendars, uid, existing, failed))
               return "already_there";
            if (failed)
               return "not_saved";
         }

         __int64 calendarId = 0;
         const ItemTarget answer = target(true, calendarId);
         if (answer == ItemTargetLeftOut)
            return "folder_left_out";
         if (answer != ItemTargetFound)
            return "target_failed";

         // The calendar it goes into, for a deleted folder too: its own, made
         // in this run, is not among the calendars listed when it began.
         if (CalendarStore::FindByUid(accountId, calendarId, uid, existing))
            return "already_there";

         CalendarObjectRecord record;
         record.uid = uid;
         record.component = ItemWide(std::string(summary.component.c_str()));
         record.data = ItemWide(text);
         record.etag = Dav::Wide(Dav::EtagOf(data));
         record.start = summary.start;
         record.end = summary.end;
         record.first = summary.first;
         record.last = summary.last;
         record.uri = Imip::UriFor(summary.uid);

         CalendarObjectRecord inserted;
         __int64 token = 0;
         if (!CalendarStore::Insert(accountId, calendarId, record, inserted, token))
            return "not_saved";

         return nullptr;
      }
   }

   MigrationJob::Outcome
   MigrationJob::RunPstItems_(bool calendars, String &problem)
   {
      PhaseTally &tally = calendars ? report_.calendars : report_.contacts;
      const __int64 accountId = owner_->GetID();

      auto skip = [&](const AnsiString &reason) -> Outcome
      {
         tally = PhaseTally();
         tally.state = "skipped";
         tally.reason = reason;
         return OutcomeDone;
      };

      // ------------------------------------------------------------ the file

      // Found and opened as the mail phase found and opened it. The mail is
      // copied by now, so what has gone wrong since skips the phase with the
      // reason rather than failing the job; a file Outlook holds open waits.
      const String domain = StringParser::ExtractDomain(owner_->GetAddress());
      String file;
      String directory;
      String why;
      bool missing = false;
      if (!PstImportPath::Resolve(job_.sourceUser, domain, file, directory, why, missing))
         return skip(ItemUtf8(why));

      std::shared_ptr<PstFileSource> source = std::make_shared<PstFileSource>();
      const PstFileSource::OpenResult opened = source->Open(file);
      if (opened == PstFileSource::OpenInUse)
      {
         problem = _T("The file is open in Outlook or another program that does not share it; close it, and the job tries again.");
         return OutcomeRetry;
      }
      if (opened != PstFileSource::OpenDone)
         return skip("the file could not be opened again after its mail was copied");

      const String finalPath = PstImportPath::Comparable(source->FinalPath());
      if (!finalPath.IsEmpty() && !PstImportPath::IsInside(finalPath, directory))
         return skip("the file changed between the check and the read");

      // The reader, in a process of its own, as the mail phase's: the budget
      // counted there, and the stop ending it (Pst/PstReaderProcess.h).
      PstReaderProcess::TestHook hook;
      String hookSaid;
      if (PstReaderProcess::ReadTestHook(file, directory, hook, hookSaid))
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7553, "MigrationJob::RunPstItems_", Formatter::Format(_T("Migration job {0}: {1}"), job_id_, hookSaid));

      PstReaderProcess store(source, [this]() { return StopRequested_(); },
                             Formatter::Format(_T("Migration job {0} ({1})"), job_id_, PstImportPath::RelativeName(file, directory)),
                             [this]() { report_.pst.readerRestarts++; }, hook);

      const String spentSentence = _T("Reading this file has cost more work than any PST Outlook writes needs for its size: it is damaged, or built to exhaust the reader, and it is not read further. What the job copied stays; retry the job to go on from where it stopped, or repair the file with Outlook's Inbox Repair Tool (scanpst.exe).");

      std::vector<PstFolderInfo> folders;
      std::vector<std::string> unread;
      uint32_t defaultContacts = 0;
      uint32_t defaultCalendar = 0;
      uint32_t defaultTasks = 0;
      try
      {
         store.Open(std::string(ItemUtf8(job_.sourceSecret).c_str()));
         store.ListFolders(folders, unread);
         store.ReadItemFolders(defaultContacts, defaultCalendar, defaultTasks);
      }
      catch (PstStopped &)
      {
         Save_();
         return OutcomeStopped;
      }
      catch (PstError &error)
      {
         if (store.FileExhausted())
         {
            problem = spentSentence;
            return OutcomeFailed;
         }
         return skip(AnsiString("the file could not be read: ") + AnsiString(ItemReportText(error.what(), 500)));
      }
      catch (std::bad_alloc &)
      {
         problem = _T("The server ran short of memory reading the file.");
         return OutcomeRetry;
      }
      catch (PstReaderUnavailable &unavailable)
      {
         problem = ItemWide(unavailable.what());
         return OutcomeRetry;
      }
      catch (std::exception &error)
      {
         // A PstReaderFailed among them: the reader ended twice on the store,
         // its folders or its default folders.
         return skip(AnsiString("the file could not be read: ") + AnsiString(ItemReportText(error.what(), 500)));
      }

      const std::string recordKey = PstFormat::Hex(store.GetRecordKey());

      // ------------------------------------------------------------ the plan

      struct Planned
      {
         Planned() : nid(0), taskFolder(false), isDefault(false), deleted(false), target(0), checked(false), made(false),
                     fallBack(false), leftOut(false) { }

         uint32_t nid;
         bool taskFolder;
         bool isDefault;           // a default folder: into the account's own book or calendar
         bool deleted;             // under Deleted Items
         std::string name;         // the folder's own name, UTF-8
         AnsiString source;
         AnsiString targetText;
         __int64 target;           // the address book's or the calendar's id, once decided
         bool checked;             // the target the row keeps found there in this run
         bool made;                // a calendar this job made
         bool fallBack;            // its own could not be made, its attempts spent: none is made
         bool leftOut;             // a deleted folder with none of its own
         String key;
         MigrationFolderRecord row;
      };

      // Which folders (the header, WHICH FOLDERS): this phase's kinds, not
      // hidden, not a contacts folder Exchange keeps for itself; those under
      // Deleted Items apart, to come last.
      std::map<uint32_t, const PstFolderInfo *> byNid;
      for (const PstFolderInfo &folder : folders)
         byNid[folder.nid] = &folder;

      auto underDeletedItems = [&](const PstFolderInfo &folder) -> bool
      {
         const PstFolderInfo *at = &folder;
         for (int depth = 0; at != nullptr && depth <= PstStore::MaximumFolderDepth + 1; depth++)
         {
            if (at->role == PstFolderInfo::RoleTrash)
               return true;
            std::map<uint32_t, const PstFolderInfo *>::const_iterator parent = byNid.find(at->parentNid);
            at = parent == byNid.end() ? nullptr : parent->second;
         }
         return false;
      };

      std::vector<const PstFolderInfo *> live;
      std::vector<const PstFolderInfo *> deleted;
      int leftOutFolders = 0;
      for (const PstFolderInfo &folder : folders)
      {
         const bool calendarFolder = OfClass(folder.containerClass, "IPF.Appointment");
         const bool taskFolder = OfClass(folder.containerClass, "IPF.Task");
         const bool contactFolder = OfClass(folder.containerClass, "IPF.Contact");
         if (!(calendars ? (calendarFolder || taskFolder) : contactFolder))
            continue;

         const AnsiString sourceName = AnsiString(DisplayPath(folder.path));
         if (folder.hidden)
         {
            Tally_(sourceName).note = "a folder Outlook keeps hidden: not imported";
            leftOutFolders++;
            continue;
         }
         if (contactFolder && PstStore::IsSystemContactsClass(folder.containerClass))
         {
            Tally_(sourceName).note = AnsiString("a contacts folder Exchange or Outlook keeps for itself (") +
                                  AnsiString(ItemReportText(folder.containerClass, 100)) + "): not imported";
            leftOutFolders++;
            continue;
         }

         if (underDeletedItems(folder))
            deleted.push_back(&folder);
         else
            live.push_back(&folder);
      }

      // The default folders: those the root folder names, where they are
      // among these; else the first of each kind outside Deleted Items.
      auto among = [&](uint32_t nid) -> bool
      {
         for (const PstFolderInfo *folder : live)
         {
            if (folder->nid == nid)
               return true;
         }
         return false;
      };
      if (!among(defaultContacts))
         defaultContacts = 0;
      if (!among(defaultCalendar))
         defaultCalendar = 0;
      if (!among(defaultTasks))
         defaultTasks = 0;
      for (const PstFolderInfo *folder : live)
      {
         if (defaultContacts == 0 && OfClass(folder->containerClass, "IPF.Contact"))
            defaultContacts = folder->nid;
         if (defaultCalendar == 0 && OfClass(folder->containerClass, "IPF.Appointment"))
            defaultCalendar = folder->nid;
         if (defaultTasks == 0 && OfClass(folder->containerClass, "IPF.Task"))
            defaultTasks = folder->nid;
      }

      auto isDefault = [&](uint32_t nid) -> bool
      {
         return calendars ? (nid == defaultCalendar || nid == defaultTasks) : nid == defaultContacts;
      };

      std::vector<const PstFolderInfo *> chosen;
      for (const PstFolderInfo *folder : live)
      {
         if (isDefault(folder->nid))
            chosen.push_back(folder);
      }
      for (const PstFolderInfo *folder : live)
      {
         if (!isDefault(folder->nid))
            chosen.push_back(folder);
      }
      const size_t liveCount = chosen.size();
      chosen.insert(chosen.end(), deleted.begin(), deleted.end());

      if (chosen.empty())
      {
         if (leftOutFolders > 0)
            return skip(calendars ? "every calendar and tasks folder of the file was left out: hidden"
                                  : "every contacts folder of the file was left out: hidden, or kept by Exchange or Outlook for itself");
         return skip(calendars ? "the file has no calendar or tasks folder" : "the file has no contacts folder");
      }

      std::vector<MigrationFolderRecord> rows;
      if (!MigrationStore::ListFolders(job_id_, rows))
      {
         problem = _T("The job's resume state could not be read.");
         return OutcomeRetry;
      }

      // The rows by their key - what a row's mark says before its "/" - so
      // that a hundred thousand folders are not each looked for among as many
      // rows.
      std::map<String, size_t> rowAt;
      for (size_t i = 0; i < rows.size(); i++)
      {
         const int slash = rows[i].uidValidity.Find(_T("/"));
         rowAt.insert(std::make_pair(slash >= 0 ? rows[i].uidValidity.Left(slash) : rows[i].uidValidity, i));
      }

      std::vector<Planned> plan;
      bool resuming = false;
      int madeCalendars = 0;
      for (size_t at = 0; at < chosen.size(); at++)
      {
         const PstFolderInfo *folder = chosen[at];

         // A row for each folder, written here: a file of a hundred thousand
         // folders is as many rows, and the stop is heard between two.
         if (StopRequested_())
         {
            Save_();
            return OutcomeStopped;
         }

         Planned planned;
         planned.nid = folder->nid;
         planned.taskFolder = OfClass(folder->containerClass, "IPF.Task");
         planned.isDefault = isDefault(folder->nid);
         planned.deleted = at >= liveCount;
         planned.name = folder->path.empty() ? std::string() : folder->path.back();
         planned.source = AnsiString(DisplayPath(folder->path));
         // The store's record key, its first eight digits (the job has one
         // store), the folder's NID and the phase: 19 characters, leaving the
         // row's mark room for the guard and two held items.
         planned.key = ItemWide(recordKey.substr(0, 8) + ":" + PstFormat::Hex32(folder->nid) + (calendars ? ":e" : ":c"));

         std::map<String, size_t>::const_iterator existing = rowAt.find(planned.key);
         if (existing != rowAt.end())
         {
            planned.row = rows[existing->second];
            resuming = true;

            // Where an earlier run put its items, kept; its name is read
            // back when the first item goes there.
            if (ParseTarget(planned.row.targetFolder, planned.target, planned.made) && planned.made)
               madeCalendars++;
         }
         else
         {
            planned.row.jobId = job_id_;
            // Shown, never looked up (rows are found by their key): a
            // character a three-byte MySQL column cannot hold is U+FFFD.
            planned.row.folder = ItemWide(PstFormat::BmpOnly(std::string(planned.source.c_str())));
            planned.row.uidValidity = planned.key;
            if (!MigrationStore::SaveFolder(planned.row))
            {
               problem = _T("The job's resume state could not be written.");
               return OutcomeRetry;
            }
         }

         plan.push_back(planned);
      }

      // Where a folder's items go (the header, WHERE THEY GO): decided the
      // first time one of them is stored, and kept in its row. make false asks
      // without making anything.
      auto resolve = [&](Planned &planned, bool make) -> ItemTarget
      {
         if (planned.leftOut)
            return ItemTargetLeftOut;
         if (planned.target != 0 && planned.checked)
            return ItemTargetFound;

         // Kept by an earlier run: used while it is there, its name read back.
         // A book or calendar deleted since is decided again, and so is a
         // deleted folder's default, which a row of an earlier version may keep.
         if (planned.target != 0)
         {
            AddressBookRecord book;
            CalendarRecord calendar;
            bool keptDefault = false;
            if (planned.deleted)
            {
               CalendarRecord defaultCalendar;
               keptDefault = calendars ? (CalendarStore::EnsureDefault(accountId, defaultCalendar) && defaultCalendar.id == planned.target)
                                       : AddressBookStore::IsDefault(accountId, planned.target);
            }
            if (!keptDefault &&
                (calendars ? CalendarStore::Get(accountId, planned.target, calendar) : AddressBookStore::Get(accountId, planned.target, book)))
            {
               planned.checked = true;
               planned.targetText = calendars ? AnsiString("calendar: " + ItemUtf8(calendar.displayName))
                                              : AnsiString("address book: " + ItemUtf8(book.name));
               Tally_(planned.source).target = planned.targetText;
               return ItemTargetFound;
            }
         }

         __int64 id = 0;
         __int64 defaultId = 0;
         bool made = false;
         AnsiString text;
         AnsiString note;
         if (!calendars)
         {
            AddressBookRecord book;
            if (!AddressBookStore::EnsureDefault(accountId, book))
               return ItemTargetFailed;
            id = defaultId = book.id;
            text = "address book: " + ItemUtf8(book.name);

            if (!planned.isDefault)
            {
               const String name = TargetName(planned.name);
               AddressBookRecord own;
               if (!AddressBookStore::IsUsableName(name))
                  note = "its name cannot be an address book's";
               else if (AddressBookStore::FindByName(accountId, name, own))
               {
                  id = own.id;
                  text = "address book: " + ItemUtf8(own.name);
                  if (id == defaultId)
                     note = "its name is the default address book's";
               }
               else if (planned.fallBack)
                  note = "its own address book could not be made";
               else if (!make)
               {
                  // Not found: not there, or the database could not say.
                  std::vector<AddressBookRecord> books;
                  if (!AddressBookStore::ListBooks(accountId, books))
                     return ItemTargetFailed;
                  return ItemTargetNotYet;
               }
               else if (AddressBookStore::CreateBook(accountId, name, own))
               {
                  id = own.id;
                  text = "address book: " + ItemUtf8(own.name);
               }
               else
               {
                  // Not made: the ceiling of books, or the database.
                  std::vector<AddressBookRecord> books;
                  if (!AddressBookStore::ListBooks(accountId, books))
                     return ItemTargetFailed;
                  if (AddressBookStore::FindByName(accountId, name, own))
                  {
                     id = own.id;
                     text = "address book: " + ItemUtf8(own.name);
                  }
                  else if ((int) books.size() >= AddressBookStore::MaximumBooks)
                     note = "the account has as many address books as it may have";
                  else
                     return ItemTargetFailed;
               }
            }
         }
         else
         {
            CalendarRecord calendar;
            if (!CalendarStore::EnsureDefault(accountId, calendar))
               return ItemTargetFailed;
            id = defaultId = calendar.id;
            text = "calendar: " + ItemUtf8(calendar.displayName);

            if (!planned.isDefault)
            {
               // The segment is this file's and this folder's: a calendar under
               // it is one this import made.
               const String segment = ItemWide("pst-" + recordKey.substr(0, 8) + "-" + PstFormat::Hex32(planned.nid));
               CalendarRecord own;
               if (CalendarStore::FindByName(accountId, segment, own))
               {
                  id = own.id;
                  text = "calendar: " + ItemUtf8(own.displayName);
                  made = true;
                  if (!planned.made)
                     madeCalendars++;
               }
               else if (planned.fallBack)
                  note = "its own calendar could not be made";
               else if (madeCalendars >= MaximumCalendarsPerJob)
                  note = "the job has made as many calendars as one job may";
               else if (!make)
                  return ItemTargetNotYet;
               else
               {
                  const String display = TargetName(planned.name);
                  if (!CalendarStore::CreateCalendar(accountId, segment, display.IsEmpty() ? segment : display, String(), own))
                     return ItemTargetFailed;
                  id = own.id;
                  text = "calendar: " + ItemUtf8(own.displayName);
                  made = true;
                  madeCalendars++;
               }
            }
         }

         const String folderName = ItemWide(std::string(planned.source.c_str()));

         // A folder under Deleted Items never goes into the default: one that
         // cannot have its own is left out, and says why.
         if (planned.deleted && id == defaultId)
         {
            planned.leftOut = true;
            const AnsiString why = note.IsEmpty() ? AnsiString("it has no book or calendar of its own") : note;
            Tally_(planned.source).note = AnsiString("a folder under Deleted Items with no ") +
                                          (calendars ? "calendar" : "address book") + " of its own (" + why + "): not imported";
            Record_(Formatter::Format(_T("the folder {0} was not imported: it is under Deleted Items, and {1}, so it would have gone into the default {2}"),
               folderName, ItemWide(std::string(why.c_str())), String(calendars ? _T("calendar") : _T("address book"))));
            Save_();
            return ItemTargetLeftOut;
         }

         planned.target = id;
         planned.checked = true;
         planned.made = made;
         planned.targetText = text;
         planned.row.targetFolder = Formatter::Format(_T("#{0}{1}"), id, String(made ? _T("*") : _T("")));
         if (!MigrationStore::SaveFolder(planned.row))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7410, "MigrationJob::RunPstItems_",
               Formatter::Format(_T("Migration job {0}: where the PST folder {1} goes could not be written to its resume state. The job goes on; a later run decides it again, by the same rule."),
                  job_id_, folderName));
         }

         Tally_(planned.source).target = text;
         if (!note.IsEmpty())
         {
            Record_(Formatter::Format(_T("the folder {0}: {1}, so its items went into the default {2}"), folderName,
               ItemWide(std::string(note.c_str())), String(calendars ? _T("calendar") : _T("address book"))));
            Save_();
         }
         return ItemTargetFound;
      };

      // A phase begun before keeps what it counted; a new one starts afresh.
      if (!resuming)
         tally = PhaseTally();
      tally.state = "pending";
      tally.reason.Empty();

      // ------------------------------------------------------------ the items

      const char *const kind = calendars ? "item" : "contact";
      int sinceMark = 0;
      int recorded = 0;
      bool givenUpReported = false;

      // A name-to-id map that could not be read turns every contact into
      // no_address and every event into no_start: said once, here, since
      // those items are counted rather than named.
      bool namesSaid = false;
      for (const ErrorEntry &entry : report_.errors)
         namesSaid = namesSaid || entry.error.Find("list of named properties could not be read") >= 0;
      if (!namesSaid && !store.NamesProblem().empty())
      {
         Record_(Formatter::Format(_T("the file's list of named properties could not be read ({0}), so its {1} are read without what it names - their e-mail addresses, their times - and counted no_address or no_start"),
            ItemWide(ItemReportText(store.NamesProblem(), 300)), String(calendars ? _T("calendar items and tasks") : _T("contacts"))));
      }

      auto storeFailed = [](const char *reason) -> bool
      {
         return reason && (strcmp(reason, "not_saved") == 0 || strcmp(reason, "target_failed") == 0);
      };

      // The whole file's budget spent: the mark back where the last item
      // left it, and the job fails with what it copied kept.
      auto spent = [&](MigrationFolderRecord &row, const String &key) -> Outcome
      {
         row.uidValidity = key;
         MigrationStore::SaveFolder(row);
         Save_();
         problem = spentSentence;
         return OutcomeFailed;
      };

      // A stop between two reads: the same, and the phase ends there.
      auto stopped = [&](MigrationFolderRecord &row, const String &key) -> Outcome
      {
         row.uidValidity = key;
         MigrationStore::SaveFolder(row);
         Save_();
         return OutcomeStopped;
      };

      for (Planned &planned : plan)
      {
         MigrationFolderRecord &row = planned.row;
         if (row.done)
            continue;

         FolderTally &folderTally = Tally_(planned.source);
         if (!planned.targetText.IsEmpty())
            folderTally.target = planned.targetText;
         folderTally.note = calendars ? AnsiString("calendar items and tasks: imported by the calendars phase")
                                      : AnsiString("contacts: imported by the contacts phase");
         const String folderName = ItemWide(std::string(planned.source.c_str()));

         bool markedList = false;
         uint32_t markedNid = 0;
         int markedAttempt = 0;
         std::vector<HeldItem> held;
         ParseItemMarker(row.uidValidity, planned.key, markedList, markedNid, markedAttempt, held);

         // The row's mark: the key, and every item held with its count.
         auto heldText = [&]() -> String
         {
            String out;
            for (const HeldItem &item : held)
               out += String(item.target ? _T("/t") : _T("/s")) + ItemWide(PstFormat::Hex32(item.nid)) + Formatter::Format(_T(".{0}"), item.count);
            return out;
         };
         auto rest = [&]() -> String
         {
            return planned.key + heldText();
         };

         // Held items a folder that is not read cannot settle: counted not_saved.
         auto dropHeld = [&]()
         {
            tally.skipped["not_saved"] += (int) held.size();
            folderTally.skipped["not_saved"] += (int) held.size();
            held.clear();
         };

         // The folder's list of items, guarded as the mail phase guards one.
         const int listAttempt = markedList ? markedAttempt + 1 : 1;
         if (listAttempt > 2)
         {
            folderTally.note = "the reader stopped twice while reading this folder's list of items; the folder was not copied";
            Record_(Formatter::Format(_T("the folder {0} was not copied: the reader stopped twice while reading its list of items"), folderName));
            dropHeld();
            row.done = true;
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            Save_();
            continue;
         }

         row.uidValidity = planned.key + Formatter::Format(_T("/l.{0}"), listAttempt) + heldText();
         MigrationStore::SaveFolder(row);

         std::vector<uint32_t> items;
         std::string listProblem;
         try
         {
            store.ListMessages(planned.nid, items);
         }
         catch (PstStopped &)
         {
            return stopped(row, rest());
         }
         catch (PstError &error)
         {
            if (store.FileExhausted())
               return spent(row, rest());
            listProblem = error.what();
         }
         catch (std::bad_alloc &)
         {
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);
            problem = _T("The server ran short of memory reading a folder's list of items.");
            return OutcomeRetry;
         }
         catch (PstReaderFailed &)
         {
            listProblem = "the PST reader's process ended twice while reading it";
         }
         catch (PstReaderUnavailable &unavailable)
         {
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);
            Save_();
            problem = ItemWide(unavailable.what());
            return OutcomeRetry;
         }
         catch (std::exception &error)
         {
            listProblem = error.what();
         }

         if (!listProblem.empty())
         {
            const std::string said = ItemReportText(listProblem, 500);
            folderTally.note = AnsiString("the folder's list of items could not be read: ") + AnsiString(said);
            Record_(Formatter::Format(_T("the folder {0} was not copied: its list of items could not be read: {1}"), folderName, ItemWide(said)));
            dropHeld();
            row.done = true;
            row.uidValidity = planned.key;
            MigrationStore::SaveFolder(row);
            Save_();
            continue;
         }

         row.uidValidity = rest();

         // A deleted folder's events are looked for across the account's own
         // calendars, listed once here.
         std::vector<__int64> accountCalendars;
         if (calendars && planned.deleted)
         {
            std::vector<CalendarRecord> own;
            if (CalendarStore::ListCalendars(accountId, own))
            {
               for (const CalendarRecord &calendar : own)
                  accountCalendars.push_back(calendar.id);
            }
         }

         // One item read and stored, as far as it goes: the reason word, or
         // nullptr when it is stored. ended says the phase ends here - a stop,
         // the file's budget spent, memory short - with outcome to return.
         auto importItem = [&](uint32_t itemNid, std::string &whyNot, std::vector<std::string> &notes, bool &ended, Outcome &outcome) -> const char *
         {
            whyNot.clear();
            notes.clear();
            auto target = [&](bool make, __int64 &id) -> ItemTarget
            {
               const ItemTarget answer = resolve(planned, make);
               id = answer == ItemTargetFound ? planned.target : 0;
               return answer;
            };
            try
            {
               return calendars ? ImportCalendarItem(store, itemNid, planned.taskFolder, planned.deleted ? &accountCalendars : nullptr, accountId,
                                                     target, whyNot, notes)
                                : ImportContact(store, itemNid, accountId, target, recordKey, whyNot, notes);
            }
            catch (PstStopped &)
            {
               ended = true;
               outcome = stopped(row, rest());
            }
            catch (PstTooLarge &error)
            {
               if (store.FileExhausted())
               {
                  ended = true;
                  outcome = spent(row, rest());
                  return nullptr;
               }
               whyNot = error.what();
               return "too_large";
            }
            catch (PstError &error)
            {
               if (store.FileExhausted())
               {
                  ended = true;
                  outcome = spent(row, rest());
                  return nullptr;
               }
               whyNot = error.what();
               return "unreadable";
            }
            catch (PstReaderFailed &error)
            {
               // The reader ended on it twice: that item's failure, and a new
               // reader for the next - unless the file ends it on everything.
               whyNot = error.what();
               if (store.FailuresInARow() >= PstReaderProcess::MaximumFailuresInARow)
               {
                  row.uidValidity = rest();
                  MigrationStore::SaveFolder(row);
                  Save_();
                  problem = Formatter::Format(_T("The PST reader's process ended on {0} items in a row, each read twice, and the file is not read further: it is damaged throughout, or built to end the reader. What the job copied stays; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and retry the job to go on from where it stopped."),
                     PstReaderProcess::MaximumFailuresInARow);
                  ended = true;
                  outcome = OutcomeFailed;
                  return nullptr;
               }
               return "reader_failed";
            }
            catch (PstReaderUnavailable &unavailable)
            {
               row.uidValidity = rest();
               MigrationStore::SaveFolder(row);
               Save_();
               problem = ItemWide(unavailable.what());
               ended = true;
               outcome = OutcomeRetry;
            }
            catch (std::bad_alloc &)
            {
               // The server's, not the item's: the mark stays before it, and
               // the job reads it again when it tries again.
               row.uidValidity = rest();
               MigrationStore::SaveFolder(row);
               Save_();
               problem = Formatter::Format(_T("The server ran short of memory reading the {0} {1} of {2}; the job goes on from it."),
                  String(AnsiString(kind)), ItemNodeName(itemNid), folderName);
               ended = true;
               outcome = OutcomeRetry;
            }
            catch (std::exception &error)
            {
               whyNot = error.what();
               return "unreadable";
            }
            return nullptr;
         };

         // Every read under the reader's guard: the attempt it is at, from the
         // row's mark, written into the row before the read; past two, the
         // item is left out unread (reader_stopped), as one that stopped the
         // service twice - after the mark or held, wherever it is read.
         uint32_t guardedNid = 0;
         int guardedAttempt = 0;
         auto guardedRead = [&](uint32_t itemNid, std::string &whyNot, std::vector<std::string> &notes, bool &ended, Outcome &outcome) -> const char *
         {
            // A second read of one item in a run - a held item tried at the
            // folder's end, then in the default - keeps the attempt the first
            // wrote, so a crash only the second reaches still counts.
            int attempt = 1;
            if (!markedList && itemNid == markedNid)
            {
               attempt = markedAttempt + 1;
               markedNid = 0;
            }
            else if (itemNid == guardedNid)
               attempt = guardedAttempt;
            guardedNid = itemNid;
            guardedAttempt = attempt;
            if (attempt > 2)
            {
               whyNot = "the reader stopped twice while reading it, and it is left out so that it cannot stop the job again";
               notes.clear();
               return "reader_stopped";
            }

            row.uidValidity = planned.key + _T("/") + ItemWide(PstFormat::Hex32(itemNid)) + Formatter::Format(_T(".{0}"), attempt) + heldText();
            if (!MigrationStore::SaveFolder(row))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7410, "MigrationJob::RunPstItems_",
                  Formatter::Format(_T("Migration job {0}: the resume mark of the PST folder {1} could not be written. The job goes on; an interruption before the next mark reads that folder's items again, and none is stored twice."),
                     job_id_, folderName));
            }
            return importItem(itemNid, whyNot, notes, ended, outcome);
         };

         // An item dealt with: counted, named where there is more to say, and -
         // for one after the mark - the mark past it.
         auto countItem = [&](uint32_t itemNid, const char *reason, const std::string &whyNot, const std::vector<std::string> &notes, bool advance)
         {
            if (reason)
            {
               tally.skipped[reason]++;
               folderTally.skipped[reason]++;

               if (!whyNot.empty() && recorded < MaxRecordedItems)
               {
                  recorded++;
                  Record_(Formatter::Format(_T("the {0} {1} in {2} was not imported: {3}"), String(AnsiString(kind)), ItemNodeName(itemNid), folderName,
                     ItemWide(ItemReportText(whyNot, 500))));
               }
            }
            else
            {
               tally.copied++;
               folderTally.copied++;

               for (const std::string &note : notes)
               {
                  if (recorded >= MaxRecordedItems)
                     break;
                  recorded++;
                  Record_(Formatter::Format(_T("the {0} {1} in {2} was imported without something: {3}"), String(AnsiString(kind)),
                     ItemNodeName(itemNid), folderName, ItemWide(ItemReportText(note, 500))));
               }

               if (!progressed_)
               {
                  progressed_ = true;
                  job_.attempts = 0;
               }
            }

            if (advance)
               row.lastUid = itemNid;
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);

            if (!advance || (reason && strcmp(reason, "reader_stopped") == 0) || ++sinceMark >= ItemsMarkEvery)
            {
               sinceMark = 0;
               Save_();
            }
         };

         // A held item given up as the store's refusal for good: counted
         // not_saved, named, and logged once a run.
         auto giveUp = [&](uint32_t itemNid, const std::string &why)
         {
            if (!givenUpReported)
            {
               givenUpReported = true;
               const String what = String(AnsiString(kind)) + _T(" ") + ItemNodeName(itemNid);
               const String where = owner_->GetAddress() + (calendars ? _T("'s calendar") : _T("'s address book"));
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7411, "MigrationJob::RunPstItems_",
                  Formatter::Format(_T("Migration job {0}: the {1} of the PST folder {2} could not be saved into {3}: {4}. It is counted not_saved in the job's report, and the job goes on; any other such item of this run is in the report only."),
                     job_id_, what, folderName, where, ItemWide(why)));
            }
            countItem(itemNid, "not_saved", why, std::vector<std::string>(), false);
         };

         // A folder under Deleted Items with no book or calendar of its own:
         // what is held, and every item from here on, counted and not read.
         bool folderLeftOut = false;
         auto leaveOut = [&](size_t from)
         {
            int unread = (int) held.size();
            for (size_t i = from; i < items.size(); i++)
            {
               if ((__int64) items[i] > row.lastUid)
                  unread++;
            }
            tally.skipped["folder_left_out"] += unread;
            folderTally.skipped["folder_left_out"] += unread;
            held.clear();
            folderLeftOut = true;
         };

         // The job pauses with the held items: the mark before any item not
         // yet dealt with, and their counts in the row.
         auto pauseHeld = [&](Outcome &outcome) -> bool
         {
            const uint32_t first = held.empty() ? 0 : held.front().nid;
            const String what = String(AnsiString(kind)) + _T(" ") + ItemNodeName(first);
            const String where = owner_->GetAddress() + (calendars ? _T("'s calendar") : _T("'s address book"));
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7411, "MigrationJob::RunPstItems_",
               Formatter::Format(_T("Migration job {0}: the {1} of the PST folder {2}, and what else is held with it, could not be saved into {3}, and nothing after it was stored (attempt {4} in a row). The job pauses and tries again."),
                  job_id_, what, folderName, where, held.empty() ? 1 : held.front().count));

            problem = Formatter::Format(_T("The {0} of {1} could not be saved into {2}. The job goes on from it once that is mended."),
               what, folderName, where);
            Save_();
            outcome = OutcomeRetry;
            return false;
         };

         // A held item at MaxItemSaveAttempts, decided by its count: given up;
         // or, where it was its folder's own book or calendar that was
         // refused, the folder goes where a folder with none of its own goes -
         // the default, or left out under Deleted Items - and the item is tried
         // there, once, under the guard.
         // A held item leaves the mark only as its outcome is written: taken
         // out of held just before countItem, whose one save of the row both
         // drops it from the mark and moves nothing else - and whose report is
         // then saved at once (a held item's count is too rare to wait for).
         auto release = [&](uint32_t itemNid)
         {
            for (size_t i = 0; i < held.size(); i++)
            {
               if (held[i].nid == itemNid)
               {
                  held.erase(held.begin() + (std::ptrdiff_t) i);
                  return;
               }
            }
         };

         // A held item at MaxItemSaveAttempts, decided by its count: given up;
         // or, where it was its folder's own book or calendar that was
         // refused, the folder goes where a folder with none of its own goes -
         // the default, or left out under Deleted Items - and the item is tried
         // there, once, under the guard. It stays held, in the row's mark, until
         // then.
         auto decide = [&](const HeldItem &item, Outcome &outcome) -> bool
         {
            if (!item.target || planned.isDefault)
            {
               release(item.nid);
               giveUp(item.nid, refusedWordOf(calendars) + " in " + std::to_string(MaxItemSaveAttempts) + " runs");
               return true;
            }

            planned.fallBack = true;
            std::string whyNot;
            std::vector<std::string> notes;
            bool ended = false;
            const char *reason = guardedRead(item.nid, whyNot, notes, ended, outcome);
            if (ended)
               return false;

            if (reason && strcmp(reason, "folder_left_out") == 0)
            {
               // Still held: counted with every other held item.
               leaveOut(items.size());
               return true;
            }
            release(item.nid);
            if (reason && (strcmp(reason, "not_saved") == 0 || strcmp(reason, "target_failed") == 0))
               giveUp(item.nid, "its folder's own " + std::string(calendars ? "calendar" : "address book") + " could not be made, and the default refused it too");
            else
               countItem(item.nid, reason, whyNot, notes, false);
            return true;
         };

         // A run's turn for the held items: each count up one, in the mark,
         // and those at MaxItemSaveAttempts decided one by one - each still
         // held until its own outcome is written.
         auto ageHeld = [&](Outcome &outcome) -> bool
         {
            for (HeldItem &item : held)
               item.count++;
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);

            std::vector<HeldItem> capped;
            for (const HeldItem &item : held)
            {
               if (item.count >= MaxItemSaveAttempts)
                  capped.push_back(item);
            }

            for (const HeldItem &item : capped)
            {
               // A folder left out has counted every item still held.
               if (folderLeftOut)
                  break;
               if (!decide(item, outcome))
                  return false;
            }
            return true;
         };

         // The held items tried once more, each under the guard, kept in the
         // row's mark until it is decided. One stored - or proof, an item after
         // them just stored - shows the store works, and each still refused is
         // its own refusal, given up. None stored: the counts decide, and the
         // job pauses for those left.
         auto settleHeld = [&](bool proof, Outcome &outcome) -> bool
         {
            bool stored = proof;
            std::vector<uint32_t> order;
            for (const HeldItem &item : held)
               order.push_back(item.nid);

            for (uint32_t heldNid : order)
            {
               std::string whyNot;
               std::vector<std::string> notes;
               bool ended = false;
               const char *reason = guardedRead(heldNid, whyNot, notes, ended, outcome);
               if (ended)
                  return false;

               if (reason && strcmp(reason, "folder_left_out") == 0)
               {
                  leaveOut(items.size());
                  return true;
               }
               if (reason && (strcmp(reason, "not_saved") == 0 || strcmp(reason, "target_failed") == 0))
                  continue;

               release(heldNid);
               countItem(heldNid, reason, whyNot, notes, false);
               if (!reason)
                  stored = true;
            }

            if (stored)
            {
               // Each still held is its own refusal, released only as it is
               // given up.
               const std::vector<HeldItem> own = held;
               for (const HeldItem &item : own)
               {
                  release(item.nid);
                  giveUp(item.nid, refusedWordOf(calendars) + " twice, and stored an item after it");
               }
               return true;
            }

            if (!ageHeld(outcome))
               return false;
            if (!held.empty() && !folderLeftOut)
               return pauseHeld(outcome);
            return true;
         };

         for (size_t index = 0; index < items.size() && !folderLeftOut; index++)
         {
            const uint32_t nid = items[index];
            if ((__int64) nid <= row.lastUid)
               continue;

            if (StopRequested_())
            {
               row.uidValidity = rest();
               MigrationStore::SaveFolder(row);
               Save_();
               return OutcomeStopped;
            }

            std::string whyNot;
            std::vector<std::string> notes;
            bool ended = false;
            Outcome outcome = OutcomeDone;
            const char *reason = guardedRead(nid, whyNot, notes, ended, outcome);
            if (ended)
               return outcome;

            if (reason && strcmp(reason, "folder_left_out") == 0)
            {
               leaveOut(index);
               break;
            }

            if (!storeFailed(reason))
            {
               countItem(nid, reason, whyNot, notes, true);
               if (!reason && !held.empty())
               {
                  if (!settleHeld(true, outcome))
                     return outcome;
                  if (folderLeftOut)
                     leaveOut(index + 1);
               }
               continue;
            }

            // Refused: held, the mark past it. With no room - MaxHeldItems held
            // and none stored since - the held items' counts go up first; if
            // that makes no room, the job pauses with the mark before this one.
            if (held.size() >= MaxHeldItems)
            {
               if (!ageHeld(outcome))
                  return outcome;
               if (folderLeftOut)
               {
                  leaveOut(index);
                  break;
               }
               if (held.size() >= MaxHeldItems)
               {
                  pauseHeld(outcome);
                  return outcome;
               }
            }

            // Its count starts at nought: the turn this run gives the held
            // items makes it one, so the fourth run decides it - three pauses.
            HeldItem item;
            item.nid = nid;
            item.count = 0;
            item.target = strcmp(reason, "target_failed") == 0;
            held.push_back(item);
            row.lastUid = nid;
            row.uidValidity = rest();
            MigrationStore::SaveFolder(row);
         }

         // The folder's end: what is held is tried once more.
         if (!folderLeftOut && !held.empty())
         {
            Outcome outcome = OutcomeDone;
            if (!settleHeld(false, outcome))
               return outcome;
         }

         row.done = true;
         row.uidValidity = planned.key;
         MigrationStore::SaveFolder(row);

         // The report is written every ItemsMarkEvery items and folders, not
         // after each folder: it names every folder, so writing it after each
         // of a hundred thousand would write it that many times over. A folder
         // left out is written at once, after its row.
         if (folderLeftOut || ++sinceMark >= ItemsMarkEvery)
         {
            sinceMark = 0;
            Save_();
         }
      }

      tally.state = "copied";
      Save_();
      return OutcomeDone;
   }
}
