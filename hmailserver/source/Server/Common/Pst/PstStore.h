// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST's messaging layer ([MS-PST] 2.4): the message store and its password,
// the name-to-id map (for the categories, PS_PUBLIC_STRINGS "Keywords"), the
// folder hierarchy under the IPM subtree - the folders a person sees in
// Outlook - and each message read whole: its properties, its recipients and its
// attachments, an embedded message among them read the same way, at most ten
// deep.
//
// What is left out, and why:
//   - Everything outside the IPM subtree (the search root, Outlook's views and
//     its own bookkeeping), search folders anywhere (their messages are other
//     folders'), and associated (FAI) items, which are a folder's settings and
//     views rather than mail.
//   - Special folders are found by what the store says they are, never by their
//     names: the Sent Items and Deleted Items folders from the message store's
//     PR_IPM_SENTMAIL_ENTRYID and PR_IPM_WASTEBASKET_ENTRYID, Drafts and Junk
//     E-mail from the root folder's PR_IPM_DRAFTS_ENTRYID and the fifth entry of
//     its PR_ADDITIONAL_REN_ENTRYIDS - or, where the root has neither, the Inbox's.
//
// A message or an attachment that cannot be read is that message's or that
// attachment's failure - PstError from ReadMessage, a warning in the message -
// and never the file's.
//
// WHAT ONE READING MAY COST. Besides the file's work budget (PstFormat.h), which
// every page, block, heap allocation, cell, subnode, attachment and embedded
// message is charged to - the store opened, each folder of the list, each
// folder's list of messages and each message read being a unit of it - a message
// read whole keeps at most MaximumMessageBytes of text and data (its bodies,
// headers, recipients, attachments' names and bytes, and its embedded messages'
// the same), reads at most MaximumParts attachments and embedded messages however
// deep they go, keeps at most MaximumRecipients recipients and MaximumWarnings
// warnings of at most MaximumWarningBytes each; and the list of folders names at
// most MaximumFolders folders, keeps each name to MaximumFolderNameBytes, all their
// paths together to MaximumFolderPathBytes, and at most MaximumProblems sentences
// about what it could not read. Past the message's limits the message is refused
// with PstTooLarge; past the list's, the file with PstError.

#pragma once

#include "PstFile.h"
#include "PstLtp.h"

#include <memory>

namespace HM
{
   struct PstWarning
   {
      PstWarning() { }
      PstWarning(const std::string &w, const std::string &d) : word(w), detail(d) { }

      std::string word;      // for counting: "attachment_not_imported", "rtf_unreadable" ...
      std::string detail;    // a sentence naming what, for the report
   };

   struct PstRecipient
   {
      PstRecipient() : type(1) { }

      int type;              // 1 To, 2 Cc, 3 Bcc (PR_RECIPIENT_TYPE)
      std::string name;      // UTF-8
      std::string address;   // an SMTP address, or empty when the PST had none
   };

   struct PstMessage;

   struct PstAttachment
   {
      PstAttachment() : nid(0), method(0), hidden(false) { }

      uint32_t nid;
      int method;            // PR_ATTACH_METHOD: 1 by value, 5 an embedded message
      std::string fileName;  // PR_ATTACH_LONG_FILENAME, else PR_ATTACH_FILENAME, else PR_DISPLAY_NAME
      std::string mimeType;  // PR_ATTACH_MIME_TAG
      std::string contentId; // PR_ATTACH_CONTENT_ID
      bool hidden;           // PR_ATTACHMENT_HIDDEN
      std::string data;
      std::shared_ptr<PstMessage> embedded;
   };

   struct PstMessage
   {
      PstMessage() : nid(0), hasTransportHeaders(false), hasBody(false), hasHtml(false), htmlCodePage(-1), hasRtf(false),
                     submitTime(0), deliveryTime(0), creationTime(0), messageFlags(0), flagStatus(0), lastVerb(0) { }

      uint32_t nid;
      std::string messageClass;
      std::string subject;               // the subject marker removed

      bool hasTransportHeaders;
      std::string transportHeaders;

      bool hasBody;
      std::string body;                  // PR_BODY as UTF-8

      bool hasHtml;
      std::string html;                  // PR_HTML's bytes as they are
      int htmlCodePage;                  // PR_INTERNET_CPID, -1 when the message has none

      bool hasRtf;
      std::string rtfCompressed;         // PR_RTF_COMPRESSED

      std::string messageId;
      std::string inReplyTo;
      std::string references;

      std::string senderName;
      std::string senderAddress;
      std::string representingName;
      std::string representingAddress;

      uint64_t submitTime;               // FILETIMEs, 0 when absent
      uint64_t deliveryTime;
      uint64_t creationTime;

