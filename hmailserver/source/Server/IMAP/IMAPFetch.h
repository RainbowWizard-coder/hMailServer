// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommandRangeAction.h"
#include "../Common/Mime/Mime.h"
#include "IMAPFetchParser.h"

namespace HM
{

   class IMAPConnection;
   class Message;
   class ByteBuffer;
   class IMAPFetchParser;


   class IMAPFetch : public IMAPCommandRangeAction
   {
      friend class IMAPFetchHeaderFieldsTester;
      friend class IMAPFetchSectionsTester;

   public:
      IMAPFetch();
      virtual ~IMAPFetch();

      virtual IMAPResult DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pMessage, const std::shared_ptr<IMAPCommandArgument> pArgument);

      // Fetch what still exists, then say that the rest is gone (RFC 2180 4.1.3).
      virtual MissingMessagePolicy GetMissingMessagePolicy() const { return MissingMessagePolicy::ReportAfterActing; }

      // FETCH sets \Seen on the message it returns, so it works on the object the
      // collection holds, as the single-message form always did.
      virtual bool UsesLiveMessages() const { return true; }

      // How many messages of the range the object store confirms it does not
      // have (MessageStore::Missing): left out of the answer, the rest fetched,
      // and the command answered NO (IMAPCommandFetch, IMAPCommandUID) - so a
      // message lost from the store never stops a client's sync at itself.
      int GetLost() const { return lost_; }

      // How many messages of the range the store holds but cannot read -
      // damaged (HM7350): left out the same way, so that one damaged message
      // does not stop every FETCH 1:* at itself, the rest fetched, and the
      // command answered NO. A closed mailbox key is not counted here: every
      // message of such a mailbox is unreadable until its owner signs in, and
      // the command stops with NO [UNAVAILABLE] at the first.
      int GetDamaged() const { return damaged_; }

      // The tagged NO a FETCH ends with when it left messages out (GetLost,
      // GetDamaged), or empty when it left none out.
      String GetLeftOutRefusal() const;


   private:

      int lost_ = 0;
      int damaged_ = 0;

      String CreateEnvelopeStructure_(MimeHeader& oHeader);
      String GetPartStructure_(std::shared_ptr<MimeBody> oPart, bool includeExtensionData, int iRecursion);
      String IteratePartRecursive_(std::shared_ptr<MimeBody> oPart, bool includeExtensionData, int iRecursion);

      // RFC 3501 7.4.2: an ENVELOPE address list - "(" 1*address ")" - for one
      // header field, or NIL when the field is absent or holds no address.
      String CreateAddressListStructure_(MimeHeader& oHeader, const char *fieldName);

      // An envelope string: quoted, and encoded where it cannot be sent as it is.
      // The "N" form answers NIL for an empty value.
      static String EnvelopeString_(const AnsiString &value);
      static String EnvelopeNString_(const AnsiString &value);

      // A BODYSTRUCTURE string from the message's header, quoted with its quote
      // and backslash escaped and a control character sent as a space.
      static String StructureString_(const String &value);

      // RFC 3501 7.4.2: body-fld-lines, the size of a body in text lines.
      static long CountTextLines_(const AnsiString &content);

      // Where a message's text begins, in its own bytes: after the first empty
      // line; 2 for a message that opens with one (an empty header), and the
      // whole length for one that has none (all header, no text).
      static size_t TextStartOf_(const char *data, size_t size);

      // A section of raw bytes - the whole, the header or the text - with the
      // client's <start.count> applied, into a buffer.
      std::shared_ptr<ByteBuffer> RawSection_(const char *data, size_t size, bool header, bool text, IMAPFetchParser::BodyPart &oPart);

      // The FLAGS data item for a message; includeSeen adds \Seen where this
      // FETCH is about to set it.
      static String CreateFlagsItem_(std::shared_ptr<Message> pMessage, bool includeSeen);
      std::shared_ptr<MimeBody>GetMessagePartByPartNo_(std::shared_ptr<MimeBody>pBody, long iPartNo);

      std::shared_ptr<ByteBuffer> GetByteBufferByBodyPart_(const String &messageFileName, std::shared_ptr<MimeBody> pBodyPart, IMAPFetchParser::BodyPart &oPart);
      std::shared_ptr<MimeBody> GetBodyPartByRecursiveIdentifier_(std::shared_ptr<MimeBody> pBody, const String &sName, bool loadEncapsulated = true);


      void GetBytesToSend_(int iBufferSize, IMAPFetchParser::BodyPart &oPart, int &iOutStart, int &iOutCount);

      void ReportCriticalError_(const String &messageFileName, const String &sMessage);

      // RFC 8970: the glance-sized body snippet the PREVIEW data item carries.
      String CreatePreviewText_(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<Message> pMessage);

