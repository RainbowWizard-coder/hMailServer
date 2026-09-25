// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstStore.h.

#include "StdAfx.h"

#include "PstStore.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace HM
{
   const int PstStore::MaximumFolderDepth;
   const int PstStore::MaximumEmbeddedDepth;
   const size_t PstStore::MaximumMessageBytes;
   const size_t PstStore::MaximumMessagesPerFolder;
   const size_t PstStore::MaximumParts;
   const size_t PstStore::MaximumRecipients;
   const size_t PstStore::MaximumWarnings;
   const size_t PstStore::MaximumWarningBytes;
   const size_t PstStore::MaximumFolders;
   const size_t PstStore::MaximumFolderNameBytes;
   const size_t PstStore::MaximumFolderPathBytes;
   const size_t PstStore::MaximumProblems;

   namespace
   {
      // [MS-OXPROPS]: the properties read here.
      const uint16_t PropMessageClass = 0x001A;
      const uint16_t PropSubject = 0x0037;
      const uint16_t PropClientSubmitTime = 0x0039;
      const uint16_t PropSentRepresentingName = 0x0042;
      const uint16_t PropSentRepresentingAddrType = 0x0064;
      const uint16_t PropSentRepresentingEmail = 0x0065;
      const uint16_t PropTransportHeaders = 0x007D;
      const uint16_t PropRecipientType = 0x0C15;
      const uint16_t PropSenderName = 0x0C1A;
      const uint16_t PropSenderAddrType = 0x0C1E;
      const uint16_t PropSenderEmail = 0x0C1F;
      const uint16_t PropDeliveryTime = 0x0E06;
      const uint16_t PropMessageFlags = 0x0E07;
      const uint16_t PropRecordKey = 0x0FF9;
      const uint16_t PropBody = 0x1000;
      const uint16_t PropRtfCompressed = 0x1009;
      const uint16_t PropHtml = 0x1013;
      const uint16_t PropInternetMessageId = 0x1035;
      const uint16_t PropInternetReferences = 0x1039;
      const uint16_t PropInReplyTo = 0x1042;
      const uint16_t PropLastVerbExecuted = 0x1081;
      const uint16_t PropFlagStatus = 0x1090;
      const uint16_t PropAttrHidden = 0x10F4;
      const uint16_t PropDisplayName = 0x3001;
      const uint16_t PropAddrType = 0x3002;
      const uint16_t PropEmailAddress = 0x3003;
      const uint16_t PropCreationTime = 0x3007;
      const uint16_t PropIpmSubtree = 0x35E0;
      const uint16_t PropIpmWastebasket = 0x35E3;
      const uint16_t PropIpmSentMail = 0x35E4;
      const uint16_t PropContentCount = 0x3602;
      const uint16_t PropContainerClass = 0x3613;
      const uint16_t PropIpmDrafts = 0x36D7;
      const uint16_t PropAdditionalRenEntryIds = 0x36D8;
      const uint16_t PropAttachDataBinary = 0x3701;
      const uint16_t PropAttachFilename = 0x3704;
      const uint16_t PropAttachMethod = 0x3705;
      const uint16_t PropAttachLongFilename = 0x3707;
      const uint16_t PropAttachMimeTag = 0x370E;
      const uint16_t PropAttachContentId = 0x3712;
      const uint16_t PropSmtpAddress = 0x39FE;
      const uint16_t PropInternetCodePage = 0x3FDE;
      const uint16_t PropMessageCodePage = 0x3FFD;
      const uint16_t PropSenderSmtpAddress = 0x5D01;
      const uint16_t PropSentRepresentingSmtpAddress = 0x5D02;
      const uint16_t PropPstPassword = 0x67FF;
      const uint16_t PropAttachmentHidden = 0x7FFE;

      // [MS-PST] 2.4.7: the name-to-id map's streams.
      const uint16_t PropNameidStreamEntry = 0x0003;
      const uint16_t PropNameidStreamString = 0x0004;

      const size_t ShortText = 64 * 1024;
      const size_t LongText = 64 * 1024 * 1024;
      const size_t HeaderText = 4 * 1024 * 1024;

      bool StartsWithNoCase(const std::string &text, const char *prefix)
      {
         size_t i = 0;
         for (; prefix[i] != '\0'; i++)
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

      bool EqualsNoCase(const std::string &text, const char *other)
      {
         return text.size() == strlen(other) && StartsWithNoCase(text, other);
      }

      // An address a PST holds, as mail can use it: an SMTP address as it is,
      // and for any other kind - an Exchange (X.500) DN above all - the SMTP
      // address the PST keeps beside it, or none.
      std::string SmtpOf(const std::string &addressType, const std::string &address, const std::string &smtp)
      {
         if (!smtp.empty() && smtp.find('@') != std::string::npos)
            return smtp;
         if ((addressType.empty() || EqualsNoCase(addressType, "SMTP")) && address.find('@') != std::string::npos &&
             address.find('/') == std::string::npos)
            return address;
         return std::string();
      }

      std::string OptionalText(const PstPropertyContext &pc, uint16_t id, size_t limit)
      {
         std::string value;
         pc.Text(id, limit, value);
         return value;
      }

      std::string NodeText(uint32_t nid)
      {
         return "0x" + PstFormat::Hex32(nid);
      }

      // One of a message's own limits (PstStore.h): the message is refused.
      // Its own class, so that no per-part catch below takes it for one part's
      // damage and turns it into a warning.
      class MessageCeiling : public PstTooLarge
      {
      public:
         explicit MessageCeiling(const std::string &what) : PstTooLarge(what) { }
      };

      const char *const TooLargeSentence = "The message is larger than this reader takes (128 MB, its attachments and embedded messages included).";
   }

   void
   PstStore::Charge_(Reading &reading, size_t bytes)
   {
      if (bytes > reading.bytes)
         throw MessageCeiling(TooLargeSentence);
      reading.bytes -= bytes;
   }

   void
   PstStore::Warn_(PstMessage &message, const std::string &word, const std::string &detail)
   {
      if (message.warnings.size() + 1 < MaximumWarnings)
      {
         message.warnings.push_back(PstWarning(word, PstFormat::CutUtf8(detail, MaximumWarningBytes)));
      }
      else if (message.warnings.size() + 1 == MaximumWarnings)
      {
         message.warnings.push_back(PstWarning("warnings_left_out",
            "the message had more than " + std::to_string(MaximumWarnings) + " warnings; the rest were not kept"));
      }
   }

   PstStore::PstStore(std::shared_ptr<PstByteSource> source) :
      file_(source),
      ipm_subtree_(0),
      sent_(0),
      trash_(0),
      drafts_(0),
      junk_(0),
      keywords_id_(0)
   {
   }

   bool
   PstStore::PasswordMatches(const std::string &password, uint32_t stored)
   {
      return PstFormat::Crc(password) == stored || PstFormat::Crc(PstFormat::Utf8ToUtf16(password)) == stored;
   }

   std::string
   PstStore::WithoutSubjectMarker(const std::string &subject)
   {
      // [MS-PST] 2.5.3.1.1.1: a subject whose first character is 0x01 has the
      // prefix's length as its second; both are the store's, not the subject's.
      if (subject.size() < 2 || subject[0] != '\x01')
         return subject;

      size_t second = 1;
      const unsigned char lead = (unsigned char) subject[second];
      size_t length = 1;
      if ((lead & 0xE0) == 0xC0) length = 2;
      else if ((lead & 0xF0) == 0xE0) length = 3;
      else if ((lead & 0xF8) == 0xF0) length = 4;

      if (second + length > subject.size())
         return std::string();

      return subject.substr(second + length);
   }

   void
   PstStore::RolesFrom_(const PstPropertyContext &pc)
   {
      std::string entryId;
      uint32_t nid = 0;

      if (drafts_ == 0 && pc.Bytes(PropIpmDrafts, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
         drafts_ = nid;

      std::vector<std::string> entryIds;
      if (junk_ == 0 && pc.MultiBinary(PropAdditionalRenEntryIds, 64 * 1024, entryIds) && entryIds.size() > 4 &&
          PstFormat::NidOfEntryId(entryIds[4], record_key_, nid))
         junk_ = nid;
   }

   void
   PstStore::Open(const std::string &password)
   {
      file_.Budget().BeginUnit();
      file_.Open();

      PstFile::Node node;
      if (!file_.FindNode(PstNidMessageStore, node))
         throw PstError("The file has no message store.");

      PstPropertyContext store;
      store.Read(file_, node.data, node.sub);

      const bool hasRecordKey = store.Bytes(PropRecordKey, 64, record_key_) && record_key_.size() == 16;

      std::string entryId;
      uint32_t nid = 0;

      // An OST is read as a PST where it names its folders as one does (a PST
      // EntryID, [MS-PST] 2.4.3.2, which the store's record key is part of).
      // One that caches an Exchange mailbox names them by the server's own
      // EntryIDs, which [MS-PST] does not map to the file's nodes: refused,
      // with what to do - before anything else of its store is asked, so that
      // such a file is never refused as damaged or as protected instead.
      if (file_.IsOffline() &&
          (!hasRecordKey || !store.Bytes(PropIpmSubtree, 64, entryId) || !PstFormat::NidOfEntryId(entryId, record_key_, nid)))
         throw PstRefusal(PstRefusal::ReasonOffline, OfflineSentence());

      if (!hasRecordKey)
         throw PstError("The file's message store has no record key.");

      display_name_ = OptionalText(store, PropDisplayName, ShortText);

      // [MS-PST] 2.4.3.2: PidTagPstPassword, when set, is the CRC of the
      // password. It protects nothing - the file is not encrypted with it - but
      // it is what the file's owner asked for, and it is kept.
      int32_t stored = 0;
      if (store.Int32(PropPstPassword, stored) && stored != 0)
      {
         if (password.empty())
            throw PstRefusal(PstRefusal::ReasonPasswordNeeded, "This PST is protected with a password; give it as the job's secret.");
         if (!PasswordMatches(password, (uint32_t) stored))
            throw PstRefusal(PstRefusal::ReasonPasswordWrong, "The password given is not this PST's; give the right one as the job's secret.");
      }

      if (!store.Bytes(PropIpmSubtree, 64, entryId) || !PstFormat::NidOfEntryId(entryId, record_key_, nid))
         throw PstError("The file's message store names no folder tree (PR_IPM_SUBTREE_ENTRYID).");
      ipm_subtree_ = nid;

      if (store.Bytes(PropIpmWastebasket, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
         trash_ = nid;
      if (store.Bytes(PropIpmSentMail, 64, entryId) && PstFormat::NidOfEntryId(entryId, record_key_, nid))
         sent_ = nid;

      // Drafts and Junk E-mail are named on the root folder.
      PstFile::Node root;
      if (file_.FindNode(PstNidRootFolder, root))
      {
         try
         {
            PstPropertyContext rootProperties;
            rootProperties.Read(file_, root.data, root.sub);
            RolesFrom_(rootProperties);
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstError &)
         {
            // A damaged root folder costs the two roles, not the file.
         }
      }

      // [MS-PST] 2.4.7: the name-to-id map, for the categories - PS_PUBLIC_STRINGS
      // (GUID index 2) "Keywords". Each NAMEID is dwPropertyID (the string's
      // offset in the string stream, for a named one), a word whose low bit says
      // it is named by string and whose other fifteen are the GUID index, and
      // wPropIdx; its property id is 0x8000 + wPropIdx.
      PstFile::Node names;
      if (file_.FindNode(PstNidNameToIdMap, names))
      {
         try
         {
            PstPropertyContext map;
            map.Read(file_, names.data, names.sub);

            std::string entries;
            std::string strings;
            if (map.Bytes(PropNameidStreamEntry, 16 * 1024 * 1024, entries) && map.Bytes(PropNameidStreamString, 16 * 1024 * 1024, strings))
            {
               const std::string keywords = PstFormat::Utf8ToUtf16("Keywords");
               for (size_t at = 0; at + 8 <= entries.size(); at += 8)
               {
                  const uint32_t identifier = PstFormat::U32(entries, at, "the name-to-id map");
                  const uint16_t kind = PstFormat::U16(entries, at + 4, "the name-to-id map");
                  const uint16_t index = PstFormat::U16(entries, at + 6, "the name-to-id map");
                  if ((kind & 1) == 0 || (kind >> 1) != 2 || !PstFormat::Fits(identifier, 4, strings.size()))
                     continue;

                  const uint32_t length = PstFormat::U32(strings, identifier, "the name-to-id map's strings");
                  if (!PstFormat::Fits((uint64_t) identifier + 4, length, strings.size()))
                     continue;

                  if (strings.compare(identifier + 4, length, keywords) == 0)
                  {
                     keywords_id_ = (uint16_t) (0x8000 + index);
                     break;
                  }
               }
            }
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstError &)
         {
            // Without the map, no categories; the mail is the same.
         }
      }
   }

   bool
   PstStore::IsMailFolderClass(const std::string &containerClass)
   {
      if (containerClass.empty() || EqualsNoCase(containerClass, "IPF.Note") || EqualsNoCase(containerClass, "IPF.Imap"))
         return true;
      return StartsWithNoCase(containerClass, "IPF.Note.");
   }

   bool
   PstStore::IsMailMessageClass(const std::string &messageClass)
   {
      if (messageClass.empty() || EqualsNoCase(messageClass, "IPM") || EqualsNoCase(messageClass, "IPM.Note") ||
          EqualsNoCase(messageClass, "IPM.Post"))
         return true;

      return StartsWithNoCase(messageClass, "IPM.Note.") || StartsWithNoCase(messageClass, "IPM.Post.") ||
             StartsWithNoCase(messageClass, "IPM.Schedule.") || StartsWithNoCase(messageClass, "REPORT.");
   }

   void
   PstStore::ListFolders(std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems)
   {
      folders.clear();
      problems.clear();

      // A folder waiting to be read: its NID, its parent's, where its parent is
      // in folders (-1 for a top-level folder) and how deep it is. Its path is
      // made from its parent's only when it is listed, so that what waits costs
      // a few bytes a folder, however long the names above it.
      struct Pending
      {
         uint32_t nid;
         uint32_t parent;
         int parentIndex;
         int depth;
      };

      std::set<uint32_t> visited;
      visited.insert(ipm_subtree_);

      size_t pathBytes = 0;
      size_t problemsLeftOut = 0;
      auto problem = [&](const std::string &sentence)
      {
         if (problems.size() < MaximumProblems)
            problems.push_back(sentence);
         else
            problemsLeftOut++;
      };

      // Where a folder is, for a sentence: " under" its parent's path, cut short.
      auto under = [&](int parentIndex) -> std::string
      {
         if (parentIndex < 0)
            return std::string();
         std::string path;
         for (const std::string &part : folders[(size_t) parentIndex].path)
         {
            path += (path.empty() ? std::string() : std::string(" / ")) + PstFormat::CutUtf8(part, 255);
            if (path.size() > 255)
               break;
         }
         return " under " + PstFormat::CutUtf8(path, 255);
      };

      // Depth first, children in the order their parent's table lists them,
      // parents before children; an explicit stack, so no folder tree can run
      // this out of its own.
      std::vector<Pending> stack;
      Pending top;
      top.nid = ipm_subtree_;
      top.parent = 0;
      top.parentIndex = -1;
      top.depth = 0;
      stack.push_back(top);

      bool inboxRolesRead = false;

      while (!stack.empty())
      {
         const Pending current = stack.back();
         stack.pop_back();

         // A folder and its list of children are one unit of the work budget.
         file_.Budget().BeginUnit();

         int ownIndex = -1;
         if (current.nid != ipm_subtree_)
         {
            PstFolderInfo folder;
            folder.nid = current.nid;
            folder.parentNid = current.parent;

            try
            {
               PstFile::Node node;
               if (!file_.FindNode(current.nid, node))
                  throw PstError("its node is not in the file");

               PstPropertyContext properties;
               properties.Read(file_, node.data, node.sub);

               std::string name;
               properties.Text(PropDisplayName, ShortText, name);
               name = PstFormat::CutUtf8(name, MaximumFolderNameBytes);
               properties.Text(PropContainerClass, ShortText, folder.containerClass);
               folder.containerClass = PstFormat::CutUtf8(folder.containerClass, 255);
               properties.Int32(PropContentCount, folder.contentCount);
               folder.mail = IsMailFolderClass(folder.containerClass);
               properties.Boolean(PropAttrHidden, folder.hidden);

               if (current.parentIndex >= 0)
                  folder.path = folders[(size_t) current.parentIndex].path;
               folder.path.push_back(name);

               // Drafts and Junk E-mail on the Inbox, where the root did not
               // name them.
               if (!inboxRolesRead && current.depth == 1 && EqualsNoCase(name, "Inbox") && (drafts_ == 0 || junk_ == 0))
               {
                  inboxRolesRead = true;
                  RolesFrom_(properties);
               }
            }
            catch (PstError &error)
            {
               // The work budget run out for the whole file is the file's end;
               // for this folder alone, this folder's.
               if (file_.Budget().FileExhausted())
                  throw;

               problem("The folder " + NodeText(current.nid) + under(current.parentIndex) +
                       " could not be read, and nothing in it was: " + error.what());
               continue;
            }

            size_t bytes = 0;
            for (const std::string &part : folder.path)
               bytes += part.size() + 1;
            if (bytes > MaximumFolderPathBytes - pathBytes)
               throw PstError("The names of the file's folders add up to more than this reader takes (" +
                              std::to_string(MaximumFolderPathBytes / (1024 * 1024)) + " MB).");
            pathBytes += bytes;

            folders.push_back(folder);
            ownIndex = (int) folders.size() - 1;
         }

         if (current.depth >= MaximumFolderDepth)
         {
            problem("The folders below " + NodeText(current.nid) + " are nested deeper than " +
                    std::to_string(MaximumFolderDepth) + " levels and were not read.");
            continue;
         }

         // The children, from the folder's hierarchy table: normal folders
         // only - a search folder's messages are other folders'.
         std::vector<Pending> children;
         bool tooMany = false;
         try
         {
            PstFile::Node table;
            if (file_.FindNode((current.nid & ~0x1Fu) | PstNidTypeHierarchyTable, table))
            {
               PstTable hierarchy;
               hierarchy.Read(file_, table.data, table.sub, MaximumFolders);
               for (size_t row = 0; row < hierarchy.RowCount(); row++)
               {
                  const uint32_t child = hierarchy.RowId(row);
                  if ((child & 0x1F) != PstNidTypeNormalFolder || !visited.insert(child).second)
                     continue;

                  if (visited.size() > MaximumFolders + 1)
                  {
                     tooMany = true;
                     break;
                  }

                  Pending next;
                  next.nid = child;
                  next.parent = current.nid;
                  next.parentIndex = ownIndex;
                  next.depth = current.depth + 1;
                  children.push_back(next);
               }
            }
         }
         catch (PstError &error)
         {
            if (file_.Budget().FileExhausted())
               throw;

            problem("The list of folders under " + NodeText(current.nid) + " could not be read: " + error.what());
         }

         if (tooMany)
            throw PstError("The file names more than " + std::to_string(MaximumFolders) + " folders, which is more than this reader takes.");

         for (std::vector<Pending>::reverse_iterator it = children.rbegin(); it != children.rend(); ++it)
            stack.push_back(*it);
      }

      if (problemsLeftOut > 0)
         problems.push_back("And " + std::to_string(problemsLeftOut) + " more folders or lists of folders could not be read.");

      for (PstFolderInfo &folder : folders)
      {
         if (folder.nid == sent_)
            folder.role = PstFolderInfo::RoleSent;
         else if (folder.nid == trash_)
            folder.role = PstFolderInfo::RoleTrash;
         else if (folder.nid == drafts_)
            folder.role = PstFolderInfo::RoleDrafts;
         else if (folder.nid == junk_)
            folder.role = PstFolderInfo::RoleJunk;
      }
   }

   void
   PstStore::ListMessages(uint32_t folderNid, std::vector<uint32_t> &messages)
   {
      messages.clear();

      file_.Budget().BeginUnit();

      PstFile::Node node;
      if (!file_.FindNode((folderNid & ~0x1Fu) | PstNidTypeContentsTable, node))
         return;

      PstTable contents;
      contents.Read(file_, node.data, node.sub, MaximumMessagesPerFolder);
      for (size_t row = 0; row < contents.RowCount(); row++)
      {
         const uint32_t nid = contents.RowId(row);
         if ((nid & 0x1F) == PstNidTypeNormalMessage)
            messages.push_back(nid);
      }

      std::sort(messages.begin(), messages.end());
      messages.erase(std::unique(messages.begin(), messages.end()), messages.end());
   }

   void
   PstStore::ReadMessage(uint32_t nid, PstMessage &message)
   {
      message = PstMessage();

      // A message read whole is one unit of the work budget.
      file_.Budget().BeginUnit();

      PstFile::Node node;
      if (!file_.FindNode(nid, node))
         throw PstError("The message's node is not in the file.");

      PstPropertyContext pc;
      pc.Read(file_, node.data, node.sub);

      Reading reading;
      reading.bytes = MaximumMessageBytes;
      ReadMessage_(pc, nid, 0, reading, message);
   }

   void
   PstStore::ReadMessage_(const PstPropertyContext &pc, uint32_t nid, int depth, Reading &reading, PstMessage &message)
   {
      message.nid = nid;

      // The code page of the message's eight-bit strings: its own, else its
      // Internet one, else Western European, which is what Outlook writes when
      // it names none.
      int32_t codePage = 0;
      int32_t internetCodePage = 0;
      const bool hasInternet = pc.Int32(PropInternetCodePage, internetCodePage);
      if (!pc.Int32(PropMessageCodePage, codePage) || codePage <= 0)
         codePage = hasInternet && internetCodePage > 0 ? internetCodePage : 1252;

      bool undecoded = false;
      auto text = [&](uint16_t id, size_t limit, std::string &value) -> bool
      {
         bool decoded = true;
         if (!pc.Text(id, codePage, limit, value, decoded))
            return false;
         if (!decoded && !undecoded)
         {
            undecoded = true;
            Warn_(message, "text_not_decoded",
               "text in code page " + std::to_string(codePage) + ", which this reader does not convert, kept as its ASCII");
         }
         return true;
      };

      // A property the message can do without - its subject, its headers,
      // an identifier, a sender's name - that is larger than its limit is left
      // out with a warning, rather than the message with it.
      auto optional = [&](uint16_t id, size_t limit, std::string &value, const char *what) -> bool
      {
         try
         {
            return text(id, limit, value);
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstTooLarge &)
         {
            value.clear();
            Warn_(message, "property_too_large", std::string(what) + " is larger than this reader takes (" +
                  std::to_string(limit / 1024) + " KB) and was left out");
            return false;
         }
      };

      optional(PropMessageClass, ShortText, message.messageClass, "its message class");

      std::string subject;
      if (optional(PropSubject, ShortText, subject, "its subject"))
         message.subject = WithoutSubjectMarker(subject);

      // The large values - the headers and the three bodies - are read within
      // what is left of the message's ceiling as well as their own limit, and
      // charged as each is read, so that no more than the ceiling is ever
      // allocated for them.
      auto within = [&](size_t limit) -> size_t
      {
         return limit < reading.bytes ? limit : reading.bytes;
      };

      message.hasTransportHeaders = optional(PropTransportHeaders, HeaderText, message.transportHeaders, "its Internet headers");
      Charge_(reading, message.transportHeaders.size());

      message.hasBody = text(PropBody, within(LongText), message.body);
      Charge_(reading, message.body.size());

      uint16_t htmlType = 0;
      if (pc.TypeOf(PropHtml, htmlType))
      {
         if (htmlType == PstTypeString || htmlType == PstTypeString8)
         {
            message.hasHtml = text(PropHtml, within(LongText), message.html);
            message.htmlCodePage = 65001;
         }
         else
         {
            message.hasHtml = pc.Bytes(PropHtml, within(LongText), message.html);
            message.htmlCodePage = hasInternet ? internetCodePage : -1;
         }
         Charge_(reading, message.html.size());
      }

      message.hasRtf = pc.Bytes(PropRtfCompressed, within(LongText), message.rtfCompressed);
      Charge_(reading, message.rtfCompressed.size());

      optional(PropInternetMessageId, ShortText, message.messageId, "its Message-ID");
      optional(PropInReplyTo, ShortText, message.inReplyTo, "its In-Reply-To");
      optional(PropInternetReferences, ShortText, message.references, "its References");

      std::string addressType, address, smtp;
      optional(PropSenderName, ShortText, message.senderName, "its sender's name");
      optional(PropSenderAddrType, ShortText, addressType, "its sender's address type");
      optional(PropSenderEmail, ShortText, address, "its sender's address");
      optional(PropSenderSmtpAddress, ShortText, smtp, "its sender's SMTP address");
      message.senderAddress = SmtpOf(addressType, address, smtp);

      addressType.clear();
      address.clear();
      smtp.clear();
      optional(PropSentRepresentingName, ShortText, message.representingName, "the name it was sent on behalf of");
      optional(PropSentRepresentingAddrType, ShortText, addressType, "the address type it was sent on behalf of");
      optional(PropSentRepresentingEmail, ShortText, address, "the address it was sent on behalf of");
      optional(PropSentRepresentingSmtpAddress, ShortText, smtp, "the SMTP address it was sent on behalf of");
      message.representingAddress = SmtpOf(addressType, address, smtp);

      pc.Time(PropClientSubmitTime, message.submitTime);
      pc.Time(PropDeliveryTime, message.deliveryTime);
      pc.Time(PropCreationTime, message.creationTime);

      pc.Int32(PropMessageFlags, message.messageFlags);
      pc.Int32(PropFlagStatus, message.flagStatus);
      pc.Int32(PropLastVerbExecuted, message.lastVerb);

      if (keywords_id_ != 0)
      {
         try
         {
            pc.MultiText(keywords_id_, codePage, ShortText, message.categories);
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstError &error)
         {
            message.categories.clear();
            Warn_(message, "categories_unreadable", std::string("the categories could not be read: ") + error.what());
         }
      }

      // The rest of what is kept counts against the message's own ceiling too.
      size_t kept = message.messageClass.size() + message.subject.size() + message.messageId.size() +
                    message.inReplyTo.size() + message.references.size() + message.senderName.size() +
                    message.senderAddress.size() + message.representingName.size() + message.representingAddress.size();
      for (const std::string &category : message.categories)
         kept += category.size() + sizeof(std::string);
      Charge_(reading, kept);

      ReadRecipients_(pc, codePage, reading, message);
      ReadAttachments_(pc, depth, reading, message);
   }

   void
   PstStore::ReadRecipients_(const PstPropertyContext &pc, int codePage, Reading &reading, PstMessage &message)
   {
      std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = pc.GetSubnodes().find(PstNidRecipientTable);
      if (subnode == pc.GetSubnodes().end())
         return;

      try
      {
         PstTable table;
         table.Read(file_, subnode->second.data, subnode->second.sub, 100000);

         // At most MaximumRecipients, each cell at most ShortText, and what
         // each keeps charged to the message's ceiling.
         const size_t rows = table.RowCount() < MaximumRecipients ? table.RowCount() : MaximumRecipients;
         for (size_t row = 0; row < rows; row++)
         {
            PstRecipient recipient;
            int32_t type = 1;
            if (table.CellInt32(row, PropRecipientType, type))
               recipient.type = type & 0x0F;

            std::string addressType, address, smtp;
            table.CellText(row, PropDisplayName, codePage, recipient.name, ShortText);
            table.CellText(row, PropAddrType, codePage, addressType, ShortText);
            table.CellText(row, PropEmailAddress, codePage, address, ShortText);
            table.CellText(row, PropSmtpAddress, codePage, smtp, ShortText);
            recipient.address = SmtpOf(addressType, address, smtp);

            Charge_(reading, sizeof(PstRecipient) + recipient.name.size() + recipient.address.size());
            message.recipients.push_back(recipient);
         }

         if (table.RowCount() > rows)
         {
            Warn_(message, "recipients_left_out", "the message lists " + std::to_string(table.RowCount()) +
                  " recipients; the first " + std::to_string(MaximumRecipients) + " were kept");
         }
      }
      catch (MessageCeiling &)
      {
         throw;
      }
      catch (PstBudgetExhausted &)
      {
         throw;
      }
      catch (PstError &error)
      {
         message.recipients.clear();
         Warn_(message, "recipients_unreadable", std::string("the list of recipients could not be read: ") + error.what());
      }
   }

   void
   PstStore::ReadAttachments_(const PstPropertyContext &pc, int depth, Reading &reading, PstMessage &message)
   {
      std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = pc.GetSubnodes().find(PstNidAttachmentTable);
      if (subnode == pc.GetSubnodes().end())
         return;

      // The attachments' NIDs, each once: a table that lists one attachment
      // again and again names one attachment.
      std::vector<uint32_t> nids;
      try
      {
         // Read up to ten times what may be taken, so that a message of more
         // than MaximumParts attachments is refused as that, below, rather
         // than read without its attachments.
         PstTable table;
         table.Read(file_, subnode->second.data, subnode->second.sub, MaximumParts * 10);
         std::set<uint32_t> seen;
         for (size_t row = 0; row < table.RowCount(); row++)
         {
            const uint32_t nid = table.RowId(row);
            if (seen.insert(nid).second)
               nids.push_back(nid);
         }
      }
      catch (PstBudgetExhausted &)
      {
         throw;
      }
      catch (PstError &error)
      {
         Warn_(message, "attachments_unreadable", std::string("the list of attachments could not be read: ") + error.what());
         return;
      }

      for (uint32_t nid : nids)
      {
         // Every attachment and embedded message, at every depth, counts
         // against the one message's MaximumParts: an embedded message that
         // holds itself, or a chain of them each listing the next many times,
         // ends there rather than growing as a power of its depth.
         if (++reading.parts > MaximumParts)
            throw MessageCeiling("The message holds more than " + std::to_string(MaximumParts) +
                                 " attachments and embedded messages, counting those inside its embedded messages; this reader takes no more.");
         file_.Budget().Item();

         PstAttachment attachment;
         attachment.nid = nid;

         try
         {
            std::map<uint32_t, PstFile::Subnode>::const_iterator own = pc.GetSubnodes().find(nid);
            if (own == pc.GetSubnodes().end())
               throw PstError("its node is not in the message");

            PstPropertyContext properties;
            properties.Read(file_, own->second.data, own->second.sub);

            int32_t method = 0;
            properties.Int32(PropAttachMethod, method);
            attachment.method = method;

            if (!properties.Text(PropAttachLongFilename, ShortText, attachment.fileName) || attachment.fileName.empty())
            {
               if (!properties.Text(PropAttachFilename, ShortText, attachment.fileName) || attachment.fileName.empty())
                  properties.Text(PropDisplayName, ShortText, attachment.fileName);
            }
            properties.Text(PropAttachMimeTag, ShortText, attachment.mimeType);
            properties.Text(PropAttachContentId, ShortText, attachment.contentId);
            properties.Boolean(PropAttachmentHidden, attachment.hidden);

            Charge_(reading, sizeof(PstAttachment) + attachment.fileName.size() + attachment.mimeType.size() + attachment.contentId.size());

            if (method == 1)
            {
               // By value: the bytes, within what is left of the message's
               // ceiling. An attachment with no data is an empty file.
               bool present = false;
               try
               {
                  present = properties.Bytes(PropAttachDataBinary, reading.bytes, attachment.data);
               }
               catch (PstBudgetExhausted &)
               {
                  throw;
               }
               catch (PstTooLarge &)
               {
                  // Past what is left of the message's ceiling: the message
                  // is not taken, rather than taken without it.
                  throw MessageCeiling(TooLargeSentence);
               }
               if (present)
                  Charge_(reading, attachment.data.size());
            }
            else if (method == 5)
            {
               if (depth + 1 >= MaximumEmbeddedDepth)
               {
                  Warn_(message, "embedded_too_deep",
                     "a message embedded more than " + std::to_string(MaximumEmbeddedDepth) + " deep was left out");
                  continue;
               }

               uint32_t objectNid = 0;
               if (!properties.ObjectNid(PropAttachDataBinary, objectNid))
                  throw PstError("the embedded message is not there");

               std::map<uint32_t, PstFile::Subnode>::const_iterator object = properties.GetSubnodes().find(objectNid);
               if (object == properties.GetSubnodes().end())
                  throw PstError("the embedded message's node is not there");

               PstPropertyContext inner;
               inner.Read(file_, object->second.data, object->second.sub);

               attachment.embedded = std::make_shared<PstMessage>();
               ReadMessage_(inner, objectNid, depth + 1, reading, *attachment.embedded);

               for (const PstWarning &warning : attachment.embedded->warnings)
                  Warn_(message, warning.word, "in the embedded message: " + warning.detail);
            }
            else
            {
               // By reference (2, 3, 4), OLE (6), a web reference - the cloud
               // attachments Outlook writes for OneDrive links (7) - or none
               // (0): nothing a message can carry.
               Warn_(message, "attachment_not_imported",
                  "the attachment \"" + PstFormat::CutUtf8(attachment.fileName, 255) + "\" (method " + std::to_string(method) +
                  ", " + (method == 7 ? std::string("a web link") : method == 6 ? std::string("an OLE object") :
                          method == 0 ? std::string("no data") : std::string("a reference to a file")) + ") was not imported");
               continue;
            }
         }
         catch (MessageCeiling &)
         {
            throw;
         }
         catch (PstBudgetExhausted &)
         {
            throw;
         }
         catch (PstTooLarge &error)
         {
            // A value of the attachment, or of an embedded message, past its
            // limit: the message is not taken, rather than taken without it.
            throw MessageCeiling(std::string("An attachment of the message is larger than this reader takes: ") + error.what());
         }
         catch (PstError &error)
         {
            Warn_(message, "attachment_unreadable",
               "the attachment " + (attachment.fileName.empty() ? NodeText(nid) : "\"" + PstFormat::CutUtf8(attachment.fileName, 255) + "\"") +
               " could not be read: " + error.what());
            continue;
         }

         message.attachments.push_back(attachment);
      }
   }
}