      int32_t messageFlags;              // PR_MESSAGE_FLAGS
      int32_t flagStatus;                // PR_FLAG_STATUS
      int32_t lastVerb;                  // PR_LAST_VERB_EXECUTED

      std::vector<std::string> categories;
      std::vector<PstRecipient> recipients;
      std::vector<PstAttachment> attachments;
      std::vector<PstWarning> warnings;
   };

   // Contacts, calendar items and tasks, read by PstStoreItems.cpp (PstItems.h).
   struct PstContact;
   struct PstAppointment;
   struct PstTask;

   struct PstFolderInfo
   {
      PstFolderInfo() : nid(0), parentNid(0), role(0), contentCount(0), mail(true), hidden(false) { }

      enum Role
      {
         RoleNone = 0,
         RoleSent = 1,
         RoleTrash = 2,
         RoleDrafts = 3,
         RoleJunk = 4
      };

      uint32_t nid;
      uint32_t parentNid;                // the IPM subtree's NID for a top-level folder
      std::vector<std::string> path;     // UTF-8 names from the IPM subtree down, this folder last
      std::string containerClass;
      int role;
      int32_t contentCount;              // PR_CONTENT_COUNT
      bool mail;                         // a mail folder, by its container class
      bool hidden;                       // PR_ATTR_HIDDEN: a folder Outlook does not show
   };

   class PstStore
   {
   public:
      explicit PstStore(std::shared_ptr<PstByteSource> source);

      // The header, the message store, its password and the name-to-id map.
      // Refused with a PstRefusal; a store that cannot be read throws PstError.
      void Open(const std::string &password);

      const std::string &GetRecordKey() const { return record_key_; }
      const std::string &GetDisplayName() const { return display_name_; }
      int GetVersion() const { return file_.GetVersion(); }
      int GetCryptMethod() const { return file_.GetCryptMethod(); }

      // The folders of the IPM subtree, parents before children, each branch in
      // the order the hierarchy table lists it. What could not be read is in
      // problems, a sentence each, and the rest is listed.
      void ListFolders(std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems);

      // A folder's messages - normal messages only, not its associated items -
      // by NID, ascending.
      void ListMessages(uint32_t folderNid, std::vector<uint32_t> &messages);

      // A message read whole. Throws PstError when the message itself cannot
      // be read, PstTooLarge when it is past one of the limits above (and
      // PstBudgetExhausted, a PstTooLarge, when it cost more work than the
      // budget allows); what is missing from one that can is in its warnings.
      void ReadMessage(uint32_t nid, PstMessage &message);

      // The file's work budget, and the job's stop check (PstFormat.h).
      PstBudget &Budget() { return file_.Budget(); }

      // Whether a folder's container class is mail's: none, IPF.Note and its
      // sub-classes, or IPF.Imap.
      static bool IsMailFolderClass(const std::string &containerClass);

      // Whether a message's class is imported as mail: notes and their
      // sub-classes (S/MIME, receipts), posts, meeting requests and responses,
      // and reports.
      static bool IsMailMessageClass(const std::string &messageClass);

      // [MS-PST] 2.4.3.2 does not say how a password's CRC is taken; both the
      // eight-bit and the UTF-16LE forms of it are accepted.
      static bool PasswordMatches(const std::string &password, uint32_t stored);

      // The subject with Outlook's prefix marker (0x01 and a length character)
      // removed.
      static std::string WithoutSubjectMarker(const std::string &subject);

      static const int MaximumFolderDepth = 64;
      static const int MaximumEmbeddedDepth = 10;
      static const size_t MaximumMessageBytes = 128 * 1024 * 1024;
      static const size_t MaximumMessagesPerFolder = 1000000;
      static const size_t MaximumParts = 10000;
      static const size_t MaximumRecipients = 5000;
      static const size_t MaximumWarnings = 100;
      static const size_t MaximumWarningBytes = 1024;
      static const size_t MaximumFolders = 100000;
      static const size_t MaximumFolderNameBytes = 1024;
      static const size_t MaximumFolderPathBytes = 16 * 1024 * 1024;
      static const size_t MaximumProblems = 1000;

      // What is not mail (PstStoreItems.cpp, PstItems.h).
      //
      // A contact (IPM.Contact), a calendar item (IPM.Appointment, or a
      // meeting request kept in a calendar folder) and a task (IPM.Task), each
      // read whole with its named properties found through the name-to-id map,
      // each a unit of the work budget as a message is. Each throws PstError
      // when the item itself cannot be read (PstTooLarge past a limit,
      // PstBudgetExhausted past the budget); a part that cannot (a photo, a
      // recurrence, a list of attendees) is left out and said in the item's
      // warnings, at most MaximumWarnings of them.
      void ReadContact(uint32_t nid, PstContact &contact);
      void ReadAppointment(uint32_t nid, PstAppointment &appointment);
      void ReadTask(uint32_t nid, PstTask &task);

