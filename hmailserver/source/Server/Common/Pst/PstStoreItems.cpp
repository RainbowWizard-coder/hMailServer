// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The messaging layer's reading of what is not mail (PstStore.h, PstItems.h):
// contacts, calendar items, tasks, notes and journal entries.
//
// Most of what these items carry is in NAMED properties - the e-mail addresses
// of a contact, the start of an appointment and its recurrence, the due date of
// a task - whose property ids differ from file to file and are found through the
// store's name-to-id map ([MS-PST] 2.4.7): each NAMEID entry is dwPropertyID (for
// a numeric name, its LID), a word whose low bit says it is named by a string
// and whose other fifteen bits are the GUID's index (1 PS_MAPI, 2
// PS_PUBLIC_STRINGS, 3 onwards the GUID stream's entries), and wPropIdx, the
// property id less 0x8000. The map is read once, the first time a name is asked
// for, and only the property sets these items use are kept - at most 65,536
// names, whatever the file claims.
//
// WHAT ONE ITEM MAY COST. Each contact, calendar item and task read is a unit of
// the file's work budget (PstFormat.h), as a message is, and every page, block,
// heap allocation, cell and subnode read for it is charged there - with the
// recurrence blob and the time zones, each attachment looked at for a contact's
// picture, and the name-to-id map the first time it is read. What the budget
// and the job's stop throw (PstBudgetExhausted, PstStopped) is never taken for
// damage in one part of an item: it goes to the caller as it came. An item
// keeps at most PstStore::MaximumRecipients attendees, each name cut to
// MaximumAttendeeBytes, and PstStore::MaximumWarnings warnings of at most
// PstStore::MaximumWarningBytes each; a contact's attachments are each looked
// at once, and the first one marked as its picture is the only one read.
//
// WHAT AN ITEM KEEPS of a text: 64 KB of a name or another short text, 256 KB of
// its notes or description, cut at a character with a warning when it is
// longer; a short text of more than 1 MB, or a body of more than 4 MB, is left
// out, with a warning. Neither costs the item. Notes or a description kept only as RTF (no PR_BODY) are read
// through the RTF reader the mail phase uses, where the RTF carries plain text or
// is written in RTF; HTML carried in RTF is left out, with a warning.

#include "StdAfx.h"