      void AppendOutput_(String &sOutput, const String &sAppend);
      void SendAndReset_(std::shared_ptr<IMAPConnection> pConnection, String &sOutput);

      std::shared_ptr<MimeBody> LoadMimeBody_(std::shared_ptr<IMAPFetchParser> pParser, const String &fileName);
      bool GetMessageBodyNeeded_(std::shared_ptr<IMAPFetchParser> pParser);

      bool append_space_;

      std::shared_ptr<IMAPFetchParser> parser_;

      // A HEADER.FIELDS or HEADER.FIELDS.NOT part's list read once for the
      // command: the names asked for, each once, in the order asked, found by
      // their key (the bytes of the name as _stricmp folds them, which is how
      // the header's own lookup compares); and the names left out, by their key
      // as CompareNoCase folds them. Each section then reads the message's
      // header once, field by field. The list - a client's, of any length -
      // used to be copied and read whole for every message, and the header
      // searched for each name in it: the product of the list, the header and
      // the set.
      class HeaderFieldsLookup_
      {
      public:
         std::vector<String> names;
         std::map<std::string, std::vector<size_t> > by_key;
         std::set<String> excluded;
      };

      HeaderFieldsLookup_ &HeaderFieldsLookupFor_(IMAPFetchParser::BodyPart &oPart);
      static std::string StricmpKey_(const char *name);
      static String CompareNoCaseKey_(const String &name);

      std::map<const IMAPFetchParser::BodyPart *, HeaderFieldsLookup_> header_fields_lookups_;

      // A body a section's part number reached, kept for the other sections of
      // the message: the parts it holds, listed once; the message it
      // encapsulates, parsed once; and what its header says it is, asked once.
      // Each section walked the part list from its first part to the one it
      // named, parsed an encapsulated message afresh on its way through one,
      // and searched the part's header for its type and encoding: the product
      // of the sections and the parts, the encapsulated message or the header.
      // body is held so that its address names no other body while this stands.
      class NavigatedBody_
      {
      public:
         std::shared_ptr<MimeBody> body;

         bool parts_listed = false;
         std::vector<std::shared_ptr<MimeBody> > parts;

         bool encapsulated_loaded = false;
         std::shared_ptr<MimeBody> encapsulated;

         bool kind_known = false;
         bool encapsulated_message = false;
         bool multipart = false;
         const char *transfer_encoding = nullptr;
      };

      NavigatedBody_ &NavigatedBodyFor_(const std::shared_ptr<MimeBody> &body);
      const NavigatedBody_ &KindOf_(const std::shared_ptr<MimeBody> &body);

      // The message the bodies are of, held for the same reason, and the bodies.
      // Let go of at the end of each message's sections (ForgetMessageSections_).
      std::shared_ptr<MimeBody> navigated_message_;
      std::map<const MimeBody *, NavigatedBody_> navigated_bodies_;

      // What the latest section cost to make, beyond what it holds: the bytes it
      // read, looked through or made before its partial range was cut from it -
      // a message file read whole for its HEADER or TEXT, a header passed field
      // by field, a part written out, an encapsulated message parsed on the way.
      // DoAction holds the greater of the two to the ceiling a message's
      // sections share; a section that would pass it while parsing its way to a
      // part stops there and says so (section_refused_).
      size_t section_cost_ = 0;
      bool section_refused_ = false;

      // The ceiling of the sections of the message DoAction is reading - sixteen
      // times the larger of the message and its largest section, and a megabyte -
      // and what its sections have cost so far. Not set outside DoAction.
      bool ceiling_set_ = false;
      size_t ceiling_largest_ = 0;
      size_t ceiling_used_ = 0;
      String ceiling_file_;
      bool ceiling_file_sized_ = false;

      bool WithinCeiling_(size_t sectionCost);
      void ForgetMessageSections_();

   };

   // HEADER.FIELDS and HEADER.FIELDS.NOT sections as the names prepared once per
   // command answer them, held byte for byte to what 6.3.5's code - kept in the
   // tester, which looked each name up in the header for every message - made
   // of the same generated headers and name lists; and one command's sections
   // over many messages held to the time of a pass over each header's fields.
   // ClassTester runs it on Windows, imap-parse-tests on Linux.
   class IMAPFetchHeaderFieldsTester
   {
   public:
      void Test(int cases = 20000);
   };

   // The body a section's part number names, as the bodies kept for a message's
   // sections find it, held to what 6.3.5's walk - kept in the tester, the part
   // list walked from its first part and an encapsulated message parsed afresh
   // for every section - found over generated messages and part numbers, through
   // one IMAPFetch as one command's messages are; and many sections over a
   // message held to the time of reading it once. ClassTester runs it on
   // Windows, imap-parse-tests on Linux.
   class IMAPFetchSectionsTester
   {
   public:
      void Test(int cases = 20000);
   };

}
