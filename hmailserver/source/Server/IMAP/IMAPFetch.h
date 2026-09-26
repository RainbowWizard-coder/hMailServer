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

   };

}
