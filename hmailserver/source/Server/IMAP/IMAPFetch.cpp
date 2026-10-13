// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPFetch.h"
#include "IMAPFetchParser.h"
#include "IMAPConnection.h"
#include "IMAPFolderView.h"

#include "../Common/Application/FolderManager.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/MessageData.h"
#include "../Common/BO/Messages.h"
#include "../Common/Mime/MimeCode.h"
#include "../Common/Util/Charset.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/Unicode.h"
#include "IMAPAddressList.h"
#include "../Common/Util/ByteBuffer.h"
#include "../Common/BO/ACLPermission.h"

#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Util/FileUtilities.h"

#include <boost/filesystem.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   IMAPFetch::IMAPFetch() :
      append_space_(false)
   {
      
   }

   IMAPFetch::~IMAPFetch()
   {

   }

   IMAPResult
   IMAPFetch::DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pMessage, const std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pArgument || !pMessage)
         return IMAPResult(IMAPResult::ResultBad, "Invalid parameters");

      const String messageFileName = PersistentMessage::GetFileName(pConnection->GetAccountOwningCurrentFolder(), pMessage);

      append_space_ = false;

      // Parse the command
      if (!parser_)
      {
         parser_ = std::shared_ptr<IMAPFetchParser>(new IMAPFetchParser());
         String sTemp = pArgument->Command();

         // The result of this parse was discarded, and it is the only thing that
         // reads the FETCH data items. So a command whose items do not parse -
         // mismatched brackets, an unterminated "<start.size>" - was answered
         // "* n FETCH ()" followed by "OK FETCH completed": the client is told its
         // command succeeded and handed nothing, which is the worst of the three
         // available answers, and it is the answer that makes ValidateSyntax_
         // pointless. RFC 3501 asks for BAD here.
         //
         // DoForMails stops on a non-OK result, so this is reported once for the
         // command rather than once per message in the range, and it is reported
         // before any part of the untagged response has been written.
         IMAPResult parseResult = parser_->ParseCommand(sTemp);

         if (parseResult.GetResult() != IMAPResult::ResultOK)
         {
            // The parser is built for the first message only and reused for the
            // rest of the range. Do not keep one built from input that did not
            // parse.
            parser_.reset();

            return parseResult;
         }

         // RFC 7162 section 3.1: MODSEQ and CHANGEDSINCE enable CONDSTORE, and the
         // first enabling command reports HIGHESTMODSEQ before any FETCH line.
         if (parser_->GetShowModSeq() || parser_->GetHasChangedSince())
            pConnection->OnCondstoreEnablingCommand();
      }

      // RFC 7162 (CONDSTORE): with a CHANGEDSINCE modifier, only return messages whose
      // mod-sequence is greater than the supplied value; silently skip the rest.
      if (parser_->GetHasChangedSince() && pMessage->GetModSeq() <= parser_->GetChangedSince())
         return IMAPResult();

      // If we're going to touch the file, make sure it's there.
      bool willReadMessageFile =
         parser_->GetShowEnvelope() ||
         parser_->GetShowBodyStructure() ||
         parser_->GetShowBodyStructureNonExtensible()||
         parser_->GetShowPreview() ||
         parser_->GetPartsToLookAt().size() > 0;

      // Whatever an earlier message on this thread met.
      MessageStore::ClearUnavailable();
      MessageStore::ClearUnreadable();

      if (willReadMessageFile)
      {
         // A file that is not there may belong to a message another session
         // has expunged since this command resolved it - the deleting session
         // removes the row and the file together. EnsureFileExistance would
         // then write a "file missing" placeholder and save the row again for
         // a message that no longer exists: the HM5026 errors of the lab's
         // imaptest load, several clients working on one mailbox. Such a
         // message is left out, like one expunged before the command began,
         // and this session hears of the expunge at its next opportunity
         // (RFC 2180 4.1.3). Asked only when the file is missing, so the
         // ordinary FETCH pays nothing for it.
         std::shared_ptr<IMAPFolder> pFolder = pConnection->GetCurrentFolder();

         if (pFolder && !MessageStore::Exists(messageFileName) &&
             !pFolder->GetMessages()->GetItemByDBID((unsigned __int64) pMessage->GetID()))
         {
            std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
            if (view)
               view->MarkVanished(pMessage->GetID());

            return IMAPResult();
         }

         // Ensure that the data file exists.
         // The folder's owner, not the reader - same reason as the path built
         // above. Asking under the reader's directory would find nothing and
         // rewrite the delegate a placeholder over the owner's mail.
         PersistentMessage::EnsureFileExistance(pConnection->GetAccountOwningCurrentFolder(), pMessage);

         // Lost from the object store for good (HM7476): nothing of this
         // message is sent, the rest of the range is fetched, and the command
         // ends NO (docs/ObjectStorage.md).
         if (MessageStore::Missing())
         {
            lost_++;
            return IMAPResult();
         }

         // The object store the message is kept in did not answer
         // (docs/ObjectStorage.md): asked here, before a byte of this message's
         // response is sent, and answered as RFC 5530 says a subsystem that is
         // down is - the client keeps what it has and asks again.
         if (MessageStore::Unavailable())
            return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message store cannot be reached just now. Try again in a moment.");
      }

      MimeHeader oMimeHeader;

      // Whether this FETCH sets \Seen (a non-PEEK BODY[], RFC822, RFC822.TEXT
      // ...). Decided before anything is written, so that a FLAGS item in the
      // same response already shows it, and so that a response without one can
      // carry it (RFC 3501 6.4.5: "if this causes the flags to change, they
      // SHOULD be included as part of the FETCH responses"). The flag itself is
      // still stored at the end, once the data has gone out.
      bool seenWillBeSet = false;
      if (parser_->GetSetSeenFlag() && !pConnection->GetCurrentFolderReadOnly() && !pMessage->GetFlagSeen())
         seenWillBeSet = pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteSeen);

      // RFC 3501 7.2.6: a keyword is announced in an untagged FLAGS before a
      // FETCH uses it. Done here, before any of this response is written - an
      // untagged FLAGS in the middle of a FETCH response would break it.
      if (parser_->GetShowFlags() || seenWillBeSet)
         pConnection->AnnounceKeywords(pMessage->GetKeywordList());

      // Time to output this...
      String sOutput;
   
      sOutput.Format(_T("* %d FETCH ("), messageIndex);

      // We should always show UID when client is issuing UID fetch..
      if (parser_->GetShowUID() || GetIsUID()) 
      {
         String sUIDTag;
         sUIDTag.Format(_T("UID %u"), pMessage->GetUID());
         
         AppendOutput_(sOutput, sUIDTag);
      }

      // RFC 7162 (CONDSTORE/QRESYNC): emit MODSEQ when the data item is requested.
      if (parser_->GetShowModSeq())
      {
         String sModSeq;
         sModSeq.Format(_T("MODSEQ (%I64d)"), pMessage->GetModSeq());
         AppendOutput_(sOutput, sModSeq);
      }

      if (parser_->GetShowRFCSize())
      {
         String sTemp;
         sTemp.Format(_T("RFC822.SIZE %d"), pMessage->GetSize());

         AppendOutput_(sOutput, sTemp);
      }

      if (parser_->GetShowFlags())
         AppendOutput_(sOutput, CreateFlagsItem_(pMessage, seenWillBeSet));


      if (parser_->GetShowInternalDate())
      {
         String sTemp = "INTERNALDATE \"";

         String sCreateTime = pMessage->GetCreateTime();
         
         if (!Time::SeemsToBeValidYear(sCreateTime))
            sCreateTime = Time::GetCurrentDateTime();

         String sMIMECreateTime = Time::GetIMAPInternalDate(sCreateTime);

         sTemp += sMIMECreateTime + "\"";

         AppendOutput_(sOutput, sTemp);
      }

      // RFC 8514: when the message was saved into this mailbox - a fresh date
      // on COPY, unlike INTERNALDATE, which COPY must preserve.
      if (parser_->GetShowSaveDate())
      {
         String sTemp = "SAVEDATE \"";

         String sSaveDate = pMessage->GetSaveDate();

         if (!Time::SeemsToBeValidYear(sSaveDate))
            sSaveDate = pMessage->GetCreateTime();
         if (!Time::SeemsToBeValidYear(sSaveDate))
            sSaveDate = Time::GetCurrentDateTime();

         sTemp += Time::GetIMAPInternalDate(sSaveDate) + "\"";

         AppendOutput_(sOutput, sTemp);
      }

      // RFC 8474: the stable email id - the same across COPY, unlike the UID.
      if (parser_->GetShowEmailId())
      {
         String sTemp;

         String sEmailId = pMessage->GetEmailId();
         if (sEmailId.IsEmpty())
            sTemp = "EMAILID NIL";
         else
            sTemp.Format(_T("EMAILID (%s)"), sEmailId.c_str());

         AppendOutput_(sOutput, sTemp);
      }

      // RFC 8474 section 5: a server without thread ids answers NIL rather
      // than inventing one.
      if (parser_->GetShowThreadId())
      {
         AppendOutput_(sOutput, _T("THREADID NIL"));
      }

      
      AnsiString sMessageHeader;

      if (parser_->GetShowEnvelope())
      {
         // Parse the MIME header
         if (sMessageHeader.IsEmpty())
         {
            sMessageHeader = PersistentMessage::LoadHeader(messageFileName);

            oMimeHeader.Load(sMessageHeader, sMessageHeader.length());
         }

         String sTemp;
         sTemp = "ENVELOPE " + CreateEnvelopeStructure_(oMimeHeader);
         AppendOutput_(sOutput, sTemp);

      }

      // Sometimes we need to load the entire mail into memory.
      // If the user want's to download a specific attachment or
      // if he wants to look at the structure of the mime message.
      //
      // Loaded here, before a byte of this message's response is sent: an
      // object store that stopped answering after the existence check above
      // leaves an empty header and an empty structure, which a client would
      // cache as the message's for good (docs/ObjectStorage.md). Answered NO
      // [UNAVAILABLE] instead, as the check above answers it.
      std::shared_ptr<MimeBody> pMimeBody = LoadMimeBody_(parser_, messageFileName);

      if (willReadMessageFile && MessageStore::Unavailable())
         return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message store cannot be reached just now. Try again in a moment.");

      // RFC 8970: a server-generated snippet of the message body, so a client
      // can render its message list without fetching any bodies.
      AnsiString previewUtf8;
      if (parser_->GetShowPreview())
      {
         String previewText = CreatePreviewText_(pConnection, pMessage);

         if (willReadMessageFile && MessageStore::Unavailable())
            return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message store cannot be reached just now. Try again in a moment.");

         Unicode::WideToMultiByte(previewText, previewUtf8);

         // RFC 8970 section 5: at most 256 octets. The character truncation in
         // CreatePreviewText_ keeps it short; this keeps it LEGAL for messages
         // whose characters are several UTF-8 octets each, cutting on a
         // character boundary so the last kept character stays whole.
         const size_t maxPreviewOctets = 256;
         if (previewUtf8.size() > maxPreviewOctets)
         {
            size_t cut = maxPreviewOctets;
            while (cut > 0 && (static_cast<unsigned char>(previewUtf8.c_str()[cut]) & 0xC0) == 0x80)
               cut--;
            previewUtf8 = previewUtf8.substr(0, cut);
         }
      }

      // Every part this message's response carries is read here, before any of
      // the response is sent. A part read after an earlier one had gone out as
      // a literal could fail only by leaving the untagged FETCH unfinished in
      // front of the tagged NO.
      std::vector<IMAPFetchParser::BodyPart> &vecPartsToPeekAt = parser_->GetPartsToLookAt();
      std::vector<std::shared_ptr<ByteBuffer> > partBuffers;

      // What the parts read so far hold, against a ceiling of sixteen times the
      // message - or the largest part read, should the size on record be short -
      // and a megabyte. Each part is at most about the message, so no FETCH a
      // client means comes near it; but the parts are all read before any is
      // sent, and a FETCH naming the whole message twenty-five thousand times -
      // "BODY.PEEK[]<0> BODY.PEEK[]<1> ...", a command line's worth - held
      // twenty-five thousand copies of it in memory at once: a quarter of a
      // terabyte for a message of ten megabytes, in the client's own mailbox.
      //
      // Each part counts what it holds or what it cost to make, whichever is
      // more (section_cost_): the message file read whole for its HEADER or
      // TEXT, a header passed field by field for HEADER.FIELDS, a part written
      // out or a header listed before a partial range cut a byte from it, an
      // encapsulated message parsed on the way to a part inside it. Counted by
      // what they held, "BODY.PEEK[HEADER]<0.1>" over a message of 20 MB held a
      // byte and read the 20 MB, each of the ten thousand times a command line
      // can name it, and HEADER.FIELDS, HEADER.FIELDS.NOT, MIME, TEXT and a
      // part's content cut short the same way read or made their header or
      // part whole: the product of the sections and the message, which the
      // ceiling now stops at about seventeen times the message. The bodies part
      // numbers reach are kept for the message's other sections, each part
      // list read once and each encapsulated message parsed once
      // (NavigatedBody_), so the sections a client means - every part of a
      // forwarded message, every MIME header of a message of thousands of
      // parts - cost about what reading the message once costs and stay far
      // under it. Let go of at the end of the message's sections.
      size_t largestPart = (size_t) (pMessage->GetSize() > 0 ? pMessage->GetSize() : 0);

      class SectionsOfOneMessage
      {
      public:
         explicit SectionsOfOneMessage(IMAPFetch &fetch) : fetch_(fetch) {}
         ~SectionsOfOneMessage() { fetch_.ForgetMessageSections_(); }
      private:
         IMAPFetch &fetch_;
      };

      SectionsOfOneMessage sectionsOfOneMessage(*this);

      ForgetMessageSections_();
      ceiling_set_ = true;
      ceiling_largest_ = largestPart;
      ceiling_file_ = messageFileName;

      for (IMAPFetchParser::BodyPart &oPart : vecPartsToPeekAt)
      {
         const String partFileName = PersistentMessage::GetFileName(pConnection->GetAccountOwningCurrentFolder(), pMessage);
         // A stored file the store cannot read - a container cut short or
         // whose SHA-256 does not match (HM7350, reported by the store) - makes
         // MessageStore::ReadRange throw. Nothing of this message has been sent,
         // so the command is answered NO for it rather than the throw reaching
         // the connection's catch-all, which logged a critical error with a
         // minidump and dropped the session.
         std::shared_ptr<ByteBuffer> pBuffer;
         try
         {
            pBuffer = GetByteBufferByBodyPart_(partFileName, pMimeBody, oPart);
         }
         catch (const std::exception &)
         {
            if (MessageStore::Unavailable())
               return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message store cannot be reached just now. Try again in a moment.");
            if (MessageStore::UnreadableKeyClosed())
               return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message is encrypted at rest and cannot be read until the mailbox's owner signs in.");

            // Damaged: left out, the rest of the range fetched (GetDamaged).
            damaged_++;
            return IMAPResult();
         }

         // A part read that did not throw but found the object store gone - the
         // raw HEADER and TEXT path answers an empty buffer when ReadAll fails -
         // is the same "not now" as the checks above, not an empty section.
         // Asked after each read: a later door clears the mark.
         if (MessageStore::Unavailable())
            return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message store cannot be reached just now. Try again in a moment.");

         const size_t held = pBuffer ? pBuffer->GetSize() : 0;
         if (held > ceiling_largest_)
            ceiling_largest_ = held;

         const size_t cost = held > section_cost_ ? held : section_cost_;

         // Nothing of this message has been sent, so the command ends here with
         // nothing of it half written.
         if (section_refused_ || !WithinCeiling_(cost))
            return IMAPResult(IMAPResult::ResultNo, "[LIMIT] The sections asked for hold many times the message. Ask for fewer.");

         ceiling_used_ += cost;

         partBuffers.push_back(pBuffer);
      }

      // A stored message the store refused to read - damaged (HM7350), or in a
      // mailbox encrypted at rest whose key is closed, which is what a
      // delegate meets while the owner is not signed in - reads as nothing: an
      // empty header, so an ENVELOPE of NILs, an empty structure, and empty
      // sections. Answered OK, that was cached by the client under the UID for
      // good, and a non-PEEK section set \Seen on a message nobody had read.
      // Asked here, after every read and before a byte of this message is
      // sent; the key's case is temporary, as RFC 5530 UNAVAILABLE says
      // (docs/EncryptionAtRest.md).
      if (willReadMessageFile && MessageStore::Unreadable())
      {
         if (MessageStore::UnreadableKeyClosed())
            return IMAPResult(IMAPResult::ResultNo, "[UNAVAILABLE] The message is encrypted at rest and cannot be read until the mailbox's owner signs in.");

         // Damaged: left out, the rest of the range fetched (GetDamaged). It
         // used to end the command here, so one damaged message stopped every
         // FETCH 1:* - a client's whole sync of the folder - at itself.
         damaged_++;
         return IMAPResult();
      }

      if (parser_->GetShowPreview())
      {
         // Emitted as a literal: the preview is UTF-8 by definition (RFC 8970
         // section 5) and an IMAP quoted string cannot safely carry that.
         String sTemp;
         sTemp.Format(_T("PREVIEW {%Iu}\r\n"), previewUtf8.size());
         AppendOutput_(sOutput, sTemp);
         SendAndReset_(pConnection, sOutput);

         std::shared_ptr<ByteBuffer> pPreviewBuffer = std::shared_ptr<ByteBuffer>(new ByteBuffer);
         pPreviewBuffer->Add((const BYTE*) previewUtf8.c_str(), previewUtf8.size());
         pConnection->EnqueueWrite(pPreviewBuffer);
      }

      if (parser_->GetShowBodyStructure() ||
          parser_->GetShowBodyStructureNonExtensible())
      {
         String sResult = "";
         if (parser_->GetShowBodyStructure())
            sResult = "BODYSTRUCTURE " + IteratePartRecursive_(pMimeBody, true, 0);
         else
            sResult = "BODY " + IteratePartRecursive_(pMimeBody, false, 0);

         AppendOutput_(sOutput, sResult);
      }

      for (size_t partIndex = 0; partIndex < vecPartsToPeekAt.size(); partIndex++)
      {
         IMAPFetchParser::BodyPart &oPart = vecPartsToPeekAt[partIndex];
         std::shared_ptr<ByteBuffer> pBuffer = partBuffers[partIndex];

         int iOctetStart = oPart.octet_start_;
         int iOctetCount = oPart.octet_count_;

         String sPartIdentifier;
         if (iOctetStart == -1 && iOctetCount == -1)
            sPartIdentifier.Format(_T("%s"), oPart.GetDescription().c_str());
         else
            // RFC 3501: a partial response echoes only the origin octet.
            sPartIdentifier.Format(_T("%s<%d>"), oPart.GetDescription().c_str(), iOctetStart);

         // RFC 3516: BINARY.SIZE answers a number, not a literal.
         if (oPart.GetShowBinarySize())
         {
            String sTemp;
            sTemp.Format(_T("%s %Iu"), oPart.GetDescription().c_str(), pBuffer->GetSize());
            AppendOutput_(sOutput, sTemp);
            continue;
         }

         if (pBuffer->GetSize() > 0)
         {
            // RFC 3516 section 4.3: BINARY content is announced with literal8
            // (~{n}), not the plain literal. It matters for exactly the content
            // BINARY exists to deliver - decoded bytes can contain NUL, which a
            // plain literal is not permitted to carry, so a client reading
            // strictly would be entitled to treat the response as malformed.
            // Only BINARY uses it; BODY[] keeps the plain literal, and a client
            // only sees literal8 in reply to a command that announced BINARY
            // support by using it.
            const String literalPrefix = oPart.GetShowBinaryContent() ? _T("~{") : _T("{");

            // Send part size information
            String sTemp;
            sTemp.Format(_T("%s %s%Iu}\r\n"), sPartIdentifier.c_str(), literalPrefix.c_str(), pBuffer->GetSize()); // Add 2 for trailing newline.

            AppendOutput_(sOutput, sTemp);
            SendAndReset_(pConnection, sOutput);

            // Send the actual part
            pConnection->EnqueueWrite(pBuffer);
         }
         else
         {
            // The client requested a part that does not exist.
            // Just give the client an empty string for that part.
            String sTemp;
            sTemp.Format(_T("%s \"\""), sPartIdentifier.c_str());
            AppendOutput_(sOutput, sTemp);
         }
      }

      if (seenWillBeSet)
      {
         // Since the user has looked at the email, we should set the Seen flag -
         // on the folder's live message and in its row, as one step under the
         // message's flag-change lock (FolderManager::ChangeMessageFlags), so
         // that a STORE another session makes at the same moment is neither
         // undone nor undoes this.
         Application::Instance()->GetFolderManager()->ChangeMessageFlags(
            pConnection->GetCurrentFolder()->GetAccountID(),
            pConnection->GetCurrentFolder()->GetID(),
            pMessage->GetID(),
            [](Message &message) { message.SetFlagSeen(true); return true; },
            pMessage);

         // The change is part of this response when the client did not ask for
         // FLAGS itself (RFC 3501 6.4.5), as imaptest and Dovecot expect.
         if (!parser_->GetShowFlags())
            AppendOutput_(sOutput, CreateFlagsItem_(pMessage, true));
      }

      sOutput += ")\r\n";
      SendAndReset_(pConnection, sOutput);

      return IMAPResult();
   }

   String
   IMAPFetch::CreateFlagsItem_(std::shared_ptr<Message> pMessage, bool includeSeen)
   {
      String sTemp = "FLAGS (";

      std::set<String> setFlags;

      if (pMessage->GetFlagSeen() || includeSeen)
         setFlags.insert("Seen");
      if (pMessage->GetFlagDeleted())
         setFlags.insert("Deleted");
      if (pMessage->GetFlagFlagged())
         setFlags.insert("Flagged");
      if (pMessage->GetFlagDraft())
         setFlags.insert("Draft");
      if (pMessage->GetFlagAnswered())
         setFlags.insert("Answered");

      auto iter = setFlags.begin();
      auto iterEnd = setFlags.end();

      for (; iter != iterEnd; iter++)
      {
         if (iter != setFlags.begin())
            sTemp += " ";

         sTemp += "\\" + (*iter);
      }

      // The keywords after the system flags, as stored. DoAction has announced
      // them to this session already.
      std::vector<String> keywords = pMessage->GetKeywordList();

      for (size_t i = 0; i < keywords.size(); i++)
      {
         if (!setFlags.empty() || i > 0)
            sTemp += " ";
         sTemp += keywords[i];
      }

      sTemp += ")";

      return sTemp;
   }

   void
   IMAPFetch::GetBytesToSend_(int iBufferSize, IMAPFetchParser::BodyPart &oPart, int &iOutStart, int &iOutCount)
   {
      int iOctetStart = oPart.octet_start_;
      int iOctetCount = oPart.octet_count_;

      if (iOctetStart == -1 && iOctetCount == -1)
      {
         iOutStart = 0;
         iOutCount = iBufferSize;
         return;
      }

      // A client-supplied "<start.size>" partial range. Clamp it defensively:
      // a negative start would read before the buffer (heap disclosure) and a
      // negative or overlarge size would ask for a near-SIZE_MAX allocation
      // (crash / OutOfMemoryHandler). A start at or past the end yields empty.
      if (iOctetStart < 0)
         iOctetStart = 0;
      if (iOctetStart > iBufferSize)
         iOctetStart = iBufferSize;

      int iRemaining = iBufferSize - iOctetStart;

      if (iOctetCount < 0 || iOctetCount > iRemaining)
         iOctetCount = iRemaining;

      iOutStart = iOctetStart;
      iOutCount = iOctetCount;
   }

   std::shared_ptr<MimeBody>
   IMAPFetch::GetBodyPartByRecursiveIdentifier_(std::shared_ptr<MimeBody> pBody, const String &sName, bool loadEncapsulated)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns a body part by a given identifier. An identifier can be 1, 2, 1.2 etc.
   //---------------------------------------------------------------------------()
   {
      if (!pBody || sName.IsEmpty())
      {
         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      // The bodies kept are the message's (NavigatedBody_).
      NavigateMessage_(pBody);

      String sTempName = sName;


      // First split the string from 1, 2.1 and so on.
      std::vector<String> vecPartPath = StringParser::SplitString(sTempName, ".");

      // Iterate over the vector to find the part.
      auto iterPart = vecPartPath.begin();

      while (iterPart != vecPartPath.end())
      {
         String sSubPart = *iterPart;

         if (StringParser::IsNumeric(sSubPart))
         {
            int iRequestPartNo = _ttoi(sSubPart);

            pBody = GetMessagePartByPartNo_(pBody, iRequestPartNo);

            if (!pBody)
            {
               // Client is requesting an non-existant body.
               return pBody;
            }

            // loadEncapsulated controls only the final path component; intermediate
            // message/rfc822 parts must always be loaded so traversal can continue.
            auto nextIter = std::next(iterPart);
            bool isFinalComponent = (nextIter == vecPartPath.end());
            bool shouldLoad = isFinalComponent ? loadEncapsulated : true;

            if (shouldLoad && KindOf_(pBody).encapsulated_message)
            {
               // Parsed once for the message's sections, and counted to their
               // ceiling before it is: a part number can name an encapsulated
               // message inside another, as deep as the message nests them, and
               // each is parsed whole on the way.
               NavigatedBody_ &navigated = NavigatedBodyFor_(pBody);

               if (!navigated.encapsulated_loaded)
               {
                  const size_t parse = pBody->GetContent().size();
                  if (!WithinCeiling_(section_cost_ + parse))
                  {
                     section_refused_ = true;
                     std::shared_ptr<MimeBody> empty;
                     return empty;
                  }

                  section_cost_ += parse;
                  navigated.encapsulated_loaded = true;

                  try
                  {
                     navigated.encapsulated = pBody->LoadEncapsulatedMessage();
                  }
                  catch (...)
                  {
                     navigated.encapsulated.reset();

                     String sErrorMessage = "Error when creating ENVELOPE structure for encapsulated message.";

                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5060, "IMAPFetch::GetBodyPartByRecursiveIdentifier_", sErrorMessage);
                  }
               }

               if (!navigated.encapsulated)
               {
                  std::shared_ptr<MimeBody> empty;
                  return empty;
               }

               pBody = navigated.encapsulated;
            }

         }
     
         iterPart++;
      }

      return pBody;
   }

   IMAPFetch::NavigatedBody_ &
   IMAPFetch::NavigatedBodyFor_(const std::shared_ptr<MimeBody> &body)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // What is kept of body for the message's sections. The entry holds body, so
   // an entry found by its address is body's own.
   //---------------------------------------------------------------------------()
   {
      NavigatedBody_ &navigated = navigated_bodies_[body.get()];
      if (!navigated.body)
         navigated.body = body;

      return navigated;
   }

   const IMAPFetch::NavigatedBody_ &
   IMAPFetch::KindOf_(const std::shared_ptr<MimeBody> &body)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // body->IsEncapsulatedRFC822Message(), IsMultiPart() and
   // GetTransferEncoding(), asked once for the message's sections: each
   // searches body's header, a header of any length.
   //---------------------------------------------------------------------------()
   {
      NavigatedBody_ &navigated = NavigatedBodyFor_(body);

      if (!navigated.kind_known)
      {
         navigated.encapsulated_message = body->IsEncapsulatedRFC822Message();
         navigated.multipart = body->IsMultiPart();
         navigated.transfer_encoding = body->GetTransferEncoding();
         navigated.kind_known = true;
      }

      return navigated;
   }

   bool
   IMAPFetch::WithinCeiling_(size_t sectionCost)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether a section costing sectionCost, after those already counted, stays
   // under the ceiling of the message's sections: sixteen times the larger of
   // the message and its largest section, and a megabyte. The size on record
   // can be short, so before a section is refused the file's own size is
   // asked, once. Always true outside DoAction.
   //---------------------------------------------------------------------------()
   {
      if (!ceiling_set_)
         return true;

      for (;;)
      {
         const size_t ceiling = ceiling_largest_ * (size_t) 16 + (size_t) 1024 * 1024;

         if (ceiling_used_ <= ceiling && sectionCost <= ceiling - ceiling_used_)
            return true;

         if (ceiling_file_sized_)
            return false;

         ceiling_file_sized_ = true;

         const long fileSize = MessageStore::FileSize(ceiling_file_);
         if (fileSize <= 0 || (size_t) fileSize <= ceiling_largest_)
            return false;

         ceiling_largest_ = (size_t) fileSize;
      }
   }

   void
   IMAPFetch::ForgetMessageSections_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Lets go of what was kept for one message's sections.
   //---------------------------------------------------------------------------()
   {
      navigated_bodies_.clear();
      navigated_message_.reset();
      stored_message_asked_ = false;
      stored_message_.reset();

      ceiling_set_ = false;
      ceiling_largest_ = 0;
      ceiling_used_ = 0;
      ceiling_file_.Empty();
      ceiling_file_sized_ = false;
   }

   void
   IMAPFetch::NavigateMessage_(const std::shared_ptr<MimeBody> &message)
   {
      if (navigated_message_ == message)
         return;

      navigated_bodies_.clear();
      navigated_message_ = message;
      stored_message_asked_ = false;
      stored_message_.reset();
   }

   bool
   IMAPFetch::StoredMessage_(const String &messageFileName)
   {
      if (stored_message_asked_)
         return stored_message_ != nullptr;

      stored_message_asked_ = true;

      // What the parse read: the top-level body is placed from the first byte
      // to the last of what it was handed. Not once any part of the message
      // was changed since it was read: the stored bytes are no longer its
      // parts, and the sections are written out from the parse, as they were
      // before. Asked here, once for the message's sections, because it walks
      // every part; GetStoredBounds, asked at every section, looks at the part
      // alone.
      size_t start = 0;
      size_t headerEnd = 0;
      size_t parsed = 0;
      if (!navigated_message_ || !navigated_message_->GetStoredBounds(start, headerEnd, parsed) || start != 0 ||
          navigated_message_->IsAnyChildModified())
         return false;

      std::shared_ptr<const std::string> stored = navigated_message_->GetSourceBytes();

      if (!stored)
      {
         // Read once for the message's sections, as an encapsulated message is
         // parsed once for them, and counted to their ceiling the same way.
         if (!WithinCeiling_(section_cost_ + parsed))
         {
            section_refused_ = true;
            return false;
         }

         section_cost_ += parsed;

         // Through the store's door, which answers a container's original and
         // says so when the store is not there (MessageStore::Unavailable) or
         // the file is damaged (Unreadable) - which DoAction asks after every
         // section. A file that is simply gone - the message expunged by
         // another session in the moment between - leaves the sections to be
         // answered from the parsed message, as they were before.
         AnsiString bytes;
         if (!MessageStore::ReadAll(messageFileName, bytes))
            return false;

         stored = std::shared_ptr<const std::string>(new std::string(std::move(static_cast<std::string &>(bytes))));
      }

      // The parts were placed in bytes of this length; bytes of another are not
      // what was parsed.
      if (stored->size() != parsed)
         return false;

      stored_message_ = stored;
      return true;
   }

   bool
   IMAPFetch::BodyAsStored_(const String &messageFileName, const std::shared_ptr<MimeBody> &part, const char *&data, size_t &size)
   {
      data = nullptr;
      size = 0;

      size_t entityStart = 0;
      size_t bodyStart = 0;
      size_t entityEnd = 0;
      if (!part || !part->GetStoredBounds(entityStart, bodyStart, entityEnd))
         return false;

      // MimeBody::Load takes a part's content from the first byte of its body
      // and keeps going, so content as long as the body is the body. It is all
      // of it unless the part is a multipart whose parts were read out of it,
      // when it is the preamble alone: the parts, their boundaries and the
      // epilogue are only in the stored message.
      const AnsiString &content = part->GetContent();
      if (content.size() == entityEnd - bodyStart)
      {
         data = content.c_str();
         size = content.size();
         return true;
      }

      if (!StoredMessage_(messageFileName) || entityEnd > stored_message_->size() || bodyStart > entityEnd)
         return false;

      data = stored_message_->data() + bodyStart;
      size = entityEnd - bodyStart;
      return true;
   }

   bool
   IMAPFetch::MimeHeaderAsStored_(const String &messageFileName, const std::shared_ptr<MimeBody> &part, const char *&data, size_t &size)
   {
      data = nullptr;
      size = 0;

      size_t entityStart = 0;
      size_t bodyStart = 0;
      size_t entityEnd = 0;
      if (!part || !part->GetStoredBounds(entityStart, bodyStart, entityEnd))
         return false;

      if (!StoredMessage_(messageFileName) || bodyStart > stored_message_->size() || entityStart > bodyStart)
         return false;

      data = stored_message_->data() + entityStart;
      size = bodyStart - entityStart;
      return true;
   }

   std::shared_ptr<ByteBuffer>
   IMAPFetch::StoredSection_(const char *data, size_t size, IMAPFetchParser::BodyPart &oPart)
   {
      std::shared_ptr<ByteBuffer> pOutBuf = std::shared_ptr<ByteBuffer>(new ByteBuffer);

      int iStart = 0;
      int iCount = 0;
      GetBytesToSend_((int) size, oPart, iStart, iCount);

      if (iCount > 0)
         pOutBuf->Add((const BYTE*) data + iStart, iCount);

      return pOutBuf;
   }


   std::string
   IMAPFetch::StricmpKey_(const char *name)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The bytes of name, each as _stricmp folds it before comparing - tolower in
   // the current locale - up to its end: two names _stricmp finds the same have
   // the same key, and two it does not, not.
   //---------------------------------------------------------------------------()
   {
      std::string key;

      if (name)
      {
         for (const char *at = name; *at; at++)
            key += (char) ::tolower((unsigned char) *at);
      }

      return key;
   }

   String
   IMAPFetch::CompareNoCaseKey_(const String &name)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // name as CompareNoCase (ssicmp) folds it before comparing - each character
   // lowered in the classic locale - up to its first NUL, where it stops.
   //---------------------------------------------------------------------------()
   {
      const std::ctype<wchar_t> &ct = std::use_facet<std::ctype<wchar_t> >(std::locale::classic());

      String key;
      for (int i = 0; i < name.GetLength() && name.GetAt(i) != 0; i++)
         key += (wchar_t) ct.tolower(name.GetAt(i));

      return key;
   }

   IMAPFetch::HeaderFieldsLookup_ &
   IMAPFetch::HeaderFieldsLookupFor_(IMAPFetchParser::BodyPart &oPart)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // oPart's HEADER.FIELDS and HEADER.FIELDS.NOT lists, prepared for every
   // message of the command at its first (HeaderFieldsLookup_). oPart is the
   // parser's own, which lives as long as the command.
   //---------------------------------------------------------------------------()
   {
      auto existing = header_fields_lookups_.find(&oPart);
      if (existing != header_fields_lookups_.end())
         return (*existing).second;

      HeaderFieldsLookup_ &lookup = header_fields_lookups_[&oPart];

      // Each name once - the first spelling of those MakeUpper makes the same -
      // in the order asked, keyed as the header's lookup compares the name
      // GetRawFieldValue narrows it to.
      std::set<String> included;
      for (const String &name : oPart.GetHeaderFields())
      {
         String upper = name;
         upper.MakeUpper();

         if (!included.insert(upper).second)
            continue;

         const AnsiString narrow = name;
         lookup.by_key[StricmpKey_(narrow.c_str())].push_back(lookup.names.size());
         lookup.names.push_back(name);
      }

      for (const String &name : oPart.GetHeaderFieldsNOT())
         lookup.excluded.insert(CompareNoCaseKey_(name));

      return lookup;
   }

   std::shared_ptr<ByteBuffer>
   IMAPFetch::GetByteBufferByBodyPart_(const String &messageFileName, std::shared_ptr<MimeBody> pBodyPart, IMAPFetchParser::BodyPart &oPart)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns a buffer containing the data for the given body/part
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<ByteBuffer> pOutBuf = std::shared_ptr<ByteBuffer>(new ByteBuffer);

      // What making this section costs beyond what it holds (section_cost_).
      section_cost_ = 0;
      section_refused_ = false;

      if (!pBodyPart)
      {
         // No body provided. We'll have to return
         // an empty buffer.
         return pOutBuf;
      }

      // The bodies kept are the message's (NavigatedBody_), and pBodyPart is it.
      NavigateMessage_(pBodyPart);

      // RFC 3516 (BINARY): the section's content with its transfer encoding
      // decoded - what an attachment save has always done, applied to FETCH.
      //
      // Only a LEAF part has a transfer encoding to undo. The first cut decoded
      // whatever the section resolved to and served the entity's raw text, which
      // is right for a leaf and silently wrong for everything else: MimeBody::Load
      // truncates a multipart entity's text at the first boundary, so BINARY[] for
      // a whole message, a section naming a multipart part, and a section naming a
      // message/rfc822 part all answered with the MIME preamble - normally zero
      // bytes. A client asking for BINARY[] was told the message was empty. That
      // shipped in 6.2.22-pre2 as a documented limitation; this is the fix.
      //
      // The identity cases are served exactly as their BODY equivalents, because
      // multipart and message/rfc822 have no content transfer encoding to speak
      // of - "the encoding is identity" is not an approximation there, it is what
      // the MIME grammar says.
      if (oPart.GetShowBinaryContent() || oPart.GetShowBinarySize())
      {
         // Navigate WITHOUT loading an encapsulated message, like the BODY[N] path
         // below: loading it here would make a message/rfc822 section answer with
         // the inner message's parsed body rather than the section's own bytes.
         std::shared_ptr<MimeBody> pBinaryPart = pBodyPart;

         if (!oPart.GetName().IsEmpty())
            pBinaryPart = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName(), false);

         if (!pBinaryPart)
            return pOutBuf;

         AnsiString content;

         if (oPart.GetName().IsEmpty())
         {
            // BINARY[] - the entire message, headers included, from the file, the
            // same source BODY[] is served from. A top-level entity is 7bit, 8bit,
            // binary or multipart in practice, all of which are identity; a
            // single-part message whose own Content-Transfer-Encoding is base64 is
            // the one shape this still hands back encoded, and that is the
            // remaining half of the UNKNOWN-CTE work rather than something this
            // path can decide.
            const int fileSize = MessageStore::FileSize(messageFileName);

            int iWholeStart = 0;
            int iWholeCount = 0;
            GetBytesToSend_(fileSize, oPart, iWholeStart, iWholeCount);

            if (iWholeCount > 0)
            {
               std::vector<BYTE> buffer((size_t) iWholeCount);
               MessageStore::ReadRange(messageFileName, &buffer[0], iWholeStart, iWholeCount);
               pOutBuf->Add(&buffer[0], iWholeCount);
            }

            return pOutBuf;
         }

         const NavigatedBody_ &kind = KindOf_(pBinaryPart);

         if (kind.encapsulated_message)
         {
            // The "content" of a message/rfc822 part is the whole inner message,
            // headers and all: the part's own bytes, as BODY[N] serves them. It
            // was the inner message parsed and written out again, which dropped
            // a multipart's epilogue - BINARY.SIZE[2] came out 19 octets short in
            // imaptest's fetch-binary-mime.
            content = pBinaryPart->GetContent();
         }
         else if (kind.multipart)
         {
            // A multipart's content is its sub-parts, raw and undecoded: its
            // body as it is stored, preamble and epilogue and all, as BODY[N]
            // serves it. It was the parts written out again, with boundary
            // lines spelt afresh and the epilogue gone.
            const char *stored = nullptr;
            size_t storedSize = 0;
            if (BodyAsStored_(messageFileName, pBinaryPart, stored, storedSize))
               content.assign(stored, storedSize);
            else if (section_refused_)
               return pOutBuf;
            else
               pBinaryPart->Store(content, false);
         }
         else
         {
            std::unique_ptr<MimeCodeBase> pCoder(MimeEnvironment::CreateCoder(kind.transfer_encoding));
            pCoder->SetInput(pBinaryPart->GetContent(), pBinaryPart->GetContentLength(), false);
            pCoder->GetOutput(content);

            section_cost_ += pBinaryPart->GetContent().size();
         }

         // Made whole before the range is cut from it.
         section_cost_ += content.size();

         // The partial range applies to the DECODED bytes - that is the
         // point of asking for BINARY rather than BODY.
         int iByteStart = 0;
         int iByteCount = 0;
         GetBytesToSend_(content.GetLength(), oPart, iByteStart, iByteCount);

         if (iByteCount > 0)
            pOutBuf->Add((const BYTE*) content.c_str() + iByteStart, iByteCount);

         return pOutBuf;
      }

      // For BODY[X.MIME], navigate to the named part without loading any encapsulated
      // message so that GetHeaderContents() returns the outer MIME headers of the part
      // (e.g. Content-Type: message/rfc822) rather than the inner message's headers.
      if (oPart.GetShowBodyMime())
      {
         if (!oPart.GetName().IsEmpty())
            pBodyPart = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName(), false);

         if (!pBodyPart)
            return pOutBuf;

         // The part's MIME header as it is stored - the lines from the part's
         // first byte to its blank line, folds, spacing and 8-bit bytes as the
         // sender wrote them (RFC 3501 6.4.5) - so that it and BODY[N] are the
         // part, byte for byte. It was each field written out again as "Name:
         // value" with the value unfolded, which no client could put back.
         const char *stored = nullptr;
         size_t storedSize = 0;
         if (MimeHeaderAsStored_(messageFileName, pBodyPart, stored, storedSize))
            return StoredSection_(stored, storedSize, oPart);

         if (section_refused_)
            return pOutBuf;

         int iByteStart = 0;
         int iByteCount = 0;
         AnsiString sHeaderContents = pBodyPart->GetHeaderContents();
         section_cost_ += sHeaderContents.size();
         GetBytesToSend_(sHeaderContents.GetLength(), oPart, iByteStart, iByteCount);
         sHeaderContents = sHeaderContents.Mid(iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) sHeaderContents.GetBuffer(0), sHeaderContents.GetLength());
         return pOutBuf;
      }

      // For BODY[N] with no sub-specifier: navigate without loading the encapsulated
      // message, then answer with the part's body. Must run before the default
      // navigation below, which loads the encapsulated message unconditionally.
      if (oPart.GetShowBodyContent())
      {
         if (!oPart.GetName().IsEmpty())
            pBodyPart = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName(), false);

         if (!pBodyPart)
            return pOutBuf;

         // The part's body as it is stored (BodyAsStored_). For a part that is
         // not a multipart that is its content, as it always was - and for a
         // message/rfc822 part the whole inner message, headers included. For a
         // multipart it is the stored bytes from the end of its MIME header to
         // its end, preamble, boundary lines and epilogue as they are: it was
         // the parts written out again, the boundaries spelt afresh, the
         // epilogue dropped and a missing closing boundary added, so the
         // section was not the part and its length was not the stored one.
         const char *stored = nullptr;
         size_t storedSize = 0;
         if (BodyAsStored_(messageFileName, pBodyPart, stored, storedSize))
            return StoredSection_(stored, storedSize, oPart);

         if (section_refused_)
            return pOutBuf;

         int iByteStart = 0;
         int iByteCount = 0;
         AnsiString body;
         if (KindOf_(pBodyPart).encapsulated_message)
            body = pBodyPart->GetContent();
         else
            pBodyPart->Store(body, false);

         section_cost_ += body.size();

         GetBytesToSend_(body.GetLength(), oPart, iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) body.GetBuffer() + iByteStart, iByteCount);
         return pOutBuf;
      }

      // BODY[TEXT] and BODY[HEADER] of the message itself - and RFC822.TEXT and
      // RFC822.HEADER, which are the same sections - are served from the file,
      // the source BODY[] is served from: the header up to and including its
      // blank line, and every byte after it. They were rebuilt from the parsed
      // MIME tree, which is not the message. A multipart's text lost its
      // epilogue and had its boundary lines spelt afresh, and the header came
      // back with its folded fields unfolded - so HEADER and TEXT no longer
      // added up to BODY[] and RFC822.SIZE, and a client comparing them
      // (imaptest's fetch-body tests) was handed a different message.
      if (oPart.GetName().IsEmpty() && (oPart.GetShowBodyText() || oPart.GetShowBodyHeader()))
      {
         AnsiString message;
         if (!MessageStore::ReadAll(messageFileName, message))
            return pOutBuf;

         const std::string &bytes = message;
         section_cost_ += bytes.size();
         return RawSection_(bytes.data(), bytes.size(), oPart.GetShowBodyHeader(), oPart.GetShowBodyText(), oPart);
      }

      // The same for a message/rfc822 part: its HEADER and TEXT (RFC 3501
      // 6.4.5, "BODY[1.TEXT]") come from the encapsulated message's own bytes,
      // which are the part's content, rather than from the inner message parsed
      // and written out again - that lost a multipart's epilogue and respelt its
      // boundary lines.
      if (!oPart.GetName().IsEmpty() && (oPart.GetShowBodyText() || oPart.GetShowBodyHeader()))
      {
         std::shared_ptr<MimeBody> pNamedPart = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName(), false);

         if (pNamedPart && KindOf_(pNamedPart).encapsulated_message)
         {
            const AnsiString &content = pNamedPart->GetContent();
            const std::string &bytes = content;
            return RawSection_(bytes.data(), bytes.size(), oPart.GetShowBodyHeader(), oPart.GetShowBodyText(), oPart);
         }
      }

      // First make sure that we got the right part of it.
      if (!oPart.GetName().IsEmpty())
      {
         String sMimePart;
         pBodyPart  = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName());

         if (!pBodyPart)
            return pOutBuf;
      }

      int iByteStart = 0;
      int iByteCount = 0;

      // HEADER and TEXT of a numbered part that is not a message/rfc822 - which
      // RFC 3501 6.4.5 does not give a meaning, and which have always answered
      // as the part's MIME header and its body - are those as they are stored,
      // the same bytes as BODY[N.MIME] and BODY[N].
      if (!oPart.GetName().IsEmpty() && (oPart.GetShowBodyHeader() || oPart.GetShowBodyText()))
      {
         const char *stored = nullptr;
         size_t storedSize = 0;
         const bool asStored = oPart.GetShowBodyHeader() ?
            MimeHeaderAsStored_(messageFileName, pBodyPart, stored, storedSize) :
            BodyAsStored_(messageFileName, pBodyPart, stored, storedSize);

         if (asStored)
            return StoredSection_(stored, storedSize, oPart);

         if (section_refused_)
            return pOutBuf;
      }

      if (oPart.GetShowBodyHeader())
      {
         // Add HEADER
         AnsiString sHeaderContents = pBodyPart->GetHeaderContents();
         section_cost_ += sHeaderContents.size();
         GetBytesToSend_(sHeaderContents.GetLength(), oPart, iByteStart, iByteCount);
         sHeaderContents = sHeaderContents.Mid(iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) sHeaderContents.GetBuffer(0), sHeaderContents.GetLength());
      }
      else if (oPart.GetShowBodyText())
      {
         // RFC: The TEXT part specifier refers to the text body of the message,
         // omitting the [RFC-2822] header.
         // All sub-bodies in raw format, except for the current header
         AnsiString body;
         pBodyPart->Store(body, false);
         section_cost_ += body.size();


         //  Does this do anything since start is always 0 at this point?
         //  Assume this should be removed..
         //  body = body.Mid(iByteStart);

         GetBytesToSend_(body.GetLength(), oPart, iByteStart, iByteCount);

    // Fix for Apple Mail BODY PEEK with start + size issue
    // Start was being ignored before as it was never used
    // Changed to mimic 4.4.4 method since iByteStart is set in GetBytesToSend_
         pOutBuf->Add((BYTE*) body.GetBuffer() + iByteStart, iByteCount);
      }
      else if (oPart.GetShowBodyFull())
      {
         /*
         BODY[]
         An empty section specification refers to the entire message, 
         including the header.

         RFC822 is
         Functionally equivalent to BODY[], differing in the syntax of
         the resulting untagged FETCH data (RFC822 is returned).
         */

         int iSize = MessageStore::FileSize(messageFileName);
         GetBytesToSend_(iSize, oPart, iByteStart, iByteCount);

         // This is the path every chunked client takes: BODY[]<start.count> is what
         // Apple Mail and Thunderbird use to pull a message down in pieces, and it
         // is the only body form served from the file rather than from the parsed
         // MIME tree.
         //
         // The buffer was a raw new[] with the delete[] after the call.
         // ReadFileToBuf throws if the file cannot be opened, and since the seek
         // result stopped being discarded it also throws if the seek to iByteStart
         // fails - so either throw leaked the whole allocation, which for a
         // non-partial BODY[] is the entire message.
         if (iByteCount > 0)
         {
            const size_t bufferSize = (size_t) iByteCount;
            std::vector<BYTE> buffer(bufferSize);

            MessageStore::ReadRange(messageFileName, &buffer[0], iByteStart, iByteCount);

            pOutBuf->Add(&buffer[0], iByteCount);
         }
      }
      else if (oPart.GetShowBodyHeaderFields())
      {
         // Each name asked for, once, with the value of the first field of the
         // header that _stricmp finds the same as it - what GetRawFieldValue
         // answers - in the order asked. The header is read once, field by field,
         // against the names prepared for the command (HeaderFieldsLookupFor_).
         const HeaderFieldsLookup_ &lookup = HeaderFieldsLookupFor_(oPart);

         std::map<size_t, const char *> firstValues;
         for (MimeField &field : pBodyPart->Fields())
         {
            section_cost_ += (size_t) field.GetLength();

            auto named = lookup.by_key.find(StricmpKey_(field.GetName()));
            if (named == lookup.by_key.end())
               continue;

            for (size_t index : (*named).second)
               firstValues.insert(std::make_pair(index, field.GetValue()));
         }

         String sResponse;
         String sFields;

         for (const std::pair<const size_t, const char *> &found : firstValues)
         {
            const String &sHeaderField = lookup.names[found.first];

            String sValue;

            sValue = found.second;

            if (sValue.GetLength() > 0)
            {
               if (!sFields.IsEmpty())
                  sFields += " ";

               sFields += sHeaderField;

               sResponse += sHeaderField + ": " + sValue + "\r\n";
            }
         }

         sResponse += "\r\n";
         section_cost_ += (size_t) sResponse.GetLength();

         GetBytesToSend_(sResponse.GetLength(), oPart, iByteStart, iByteCount);
         AnsiString sAS = sResponse.Mid(iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) sAS.GetBuffer(0), sAS.GetLength());

      }
      else if (oPart.GetShowBodyHeaderFieldsNOT())
      {
         // The names left out, prepared for the command: each field of the header
         // is looked up once, where it was compared with every name of the list.
         const HeaderFieldsLookup_ &lookup = HeaderFieldsLookupFor_(oPart);

         String sResponse;

         String sFields;

         // Read where they are: each section copied the header's every field.
         const std::vector<MimeField> &lstFields = pBodyPart->Fields();
         auto iterHeader = lstFields.begin();

         while (iterHeader != lstFields.end())
         {
            const MimeField &oField = (*iterHeader);

            section_cost_ += (size_t) oField.GetLength();

            String sHeaderName = oField.GetName();

            // Should we send this header? Not if CompareNoCase finds it the same
            // as a name left out: the same as its key being among theirs.
            bool bSendThisHeader = lookup.excluded.find(CompareNoCaseKey_(sHeaderName)) == lookup.excluded.end();

            // Check whether we should include this header.
            if (bSendThisHeader)
            {
               String sHeaderValue = oField.GetValue();

               if (sHeaderValue.GetLength() > 0)
               {
                  if (!sFields.IsEmpty())
                     sFields += " ";

                  sFields += sHeaderName;

                  sResponse += sHeaderName + ": " + sHeaderValue + "\r\n";

               }
            }

            iterHeader++;
         }


         sResponse += "\r\n";
         section_cost_ += (size_t) sResponse.GetLength();

         GetBytesToSend_(sResponse.GetLength(), oPart, iByteStart, iByteCount);
         AnsiString sAS = sResponse.Mid(iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) sAS.GetBuffer(0), sAS.GetLength());

      }

      return pOutBuf;
   }

   std::shared_ptr<MimeBody>
   IMAPFetch::GetMessagePartByPartNo_(std::shared_ptr<MimeBody> pBody, long iPartNo)
   {
      
      if (!pBody)
      {
         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      // The parts, listed once for the message's sections (NavigatedBody_):
      // walked from the first to the one named for each section, every MIME
      // header of a message of many parts was the square of their number.
      NavigatedBody_ &navigated = NavigatedBodyFor_(pBody);

      if (!navigated.parts_listed)
      {
         for (std::shared_ptr<MimeBody> oPart = pBody->FindFirstPart(); oPart; oPart = pBody->FindNextPart())
            navigated.parts.push_back(oPart);

         navigated.parts_listed = true;
         section_cost_ += navigated.parts.size();
      }

      if (navigated.parts.empty())
      {
         // This is a single part message. Should we send the first part?
         if (iPartNo == 1)
            return pBody;

         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      if (iPartNo < 1 || (unsigned long) iPartNo > navigated.parts.size())
      {
         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      return navigated.parts[(size_t) iPartNo - 1];
   }

   String
   IMAPFetch::CreateEnvelopeStructure_(MimeHeader& oHeader)
   {
      // RFC 3501 7.4.2. Every member is an nstring or an address list, and a
      // header that is absent is NIL - not "", which says the header is there
      // and empty. The Subject and Date were always sent quoted, "" when the
      // message had none, which a client then showed as an empty subject
      // rather than as a message with no subject.
      String sResult;
      sResult += "(";

      MimeField *pField = 0;

      {  // BEGIN DATE

         pField = oHeader.GetField("Date");

         if (!pField)
         {
            sResult += "NIL";
         }
         else
         {
            String sDate = pField->GetValue();

            // Time for some ugly code. Outlook Express 6.0
            // doesn't seem to base64-decode the Date-part
            // of the envelope structure even though it should
            // do that according to the RFC. Since getting help
            // from MS is pretty much impossible, we clean
            // the date a bit so that it doesn't contain any
            // "unallowed" characters.
            //
            // Before this, the date may look like this:
            // Thu, 20 Jan 2005 10:58:55 -0200 (Hor�rio brasileiro de ver�o)
            // Afterwards, it should be:
            // Thu, 20 Jan 2005 10:58:55 -0200

            int iStartPara = sDate.Find(_T("("));
            if (iStartPara >= 0)
               sDate = sDate.Mid(0, iStartPara-1);

            // Make sure no quotes are included in the date - and no backslash,
            // which a date never has either. Charset::Encode leaves printable
            // ASCII as it is, and inside the quoted string a backslash escapes
            // what follows (RFC 3501 4.3): a Date header that ended with one
            // escaped the closing quote, so every client read the rest of the
            // envelope, and whatever the sender wrote in it, as the date and
            // then as the response's own structure.
            sDate.Replace(_T("\""), _T(""));
            sDate.Replace(_T("\\"), _T(""));

            sDate = Charset::Encode(sDate);

            sResult += "\"" + sDate + "\"";
         }
      }  // END DATE

      {  // BEGIN SUBJECT

         pField = oHeader.GetField("Subject");

         if (!pField)
            sResult += " NIL";
         else
            sResult += " \"" + Charset::GetIMAPEncoded(pField->GetValue()) + "\"";

      }  //END SUBJECT

      // The address members. "If the Sender or Reply-To lines are absent in the
      // [RFC-2822] header, or are present but empty, the server sets the
      // corresponding member of the envelope to be the same value as the from
      // member" (RFC 3501 7.4.2) - present but empty included, which the code
      // before this missed: "Sender:" with nothing after it was sent as NIL.
      String sFrom = CreateAddressListStructure_(oHeader, "From");

      String sSender = CreateAddressListStructure_(oHeader, "Sender");
      if (sSender == _T("NIL"))
         sSender = sFrom;

      String sReplyTo = CreateAddressListStructure_(oHeader, "Reply-To");
      if (sReplyTo == _T("NIL"))
         sReplyTo = sFrom;

      sResult += " " + sFrom;
      sResult += " " + sSender;
      sResult += " " + sReplyTo;
      sResult += " " + CreateAddressListStructure_(oHeader, "To");
      sResult += " " + CreateAddressListStructure_(oHeader, "CC");
      sResult += " " + CreateAddressListStructure_(oHeader, "BCC");

      {  // BEGIN IN-REPLY-TO
         String sInReplyTo;

         pField = oHeader.GetField("IN-REPLY-TO");
         if (pField)
            sInReplyTo = Charset::GetIMAPEncoded(pField->GetValue());

         if (sInReplyTo.IsEmpty())
            sResult += " NIL";
         else
            sResult += " \"" + sInReplyTo +"\"";

      }  //END IN-REPLY-TO

      {  // BEGIN MESSAGEID
         String sMessageID;

         pField = oHeader.GetField("Message-ID");
         if (pField)
            sMessageID = Charset::GetIMAPEncoded(pField->GetValue());

         if (sMessageID.IsEmpty())
            sResult += " NIL";
         else
            sResult += " \"" + sMessageID +"\"";

      }  //END MESSAGEID

      // END ENVELOPE
      sResult += ")";

      return sResult;
   }

   String
   IMAPFetch::CreateAddressListStructure_(MimeHeader& oHeader, const char *fieldName)
   {
      MimeField *pField = oHeader.GetField(fieldName);
      if (!pField)
         return _T("NIL");

      std::vector<IMAPAddress> addresses = IMAPAddressList::Parse(pField->GetValue());
      if (addresses.empty())
         return _T("NIL");

      // "(" 1*address ")" - the addresses follow each other with no space
      // between them, as the grammar has it.
      String sResult = _T("(");

      for (const IMAPAddress &address : addresses)
      {
         if (address.kind == IMAPAddress::KindGroupEnd)
         {
            sResult += _T("(NIL NIL NIL NIL)");
            continue;
         }

         if (address.kind == IMAPAddress::KindGroupStart)
         {
            sResult += _T("(NIL NIL ") + EnvelopeString_(address.mailbox) + _T(" NIL)");
            continue;
         }

         // (name route mailbox host). The name is NIL when the address has
         // none - it used to be the address itself, which made every client
         // show "user@example.com <user@example.com>" for a bare address.
         sResult += _T("(") + EnvelopeNString_(address.personal_name) + _T(" ") +
                    EnvelopeNString_(address.route) + _T(" ") +
                    EnvelopeString_(address.mailbox) + _T(" ") +
                    EnvelopeString_(address.host) + _T(")");
      }

      sResult += _T(")");

      return sResult;
   }

   String
   IMAPFetch::EnvelopeString_(const AnsiString &value)
   {
      // Printable US-ASCII goes as an IMAP quoted string, its quote and
      // backslash characters escaped (RFC 3501 4.3) - a quoted local part such
      // as "john doe"@x keeps its quotes that way. Anything else is encoded as
      // the other envelope strings are.
      // Indexed through c_str(): AnsiString's operator[] is ambiguous with a
      // size_t on clang.
      const char *data = value.c_str();
      const size_t length = value.size();

      bool printableAscii = true;
      for (size_t i = 0; i < length; i++)
      {
         const unsigned char c = (unsigned char) data[i];
         if (c < 0x20 || c > 0x7E)
         {
            printableAscii = false;
            break;
         }
      }

      if (!printableAscii)
         return _T("\"") + Charset::GetIMAPEncoded(String(value.c_str())) + _T("\"");

      String quoted = _T("\"");
      for (size_t i = 0; i < length; i++)
      {
         const char c = data[i];
         if (c == '"' || c == '\\')
            quoted += _T("\\");
         quoted += (wchar_t) (unsigned char) c;
      }
      quoted += _T("\"");

      return quoted;
   }

   String
   IMAPFetch::StructureString_(const String &value)
   {
      // A BODYSTRUCTURE string taken from the message's own header - a type,
      // a charset, a file name, a boundary, a Content-ID, a disposition - which
      // anyone who can send the server mail writes. They were put between
      // quotes as they came, so a file name ending in a backslash escaped the
      // closing quote, and a Content-ID with a quote in it ended the string
      // early: every client then parsed what the sender wrote as the
      // response's own structure. Quote and backslash are escaped (RFC 3501
      // 4.3) and a control character, a folded line's CR LF among them, which
      // a quoted string cannot carry and which would end the response line,
      // is sent as a space. Anything else goes as it always has, so a value
      // without any of these is sent byte for byte as before.
      String quoted = _T("\"");
      for (int i = 0; i < value.GetLength(); i++)
      {
         wchar_t c = value.GetAt(i);
         if (c == '"' || c == '\\')
            quoted += _T("\\");
         else if (c < 0x20 || c == 0x7F)
            c = ' ';
         quoted += c;
      }
      quoted += _T("\"");

      return quoted;
   }

   String
   IMAPFetch::EnvelopeNString_(const AnsiString &value)
   {
      if (value.IsEmpty())
         return _T("NIL");

      return EnvelopeString_(value);
   }

   size_t
   IMAPFetch::TextStartOf_(const char *data, size_t size)
   {
      if (size >= 2 && data[0] == '\r' && data[1] == '\n')
         return 2;

      for (size_t i = 0; i + 3 < size; i++)
      {
         if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n')
            return i + 4;
      }

      return size;
   }

   std::shared_ptr<ByteBuffer>
   IMAPFetch::RawSection_(const char *data, size_t size, bool header, bool text, IMAPFetchParser::BodyPart &oPart)
   {
      std::shared_ptr<ByteBuffer> pOutBuf = std::shared_ptr<ByteBuffer>(new ByteBuffer);

      const size_t textStart = TextStartOf_(data, size);

      // Looked through to find where the text starts.
      section_cost_ += textStart;

      const char *sectionStart = data;
      size_t sectionLength = size;

      if (text)
      {
         sectionStart = data + textStart;
         sectionLength = size - textStart;
      }
      else if (header)
      {
         sectionLength = textStart;
      }

      int iSectionStart = 0;
      int iSectionCount = 0;
      GetBytesToSend_((int) sectionLength, oPart, iSectionStart, iSectionCount);

      if (iSectionCount > 0)
         pOutBuf->Add((const BYTE*) sectionStart + iSectionStart, iSectionCount);

      return pOutBuf;
   }

   long
   IMAPFetch::CountTextLines_(const AnsiString &content)
   {
      // A line is what a line break ends, and a last line without one still
      // counts: "hello\r\n" is one line, as is "hello", and nothing is none.
      // "Number of line breaks plus one" said two for the first, so every
      // text part and every message/rfc822 part was reported a line longer
      // than it is.
      const size_t length = content.size();
      if (length == 0)
         return 0;

      const char *data = content.c_str();

      long lines = 0;
      for (size_t i = 0; i < length; i++)
      {
         if (data[i] == '\n')
            lines++;
      }

      if (data[length - 1] != '\n')
         lines++;

      return lines;
   }

   String
   IMAPFetch::IteratePartRecursive_(std::shared_ptr<MimeBody> oPart, bool includeExtensionData, int iRecursion)
   {
      iRecursion++;

      // Not reached: GetPartStructure_ describes no message whose parts could
      // pass the depth. Were it, a body of nothing - valid wherever a body is,
      // which "" was not.
      if (iRecursion > MaxStructureDepth_)
         return _T("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"US-ASCII\") NIL NIL \"7BIT\" 0 0)");

      // A multipart is described as one while it has parts. One with none - no
      // boundary parameter, a boundary its body never holds, or one nested past
      // MimeBody::MaxNestingDepth, which the parse takes whole - is a single
      // part to every section (GetMessagePartByPartNo_: its part 1 is itself,
      // BODY[N] its whole body), and is described as one by GetPartStructure_.
      // It was "(" and its subtype with no body before it, which RFC 3501's
      // body-type-mpart ("1*body SP media-subtype") does not allow and no
      // client could read.
      if (oPart->IsMultiPart() && oPart->GetNumberOfParts() > 0)
      {
         // This part has many parts. Iterate those.


         String sResult = "(";

         std::shared_ptr<MimeBody> pSubPart = oPart->FindFirstPart();

         while (pSubPart)
         {
            sResult += IteratePartRecursive_(pSubPart, includeExtensionData, iRecursion);

            pSubPart = oPart->FindNextPart();
         }

         // Add subtype stamp.
         String sType = oPart->GetSubType();
         sType.ToUpper();

         sResult += _T(" ") + StructureString_(sType);

         if (includeExtensionData)
         {
            String sBoundary = oPart->GetBoundary();
            sResult += _T(" (\"BOUNDARY\" ") + StructureString_(sBoundary) + _T(") NIL NIL");
         }
         
         sResult += ")";

         return sResult;

      }
      else
      {
         return GetPartStructure_(oPart, includeExtensionData, iRecursion);
      }
   }

   String
   IMAPFetch::GetPartStructure_(std::shared_ptr<MimeBody> oPart, bool includeExtensionData, int iRecursion)
   {
      String sMainType = oPart->GetMainType();
      sMainType.ToUpper();
      String sSubType = oPart->GetSubType();
      sSubType.ToUpper();

      /*
         A body type of type MESSAGE and subtype RFC822 contains,
         immediately after the basic fields, the envelope structure,
         body structure, and size in text lines of the encapsulated
         message.

         Made first, because a message/rfc822 part whose message cannot be
         described is described as what it then is to the client, a part of
         bytes - "APPLICATION" "OCTET-STREAM", as Dovecot answers - rather than
         as a message with no envelope or body, which no client can read. It
         was "" - the part left out of its multipart, or a BODYSTRUCTURE of
         nothing - when the message did not parse, and a message/rfc822 with
         nothing where its body goes when messages were nested past 100, which
         a message of a few kilobytes can do: every client met a structure it
         could not parse. A message is described while every part it can hold
         stays under that depth - its multiparts nest at most
         MimeBody::MaxNestingDepth deep below it - and past that as a part of
         bytes; the size, its stored length, is the same either way.
      */
      String sEncapsulated;
      if (oPart->IsEncapsulatedRFC822Message())
      {
         bool described = false;

         if (iRecursion + (int) MimeBody::MaxNestingDepth + 1 <= MaxStructureDepth_)
         {
            try
            {
               std::shared_ptr<MimeBody> pBody = oPart->LoadEncapsulatedMessage();

               AnsiString sMessageHeader = pBody->GetHeaderContents();
               MimeHeader oHeader;
               oHeader.Load(sMessageHeader, sMessageHeader.GetLength());

               String sEnvelope = CreateEnvelopeStructure_(oHeader);
               String sBodyStructure = IteratePartRecursive_(pBody, includeExtensionData, iRecursion);

               // The lines of the encapsulated message - header and body, which is
               // this part's own content - not of the inner message's body alone.
               long lLines = CountTextLines_(oPart->GetContent());

               String sLines;
               sLines.Format(_T("%d"), lLines);

               if (!sBodyStructure.IsEmpty())
               {
                  sEncapsulated = " " + sEnvelope + " " + sBodyStructure + " " + sLines;
                  described = true;
               }
            }
            catch (...)
            {
               String sErrorMessage = "Error when creating ENVELOPE structure for encapsulated message.";

               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5061, "IMAPFetch::GetPartStructure_", sErrorMessage);
            }
         }

         if (!described)
         {
            sMainType = _T("APPLICATION");
            sSubType = _T("OCTET-STREAM");
         }
      }
      else if (oPart->IsMultiPart())
      {
         // A multipart with no parts (IteratePartRecursive_): to the client a
         // part of bytes, its whole body, as a message past the depth is.
         sMainType = _T("APPLICATION");
         sSubType = _T("OCTET-STREAM");
      }

      String sBodyParams;

      if (oPart->IsAttachment())
         sBodyParams = _T("(\"NAME\" ") + StructureString_(oPart->GetRawFilename()) + _T(")");
      else
      {
         String sCharset = oPart->GetCharset();
         
         if (sCharset.IsEmpty())
         {
            // Fallback to default. One could think that the client
            // should assume this if we don't tell it, but at least
            // JavaMail doesn't handle an empty string here.
            sCharset = "US-ASCII";
         }

         sBodyParams = _T("(\"CHARSET\" ") + StructureString_(sCharset) + _T(")");
      }
      
      // body-fld-desc is the part's Content-Description (RFC 3501 7.4.2). It
      // was the part's Subject, so a single-part message - whose part header
      // is the message header - reported its own subject as its description.
      // Encoded as the envelope's strings are: a quote would otherwise end it.
      MimeField *pField = oPart->GetField(CMimeConst::ContentDescription());
      String sDescription;
      if (pField)
         sDescription = Charset::GetIMAPEncoded(pField->GetValue());

      String sEncoding = oPart->GetTransferEncoding();
      String sContentID = oPart->GetRawFieldValue("Content-ID");

      String sSize;
      sSize.Format(_T("%d"), oPart->GetContentLength());

      String sResult;
      sResult += "(";
      sResult += StructureString_(sMainType);
      sResult += " " + StructureString_(sSubType);
      sResult += " " + sBodyParams;

      if (sContentID.IsEmpty())
         sResult += " NIL";
      else
         sResult += " " + StructureString_(sContentID);
      
      if (sDescription.IsEmpty())
         sResult += " NIL";
      else
         sResult += " \"" + sDescription + "\"";

      // RFC 3501 states 7bit should have quotes
      // Missing quotes around 7bit causes problems with Thunderbird
      // https://www.progressiverobot.com/forum/viewtopic.php?f=10&t=22887
      if (sEncoding.IsEmpty())
         sResult += " \"7bit\"";
      else
         sResult += " " + StructureString_(sEncoding);

      sResult += " " + sSize;

      sResult += sEncapsulated;

      if (sMainType.CompareNoCase(_T("TEXT")) == 0)
      {
         /*
         A body type of type TEXT contains, immediately
         after the basic fields, the size of the body in
         text lines.  Note that this size is the size in its
         content transfer encoding and not the resulting
         size after any decoding.
         */

         long lLines = CountTextLines_(oPart->GetContent());

         String sTemp;
         sTemp.Format(_T(" %d"), lLines);

         sResult += sTemp;
      }


      // body-ext-1part - the MD5, NIL, and the disposition - is extension data,
      // which BODY leaves out (RFC 3501 7.4.2: "A form of BODYSTRUCTURE without
      // extension data"), as it always left out a multipart's. BODY carried an
      // attachment's disposition and file name all the same.
      if (includeExtensionData && oPart->IsAttachment())
      {
         String sDisposition = oPart->GetDisposition();
         int iSemiPos = sDisposition.Find(_T(";"));
         if (iSemiPos > 0)
            sDisposition = sDisposition.Mid(0, iSemiPos);

         // Fall back to default if content disposition
         // isn't specified for the part.
         if (sDisposition.IsEmpty())
            sDisposition = "ATTACHMENT";
    
         sResult += _T(" NIL (") + StructureString_(sDisposition) + _T(" (\"FILENAME\" ") + StructureString_(oPart->GetRawFilename()) + _T("))");
      }

      sResult += ")";
      return sResult;
   }
   

   std::shared_ptr<MimeBody> 
   IMAPFetch::LoadMimeBody_(std::shared_ptr<IMAPFetchParser> pParser, const String &fileName)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Loads the part of a message needed. 
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<MimeBody> pMimeBody;
      
      if (pParser->GetPartsToLookAt().size() == 0 &&
          !pParser->GetShowBodyStructure() &&
          !pParser->GetShowBodyStructureNonExtensible())
      {
         // The message body is not needed.
         return pMimeBody;
      }

      // We should check whether we need to load the entire email
      // or if it's enough to only load the header. If the client
      // only requests part from the header, we should only load
      // just that.

      bool bLoadFullMail = GetMessageBodyNeeded_(parser_);

      try
      {
         pMimeBody = std::shared_ptr<MimeBody>(new MimeBody);

         // A failed load leaves an EMPTY MimeBody and the fetch then answers the client
         // from it: a message that renders as blank, with an OK response, which the
         // client caches and stops asking for. The result was discarded, so that was
         // entirely silent. Throwing routes it into the catch below, which reports
         // HM5062 naming the file.
         //
         // Be clear about what this does NOT do: ReportCriticalError_ says in its own
         // comment that it "reports an error to the ERROR log and then throws an
         // exception", and it does not throw - so this function still returns the empty
         // body afterwards and the client still gets the blank message. What changes is
         // that it is now on the record instead of invisible. Making that function throw
         // as documented changes the failure semantics of every FETCH and wants a test of
         // its own; it is written up against the ignored-return-values row.
         //
         // Except where the store refused the read - damage, or a mailbox encrypted at
         // rest whose key is closed: that is marked on the thread
         // (MessageStore::Unreadable), and DoAction refuses the message from the mark
         // before anything of it is sent, so no blank goes out for those.
         if (bLoadFullMail)
         {
            if (MessageStore::LoadMime(fileName, *pMimeBody) != MimeLoadResult::Loaded)
               throw std::runtime_error(Formatter::FormatAsAnsi("The message file {0} could not be read.", fileName));
         }
         else
         {
            // We should only load the header.
            AnsiString sHeader = PersistentMessage::LoadHeader(fileName);
            pMimeBody->MimeHeader::Load(sHeader, sHeader.GetLength());
         }

      }
      catch (...)
      {
         // A read the store refused has been said already, by the store, at the
         // level it chose (MessageStore::ReportUnreadable): damage as HM7350,
         // naming the file and the reason, and a closed key of a mailbox
         // encrypted at rest in the debug log only. A closed key is not a fault:
         // it opens at the owner's next sign-in with the password, and a
         // delegate's client asks for such a message often - every BODYSTRUCTURE
         // of a shared folder while its owner is signed out. Reported again here
         // it was a critical error for each of those, in the log an administrator
         // is told to watch. DoAction refuses both from the same mark: NO
         // [UNAVAILABLE] for the closed key, and the damaged message left out of
         // the range with the command ending NO.
         if (!MessageStore::Unreadable())
            ReportCriticalError_(fileName, "ERROR HM10001 - IMAP FETCH: Error when loading MIME message.");
      }

      return pMimeBody;

   }

   bool 
   IMAPFetch::GetMessageBodyNeeded_(std::shared_ptr<IMAPFetchParser> pParser)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // This method determines whether we need to load the entire body, or if
   // the header contains enough information.
   //---------------------------------------------------------------------------()
   {  
      if (parser_->GetShowBodyStructure() ||
          parser_->GetShowBodyStructureNonExtensible())
         return true;

      // Check if we only got one part.
      std::vector<IMAPFetchParser::BodyPart> &vecPartsToLookAt = parser_->GetPartsToLookAt();
      if (vecPartsToLookAt.size() == 1)
      {
         IMAPFetchParser::BodyPart &oBodyPart = vecPartsToLookAt[0];

         if (!oBodyPart.GetName().IsEmpty())
         {
            // If we're loading something else than the first main part.
            return true;
         }

         if (oBodyPart.GetShowBodyText() || oBodyPart.GetShowBodyHeader())
         {
            // The message's own TEXT and HEADER are read from the file as BODY[]
            // is (GetByteBufferByBodyPart_), so a parse would be spent for
            // nothing - once per chunk for a client that fetches in pieces.
            return false;
         }

         if (oBodyPart.GetShowBodyFull())
         {
            // BODY[] is served straight from the message file - see the
            // GetShowBodyFull branch in GetByteBufferByBodyPart_, which seeks in the
            // file and never touches the MimeBody it is handed. Parsing the message
            // to serve it is therefore pure waste, and it is not a small waste: this
            // is the form clients use to download a message in pieces, so the parse
            // happened once per CHUNK. A 20 MB message pulled down in 64 KB pieces
            // parsed itself over three hundred times - quadratic work on exactly the
            // access pattern Apple Mail always uses and Thunderbird uses by default,
            // which is consistent with issue #26's report that Apple Mail suffers
            // most.
            //
            // The header is still loaded, because that is cheap and it keeps a
            // usable MimeBody in the hands of every other caller in this function.
            return false;
         }

         if (!oBodyPart.GetBodyTextNeeded())
         {
            // We only need the header.
            return false;
         }
      }

      return true;
   }

   String
   IMAPFetch::CreatePreviewText_(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<Message> pMessage)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // RFC 8970: a short plain-text snippet of the message body.
   //
   // MessageData rather than the already-loaded MimeBody, deliberately: its
   // body-part selection is the one place that already knows how to walk
   // multipart/alternative and decode part charsets, and a second,
   // subtly-different traversal here would disagree with it on exactly the
   // messages where it matters. The plain-text part wins; a HTML-only message
   // gets its tags stripped and a handful of entities decoded - a preview is a
   // glance, not a rendering.
   //---------------------------------------------------------------------------
   {
      MessageData messageData;
      if (!messageData.LoadFromMessage(pConnection->GetAccountOwningCurrentFolder(), pMessage))
         return "";

      String text = messageData.GetBody();

      if (text.Trim().IsEmpty())
      {
         String html = messageData.GetHTMLBody();

         String stripped;
         stripped.reserve(html.GetLength());

         bool insideTag = false;
         for (int i = 0; i < html.GetLength(); i++)
         {
            wchar_t character = html[i];

            if (character == '<')
               insideTag = true;
            else if (character == '>')
            {
               insideTag = false;
               stripped += ' ';
            }
            else if (!insideTag)
               stripped += character;
         }

         stripped.Replace(_T("&nbsp;"), _T(" "));
         stripped.Replace(_T("&amp;"), _T("&"));
         stripped.Replace(_T("&lt;"), _T("<"));
         stripped.Replace(_T("&gt;"), _T(">"));
         stripped.Replace(_T("&quot;"), _T("\""));

         text = stripped;
      }

      // One glance-sized line: every whitespace run - including the line
      // structure of the original - becomes a single space.
      String collapsed;
      collapsed.reserve(text.GetLength());

      bool lastWasSpace = true;
      for (int i = 0; i < text.GetLength(); i++)
      {
         wchar_t character = text[i];

         if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
         {
            if (!lastWasSpace)
            {
               collapsed += ' ';
               lastWasSpace = true;
            }
         }
         else
         {
            collapsed += character;
            lastWasSpace = false;
         }
      }

      collapsed.TrimRight();

      // Short of the RFC's 256-octet ceiling; the octet-boundary cut at the
      // emission site enforces the ceiling itself for multi-octet characters.
      const int maxPreviewCharacters = 200;
      if (collapsed.GetLength() > maxPreviewCharacters)
         collapsed = collapsed.Mid(0, maxPreviewCharacters);

      return collapsed;
   }

   void
   IMAPFetch::AppendOutput_(String &sOutput, const String &sAppend)
   {
      if (append_space_)
         sOutput += " ";

      sOutput += sAppend;

      append_space_ = true;
   }

   void
   IMAPFetch::SendAndReset_(std::shared_ptr<IMAPConnection> pConnection, String &sOutput)
   {
      pConnection->SendAsciiData(sOutput);
      sOutput = "";
   }

   void
   IMAPFetch::ReportCriticalError_(const String &messageFileName, const String &sMessage)
   // ----
   // Reports an error to the ERROR log. It does NOT throw.
   //
   // The comment here said "and then throws an exception" and it never did, which
   // matters because both callers are written as though the function ends the
   // operation: each reports and then carries on to return a MimeBody that was
   // never loaded, so the client is answered from an empty body with an OK. The
   // message renders blank, and the client caches that and stops asking.
   //
   // Left as-is deliberately rather than made to match the comment. Throwing here
   // has no handler between this and TCPConnection's catch-all, so it would drop
   // the whole IMAP session over one unreadable message - a client would lose the
   // session repeatedly instead of one blank message. The right answer is a NO
   // response for that message alone, which needs the failure carried back to the
   // command handler, and that is a change worth making with a test rather than as
   // a side effect of correcting a comment.
   // ---
   {
      String sErrorMessage = sMessage + " - " + messageFileName;
      ErrorManager::Instance()->ReportError(ErrorManager::Critical, 5062, "IMAPFetch::ReportCriticalError_", sErrorMessage);
   }

   String
   IMAPFetch::GetLeftOutRefusal() const
   {
      if (lost_ > 0 && damaged_ > 0)
         return _T("Some of the messages are missing from the message store or could not be read; the others were fetched.");
      if (lost_ > 0)
         return _T("Some of the messages are missing from the message store; the others were fetched.");
      if (damaged_ > 0)
         return _T("Some of the messages could not be read; the others were fetched.");
      return String();
   }

   namespace
   {
      // The HEADER.FIELDS and HEADER.FIELDS.NOT sections as they were up to
      // 6.3.5, word for word but for the names and for ending at the section's
      // text: the list copied, and the header searched for each name in it, for
      // every message; and each field compared with every name left out. What
      // the names prepared once per command (HeaderFieldsLookupFor_) are held to.
      String OldHeaderFields_(std::shared_ptr<MimeBody> pBodyPart, IMAPFetchParser::BodyPart &oPart)
      {
         std::set<String> included_fields;
         std::vector<String> vecHeaderFields = oPart.GetHeaderFields();
         auto iterHeader = vecHeaderFields.begin();
         String sResponse;
         String sFields;

         while (iterHeader != vecHeaderFields.end())
         {
            String sHeaderField = (*iterHeader);

            String header_field_uppercase = sHeaderField;
            header_field_uppercase.MakeUpper();

            if (included_fields.find(header_field_uppercase) == included_fields.end())
            {
               String sValue;

               sValue = pBodyPart->GetRawFieldValue(sHeaderField);

               if (sValue.GetLength() > 0)
               {
                  if (!sFields.IsEmpty())
                     sFields += " ";

                  sFields += sHeaderField;

                  sResponse += sHeaderField + ": " + sValue + "\r\n";
               }

               included_fields.insert(header_field_uppercase);
            }

            iterHeader++;
         }

         sResponse += "\r\n";

         return sResponse;
      }

      String OldHeaderFieldsNOT_(std::shared_ptr<MimeBody> pBodyPart, IMAPFetchParser::BodyPart &oPart)
      {
         std::vector<String> vecHeaderFieldsNOT = oPart.GetHeaderFieldsNOT();

         String sResponse;

         String sFields;

         std::vector<MimeField> lstFields = pBodyPart->Fields();
         auto iterHeader = lstFields.begin();

         while (iterHeader != lstFields.end())
         {
            MimeField oField = (*iterHeader);

            String sHeaderName = oField.GetName();

            // Should we send this header?
            auto iterExclude = vecHeaderFieldsNOT.begin();
            bool bSendThisHeader = true;
            while (iterExclude != vecHeaderFieldsNOT.end())
            {
               if ((*iterExclude).CompareNoCase(sHeaderName) == 0)
               {
                  bSendThisHeader = false;
                  break;
               }
               iterExclude++;
            }

            // Check whether we should include this header.
            if (bSendThisHeader)
            {
               String sHeaderValue = oField.GetValue();

               if (sHeaderValue.GetLength() > 0)
               {
                  if (!sFields.IsEmpty())
                     sFields += " ";

                  sFields += sHeaderName;

                  sResponse += sHeaderName + ": " + sHeaderValue + "\r\n";

               }
            }

            iterHeader++;
         }


         sResponse += "\r\n";

         return sResponse;
      }

      // xorshift64*: the same cases on every run and every platform.
      unsigned long long NextHeaderFieldsRandom_(unsigned long long &state)
      {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return state * 2685821657736338717ULL;
      }

      double HeaderFieldsMillisecondsSince_(std::chrono::steady_clock::time_point started)
      {
         return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      }

      // One message's header: up to nine fields, named from a few names in
      // their case variants, 8-bit names, an empty name and a line with no
      // colon; their values empty, blank, folded over lines, 8-bit.
      AnsiString GeneratedHeader_(unsigned long long &state)
      {
         static const char *names[] =
         {
            "Subject", "SUBJECT", "subject", "From", "To", "X-A", "x-a", "X-a", "X-B",
            "Received", "N1", "n1", "X-\xE9", "X-\xC9", "X-?", "", "X Sp", "Content-Type"
         };
         static const char *values[] =
         {
            "v", "w", "", " ", "a b", "x: y", "a\r\n b", "a\r\n\tb\r\n c", "\xE9t\xE9", "=?utf-8?q?x?="
         };
         const int nameCount = (int) (sizeof(names) / sizeof(names[0]));
         const int valueCount = (int) (sizeof(values) / sizeof(values[0]));

         AnsiString header;
         const int fields = (int) (NextHeaderFieldsRandom_(state) % 10);
         for (int i = 0; i < fields; i++)
         {
            const int shape = (int) (NextHeaderFieldsRandom_(state) % 16);
            if (shape == 0)
            {
               header += "NoColon\r\n";
               continue;
            }

            header += names[NextHeaderFieldsRandom_(state) % nameCount];
            header += shape == 1 ? " : " : (shape == 2 ? ":" : ": ");
            header += values[NextHeaderFieldsRandom_(state) % valueCount];
            header += "\r\n";
         }

         header += "\r\nBody.\r\n";
         return header;
      }

      // A list of up to eight names, some twice, in other cases, absent from
      // every header, wide characters that narrow to the same byte or to none,
      // and now and then two spaces between two.
      String GeneratedNames_(unsigned long long &state)
      {
         static const wchar_t *names[] =
         {
            L"Subject", L"SUBJECT", L"subject", L"sUbJeCt", L"From", L"FROM", L"To", L"X-A", L"x-a",
            L"X-B", L"x-b", L"Received", L"N1", L"n1", L"Missing", L"X-\u00E9", L"X-\u00C9",
            L"X-\u20AC", L"X-?", L"\u0130", L"Content-Type", L"content-type"
         };
         const int nameCount = (int) (sizeof(names) / sizeof(names[0]));

         String list;
         const int count = (int) (NextHeaderFieldsRandom_(state) % 9);
         for (int i = 0; i < count; i++)
         {
            if (i > 0)
               list += NextHeaderFieldsRandom_(state) % 8 == 0 ? _T("  ") : _T(" ");
            list += names[NextHeaderFieldsRandom_(state) % nameCount];
         }

         return list;
      }
   }

   void
   IMAPFetchHeaderFieldsTester::Test(int cases)
   {
      // The time first. One command's HEADER.FIELDS section over 1,000 messages,
      // naming 30,000 fields of which each header holds one among its 21: 6.3.5
      // copied the list and searched the header for each name, for every
      // message - 30 million searches; each message's 21 fields are looked up
      // among the names now. The section alone, without the answer around it,
      // which repeats the list for every message whatever the server does.
      {
         String command = _T("(BODY.PEEK[HEADER.FIELDS (Subject");
         for (int i = 1; i < 30000; i++)
         {
            String name;
            name.Format(_T(" n%d"), i);
            command += name;
         }
         command += _T(")])");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK || parser.GetPartsToLookAt().size() != 1)
            HM_TEST_FAILED();

         IMAPFetchParser::BodyPart &part = parser.GetPartsToLookAt()[0];

         AnsiString header;
         for (int i = 1; i <= 20; i++)
            header += "X-F" + std::to_string(i) + ": v\r\n";
         header += "Subject: s\r\n\r\n";

         std::shared_ptr<MimeBody> body(new MimeBody);
         body->MimeHeader::Load(header, header.GetLength());

         IMAPFetch fetch;
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         for (int message = 0; message < 1000; message++)
         {
            std::shared_ptr<ByteBuffer> section = fetch.GetByteBufferByBodyPart_(_T(""), body, part);
            if (section->GetSize() != 14 || memcmp(section->GetCharBuffer(), "Subject: s\r\n\r\n", 14) != 0)
               HM_TEST_FAILED();
         }
         const double milliseconds = HeaderFieldsMillisecondsSince_(started);

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("HEADER.FIELDS of 30,000 names over 1,000 messages took " + std::to_string((long long) milliseconds) + " ms");
      }

      // Then what the sections hold: generated commands of one or two
      // HEADER.FIELDS and HEADER.FIELDS.NOT parts, some partial, each over three
      // generated headers through one IMAPFetch, as one command's messages are,
      // byte for byte against 6.3.5's code.
      static const wchar_t *suffixes[] = { L"", L"", L"", L"<0.10>", L"<5>", L"<3.1>", L"<0.100000>", L"<7.0>" };
      const int suffixCount = (int) (sizeof(suffixes) / sizeof(suffixes[0]));

      unsigned long long state = 0x9E3779B97F4A7C15ULL;
      int commands = 0;
      int sections = 0;
      int sectionsWithFields = 0;

      for (int number = 0; number < cases; number++)
      {
         String command = _T("(");
         const int partCount = 1 + (int) (NextHeaderFieldsRandom_(state) % 2);
         for (int p = 0; p < partCount; p++)
         {
            if (p > 0)
               command += _T(" ");
            command += NextHeaderFieldsRandom_(state) % 2 == 0 ? _T("BODY.PEEK[HEADER.FIELDS.NOT (") : _T("BODY.PEEK[HEADER.FIELDS (");
            command += GeneratedNames_(state);
            command += _T(")]");
            command += suffixes[NextHeaderFieldsRandom_(state) % suffixCount];
         }
         command += _T(")");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK)
            continue;

         commands++;

         std::vector<IMAPFetchParser::BodyPart> &parts = parser.GetPartsToLookAt();

         IMAPFetch fetch;
         for (int message = 0; message < 3; message++)
         {
            const AnsiString header = GeneratedHeader_(state);
            std::shared_ptr<MimeBody> body(new MimeBody);
            body->MimeHeader::Load(header, header.GetLength());

            for (IMAPFetchParser::BodyPart &part : parts)
            {
               String text;
               if (part.GetShowBodyHeaderFields())
                  text = OldHeaderFields_(body, part);
               else if (part.GetShowBodyHeaderFieldsNOT())
                  text = OldHeaderFieldsNOT_(body, part);
               else
                  throw std::logic_error("Case " + std::to_string(number) + " made a part that is not HEADER.FIELDS");

               int start = 0;
               int count = 0;
               fetch.GetBytesToSend_(text.GetLength(), part, start, count);
               const AnsiString expected = text.Mid(start, count);

               std::shared_ptr<ByteBuffer> got = fetch.GetByteBufferByBodyPart_(_T(""), body, part);

               if (got->GetSize() != expected.size() ||
                   (expected.size() > 0 && memcmp(got->GetCharBuffer(), expected.c_str(), expected.size()) != 0))
                  throw std::logic_error("HEADER.FIELDS answered other than 6.3.5's code, in case " + std::to_string(number));

               sections++;
               if (text.GetLength() > 2)
                  sectionsWithFields++;
            }
         }
      }

      // What was compared is something: nearly every command parsed, and many
      // sections hold a field.
      if (commands < cases - cases / 10 || sectionsWithFields < sections / 4)
         throw std::logic_error("HEADER.FIELDS cases compared: " + std::to_string(commands) + " commands, " +
            std::to_string(sections) + " sections, " + std::to_string(sectionsWithFields) + " with fields");
   }

   namespace
   {
      // 6.3.5's walk to the body a part number names, word for word but for the
      // names and for leaving out the error an encapsulated message that does
      // not parse reported: each section listed a body's parts from the first
      // to the one it named, and parsed an encapsulated message afresh on its
      // way through one. What the bodies kept for a message's sections
      // (NavigatedBody_) are held to.
      std::shared_ptr<MimeBody> OldPartByNumber_(std::shared_ptr<MimeBody> pBody, long iPartNo)
      {
         if (!pBody)
         {
            std::shared_ptr<MimeBody> pEmpty;
            return pEmpty;
         }

         // Fetch the first part of the message.
         std::shared_ptr<MimeBody> oPart = pBody->FindFirstPart();

         if (!oPart)
         {
            // This is a single part message. Should we send the first part?
            if (iPartNo == 1)
               return pBody;
         }

         int iCurIdx = 1;

         while (oPart)
         {
            if (iPartNo == iCurIdx)
               return oPart;

            iCurIdx++;
            oPart = pBody->FindNextPart();
         }

         std::shared_ptr<MimeBody> pEmpty;
         return pEmpty;
      }

      std::shared_ptr<MimeBody> OldBodyByPartNumber_(std::shared_ptr<MimeBody> pBody, const String &sName, bool loadEncapsulated)
      {
         if (!pBody || sName.IsEmpty())
         {
            std::shared_ptr<MimeBody> pEmpty;
            return pEmpty;
         }

         String sTempName = sName;

         std::vector<String> vecPartPath = StringParser::SplitString(sTempName, ".");

         auto iterPart = vecPartPath.begin();

         while (iterPart != vecPartPath.end())
         {
            String sSubPart = *iterPart;

            if (StringParser::IsNumeric(sSubPart))
            {
               int iRequestPartNo = _ttoi(sSubPart);

               pBody = OldPartByNumber_(pBody, iRequestPartNo);

               if (!pBody)
                  return pBody;

               auto nextIter = std::next(iterPart);
               bool isFinalComponent = (nextIter == vecPartPath.end());
               bool shouldLoad = isFinalComponent ? loadEncapsulated : true;

               if (shouldLoad && pBody->IsEncapsulatedRFC822Message())
               {
                  try
                  {
                     pBody = pBody->LoadEncapsulatedMessage();
                  }
                  catch (...)
                  {
                     std::shared_ptr<MimeBody> empty;
                     return empty;
                  }
               }
            }

            iterPart++;
         }

         return pBody;
      }

      // A message's text parsed with no file: LoadFromMemory keeps the bytes it
      // parses (MimeBody::GetSourceBytes), and a section a FETCH serves as
      // stored is cut from them, so the testers need no file. The server's own
      // parse keeps none - MessageStore::LoadMime parses a file with
      // LoadFromFile and a container's original with Load - and IMAPFetch reads
      // the stored message through the store instead (StoredMessage_), which
      // IMAPFetchPartBytesTester holds to the bytes written over messages it
      // parses from a file of their own the server's way.
      std::shared_ptr<MimeBody> ParsedMessage_(const std::string &text)
      {
         std::shared_ptr<MimeBody> body(new MimeBody);
         body->LoadFromMemory(std::shared_ptr<const std::string>(new std::string(text)), _T("generated"), false);

         return body;
      }

      // One entity of a generated message, as deep as depth allows: a text part
      // plain or quoted-printable, a base64 attachment, a multipart of up to
      // four parts mixed or alternative with a preamble and an epilogue, an
      // encapsulated message with a disposition or none, an entity with no
      // Content-Type; now and then with a field of its own.
      std::string GeneratedEntity_(unsigned long long &state, int depth)
      {
         std::string entity;
         if (NextHeaderFieldsRandom_(state) % 3 == 0)
            entity += "X-A: " + std::to_string(depth) + "\r\n";

         // Leaves only, three deep; above, a multipart or an encapsulated
         // message as often as a leaf.
         static const int kindsAbove[] = { 0, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
         const int kind = depth >= 3 ? (int) (NextHeaderFieldsRandom_(state) % 3) :
            kindsAbove[NextHeaderFieldsRandom_(state) % (sizeof(kindsAbove) / sizeof(kindsAbove[0]))];
         switch (kind)
         {
         case 0:
            entity += "Content-Type: text/plain\r\n\r\nline " + std::to_string(NextHeaderFieldsRandom_(state) % 100) + "\r\n";
            break;
         case 1:
            entity += "Content-Type: application/octet-stream\r\nContent-Transfer-Encoding: base64\r\n\r\naGVsbG8gd29ybGQ=\r\n";
            break;
         case 2:
            entity += "Content-Type: text/plain\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\na=3Db=\r\nc\r\n";
            break;
         case 3:
         case 4:
         {
            const std::string boundary = "b" + std::to_string(depth) + "x" + std::to_string(NextHeaderFieldsRandom_(state) % 1000);
            entity += std::string("Content-Type: multipart/") + (kind == 3 ? "mixed" : "alternative") + "; boundary=\"" + boundary + "\"\r\n\r\npreamble\r\n";

            const int parts = (int) (NextHeaderFieldsRandom_(state) % 5);
            for (int i = 0; i < parts; i++)
               entity += "--" + boundary + "\r\n" + GeneratedEntity_(state, depth + 1) + "\r\n";

            entity += "--" + boundary + "--\r\nepilogue\r\n";
            break;
         }
         case 5:
            entity += "Content-Type: message/rfc822\r\n";
            if (NextHeaderFieldsRandom_(state) % 2 == 0)
               entity += "Content-Disposition: attachment\r\n";
            entity += "\r\nSubject: inner " + std::to_string(depth) + "\r\n" + GeneratedEntity_(state, depth + 1);
            break;
         default:
            entity += "\r\nno type " + std::to_string(depth) + "\r\n";
            break;
         }

         return entity;
      }

      // A part number of one to four components, the shorter more often: parts
      // that are there and parts that are not, 0, and - but for a section's -
      // a leading zero, an empty component and one that is not a number.
      String GeneratedPartNumber_(unsigned long long &state, bool numbersOnly)
      {
         static const wchar_t *components[] = { L"1", L"1", L"1", L"2", L"2", L"2", L"3", L"4", L"0", L"10", L"01", L"", L"x" };
         const int count = (int) (sizeof(components) / sizeof(components[0])) - (numbersOnly ? 3 : 0);

         static const int depths[] = { 1, 1, 1, 1, 2, 2, 2, 3, 3, 4 };
         String name;
         const int depth = depths[NextHeaderFieldsRandom_(state) % (sizeof(depths) / sizeof(depths[0]))];
         for (int i = 0; i < depth; i++)
         {
            if (i > 0)
               name += _T(".");
            name += components[NextHeaderFieldsRandom_(state) % count];
         }

         return name;
      }

      // A body found by a part number, written out with its header, and what its
      // header says it is: what two bodies found apart are compared by.
      std::string DescribedBody_(const std::shared_ptr<MimeBody> &body)
      {
         if (!body)
            return "none";

         AnsiString written;
         body->Store(written, true);

         std::string described = written;
         described += body->IsEncapsulatedRFC822Message() ? "|message" : "|";
         described += body->IsMultiPart() ? "|multipart" : "|";
         return described;
      }

      bool SameBytes_(const std::shared_ptr<ByteBuffer> &one, const std::shared_ptr<ByteBuffer> &other)
      {
         return one->GetSize() == other->GetSize() &&
            (one->GetSize() == 0 || memcmp(one->GetCharBuffer(), other->GetCharBuffer(), one->GetSize()) == 0);
      }
   }

   void
   IMAPFetchSectionsTester::Test(int cases)
   {
      // Generated commands of one to six sections of parts, each through one
      // IMAPFetch over three generated messages, as one command's messages are.
      // The bodies the part numbers name, as the bodies kept for a message's
      // sections find them, against 6.3.5's walk: the same body where no
      // encapsulated message was parsed on the way, and one written out the
      // same where one was. And each section's bytes, through the IMAPFetch
      // that keeps the bodies for all of the message's sections and through one
      // of its own: the same.
      static const wchar_t *kinds[][2] =
      {
         { L"BODY.PEEK[", L"]" }, { L"BODY.PEEK[", L".MIME]" }, { L"BODY.PEEK[", L".HEADER]" }, { L"BODY.PEEK[", L".TEXT]" },
         { L"BODY.PEEK[", L".HEADER.FIELDS (Subject X-A Content-Type)]" }, { L"BODY.PEEK[", L".HEADER.FIELDS.NOT (Subject)]" },
         { L"BINARY.PEEK[", L"]" }, { L"BINARY.SIZE[", L"]" }
      };
      static const wchar_t *suffixes[] = { L"", L"", L"<0.5>", L"<3>", L"<2.0>" };
      const int kindCount = (int) (sizeof(kinds) / sizeof(kinds[0]));
      const int suffixCount = (int) (sizeof(suffixes) / sizeof(suffixes[0]));

      unsigned long long state = 0xD1B54A32D192ED03ULL;

      int found = 0;
      int foundParsed = 0;
      int foundNone = 0;
      int sections = 0;
      int sectionsWithBytes = 0;

      for (int number = 0; number < cases; number++)
      {
         String command = _T("(");
         const int sectionCount = 1 + (int) (NextHeaderFieldsRandom_(state) % 6);
         for (int s = 0; s < sectionCount; s++)
         {
            const int kind = (int) (NextHeaderFieldsRandom_(state) % kindCount);

            String section = kinds[kind][0];
            section += GeneratedPartNumber_(state, true);
            section += kinds[kind][1];
            if (kind != kindCount - 1)
               section += suffixes[NextHeaderFieldsRandom_(state) % suffixCount];

            if (s > 0)
               command += _T(" ");
            command += section;
         }
         command += _T(")");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK)
            throw std::logic_error("Case " + std::to_string(number) + " made a command that did not parse");

         IMAPFetch fetch;

         for (int m = 0; m < 3; m++)
         {
            std::shared_ptr<MimeBody> message = ParsedMessage_("Subject: s\r\n" + GeneratedEntity_(state, 0));

            for (int name = 0; name < 4; name++)
            {
               const String partNumber = GeneratedPartNumber_(state, false);
               const bool loadEncapsulated = NextHeaderFieldsRandom_(state) % 2 == 0;

               fetch.section_cost_ = 0;
               fetch.section_refused_ = false;

               std::shared_ptr<MimeBody> kept = fetch.GetBodyPartByRecursiveIdentifier_(message, partNumber, loadEncapsulated);
               std::shared_ptr<MimeBody> walked = OldBodyByPartNumber_(message, partNumber, loadEncapsulated);

               if (fetch.section_refused_)
                  throw std::logic_error("A part number was refused outside DoAction, in case " + std::to_string(number));

               if (!walked)
               {
                  if (kept)
                     throw std::logic_error("A part number found a body 6.3.5 did not, in case " + std::to_string(number));

                  foundNone++;
                  continue;
               }

               if (DescribedBody_(kept) != DescribedBody_(walked))
                  throw std::logic_error("A part number found another body than 6.3.5, in case " + std::to_string(number));

               found++;
               if (kept != walked)
                  foundParsed++;
            }

            for (IMAPFetchParser::BodyPart &part : parser.GetPartsToLookAt())
            {
               std::shared_ptr<ByteBuffer> kept = fetch.GetByteBufferByBodyPart_(_T(""), message, part);

               IMAPFetch own;
               std::shared_ptr<ByteBuffer> separate = own.GetByteBufferByBodyPart_(_T(""), message, part);

               if (fetch.section_refused_ || !SameBytes_(kept, separate))
                  throw std::logic_error("A section of the bodies kept was other than its own, in case " + std::to_string(number));

               sections++;
               if (kept->GetSize() > 0)
                  sectionsWithBytes++;
            }
         }
      }

      if (found < cases * 2 || foundParsed < cases / 4 || foundNone < cases * 4 || sectionsWithBytes < cases)
         throw std::logic_error("Part numbers compared: " + std::to_string(found) + " found, " + std::to_string(foundParsed) +
            " through a parsed message, " + std::to_string(foundNone) + " not found; sections " + std::to_string(sections) +
            ", " + std::to_string(sectionsWithBytes) + " with bytes");

      // A multipart of 9,000 parts - a multipart is read with up to 10,000 -
      // whose last part is another, whose last part is another: the MIME
      // header of every part of the third, and of its last part 91,000 times
      // more, in one command. Each section listed the parts of each from the
      // first to the one it named, two and a half billion steps.
      {
         const int parts = 9000;
         const int sections = 100000;

         std::string text;
         for (int level = 1; level <= 3; level++)
         {
            const std::string boundary = "b" + std::to_string(level);
            text += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
            for (int i = 1; i < parts; i++)
               text += "--" + boundary + "\r\nX-N: " + std::to_string(i) + "\r\n\r\nv\r\n";
            text += "--" + boundary + "\r\n";
         }
         text += "X-N: " + std::to_string(parts) + "\r\n\r\nv\r\n";
         for (int level = 3; level >= 1; level--)
            text += "--b" + std::to_string(level) + "--\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         String command = _T("(");
         for (int i = 0; i < sections; i++)
         {
            String item;
            item.Format(_T("BODY.PEEK[%d.%d.%d.MIME]"), parts, parts, i < parts ? i + 1 : parts);
            if (i > 0)
               command += _T(" ");
            command += item;
         }
         command += _T(")");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK || parser.GetPartsToLookAt().size() != (size_t) sections)
            HM_TEST_FAILED();

         IMAPFetch many;
         int number = 0;
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         for (IMAPFetchParser::BodyPart &part : parser.GetPartsToLookAt())
         {
            const std::string expected = "X-N: " + std::to_string(number < parts ? number + 1 : parts) + "\r\n";
            number++;

            std::shared_ptr<ByteBuffer> section = many.GetByteBufferByBodyPart_(_T(""), message, part);
            if (section->GetSize() < expected.size() || memcmp(section->GetCharBuffer(), expected.c_str(), expected.size()) != 0)
               HM_TEST_FAILED();
         }
         const double milliseconds = HeaderFieldsMillisecondsSince_(started);

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("100,000 MIME sections of parts 9,000 deep in three multiparts took " + std::to_string((long long) milliseconds) + " ms");

         // And of the multipart holding the other two, 18,000 parts below it:
         // its MIME header and the first bytes of its body, cut from the stored
         // bytes, 50,000 times each. Whether it was changed since it was read is
         // asked of its own header and body at each section, and of the
         // message's parts once (MimeBody::GetStoredBounds,
         // IMAPFetch::StoredMessage_) - not of the 18,000 at each, 1.8 billion
         // steps.
         String holderMime;
         holderMime.Format(_T("BODY.PEEK[%d.MIME]"), parts);
         String holderBody;
         holderBody.Format(_T("BODY.PEEK[%d]<0.4>"), parts);

         String holderCommand = _T("(");
         for (int i = 0; i < sections; i++)
         {
            if (i > 0)
               holderCommand += _T(" ");
            holderCommand += i % 2 == 0 ? holderMime : holderBody;
         }
         holderCommand += _T(")");

         IMAPFetchParser holderParser;
         if (holderParser.ParseCommand(holderCommand).GetResult() != IMAPResult::ResultOK || holderParser.GetPartsToLookAt().size() != (size_t) sections)
            HM_TEST_FAILED();

         const std::string holderHeader = "Content-Type: multipart/mixed; boundary=\"b2\"\r\n\r\n";
         const std::string holderStart = "--b2";

         IMAPFetch holder;
         int asked = 0;
         std::chrono::steady_clock::time_point holderStarted = std::chrono::steady_clock::now();
         for (IMAPFetchParser::BodyPart &part : holderParser.GetPartsToLookAt())
         {
            const std::string &expected = asked % 2 == 0 ? holderHeader : holderStart;
            asked++;

            std::shared_ptr<ByteBuffer> section = holder.GetByteBufferByBodyPart_(_T(""), message, part);
            if (section->GetSize() != expected.size() || memcmp(section->GetCharBuffer(), expected.c_str(), expected.size()) != 0)
               HM_TEST_FAILED();
         }
         const double holderMilliseconds = HeaderFieldsMillisecondsSince_(holderStarted);

         if (holderMilliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("100,000 sections of a multipart holding 18,000 parts, its MIME header and the first bytes of its body, took " +
               std::to_string((long long) holderMilliseconds) + " ms");
      }

      // Sections of the parts of a forwarded message of 4 MB, 4,000 of them in
      // one command: each parsed the forwarded message afresh, 16 GB.
      {
         std::string inner = "Subject: inner\r\nContent-Type: multipart/mixed; boundary=\"i\"\r\n\r\n";
         for (int i = 1; i <= 20; i++)
            inner += "--i\r\nX-N: " + std::to_string(i) + "\r\n\r\n" + std::string(200000, 'v') + "\r\n";
         inner += "--i--\r\n";

         const std::string text =
            "Content-Type: multipart/mixed; boundary=\"o\"\r\n\r\n"
            "--o\r\nContent-Type: text/plain\r\n\r\nfirst\r\n"
            "--o\r\nContent-Type: message/rfc822\r\n\r\n" + inner + "\r\n"
            "--o--\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         const int sections = 4000;
         String command = _T("(");
         for (int i = 0; i < sections; i++)
         {
            String item;
            item.Format(_T("BODY.PEEK[2.%d.MIME]"), 1 + i % 20);
            if (i > 0)
               command += _T(" ");
            command += item;
         }
         command += _T(")");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK || parser.GetPartsToLookAt().size() != (size_t) sections)
            HM_TEST_FAILED();

         IMAPFetch forwarded;
         int number = 0;
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         for (IMAPFetchParser::BodyPart &part : parser.GetPartsToLookAt())
         {
            const std::string expected = "X-N: " + std::to_string(1 + number % 20) + "\r\n";
            number++;

            std::shared_ptr<ByteBuffer> section = forwarded.GetByteBufferByBodyPart_(_T(""), message, part);
            if (section->GetSize() < expected.size() || memcmp(section->GetCharBuffer(), expected.c_str(), expected.size()) != 0)
               HM_TEST_FAILED();
         }
         const double milliseconds = HeaderFieldsMillisecondsSince_(started);

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("4,000 sections of a forwarded message of 4 MB took " + std::to_string((long long) milliseconds) + " ms");
      }

      // BINARY.PEEK[1]<0.1> 5,000 times over a part with 100,000 header fields:
      // each section searched the part's header for its type, its disposition
      // and its encoding, five passes over the 100,000 fields.
      {
         std::string text = "Content-Type: multipart/mixed; boundary=\"h\"\r\n\r\n--h\r\n";
         for (int i = 0; i < 100000; i++)
            text += "X-H: v\r\n";
         text += "\r\nshort\r\n--h--\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         const int sections = 5000;
         String command = _T("(");
         for (int i = 0; i < sections; i++)
            command += i > 0 ? _T(" BINARY.PEEK[1]<0.1>") : _T("BINARY.PEEK[1]<0.1>");
         command += _T(")");

         IMAPFetchParser parser;
         if (parser.ParseCommand(command).GetResult() != IMAPResult::ResultOK || parser.GetPartsToLookAt().size() != (size_t) sections)
            HM_TEST_FAILED();

         IMAPFetch binary;
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         for (IMAPFetchParser::BodyPart &part : parser.GetPartsToLookAt())
         {
            std::shared_ptr<ByteBuffer> section = binary.GetByteBufferByBodyPart_(_T(""), message, part);
            if (section->GetSize() != 1 || section->GetCharBuffer()[0] != 's')
               HM_TEST_FAILED();
         }
         const double milliseconds = HeaderFieldsMillisecondsSince_(started);

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("BINARY of a part of 100,000 header fields 5,000 times took " + std::to_string((long long) milliseconds) + " ms");
      }

      // A part number through 5,000 encapsulated messages, each in the one
      // before: each was parsed whole on the way, so one section of a message
      // of 160 kB parsed 400 MB. Under the ceiling of a message's sections it
      // stops when the parsing would pass it, and says so; a part number a few
      // deep is found as 6.3.5 found it.
      {
         const int depth = 5000;

         std::string text;
         for (int i = 0; i < depth; i++)
            text += "Content-Type: message/rfc822\r\n\r\n";
         text += "Subject: innermost\r\n\r\ninnermost\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         String shallow = _T("1.1.1");
         String deep = _T("1");
         for (int i = 1; i < depth; i++)
            deep += _T(".1");

         // As DoAction sets the ceiling, with the file's size known.
         IMAPFetch nested;
         nested.ceiling_set_ = true;
         nested.ceiling_largest_ = text.size();
         nested.ceiling_used_ = 0;
         nested.ceiling_file_sized_ = true;

         nested.section_cost_ = 0;
         nested.section_refused_ = false;
         std::shared_ptr<MimeBody> few = nested.GetBodyPartByRecursiveIdentifier_(message, shallow, true);
         if (!few || nested.section_refused_ || DescribedBody_(few) != DescribedBody_(OldBodyByPartNumber_(message, shallow, true)))
            HM_TEST_FAILED();

         nested.section_cost_ = 0;
         nested.section_refused_ = false;
         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         std::shared_ptr<MimeBody> many = nested.GetBodyPartByRecursiveIdentifier_(message, deep, true);
         const double milliseconds = HeaderFieldsMillisecondsSince_(started);

         const size_t ceiling = text.size() * 16 + size_t{1024} * 1024;
         if (many || !nested.section_refused_ || nested.section_cost_ > ceiling)
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A part number through 5,000 encapsulated messages took " + std::to_string((long long) milliseconds) + " ms to refuse");

         nested.ForgetMessageSections_();
      }
   }

   namespace
   {
      // A part of a generated message as it was written: its IMAP part number,
      // its MIME header - its lines up to and with the blank line, or every line
      // of a part that has no blank line and no body - and its body; for a
      // message/rfc822 part, the header and the body of the message it holds.
      class WrittenPart_
      {
      public:
         std::string number;
         std::string header;
         std::string body;
         bool multipart = false;
         bool message = false;
         std::string inner_header;
         std::string inner_body;

         // A part that is a header alone; a multipart with no closing boundary.
         bool header_alone = false;
         bool unclosed = false;
      };

      // Writes messages as senders write them, and says what each part is.
      class PartWriter_
      {
      public:
         explicit PartWriter_(unsigned long long seed) : state_(seed) {}

         std::string Message(std::vector<WrittenPart_> &parts)
         {
            parts.clear();

            std::string header;
            std::string body;
            Entity_(0, true, std::string(), std::string(), "Subject: s\r\n", header, body, parts);
            return header + body;
         }

      private:
         unsigned long long Next_(unsigned long long below) { return NextHeaderFieldsRandom_(state_) % below; }

         static std::string Number_(const std::string &prefix, int index)
         {
            return prefix.empty() ? std::to_string(index) : prefix + "." + std::to_string(index);
         }

         // A field: folded with a space or a tab, without a space after the
         // colon or with one before it, with a tab, 8-bit, a bare line feed in
         // it, empty, in lower case.
         std::string Field_()
         {
            static const char *fields[] =
            {
               "X-A: v\r\n",
               "X-Folded: one\r\n two\r\n",
               "X-Folded-Twice: one\r\n\ttwo\r\n  three\r\n",
               "X-No-Space:value\r\n",
               "X-Tab:\tvalue\r\n",
               "X-Before-Colon : v\r\n",
               "X-Eight: caf\xE9 cr\xC3\xA8me\r\n",
               "X-Bare-LF: a\nb\r\n",
               "X-Empty:\r\n",
               "x-lower: v\r\n"
            };
            return fields[Next_(sizeof(fields) / sizeof(fields[0]))];
         }

         std::string Fields_()
         {
            std::string fields;
            const int count = (int) Next_(3);
            for (int i = 0; i < count; i++)
               fields += Field_();
            return fields;
         }

         // The body of a part that is not a multipart or a message.
         std::string LeafBody_()
         {
            static const char *bodies[] =
            {
               "line\r\n",
               "",
               "8-bit caf\xE9 cr\xC3\xA8me\r\nmore\r\n",
               "bare\nline\nfeeds\n",
               "mixed\r\nline\nends\r\n",
               "no final line break",
               "blank lines after\r\n\r\n\r\n",
               "  indented\r\n\tand tabbed\r\n",
               "-- \r\nsignature\r\n",
               "aGVsbG8gd29ybGQ=\r\n"
            };
            return bodies[Next_(sizeof(bodies) / sizeof(bodies[0]))];
         }

         std::string LeafType_()
         {
            static const char *types[] =
            {
               "Content-Type: text/plain\r\n",
               "Content-Type: text/plain; charset=\"iso-8859-1\"\r\nContent-Transfer-Encoding: 8bit\r\n",
               "content-type:text/html\r\n",
               "Content-Type: text/plain;\r\n charset=us-ascii\r\n",
               "Content-Type: application/octet-stream\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"a.bin\"\r\n",
               ""
            };
            return types[Next_(sizeof(types) / sizeof(types[0]))];
         }

         // An entity: a root - the message, or the one a message/rfc822 part
         // holds, whose header begins with rootFields - or a part of a
         // multipart, numbered number. A root answers to the part number a
         // client names it by: a multipart root to none, its parts to
         // prefix.1, prefix.2 ... (1, 2 ... for the message); any other root to
         // prefix.1 (1).
         void Entity_(int depth, bool root, const std::string &prefix, const std::string &number, const std::string &rootFields,
                      std::string &header, std::string &body, std::vector<WrittenPart_> &parts)
         {
            // A leaf, a multipart or a message; leaves only, four deep.
            static const int kinds[] = { 0, 0, 0, 1, 1, 1, 2, 2 };
            const int kind = depth >= 4 ? 0 : kinds[Next_(sizeof(kinds) / sizeof(kinds[0]))];

            WrittenPart_ written;
            written.number = root ? (kind == 1 ? std::string() : Number_(prefix, 1)) : number;

            header = rootFields + Fields_();
            body.clear();

            if (kind == 0)
            {
               header += LeafType_() + Fields_();

               // Now and then a part that is a header alone, with no blank line
               // and no body - or, with no fields either, a part of no bytes.
               if (root || Next_(10) != 0)
               {
                  header += "\r\n";
                  body = LeafBody_();
               }
               else
                  written.header_alone = true;
            }
            else if (kind == 1)
            {
               const std::string boundary = "B" + std::to_string(++boundaries_) + "Z";
               header += Next_(2) == 0 ?
                  "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n" :
                  "Content-Type: multipart/alternative;\r\n\tboundary=" + boundary + "\r\n";
               header += Fields_() + "\r\n";

               static const char *preambles[] = { "", "preamble\r\n", "This is a multi-part message.\r\n\r\n" };
               body = preambles[Next_(sizeof(preambles) / sizeof(preambles[0]))];

               const std::string under = root ? prefix : number;
               const int count = 1 + (int) Next_(depth >= 2 ? 2 : 3);
               for (int i = 1; i <= count; i++)
               {
                  std::string partHeader;
                  std::string partBody;
                  Entity_(depth + 1, false, std::string(), Number_(under, i), std::string(), partHeader, partBody, parts);

                  body += (i == 1 ? "--" : "\r\n--") + boundary + "\r\n" + partHeader + partBody;
               }

               // The closing boundary and an epilogue, ending in a line break or
               // not; the boundary and nothing after it; or none, the last part
               // running to the end of the multipart.
               switch (Next_(5))
               {
               case 0: body += "\r\n--" + boundary + "--\r\n"; break;
               case 1: body += "\r\n--" + boundary + "--\r\nepilogue\r\n"; break;
               case 2: body += "\r\n--" + boundary + "--\r\nan epilogue with no line break"; break;
               case 3: body += "\r\n--" + boundary + "--"; break;
               default: written.unclosed = true; break;
               }

               written.multipart = true;
            }
            else
            {
               header += "Content-Type: message/rfc822\r\n";
               if (Next_(2) == 0)
                  header += "Content-Disposition: attachment\r\n";
               header += Fields_() + "\r\n";

               Entity_(depth + 1, true, written.number, std::string(), "Subject: inner\r\n", written.inner_header, written.inner_body, parts);
               body = written.inner_header + written.inner_body;

               written.message = true;
            }

            written.header = header;
            written.body = body;

            if (!written.number.empty())
               parts.push_back(written);
         }

         unsigned long long state_;
         int boundaries_ = 0;
      };

      // Where a body differs from what it should be, readable in a failure.
      std::string Difference_(const std::string &got, const std::string &expected)
      {
         size_t at = 0;
         while (at < got.size() && at < expected.size() && got[at] == expected[at])
            at++;

         return std::to_string(got.size()) + " bytes where " + std::to_string(expected.size()) + " were written, differing from byte " +
            std::to_string(at) + ": \"" + got.substr(at, 40) + "\" where \"" + expected.substr(at, 40) + "\"";
      }

      // A structure's parentheses balance outside its quoted strings, and no
      // item of it is empty: two spaces, or a space before a closing
      // parenthesis, mean something was left out.
      bool StructureReadable_(const std::string &structure)
      {
         int depth = 0;
         bool quoted = false;
         char previous = 0;

         for (size_t i = 0; i < structure.size(); i++)
         {
            const char c = structure[i];

            if (quoted)
            {
               if (c == '\\')
                  i++;
               else if (c == '"')
                  quoted = false;
               previous = '"';
               continue;
            }

            if (c == '"')
               quoted = true;
            else if (c == '(')
               depth++;
            else if (c == ')')
            {
               if (--depth < 0 || previous == ' ' || previous == '(')
                  return false;
            }
            else if (c == ' ' && (previous == ' ' || previous == '('))
               return false;

            previous = c;
         }

         return depth == 0 && !quoted && !structure.empty();
      }
   }

   void
   IMAPFetchPartBytesTester::Test(int cases)
   {
      PartWriter_ writer(0x2545F4914F6CDD1DULL);

      // A section's bytes, through the IMAPFetch that keeps the bodies of one
      // message's sections, as DoAction asks for them, of the message stored in
      // file - none, for a message parsed from memory (ParsedMessage_).
      auto sectionBytesOf = [](IMAPFetch &fetch, const std::shared_ptr<MimeBody> &message, const String &file, const std::string &item)
      {
         IMAPFetchParser parser;
         if (parser.ParseCommand(String(("(" + item + ")").c_str())).GetResult() != IMAPResult::ResultOK || parser.GetPartsToLookAt().size() != 1)
            throw std::logic_error(item + " did not parse");

         std::shared_ptr<ByteBuffer> got = fetch.GetByteBufferByBodyPart_(file, message, parser.GetPartsToLookAt()[0]);
         if (got->GetSize() == 0)
            return std::string();

         return std::string(got->GetCharBuffer(), got->GetSize());
      };

      auto sectionBytes = [&sectionBytesOf](IMAPFetch &fetch, const std::shared_ptr<MimeBody> &message, const std::string &item)
      {
         return sectionBytesOf(fetch, message, String(), item);
      };

      // A file of the test's own, removed however the test ends, for messages
      // parsed as the server parses a message it stores: from the file, through
      // MessageStore::LoadMime, which keeps none of the bytes - so that IMAPFetch
      // reads the stored message through the store (StoredMessage_), as it does
      // in the server, and not from bytes the parse kept.
      struct ScratchFile
      {
         ScratchFile() :
            path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("hm-fetch-part-bytes-%%%%-%%%%-%%%%.eml"))
         {
         }

         ~ScratchFile()
         {
            boost::system::error_code ignored;
            boost::filesystem::remove(path, ignored);
         }

         boost::filesystem::path path;
      } scratch;

      const String file = String(scratch.path.wstring());

      auto storedAndParsed = [&file](const std::string &text)
      {
         if (!FileUtilities::WriteToFile(file, AnsiString(text)))
            throw std::logic_error("A message could not be written to a file of the test's own");

         std::shared_ptr<MimeBody> body(new MimeBody);
         if (MessageStore::LoadMime(AnsiString(file), *body) != MimeLoadResult::Loaded)
            throw std::logic_error("MessageStore::LoadMime did not parse a message written to a file of the test's own");

         // The server's parse keeps no bytes: the sections cut from the stored
         // message are cut from what StoredMessage_ reads.
         if (body->GetSourceBytes())
            throw std::logic_error("MessageStore::LoadMime kept the bytes of a stored message, so the read the server makes is not what this tests");

         return body;
      };

      int parts = 0;
      int multiparts = 0;
      int messages = 0;
      int headersAlone = 0;
      int multipartsUnclosed = 0;
      int stored = 0;

      for (int number = 0; number < cases; number++)
      {
         std::vector<WrittenPart_> written;
         const std::string text = writer.Message(written);

         // One message in four from a file, as the server parses it, the rest
         // from memory.
         const bool fromFile = number % 4 == 0;
         std::shared_ptr<MimeBody> message = fromFile ? storedAndParsed(text) : ParsedMessage_(text);
         if (fromFile)
            stored++;

         const String messageFile = fromFile ? file : String();

         IMAPFetch fetch;

         for (const WrittenPart_ &part : written)
         {
            struct Expected
            {
               std::string item;
               const std::string *bytes;
            };

            const std::string &body = part.body;

            std::vector<Expected> expected;
            expected.push_back(Expected{ "BODY.PEEK[" + part.number + ".MIME]", &part.header });
            expected.push_back(Expected{ "BODY.PEEK[" + part.number + "]", &body });

            if (part.multipart)
            {
               // A multipart's content is identity: BINARY is its body.
               expected.push_back(Expected{ "BINARY.PEEK[" + part.number + "]", &body });
               expected.push_back(Expected{ "BINARY.SIZE[" + part.number + "]", &body });
            }

            if (part.message)
            {
               expected.push_back(Expected{ "BODY.PEEK[" + part.number + ".HEADER]", &part.inner_header });
               expected.push_back(Expected{ "BODY.PEEK[" + part.number + ".TEXT]", &part.inner_body });
            }
            else
            {
               expected.push_back(Expected{ "BODY.PEEK[" + part.number + ".HEADER]", &part.header });
               expected.push_back(Expected{ "BODY.PEEK[" + part.number + ".TEXT]", &body });
            }

            for (const Expected &one : expected)
            {
               const std::string got = sectionBytesOf(fetch, message, messageFile, one.item);
               if (got != *one.bytes)
                  throw std::logic_error("Case " + std::to_string(number) + ", " + one.item + ": " + Difference_(got, *one.bytes));
            }

            // Partial ranges, cut from the same bytes.
            const size_t start = body.size() < 3 ? body.size() : 3;
            const std::string partial = body.substr(start, 5);
            const std::string gotPartial = sectionBytesOf(fetch, message, messageFile, "BODY.PEEK[" + part.number + "]<3.5>");
            if (gotPartial != partial)
               throw std::logic_error("Case " + std::to_string(number) + ", BODY.PEEK[" + part.number + "]<3.5>: " + Difference_(gotPartial, partial));

            const std::string headerPartial = part.header.substr(0, 7);
            const std::string gotHeaderPartial = sectionBytesOf(fetch, message, messageFile, "BODY.PEEK[" + part.number + ".MIME]<0.7>");
            if (gotHeaderPartial != headerPartial)
               throw std::logic_error("Case " + std::to_string(number) + ", BODY.PEEK[" + part.number + ".MIME]<0.7>: " + Difference_(gotHeaderPartial, headerPartial));

            parts++;
            if (part.multipart)
               multiparts++;
            if (part.unclosed)
               multipartsUnclosed++;
            if (part.message)
               messages++;
            if (part.header_alone)
               headersAlone++;
         }
      }

      // What was compared is something: every kind of part, many times, and a
      // quarter of the messages read from their files.
      if (parts < cases * 2 || multiparts < cases / 4 || multipartsUnclosed < cases / 20 || messages < cases / 4 || headersAlone < cases / 50 || stored < cases / 4)
         throw std::logic_error("Parts compared: " + std::to_string(parts) + ", of them " + std::to_string(multiparts) + " multiparts (" +
            std::to_string(multipartsUnclosed) + " not closed), " + std::to_string(messages) + " messages and " + std::to_string(headersAlone) +
            " headers alone; " + std::to_string(stored) + " messages from their files");

      // The stored message as IMAPFetch reads it through the store for a
      // message the server parsed from its file: read once for the message's
      // sections and counted to their ceiling as DoAction sets it, refused past
      // the ceiling, and not used when it is not as long as what was parsed -
      // the parts were placed in other bytes - when the sections are answered
      // from the parsed message, as they were before.
      {
         const std::string header = "X-Folded: one\r\n two\r\nContent-Type: text/plain\r\n\r\n";
         const std::string text =
            "Subject: s\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\npreamble\r\n"
            "--b\r\n" + header + "text\r\n"
            "--b--\r\nepilogue\r\n";

         std::shared_ptr<MimeBody> message = storedAndParsed(text);
         const size_t ceiling = text.size() * 16 + size_t{1024} * 1024;

         // As DoAction sets the ceiling, with the file's size known.
         auto ceilingFor = [&file, &text](IMAPFetch &fetch, size_t used)
         {
            fetch.ceiling_set_ = true;
            fetch.ceiling_largest_ = text.size();
            fetch.ceiling_used_ = used;
            fetch.ceiling_file_ = file;
            fetch.ceiling_file_sized_ = true;
         };

         // Read once, and counted once: the first section that cuts from the
         // stored message costs the message - and the part list the part number
         // is found in, a few bytes, for which a little room is left - and the
         // next does not read it again.
         {
            IMAPFetch fetch;
            ceilingFor(fetch, ceiling - text.size() - 64);

            const std::string mime = sectionBytesOf(fetch, message, file, "BODY.PEEK[1.MIME]");
            if (mime != header || fetch.section_refused_ || fetch.section_cost_ < text.size())
               throw std::logic_error("BODY.PEEK[1.MIME] of a message parsed from its file, read through the store under the ceiling: " +
                  Difference_(mime, header) + ", costing " + std::to_string(fetch.section_cost_) + (fetch.section_refused_ ? ", refused" : ""));

            const std::string again = sectionBytesOf(fetch, message, file, "BODY.PEEK[1.MIME]<0.9>");
            if (again != header.substr(0, 9) || fetch.section_cost_ >= text.size())
               throw std::logic_error("A second section of a message parsed from its file read it again, or was not cut from it: " +
                  Difference_(again, header.substr(0, 9)) + ", costing " + std::to_string(fetch.section_cost_));

            fetch.ForgetMessageSections_();
         }

         // Past the ceiling by the read alone: refused, and nothing handed out.
         {
            IMAPFetch fetch;
            ceilingFor(fetch, ceiling - text.size() + 1);

            const std::string refused = sectionBytesOf(fetch, message, file, "BODY.PEEK[1.MIME]");
            if (!refused.empty() || !fetch.section_refused_)
               throw std::logic_error("BODY.PEEK[1.MIME] whose read of the stored message would pass the ceiling was not refused: " + Difference_(refused, std::string()));

            fetch.ForgetMessageSections_();
         }

         // A file not as long as what was parsed: the parts are not placed in
         // it, and the sections are the parsed message's, as before.
         {
            if (!FileUtilities::WriteToFile(file, AnsiString(text + "more\r\n")))
               throw std::logic_error("A message could not be written to a file of the test's own");

            IMAPFetch fetch;
            ceilingFor(fetch, 0);

            std::shared_ptr<MimeBody> part = message->FindFirstPart();
            const std::string rebuilt = part ? std::string(AnsiString(part->GetHeaderContents())) : std::string();

            const std::string mime = sectionBytesOf(fetch, message, file, "BODY.PEEK[1.MIME]");
            if (mime != rebuilt || mime == header || fetch.section_refused_)
               throw std::logic_error("BODY.PEEK[1.MIME] of a message whose file is no longer as long as what was parsed was not the parsed part's header: " + Difference_(mime, rebuilt));

            fetch.ForgetMessageSections_();
         }
      }

      // A part changed since it was read is not placed in the stored bytes. A
      // part holding it still answers where it was read from - GetStoredBounds
      // looks at the part alone, a step at every section - but nothing of the
      // message is cut from the stored bytes any more: its sections are
      // written out from the parse, the change with them.
      {
         const std::string alternativeBody =
            "--c\r\nContent-Type: text/plain\r\n\r\noriginal\r\n"
            "--c\r\nContent-Type: text/html\r\n\r\n<p>html</p>\r\n"
            "--c--";
         const std::string besideHeader = "Content-Type: text/plain;\r\n charset=us-ascii\r\n\r\n";
         const std::string text =
            "Subject: s\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
            "--b\r\nContent-Type: multipart/alternative; boundary=\"c\"\r\n\r\n" + alternativeBody + "\r\n"
            "--b\r\n" + besideHeader + "beside\r\n"
            "--b--\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);
         std::shared_ptr<MimeBody> alternative = message->FindFirstPart();
         std::shared_ptr<MimeBody> beside = message->FindNextPart();
         std::shared_ptr<MimeBody> plain = alternative ? alternative->FindFirstPart() : std::shared_ptr<MimeBody>();
         std::shared_ptr<MimeBody> html = alternative ? alternative->FindNextPart() : std::shared_ptr<MimeBody>();
         if (!beside || !plain || !html)
            throw std::logic_error("A message of a multipart and a part beside it did not parse into its parts");

         auto placed = [](const std::shared_ptr<MimeBody> &part)
         {
            size_t entityStart = 0;
            size_t bodyStart = 0;
            size_t entityEnd = 0;
            return part->GetStoredBounds(entityStart, bodyStart, entityEnd);
         };

         if (!placed(message) || !placed(alternative) || !placed(plain) || !placed(html) || !placed(beside))
            throw std::logic_error("A part of a message read and not changed is not placed in the bytes it was read from");

         // Read and not changed: the multipart's body and the MIME header beside
         // it are the stored bytes.
         {
            IMAPFetch fetch;
            const std::string body = sectionBytes(fetch, message, "BODY.PEEK[1]");
            if (body != alternativeBody)
               throw std::logic_error("BODY.PEEK[1] of a multipart read and not changed: " + Difference_(body, alternativeBody));

            const std::string mime = sectionBytes(fetch, message, "BODY.PEEK[2.MIME]");
            if (mime != besideHeader)
               throw std::logic_error("BODY.PEEK[2.MIME] of a message read and not changed: " + Difference_(mime, besideHeader));
         }

         plain->SetRawText("changed\r\n");

         if (placed(plain))
            throw std::logic_error("A part changed since it was read is still placed in the bytes it was read from");

         if (!placed(message) || !placed(alternative) || !placed(html) || !placed(beside))
            throw std::logic_error("A part not itself changed no longer answers where it was read from, which would walk the parts below it at every section");

         IMAPFetch fetch;
         const std::string body = sectionBytes(fetch, message, "BODY.PEEK[1]");
         if (body.find("changed") == std::string::npos || body.find("original") != std::string::npos)
            throw std::logic_error("BODY.PEEK[1] of a multipart holding a part changed since it was read is not the multipart as it is now: " + body);

         const std::string mime = sectionBytes(fetch, message, "BODY.PEEK[2.MIME]");
         const std::string rebuilt = AnsiString(beside->GetHeaderContents());
         if (mime != rebuilt || mime == besideHeader)
            throw std::logic_error("BODY.PEEK[2.MIME] of a message a part of which was changed since it was read was cut from the stored bytes: " + Difference_(mime, rebuilt));
      }

      // A BODYSTRUCTURE of messages nested in messages 150 deep, which a client
      // can read: it was a message/rfc822 with nothing where its body goes past
      // the hundredth. And at every depth, a message is described with its
      // message or as a part of bytes, with its size, its stored length.
      {
         std::string text;
         for (int i = 0; i < 150; i++)
            text += "Content-Type: message/rfc822\r\n\r\n";
         text += "Subject: innermost\r\n\r\ninnermost\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         IMAPFetch fetch;
         const AnsiString structure = fetch.IteratePartRecursive_(message, true, 0);
         const std::string bytes = structure;

         if (!StructureReadable_(bytes))
            throw std::logic_error("The BODYSTRUCTURE of messages nested 150 deep is not one a client can read: " + bytes.substr(bytes.size() > 400 ? bytes.size() - 400 : 0));

         const AnsiString body = fetch.IteratePartRecursive_(message, false, 0);
         if (!StructureReadable_(body))
            throw std::logic_error("The BODY of messages nested 150 deep is not one a client can read");

         if (bytes.find("\"APPLICATION\" \"OCTET-STREAM\"") == std::string::npos)
            throw std::logic_error("The BODYSTRUCTURE of messages nested 150 deep describes every one of them as a message");
      }

      // BODY leaves out extension data - an attachment's disposition among it -
      // which BODYSTRUCTURE carries.
      {
         const std::string text =
            "Subject: s\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
            "--b\r\nContent-Type: text/plain\r\n\r\ntext\r\n"
            "--b\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"a.bin\"\r\n\r\nAAAA\r\n"
            "--b\r\nContent-Type: message/rfc822\r\nContent-Disposition: attachment\r\n\r\nSubject: inner\r\n\r\ninner\r\n"
            "--b--\r\n";

         std::shared_ptr<MimeBody> message = ParsedMessage_(text);

         IMAPFetch fetch;
         const std::string extensible = AnsiString(fetch.IteratePartRecursive_(message, true, 0));
         const std::string plain = AnsiString(fetch.IteratePartRecursive_(message, false, 0));

         if (extensible.find("(\"attachment\" (\"FILENAME\" \"a.bin\"))") == std::string::npos)
            throw std::logic_error("BODYSTRUCTURE left out an attachment's disposition: " + extensible);

         if (plain.find("FILENAME") != std::string::npos || plain.find("\"attachment\"") != std::string::npos || plain.find("BOUNDARY") != std::string::npos)
            throw std::logic_error("BODY carried extension data: " + plain);

         if (!StructureReadable_(extensible) || !StructureReadable_(plain))
            throw std::logic_error("A structure of an attachment is not one a client can read");
      }

      // A multipart with no parts - no boundary parameter, a boundary its body
      // never holds, one nested past MimeBody::MaxNestingDepth, which the parse
      // takes whole - is described as the single part every section takes it
      // for: a structure a client can read, where it was "(" and a subtype with
      // no body. Its BODY[N] and BODY[N.MIME] are its body and header as written.
      {
         // Multiparts in multiparts, each holding the next as its one part, and
         // a text part in the innermost: entities[k] is the entity k levels
         // above the text part, its header and its body.
         const int levels = (int) MimeBody::MaxNestingDepth + 5;
         std::vector<std::pair<std::string, std::string> > entities;
         entities.push_back(std::make_pair(std::string("Content-Type: text/plain\r\n\r\n"), std::string("innermost\r\n")));
         for (int k = 1; k <= levels; k++)
         {
            const std::string boundary = "N" + std::to_string(k);
            const std::string innerEntity = entities.back().first + entities.back().second;

            entities.push_back(std::make_pair(
               std::string(k == levels ? "Subject: nested\r\n" : "") + "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n",
               "--" + boundary + "\r\n" + innerEntity + "\r\n--" + boundary + "--\r\n"));
         }

         struct Childless
         {
            std::string text;
            std::string header;
            std::string body;
         };

         // A boundary holding a NUL, which a sender can put in a header: no
         // boundary, as RFC 2046 has it. It was searched for as far as its NUL,
         // so the content was cut at the first line beginning "--ab" and no part
         // read: the structure was a multipart with no body, and once that was
         // mended it gave the preamble's size while BODY[1] handed out the whole
         // body.
         std::vector<Childless> messages;
         messages.push_back(Childless{ "", "Subject: s\r\nContent-Type: multipart/mixed\r\n\r\n", "no boundary is named\r\n--x\r\n" });
         messages.push_back(Childless{ "", "Subject: s\r\nContent-Type: multipart/alternative; boundary=\"absent\"\r\n\r\n", "a body without it\r\n" });
         messages.push_back(Childless{ "", std::string("Subject: s\r\nContent-Type: multipart/mixed; boundary=\"ab") + '\0' + "c\"\r\n\r\n",
            "preamble\r\n--abx\r\nContent-Type: text/plain\r\n\r\ntext\r\n--abx--\r\n" });
         for (Childless &one : messages)
            one.text = one.header + one.body;

         // The structures of a message with a multipart with no parts in it, which
         // a client can read and which describe it as a part of bytes; written
         // into extensible and plain.
         auto describedAsBytes = [](IMAPFetch &fetch, const std::shared_ptr<MimeBody> &message, std::string &extensible, std::string &plain)
         {
            extensible = AnsiString(fetch.IteratePartRecursive_(message, true, 0));
            plain = AnsiString(fetch.IteratePartRecursive_(message, false, 0));

            if (!StructureReadable_(extensible) || !StructureReadable_(plain))
               throw std::logic_error("The structure of a multipart with no parts is not one a client can read: " + extensible.substr(extensible.size() > 400 ? extensible.size() - 400 : 0));

            if (extensible.find("\"APPLICATION\" \"OCTET-STREAM\"") == std::string::npos)
               throw std::logic_error("A multipart with no parts is not described as a part of bytes: " + extensible.substr(extensible.size() > 400 ? extensible.size() - 400 : 0));
         };

         for (const Childless &one : messages)
         {
            std::shared_ptr<MimeBody> message = ParsedMessage_(one.text);

            IMAPFetch fetch;
            std::string extensible;
            std::string plain;
            describedAsBytes(fetch, message, extensible, plain);

            const std::string body = sectionBytes(fetch, message, "BODY.PEEK[1]");
            if (body != one.body)
               throw std::logic_error("BODY.PEEK[1] of a multipart with no parts: " + Difference_(body, one.body));

            const std::string mime = sectionBytes(fetch, message, "BODY.PEEK[1.MIME]");
            if (mime != one.header)
               throw std::logic_error("BODY.PEEK[1.MIME] of a multipart with no parts: " + Difference_(mime, one.header));

            // The size it is described with is that of its body, which BODY[1]
            // hands out.
            const std::string size = "\"7bit\" " + std::to_string(one.body.size());
            if (extensible.find(size) == std::string::npos || plain.find(size) == std::string::npos)
               throw std::logic_error("A multipart with no parts is not described with the size of its body, " + size + ": " + extensible);
         }

         // The nested multiparts, and each of them down to the one the parse
         // took whole, by its part number, against what it was written as.
         {
            std::shared_ptr<MimeBody> message = ParsedMessage_(entities.back().first + entities.back().second);

            IMAPFetch fetch;
            std::string extensible;
            std::string plain;
            describedAsBytes(fetch, message, extensible, plain);

            std::string number;
            for (int depth = 1; depth <= (int) MimeBody::MaxNestingDepth; depth++)
            {
               number += number.empty() ? "1" : ".1";
               const std::pair<std::string, std::string> &written = entities[(size_t) (levels - depth)];

               const std::string body = sectionBytes(fetch, message, "BODY.PEEK[" + number + "]");
               if (body != written.second)
                  throw std::logic_error("BODY.PEEK[" + number + "] of multiparts nested " + std::to_string(levels) + " deep: " + Difference_(body, written.second));

               const std::string mime = sectionBytes(fetch, message, "BODY.PEEK[" + number + ".MIME]");
               if (mime != written.first)
                  throw std::logic_error("BODY.PEEK[" + number + ".MIME] of multiparts nested " + std::to_string(levels) + " deep: " + Difference_(mime, written.first));
            }
         }
      }
   }
}