#include "PstStore.h"
#include "PstItems.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace HM
{
   namespace
   {
      // [MS-OXPROPS]: the tagged properties read here.
      const uint16_t PropImportance = 0x0017;
      const uint16_t PropMessageClass = 0x001A;
      const uint16_t PropSensitivity = 0x0036;
      const uint16_t PropSubject = 0x0037;
      const uint16_t PropSentRepresentingName = 0x0042;
      const uint16_t PropSentRepresentingAddrType = 0x0064;
      const uint16_t PropSentRepresentingEmail = 0x0065;
      const uint16_t PropRecipientType = 0x0C15;
      const uint16_t PropSenderName = 0x0C1A;
      const uint16_t PropSenderAddrType = 0x0C1E;
      const uint16_t PropSenderEmail = 0x0C1F;
      const uint16_t PropBody = 0x1000;
      const uint16_t PropRtfCompressed = 0x1009;
      const uint16_t PropDisplayName = 0x3001;
      const uint16_t PropAddrType = 0x3002;
      const uint16_t PropEmailAddress = 0x3003;
      const uint16_t PropCreationTime = 0x3007;
      const uint16_t PropLastModificationTime = 0x3008;
      const uint16_t PropIpmAppointmentEntryId = 0x36D0;
      const uint16_t PropIpmContactEntryId = 0x36D1;
      const uint16_t PropIpmTaskEntryId = 0x36D4;
      const uint16_t PropAttachDataBinary = 0x3701;
      const uint16_t PropAttachMethod = 0x3705;
      const uint16_t PropSmtpAddress = 0x39FE;
      const uint16_t PropBirthday = 0x3A42;
      const uint16_t PropInternetCodePage = 0x3FDE;
      const uint16_t PropMessageCodePage = 0x3FFD;
      const uint16_t PropSenderSmtpAddress = 0x5D01;
      const uint16_t PropSentRepresentingSmtpAddress = 0x5D02;
      const uint16_t PropRecipientFlags = 0x5FFD;
      const uint16_t PropRecipientTrackStatus = 0x5FFF;
      const uint16_t PropAttachmentContactPhoto = 0x7FFF;

      // [MS-PST] 2.4.7: the name-to-id map's streams.
      const uint16_t PropNameidStreamGuid = 0x0002;
      const uint16_t PropNameidStreamEntry = 0x0003;

      // What an item keeps of a text, and the most of one that is read to
      // keep that much: a longer one is cut, with a warning, and one past its
      // ceiling is left out, with a warning - never the item lost for it. The
      // ceilings keep what a crafted item can cost small: several properties
      // may name one large value.
      const size_t ShortText = 64 * 1024;
      const size_t BodyText = 256 * 1024;
      const size_t ShortCeiling = 1024 * 1024;
      const size_t BodyCeiling = 4 * 1024 * 1024;
      const size_t MaximumNames = 65536;
      const size_t MaximumAttendeeBytes = 1024;

      // A warning kept, cut to MaximumWarningBytes; past MaximumWarnings, one
      // more that says the rest were not kept, and then nothing - as a
      // message's (PstStore::Warn_).
      void WarnItem(std::vector<PstWarning> &warnings, const std::string &word, const std::string &detail)
      {
         if (warnings.size() + 1 < PstStore::MaximumWarnings)
         {
            warnings.push_back(PstWarning(word, PstFormat::CutUtf8(detail, PstStore::MaximumWarningBytes)));
         }
         else if (warnings.size() + 1 == PstStore::MaximumWarnings)
         {
            warnings.push_back(PstWarning("warnings_left_out",
               "the item had more than " + std::to_string(PstStore::MaximumWarnings) + " warnings; the rest were not kept"));
         }
      }

      std::string GuidBytes(const char *hex)
      {
         std::string out;
         for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
         {
            const char pair[3] = { hex[i], hex[i + 1], 0 };
            out += (char) strtol(pair, nullptr, 16);
         }
         return out;
      }

      // [MS-OXPROPS] 1.3.2: the property sets, as their sixteen bytes lie in
      // the GUID stream.
      const std::string &PsetAddress() { static const std::string guid = GuidBytes("0420060000000000c000000000000046"); return guid; }
      const std::string &PsetAppointment() { static const std::string guid = GuidBytes("0220060000000000c000000000000046"); return guid; }
      const std::string &PsetTask() { static const std::string guid = GuidBytes("0320060000000000c000000000000046"); return guid; }
      const std::string &PsetCommon() { static const std::string guid = GuidBytes("0820060000000000c000000000000046"); return guid; }
      const std::string &PsetLog() { static const std::string guid = GuidBytes("0a20060000000000c000000000000046"); return guid; }
      const std::string &PsetNote() { static const std::string guid = GuidBytes("0e20060000000000c000000000000046"); return guid; }
      const std::string &PsetMeeting() { static const std::string guid = GuidBytes("90dad86e0b451b1098da00aa003f1305"); return guid; }

      bool IsWantedSet(const std::string &guid)
      {
         return guid == PsetAddress() || guid == PsetAppointment() || guid == PsetTask() || guid == PsetCommon() || guid == PsetLog() ||
                guid == PsetNote() || guid == PsetMeeting();
      }

      bool StartsWithNoCase(const std::string &text, const char *prefix)
      {
         for (size_t i = 0; prefix[i] != '\0'; i++)
         {
            if (i >= text.size())
               return false;
            char a = text[i];
            char b = prefix[i];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
            if (a != b)
               return false;
         }
         return true;
      }

      std::string Trim(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();
         const size_t last = text.find_last_not_of(" \t\r\n");
         return text.substr(first, last - first + 1);
      }

      // An address as mail can use it: one of the SMTP kind as it is, else
      // what the file keeps beside it (a contact's original display name, a
      // recipient's SMTP address), else none.
      bool LooksLikeAddress(const std::string &address)
      {
         const size_t at = address.find('@');
         return at != std::string::npos && at > 0 && at + 1 < address.size() && address.find_first_of(" \t\r\n/<>\"") == std::string::npos &&
                address.size() <= 254;
      }

      std::string SmtpOf(const std::string &addressType, const std::string &address, const std::string &beside)
      {
         const std::string type = Trim(addressType);
         if ((type.empty() || StartsWithNoCase(type, "SMTP")) && LooksLikeAddress(Trim(address)))
            return Trim(address);
         if (LooksLikeAddress(Trim(beside)))
            return Trim(beside);
         return std::string();
      }

      std::string Lower(const std::string &text)
      {
         std::string out = text;
         for (size_t i = 0; i < out.size(); i++)
         {
            if (out[i] >= 'A' && out[i] <= 'Z')
               out[i] = (char) (out[i] - 'A' + 'a');
         }
         return out;
      }

      int CodePageOf(const PstPropertyContext &pc)
      {
         int32_t codePage = 0;
         int32_t internetCodePage = 0;
         const bool hasInternet = pc.Int32(PropInternetCodePage, internetCodePage);
         if (!pc.Int32(PropMessageCodePage, codePage) || codePage <= 0)
            codePage = hasInternet && internetCodePage > 0 ? internetCodePage : 1252;
         return codePage;
      }

      std::string KiloBytes(size_t bytes)
      {
         return std::to_string((bytes + 1023) / 1024) + " KB";
      }

      // Text kept to keep bytes, cut at a character, with a warning when it
      // was longer.
      std::string CutText(const std::string &utf8, size_t keep, std::vector<PstWarning> &warnings, const std::string &what)
      {
         if (utf8.size() <= keep)
            return utf8;
         WarnItem(warnings, "text_cut", what + " ran to " + KiloBytes(utf8.size()) + "; the first " + KiloBytes(keep) + " were kept");
         return PstFormat::CutUtf8(utf8, keep);
      }

      std::string PropertyName(uint16_t id)
      {
         char text[32];
         snprintf(text, sizeof(text), "the text property 0x%04X", (unsigned) id);
         return text;
      }

      // A property read as text, or empty: id 0 (a name the file's map does
      // not have) and a property that is not there are the same. Longer than
      // keep, it is cut, with a warning; longer than its ceiling (BodyCeiling
      // for a body, else ShortCeiling), it is left out, with a warning. Damage
      // in it is the item's (PstError), and what the budget and the stop throw
      // goes to the caller.
      std::string TextOf(const PstPropertyContext &pc, uint16_t id, int codePage, std::vector<PstWarning> &warnings,
                         size_t keep = ShortText, const std::string &what = std::string())
      {
         std::string value;
         bool decoded = true;
         if (id == 0)
            return value;

         const std::string named = what.empty() ? PropertyName(id) : what;
         const size_t ceiling = keep > ShortText ? BodyCeiling : ShortCeiling;
         try
         {
            pc.Text(id, codePage, ceiling, value, decoded);
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstTooLarge &)
         {
            WarnItem(warnings, "text_left_out", named + " is more than " + std::to_string(ceiling / (1024 * 1024)) + " MB, and was left out");
            return std::string();
         }
         return CutText(value, keep, warnings, named);
      }

      // An item's notes or description: PR_BODY, cut to BodyText; where it
      // has none, the text of its RTF (PR_RTF_COMPRESSED), read through the
      // mail phase's RTF reader, cut the same way; and where that is not plain
      // text, nothing, with a warning.
      std::string BodyOf(const PstPropertyContext &pc, int codePage, std::vector<PstWarning> &warnings, PstBudget &budget,
                         const std::string &what)
      {
         std::string body = TextOf(pc, PropBody, codePage, warnings, BodyText, what);
         if (!Trim(body).empty())
            return body;

         std::string compressed;
         try
         {
            if (!pc.Bytes(PropRtfCompressed, PstItems::MaximumRtfBytes, compressed) || compressed.empty())
               return body;
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstError &error)
         {
            WarnItem(warnings, "body_unreadable", what + ", kept only as RTF, could not be read: " + error.what());
            return body;
         }

         std::string text;
         std::string problem;
         if (!PstItems::RtfBodyText(compressed, text, problem, &budget))
         {
            WarnItem(warnings, "body_unreadable", what + " is kept only as RTF, and was left out: " + problem);
            return body;
         }
         return CutText(text, BodyText, warnings, what);
      }

      uint64_t TimeOf(const PstPropertyContext &pc, uint16_t id)
      {
         uint64_t value = 0;
         if (id != 0)
            pc.Time(id, value);
         return value;
      }

      bool BooleanOf(const PstPropertyContext &pc, uint16_t id)
      {
         bool value = false;
         if (id != 0)
            pc.Boolean(id, value);
         return value;
      }

      bool Int32Of(const PstPropertyContext &pc, uint16_t id, int32_t &value)
      {
         return id != 0 && pc.Int32(id, value);
      }

      std::vector<std::string> ListOf(const PstPropertyContext &pc, uint16_t id, int codePage, std::vector<PstWarning> &warnings)
      {
         std::vector<std::string> values;
         if (id == 0)
            return values;
         try
         {
            pc.MultiText(id, codePage, ShortText, values);
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstError &error)
         {
            values.clear();
            WarnItem(warnings, "list_unreadable", std::string("a list of names could not be read: ") + error.what());
         }
         return values;
      }

      PstPropertyContext &OpenItem(PstFile &file, uint32_t nid, PstPropertyContext &pc, const char *what)
      {
         PstFile::Node node;
         if (!file.FindNode(nid, node))
            throw PstError(std::string("The ") + what + "'s node is not in the file.");
         pc.Read(file, node.data, node.sub);
         return pc;
      }

      // A contact's date: PidLidBirthdayLocal is the day, kept as midnight UTC
      // ([MS-OXOCNTC] 2.2.1.5.2); PidTagBirthday is the local midnight in UTC,
      // in a zone the file does not name, so its day is taken as the nearest
      // one - guessed, which a zone twelve or more hours from UTC gets wrong.
      std::string BirthdayText(uint64_t local, uint64_t utc, bool &guessed)
      {
         int64_t seconds = 0;
         bool found = false;
         guessed = false;
         if (!PstItems::IsNoDate(local) && PstFormat::FileTimeToUnix(local, seconds))
            found = true;
         else if (!PstItems::IsNoDate(utc) && PstFormat::FileTimeToUnix(utc, seconds))
         {
            seconds += 43200;
            found = true;
            guessed = true;
         }
         if (!found)
            return std::string();

         int year, month, day, hour, minute, second, weekday;
         PstFormat::UnixToCivil(seconds, year, month, day, hour, minute, second, weekday);
         if (year < 1600 || year > 4499)
            return std::string();
         char text[16];
         snprintf(text, sizeof(text), "%04d%02d%02d", year, month, day);
         return text;
      }
   }

   uint16_t
   PstStore::Named_(const std::string &guid, uint32_t lid)
   {
      if (!named_)
      {
         // Tried once an item: one that failed for this item is not read
         // again for it.
         if (names_failed_now_)
            return 0;

         // Read whole into a map of its own, and kept only when the reading
         // ends: a stop half-way leaves it to be read again by the next run,
         // never half a map.
         std::shared_ptr<std::map<std::pair<std::string, uint32_t>, uint16_t> > loaded =
            std::make_shared<std::map<std::pair<std::string, uint32_t>, uint16_t> >();

         named_attempts_++;
         bool complete = true;
         try
         {
            PstFile::Node names;
            if (file_.FindNode(PstNidNameToIdMap, names))
            {
               PstPropertyContext map;
               map.Read(file_, names.data, names.sub);

               std::string guids;
               std::string entries;
               map.Bytes(PropNameidStreamGuid, 16 * 1024 * 1024, guids);
               if (map.Bytes(PropNameidStreamEntry, 16 * 1024 * 1024, entries))
               {
                  static const std::string mapi = GuidBytes("2803020000000000c000000000000046");
                  static const std::string publicStrings = GuidBytes("2903020000000000c000000000000046");

                  for (size_t at = 0; at + 8 <= entries.size() && loaded->size() < MaximumNames; at += 8)
                  {
                     const uint32_t identifier = PstFormat::U32(entries, at, "the name-to-id map");
                     const uint16_t kind = PstFormat::U16(entries, at + 4, "the name-to-id map");
                     const uint16_t index = PstFormat::U16(entries, at + 6, "the name-to-id map");

                     // Named by a string (Keywords is read beside), or an
                     // index past the named range: not one of these.
                     if ((kind & 1) != 0 || index > 0x7FFE)
                        continue;

                     const size_t guidIndex = kind >> 1;
                     std::string setGuid;
                     if (guidIndex == 1)
                        setGuid = mapi;
                     else if (guidIndex == 2)
                        setGuid = publicStrings;
                     else if (guidIndex >= 3 && PstFormat::Fits((guidIndex - 3) * 16, 16, guids.size()))
                        setGuid = guids.substr((guidIndex - 3) * 16, 16);
                     else
                        continue;

                     if (IsWantedSet(setGuid))
                        loaded->insert(std::make_pair(std::make_pair(setGuid, identifier), (uint16_t) (0x8000 + index)));
                  }
               }
            }
         }
         catch (PstBudgetExhausted &exhausted)
         {
            // The whole file's budget is the file's. The item's own, spent
            // on the map, is the map's, as its damage is.
            if (exhausted.WholeFile())
               throw;
            complete = false;
            names_problem_ = PstFormat::CutUtf8(exhausted.what(), 300);
         }
         catch (PstError &error)
         {
            complete = false;
            names_problem_ = PstFormat::CutUtf8(error.what(), 300);
         }

         if (complete)
         {
            named_ = loaded;
            names_problem_.clear();
         }
         else
         {
            // This item goes without named properties, and says so; the next
            // tries the map again, up to MaximumNameMapAttempts readings in
            // all, after which every item goes without them.
            names_failed_now_ = true;
            if (named_attempts_ >= MaximumNameMapAttempts)
               named_ = std::make_shared<std::map<std::pair<std::string, uint32_t>, uint16_t> >();
            return 0;
         }
      }

      std::map<std::pair<std::string, uint32_t>, uint16_t>::const_iterator found = named_->find(std::make_pair(guid, lid));
      return found == named_->end() ? 0 : found->second;
   }

   void
   PstStore::ReadNames()
   {
      if (named_)
         return;

      // One reading, under a unit of its own: its failure is final, and the
      // map is empty from then on (Named_).
      file_.Budget().BeginUnit();
      names_failed_now_ = false;
      named_attempts_ = MaximumNameMapAttempts - 1;
      Named_(std::string(), 0);
      if (!named_)
         named_ = std::make_shared<std::map<std::pair<std::string, uint32_t>, uint16_t> >();
   }

   void
   PstStore::BeginItem_()
   {
      file_.Budget().BeginUnit();
      names_failed_now_ = false;
   }

   void
   PstStore::NamesWarning_(std::vector<PstWarning> &warnings) const
   {
      if (!names_problem_.empty() && (names_failed_now_ || (named_ && named_->empty())))
      {
         WarnItem(warnings, "names_unreadable",
            "the file's list of named properties could not be read (" + names_problem_ +
            "), so what the item keeps under them - its e-mail addresses, its times, its recurrence - is missing");
      }
   }

   std::string
   PstStore::OfflineSentence()
   {
      return "This OST - Outlook's offline copy of a mailbox that lives on a server - names its folders by the server's own "
             "identifiers rather than by the file's, and how the one maps to the other is not in Microsoft's published PST format "
             "([MS-PST]), so its folders cannot be found; migrate the mailbox from its server with a migration job, or export its "
             "folders to a PST in Outlook.";
   }

   bool
   PstStore::IsMessageLikeFolderClass(const std::string &containerClass)
   {
      return StartsWithNoCase(containerClass, "IPF.StickyNote") || StartsWithNoCase(containerClass, "IPF.Journal");
   }

   bool
   PstStore::IsSystemContactsClass(const std::string &containerClass)
   {
      return StartsWithNoCase(containerClass, "IPF.Contact.") && containerClass.size() > 12;
   }

   void
   PstStore::DefaultItemFolders(uint32_t &contacts, uint32_t &calendar, uint32_t &tasks)
   {
      contacts = 0;
      calendar = 0;
      tasks = 0;

      // A unit of the work budget of its own.
      file_.Budget().BeginUnit();

      try
      {
         PstFile::Node root;
         if (!file_.FindNode(PstNidRootFolder, root))
            return;

         PstPropertyContext pc;
         pc.Read(file_, root.data, root.sub);

         std::string entryId;
         uint32_t nid = 0;
         if (pc.Bytes(PropIpmContactEntryId, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
            contacts = nid;
         if (pc.Bytes(PropIpmAppointmentEntryId, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
            calendar = nid;
         if (pc.Bytes(PropIpmTaskEntryId, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
            tasks = nid;
      }
      catch (PstBudgetExhausted &exhausted)
      {
         if (exhausted.WholeFile())
            throw;
         contacts = calendar = tasks = 0;
      }
      catch (PstError &)
      {
         // A root folder that cannot be read names nothing: the first folder
         // of each kind is taken instead.
         contacts = calendar = tasks = 0;
      }
   }

   void
   PstStore::ReadContact(uint32_t nid, PstContact &contact)
   {
      contact = PstContact();
      contact.nid = nid;

      // A contact read whole is one unit of the work budget.
      BeginItem_();

      PstPropertyContext pc;
      OpenItem(file_, nid, pc, "contact");
      const int codePage = CodePageOf(pc);
      auto text = [&](uint16_t id) { return Trim(TextOf(pc, id, codePage, contact.warnings)); };
      auto named = [&](uint32_t lid) { return Named_(PsetAddress(), lid); };

      contact.messageClass = text(PropMessageClass);
      contact.displayName = text(PropDisplayName);
      if (contact.displayName.empty())
         contact.displayName = Trim(WithoutSubjectMarker(text(PropSubject)));
      contact.fileUnder = text(named(0x8005));
      contact.givenName = text(0x3A06);
      contact.surname = text(0x3A11);
      contact.middleName = text(0x3A44);
      contact.prefix = text(0x3A45);
      contact.generation = text(0x3A05);
      contact.nickname = text(0x3A4F);
      contact.company = text(0x3A16);
      contact.title = text(0x3A17);
      contact.department = text(0x3A18);
      contact.modified = TimeOf(pc, PropLastModificationTime);

      // [MS-OXOCNTC] 2.2.1.2: Email1 to Email3, each a type, an address and
      // the original display name - where an Exchange address's SMTP one is.
      for (uint32_t i = 0; i < 3; i++)
      {
         const uint32_t base = 0x8080 + 0x10 * i;
         const std::string address = SmtpOf(text(named(base + 2)), text(named(base + 3)), text(named(base + 4)));
         if (address.empty())
            continue;

         bool again = false;
         for (const std::string &existing : contact.emails)
            again = again || Lower(existing) == Lower(address);
         if (!again)
            contact.emails.push_back(address);
      }

      // [MS-OXOCNTC] 2.2.1.4: the telephone numbers, each with the vCard 4
      // types that say what it is.
      const struct { uint16_t id; const char *types; } phones[] =
      {
         { 0x3A1A, "voice" },          // PidTagPrimaryTelephoneNumber
         { 0x3A08, "work,voice" },     // PidTagBusinessTelephoneNumber
         { 0x3A1B, "work,voice" },     // PidTagBusiness2TelephoneNumber
         { 0x3A57, "work,voice" },     // PidTagCompanyMainTelephoneNumber
         { 0x3A09, "home,voice" },     // PidTagHomeTelephoneNumber
         { 0x3A2F, "home,voice" },     // PidTagHome2TelephoneNumber
         { 0x3A1C, "cell" },           // PidTagMobileTelephoneNumber
         { 0x3A24, "work,fax" },       // PidTagBusinessFaxNumber
         { 0x3A25, "home,fax" },       // PidTagHomeFaxNumber
         { 0x3A21, "pager" },          // PidTagPagerTelephoneNumber
         { 0x3A1E, "voice" },          // PidTagCarTelephoneNumber
         { 0x3A2E, "voice" },          // PidTagAssistantTelephoneNumber
         { 0x3A1F, "voice" },          // PidTagOtherTelephoneNumber
      };
      for (const auto &phone : phones)
      {
         const std::string number = text(phone.id);
         if (!number.empty())
         {
            PstContact::Phone entry;
            entry.number = number;
            entry.types = phone.types;
            contact.phones.push_back(entry);
         }
      }

      // [MS-OXOCNTC] 2.2.1.3: the home, work and other addresses, and - where
      // none of the three is there - the mailing address alone.
      auto address = [&](const char *type, uint16_t poBox, uint16_t street, uint16_t city, uint16_t region, uint16_t postalCode, uint16_t country)
      {
         PstContact::Address entry;
         entry.type = type;
         entry.poBox = text(poBox);
         entry.street = text(street);
         entry.city = text(city);
         entry.region = text(region);
         entry.postalCode = text(postalCode);
         entry.country = text(country);
         if (!(entry.poBox + entry.street + entry.city + entry.region + entry.postalCode + entry.country).empty())
            contact.addresses.push_back(entry);
      };
      address("home", 0x3A5E, 0x3A5D, 0x3A59, 0x3A5C, 0x3A5B, 0x3A5A);
      address("work", named(0x804A), named(0x8045), named(0x8046), named(0x8047), named(0x8048), named(0x8049));
      address("", 0x3A64, 0x3A63, 0x3A5F, 0x3A62, 0x3A61, 0x3A60);
      if (contact.addresses.empty())
         address("", 0x3A2B, 0x3A29, 0x3A27, 0x3A28, 0x3A2A, 0x3A26);

      bool birthdayGuessed = false;
      contact.birthday = BirthdayText(TimeOf(pc, named(0x80DE)), TimeOf(pc, PropBirthday), birthdayGuessed);
      if (birthdayGuessed && !contact.birthday.empty())
      {
         WarnItem(contact.warnings, "birthday_guessed",
            "the file keeps the birthday only as a time in UTC, in a zone it does not name, so the day nearest that time was taken; "
            "it may be a day out");
      }
      contact.note = Trim(BodyOf(pc, codePage, contact.warnings, file_.Budget(), "the contact's notes"));

      const uint16_t pages[] = { 0x3A51, 0x3A50 };   // PidTagBusinessHomePage, PidTagPersonalHomePage
      for (uint16_t page : pages)
      {
         const std::string url = text(page);
         if (!url.empty())
            contact.urls.push_back(url);
      }

      if (keywords_id_ != 0)
         contact.categories = ListOf(pc, keywords_id_, codePage, contact.warnings);

      // The photo: the attachment PidTagAttachmentContactPhoto marks
      // ([MS-OXOCNTC] 2.2.1.8), a JPEG, PNG or GIF of at most 512 KB. Each
      // attachment is looked at once however often the table names it, each
      // is charged to the budget, and the first one marked is the only one
      // whose bytes are read.
      std::map<uint32_t, PstFile::Subnode>::const_iterator table = pc.GetSubnodes().find(PstNidAttachmentTable);
      if (table != pc.GetSubnodes().end())
      {
         try
         {
            PstTable attachments;
            attachments.Read(file_, table->second.data, table->second.sub, 1000);
            std::set<uint32_t> seen;
            for (size_t row = 0; row < attachments.RowCount(); row++)
            {
               const uint32_t attachmentNid = attachments.RowId(row);
               if (!seen.insert(attachmentNid).second)
                  continue;

               std::map<uint32_t, PstFile::Subnode>::const_iterator own = pc.GetSubnodes().find(attachmentNid);
               if (own == pc.GetSubnodes().end())
                  continue;

               file_.Budget().Item();

               PstPropertyContext attachment;
               attachment.Read(file_, own->second.data, own->second.sub);

               bool isPhoto = false;
               int32_t method = 0;
               attachment.Boolean(PropAttachmentContactPhoto, isPhoto);
               attachment.Int32(PropAttachMethod, method);
               if (!isPhoto)
                  continue;

               // The first attachment marked as the picture is the picture,
               // taken or not.
               std::string bytes;
               if (method == 1 && attachment.Bytes(PropAttachDataBinary, PstItems::MaximumPhotoBytes, bytes))
               {
                  const std::string type = PstItems::ImageType(bytes);
                  if (type.empty())
                     WarnItem(contact.warnings, "photo_not_imported", "the contact's picture is not a JPEG, PNG or GIF");
                  else
                  {
                     contact.photo = bytes;
                     contact.photoType = type;
                  }
               }
               break;
            }
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstTooLarge &)
         {
            WarnItem(contact.warnings, "photo_not_imported", "the contact's picture is larger than 512 KB");
         }
         catch (PstError &error)
         {
            WarnItem(contact.warnings, "photo_not_imported", std::string("the contact's picture could not be read: ") + error.what());
         }
      }

      NamesWarning_(contact.warnings);
   }

   void
   PstStore::ItemRecipients_(const PstPropertyContext &pc, int codePage, PstAppointment &appointment)
   {
      std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = pc.GetSubnodes().find(PstNidRecipientTable);
      if (subnode == pc.GetSubnodes().end())
         return;

      try
      {
         PstTable table;
         table.Read(file_, subnode->second.data, subnode->second.sub, 100000);

         // At most MaximumRecipients, as a message: each cell read with a
         // cap, each name kept to MaximumAttendeeBytes.
         const size_t rows = table.RowCount() < MaximumRecipients ? table.RowCount() : MaximumRecipients;
         for (size_t row = 0; row < rows; row++)
         {
            PstAttendee attendee;
            int32_t value = 0;
            if (table.CellInt32(row, PropRecipientType, value))
               attendee.type = value & 0x0F;
            if (table.CellInt32(row, PropRecipientFlags, value))
               attendee.flags = value;
            if (table.CellInt32(row, PropRecipientTrackStatus, value))
               attendee.trackStatus = value;

            std::string addressType, address, smtp;
            table.CellText(row, PropDisplayName, codePage, attendee.name, ShortText);
            table.CellText(row, PropAddrType, codePage, addressType, ShortText);
            table.CellText(row, PropEmailAddress, codePage, address, ShortText);
            table.CellText(row, PropSmtpAddress, codePage, smtp, ShortText);
            attendee.address = SmtpOf(addressType, address, smtp);
            attendee.name = PstFormat::CutUtf8(Trim(attendee.name), MaximumAttendeeBytes);

            appointment.attendees.push_back(attendee);
         }

         if (table.RowCount() > rows)
         {
            WarnItem(appointment.warnings, "attendees_left_out", "the item lists " + std::to_string(table.RowCount()) +
                     " attendees; the first " + std::to_string(MaximumRecipients) + " were kept");
         }
      }
      catch (PstBudgetExhausted &)
      {
         throw;
      }
      catch (PstError &error)
      {
         appointment.attendees.clear();
         WarnItem(appointment.warnings, "attendees_unreadable", std::string("the list of attendees could not be read: ") + error.what());
      }
   }

   void
   PstStore::ReadAppointment(uint32_t nid, PstAppointment &appointment)
   {
      appointment = PstAppointment();
      appointment.nid = nid;

      // A calendar item read whole is one unit of the work budget.
      BeginItem_();

      PstPropertyContext pc;
      OpenItem(file_, nid, pc, "calendar item");
      appointment.codePage = CodePageOf(pc);
      const int codePage = appointment.codePage;
      auto text = [&](uint16_t id) { return Trim(TextOf(pc, id, codePage, appointment.warnings)); };
      auto named = [&](uint32_t lid) { return Named_(PsetAppointment(), lid); };

      appointment.messageClass = text(PropMessageClass);
      appointment.subject = Trim(WithoutSubjectMarker(TextOf(pc, PropSubject, codePage, appointment.warnings)));
      appointment.location = text(named(0x8208));
      appointment.body = BodyOf(pc, codePage, appointment.warnings, file_.Budget(), "the description");
      appointment.created = TimeOf(pc, PropCreationTime);
      appointment.modified = TimeOf(pc, PropLastModificationTime);

      // [MS-OXOCAL] 2.2.1.5 and 2.2.1.6, else the common start and end
      // ([MS-OXCMSG] PidLidCommonStart, PidLidCommonEnd).
      appointment.start = TimeOf(pc, named(0x820D));
      if (appointment.start == 0)
         appointment.start = TimeOf(pc, Named_(PsetCommon(), 0x8516));
      appointment.end = TimeOf(pc, named(0x820E));
      if (appointment.end == 0)
         appointment.end = TimeOf(pc, Named_(PsetCommon(), 0x8517));

      appointment.allDay = BooleanOf(pc, named(0x8215));
      appointment.recurring = BooleanOf(pc, named(0x8223));

      int32_t value = 0;
      if (Int32Of(pc, named(0x8205), value))
         appointment.busyStatus = value;
      if (Int32Of(pc, named(0x8217), value))
      {
         appointment.meeting = (value & 0x0001) != 0;
         appointment.canceled = (value & 0x0004) != 0;
      }
      if (pc.Int32(PropSensitivity, value))
         appointment.sensitivity = value;

      appointment.reminderSet = BooleanOf(pc, Named_(PsetCommon(), 0x8503));
      if (Int32Of(pc, Named_(PsetCommon(), 0x8501), value))
         appointment.reminderMinutes = value;

      if (keywords_id_ != 0)
         appointment.categories = ListOf(pc, keywords_id_, codePage, appointment.warnings);

      // The UID: the clean global object id, else the global object id.
      std::string goid;
      const uint16_t clean = Named_(PsetMeeting(), 0x0023);
      const uint16_t global = Named_(PsetMeeting(), 0x0003);
      if ((clean != 0 && pc.Bytes(clean, 4096, goid)) || (global != 0 && pc.Bytes(global, 4096, goid)))
         appointment.uid = PstItems::UidOfGlobalObjectId(goid);

      // The recurrence and its zone: the recurring TZDEFINITION, else
      // PidLidTimeZoneStruct named by PidLidTimeZoneDescription; and the
      // zone the start was shown in, for an all-day appointment.
      int64_t startSeconds = 0;
      int year = 9999;
      if (PstFormat::FileTimeToUnix(appointment.start, startSeconds))
      {
         int month, day, hour, minute, second, weekday;
         PstFormat::UnixToCivil(startSeconds, year, month, day, hour, minute, second, weekday);
      }

      std::string blob;
      const uint16_t recur = named(0x8216);
      try
      {
         if (recur != 0)
            pc.Bytes(recur, PstItems::MaximumRecurrenceBytes, blob);
      }
      catch (PstBudgetExhausted &)
      {
         throw;
      }
      catch (PstTooLarge &)
      {
         WarnItem(appointment.warnings, "recurrence_unreadable", "its recurrence pattern is larger than any Outlook writes");
      }
      appointment.recurrenceBlob = blob;

      // Each zone read is charged to the item's unit; the blob is charged
      // where it is parsed (PstItems::AppointmentText, given the budget).
      PstBudget &budget = file_.Budget();
      std::string zone;
      if (named(0x8260) != 0 && pc.Bytes(named(0x8260), 64 * 1024, zone))
         PstItems::ParseTimeZoneDefinition(zone, year, appointment.zone, &budget);
      if (!appointment.zone.valid && named(0x8233) != 0 && pc.Bytes(named(0x8233), 1024, zone) &&
          PstItems::ParseTimeZoneStruct(zone, appointment.zone, &budget))
         appointment.zone.name = text(named(0x8234));
      if (named(0x825E) != 0 && pc.Bytes(named(0x825E), 64 * 1024, zone))
         PstItems::ParseTimeZoneDefinition(zone, year, appointment.startZone, &budget);

      // An all-day appointment whose zone the file does not name is placed on
      // the day nearest its start in UTC (PstItems::AppointmentText) - a guess,
      // which a zone twelve or more hours from UTC gets wrong, and said so.
      if (appointment.allDay && !appointment.recurring && !appointment.startZone.valid && !appointment.zone.valid)
      {
         WarnItem(appointment.warnings, "all_day_zone_unknown",
            "the file names no time zone for this all-day appointment, so its day was taken as the one nearest its start in UTC; "
            "it may be a day out");
      }

      // The organiser: the recipient row that says it is one, else the sender.
      ItemRecipients_(pc, codePage, appointment);
      for (const PstAttendee &attendee : appointment.attendees)
      {
         if ((attendee.flags & 0x0002) != 0 && !attendee.address.empty())
         {
            appointment.organizerName = attendee.name;
            appointment.organizerAddress = attendee.address;
            break;
         }
      }
      if (appointment.organizerAddress.empty() && (appointment.meeting || !appointment.attendees.empty()))
      {
         appointment.organizerName = text(PropSentRepresentingName);
         appointment.organizerAddress = SmtpOf(text(PropSentRepresentingAddrType), text(PropSentRepresentingEmail), text(PropSentRepresentingSmtpAddress));
         if (appointment.organizerAddress.empty())
         {
            appointment.organizerName = text(PropSenderName);
            appointment.organizerAddress = SmtpOf(text(PropSenderAddrType), text(PropSenderEmail), text(PropSenderSmtpAddress));
         }
      }

      NamesWarning_(appointment.warnings);
   }

   void
   PstStore::ReadTask(uint32_t nid, PstTask &task)
   {
      task = PstTask();
      task.nid = nid;

      // A task read whole is one unit of the work budget.
      BeginItem_();

      PstPropertyContext pc;
      OpenItem(file_, nid, pc, "task");
      const int codePage = CodePageOf(pc);
      auto named = [&](uint32_t lid) { return Named_(PsetTask(), lid); };

      task.messageClass = Trim(TextOf(pc, PropMessageClass, codePage, task.warnings));
      task.subject = Trim(WithoutSubjectMarker(TextOf(pc, PropSubject, codePage, task.warnings)));
      task.body = BodyOf(pc, codePage, task.warnings, file_.Budget(), "the task's notes");
      task.created = TimeOf(pc, PropCreationTime);
      task.modified = TimeOf(pc, PropLastModificationTime);
      task.start = TimeOf(pc, named(0x8104));
      task.due = TimeOf(pc, named(0x8105));
      task.completedAt = TimeOf(pc, named(0x810F));
      task.complete = BooleanOf(pc, named(0x811C));

      int32_t value = 0;
      if (Int32Of(pc, named(0x8101), value))
         task.status = value;
      if (pc.Int32(PropImportance, value))
         task.importance = value;
      if (pc.Int32(PropSensitivity, value))
         task.sensitivity = value;

      // PidLidPercentComplete is a PtypFloating64 from 0.0 to 1.0.
      uint16_t type = 0;
      std::string bytes;
      const uint16_t percent = named(0x8102);
      if (percent != 0 && pc.TypeOf(percent, type) && type == PstTypeFloating64 && pc.Bytes(percent, 8, bytes) && bytes.size() == 8)
      {
         double fraction = 0;
         memcpy(&fraction, bytes.data(), 8);
         if (fraction == fraction && fraction >= 0 && fraction <= 1)
            task.percent = (int) std::floor(fraction * 100 + 0.5);
      }

      task.reminderSet = BooleanOf(pc, Named_(PsetCommon(), 0x8503));
      task.reminderTime = TimeOf(pc, Named_(PsetCommon(), 0x8502));

      if (keywords_id_ != 0)
         task.categories = ListOf(pc, keywords_id_, codePage, task.warnings);

      NamesWarning_(task.warnings);
   }

   bool
   PstStore::AsMessage(uint32_t nid, PstMessage &message)
   {
      const bool note = StartsWithNoCase(message.messageClass, "IPM.StickyNote");
      const bool journal = StartsWithNoCase(message.messageClass, "IPM.Activity");
      if (!note && !journal)
         return false;

      // The item's text is the item: the RTF copy of it Outlook keeps beside
      // is not rebuilt as body.rtf.
      if (message.hasBody)
      {
         message.hasRtf = false;
         message.rtfCompressed.clear();
      }

      if (journal)
      {
         PstJournal fields;
         names_failed_now_ = false;
         try
         {
            PstPropertyContext pc;
            OpenItem(file_, nid, pc, "journal entry");
            const int codePage = CodePageOf(pc);
            fields.type = Trim(TextOf(pc, Named_(PsetLog(), 0x8700), codePage, message.warnings));
            fields.start = TimeOf(pc, Named_(PsetLog(), 0x8706));
            fields.end = TimeOf(pc, Named_(PsetLog(), 0x8708));
            int32_t duration = 0;
            if (Int32Of(pc, Named_(PsetLog(), 0x8707), duration) && duration >= 0)
               fields.duration = duration;
            fields.contacts = ListOf(pc, Named_(PsetCommon(), 0x853A), codePage, message.warnings);
            fields.companies = ListOf(pc, Named_(PsetCommon(), 0x8539), codePage, message.warnings);
         }
         catch (PstBudgetExhausted &)
         {
            // The budget's, as the stop's (PstStopped) and the memory's
            // (std::bad_alloc), which no catch here takes: the mail phase's
            // to deal with, as for the message's own reading.
            throw;
         }
         catch (PstError &error)
         {
            // Damage in the entry's fields: it comes in as its text.
            Warn_(message, "journal_fields_unreadable", std::string("the journal entry's type and times could not be read: ") + error.what());
         }

         NamesWarning_(message.warnings);
         message.body = PstItems::JournalText(fields) + message.body;
         message.hasBody = true;
      }

      return true;
   }
}