      // A note (IPM.StickyNote) or a journal entry (IPM.Activity), taken as a
      // message: a journal entry's type, times and people are written above
      // its text. False, the message unchanged, for any other class. Damage
      // in the entry's fields is a warning; what the budget, the job's stop
      // and the memory throw (PstBudgetExhausted, PstStopped, std::bad_alloc)
      // is the caller's, as from ReadMessage.
      bool AsMessage(uint32_t nid, PstMessage &message);

      // The folders of notes and of journal entries, whose items AsMessage
      // takes.
      static bool IsMessageLikeFolderClass(const std::string &containerClass);

      // A contacts folder Exchange or Outlook keeps for itself: a sub-class of
      // IPF.Contact (IPF.Contact.RecipientCache, .GalContacts,
      // .OrganizationalContacts, .MOC.QuickContacts, .MOC.ImContactList,
      // .Company, .PeopleCentricConversationBuddies ...). A folder a person
      // makes is IPF.Contact itself.
      static bool IsSystemContactsClass(const std::string &containerClass);

      // The contacts, calendar and tasks folders the root folder names
      // ([MS-OXOSFLD] 2.2.3: PidTagIpmContactEntryId, PidTagIpmAppointmentEntryId,
      // PidTagIpmTaskEntryId), 0 where it names none. A unit of the work
      // budget; it throws only the whole file's budget running out and the
      // job's stop (PstStopped).
      void DefaultItemFolders(uint32_t &contacts, uint32_t &calendar, uint32_t &tasks);

      // The name-to-id map read now, once, as a unit of the work budget of
      // its own, for the contacts and calendars phases: a map that cannot be
      // read costs no item, and every item read without it says so. Throws
      // only the whole file's budget running out and the job's stop.
      void ReadNames();

      // Why the map could not be read, or empty.
      const std::string &NamesProblem() const { return names_problem_; }

      // The sentence an OST whose folders cannot be found is refused with.
      static std::string OfflineSentence();

   private:
      // The property id of a numeric named property of the file, or 0 when
      // its name-to-id map has none (PstStoreItems.cpp).
      uint16_t Named_(const std::string &guid, uint32_t lid);
      void ItemRecipients_(const PstPropertyContext &pc, int codePage, PstAppointment &appointment);

      // A contact, calendar item or task begun: a unit of the work budget of
      // its own, and the name-to-id map free to be tried again; and, as it
      // ends, a warning in it when the map could not be read for it.
      void BeginItem_();
      void NamesWarning_(std::vector<PstWarning> &warnings) const;

      // What is left of one message's limits, shared by its embedded messages.
      struct Reading
      {
         Reading() : bytes(0), parts(0) { }

         size_t bytes;       // of MaximumMessageBytes
         size_t parts;       // attachments and embedded messages read so far
      };

      void ReadMessage_(const PstPropertyContext &pc, uint32_t nid, int depth, Reading &reading, PstMessage &message);
      void ReadRecipients_(const PstPropertyContext &pc, int codePage, Reading &reading, PstMessage &message);
      void ReadAttachments_(const PstPropertyContext &pc, int depth, Reading &reading, PstMessage &message);
      void RolesFrom_(const PstPropertyContext &pc);

      // The message's ceiling, charged: PstTooLarge past it.
      static void Charge_(Reading &reading, size_t bytes);

      // A warning kept, cut to MaximumWarningBytes; past MaximumWarnings, one
      // more that says the rest were not kept, and then nothing.
      static void Warn_(PstMessage &message, const std::string &word, const std::string &detail);

      PstFile file_;
      std::string record_key_;
      std::string display_name_;
      uint32_t ipm_subtree_;
      uint32_t sent_;
      uint32_t trash_;
      uint32_t drafts_;
      uint32_t junk_;
      uint16_t keywords_id_;

      // The numeric named properties, (GUID, LID) -> property id, read the
      // first time one is asked for. A reading that fails leaves the map
      // unread, for the next item to try again, MaximumNameMapAttempts times
      // in all (names_failed_now_: it failed for the item being read, which
      // does not try again); names_problem_ says why the last one failed.
      std::shared_ptr<std::map<std::pair<std::string, uint32_t>, uint16_t> > named_;
      int named_attempts_ = 0;
      bool names_failed_now_ = false;
      std::string names_problem_;

      static const int MaximumNameMapAttempts = 3;
   };
}
