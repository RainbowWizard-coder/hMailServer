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
      if (navigated_message_ != pBody)
      {
         navigated_bodies_.clear();
         navigated_message_ = pBody;
      }

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

      ceiling_set_ = false;
      ceiling_largest_ = 0;
      ceiling_used_ = 0;
      ceiling_file_.Empty();
      ceiling_file_sized_ = false;
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
      if (navigated_message_ != pBodyPart)
      {
         navigated_bodies_.clear();
         navigated_message_ = pBodyPart;
      }

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
            // A multipart's content is its sub-parts, raw and undecoded.
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
      // message, then serialize based on content type. Must run before the default
      // navigation below, which loads the encapsulated message unconditionally.
      if (oPart.GetShowBodyContent())
      {
         if (!oPart.GetName().IsEmpty())
            pBodyPart = GetBodyPartByRecursiveIdentifier_(pBodyPart, oPart.GetName(), false);

         if (!pBodyPart)
            return pOutBuf;

         int iByteStart = 0;
         int iByteCount = 0;
         AnsiString body;
         if (KindOf_(pBodyPart).encapsulated_message)
         {
            // BODY[2] on message/rfc822: the "body" of this MIME entity is the full
            // inner RFC2822 message, headers included - which is exactly the
            // part's content, byte for byte. It used to be the inner message
            // loaded and stored again, so a multipart inside lost its epilogue
            // and the section came back shorter than BODYSTRUCTURE said it was.
            body = pBodyPart->GetContent();
         }
         else
         {
            pBodyPart->Store(body, false);
         }

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

      if (iRecursion > 100)
         return "";

      if (oPart->IsMultiPart())
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

      if (oPart->IsEncapsulatedRFC822Message())
      {
         /*
            A body type of type MESSAGE and subtype RFC822 contains,
            immediately after the basic fields, the envelope structure,
            body structure, and size in text lines of the encapsulated
            message.
         */
      
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

            sResult += " " + sEnvelope;
            sResult += " " + sBodyStructure;
            sResult += " " + sLines;
         }
         catch (...)
         {
            String sErrorMessage = "Error when creating ENVELOPE structure for encapsulated message.";

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5061, "IMAPFetch::GetPartStructure_", sErrorMessage);
            
            return "";
         }

      }

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


      if (oPart->IsAttachment())
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

         if (milliseconds > 2000)
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

      // A message's text parsed as an encapsulated one is.
      std::shared_ptr<MimeBody> ParsedMessage_(const std::string &text)
      {
         std::shared_ptr<MimeBody> body(new MimeBody);

         size_t index = 0;
         bool loaded = false;
         body->Load(text.c_str(), text.size(), index, loaded);

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
               std::shared_ptr<ByteBuffer> alone = own.GetByteBufferByBodyPart_(_T(""), message, part);

               if (fetch.section_refused_ || !SameBytes_(kept, alone))
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

         if (milliseconds > 2000)
            throw std::logic_error("100,000 MIME sections of parts 9,000 deep in three multiparts took " + std::to_string((long long) milliseconds) + " ms");
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

         if (milliseconds > 2000)
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

         if (milliseconds > 2000)
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

         const size_t ceiling = text.size() * 16 + 1024 * 1024;
         if (many || !nested.section_refused_ || nested.section_cost_ > ceiling)
            HM_TEST_FAILED();

         if (milliseconds > 2000)
            throw std::logic_error("A part number through 5,000 encapsulated messages took " + std::to_string((long long) milliseconds) + " ms to refuse");

         nested.ForgetMessageSections_();
      }
   }
}

