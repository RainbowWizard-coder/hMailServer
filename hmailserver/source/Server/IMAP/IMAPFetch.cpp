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
      std::vector<IMAPFetchParser::BodyPart> vecPartsToPeekAt = parser_->GetPartsToLookAt();
      std::vector<std::shared_ptr<ByteBuffer> > partBuffers;

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

            if (shouldLoad && pBody->IsEncapsulatedRFC822Message())
            {
               try
               {
                  pBody = pBody->LoadEncapsulatedMessage();
               }
               catch (...)
               {
                  String sErrorMessage = "Error when creating ENVELOPE structure for encapsulated message.";

                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5060, "IMAPFetch::GetBodyPartByRecursiveIdentifier_", sErrorMessage);

                  std::shared_ptr<MimeBody> empty;
                  return empty;
               }
            }

         }
     
         iterPart++;
      }

      return pBody;
   }


   std::shared_ptr<ByteBuffer> 
   IMAPFetch::GetByteBufferByBodyPart_(const String &messageFileName, std::shared_ptr<MimeBody> pBodyPart, IMAPFetchParser::BodyPart &oPart)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns a buffer containing the data for the given body/part
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<ByteBuffer> pOutBuf = std::shared_ptr<ByteBuffer>(new ByteBuffer);

      if (!pBodyPart)
      {
         // No body provided. We'll have to return
         // an empty buffer.
         return pOutBuf;
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

         if (pBinaryPart->IsEncapsulatedRFC822Message())
         {
            // The "content" of a message/rfc822 part is the whole inner message,
            // headers and all: the part's own bytes, as BODY[N] serves them. It
            // was the inner message parsed and written out again, which dropped
            // a multipart's epilogue - BINARY.SIZE[2] came out 19 octets short in
            // imaptest's fetch-binary-mime.
            content = pBinaryPart->GetContent();
         }
         else if (pBinaryPart->IsMultiPart())
         {
            // A multipart's content is its sub-parts, raw and undecoded.
            pBinaryPart->Store(content, false);
         }
         else
         {
            std::unique_ptr<MimeCodeBase> pCoder(MimeEnvironment::CreateCoder(pBinaryPart->GetTransferEncoding()));
            pCoder->SetInput(pBinaryPart->GetContent(), pBinaryPart->GetContentLength(), false);
            pCoder->GetOutput(content);
         }

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
         if (pBodyPart->IsEncapsulatedRFC822Message())
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

         if (pNamedPart && pNamedPart->IsEncapsulatedRFC822Message())
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

         GetBytesToSend_(sResponse.GetLength(), oPart, iByteStart, iByteCount);
         AnsiString sAS = sResponse.Mid(iByteStart, iByteCount);
         pOutBuf->Add((BYTE*) sAS.GetBuffer(0), sAS.GetLength());

      }
      else if (oPart.GetShowBodyHeaderFieldsNOT())
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
      
      // Fetch the first part of the message.
      std::shared_ptr<MimeBody> oPart = pBody->FindFirstPart();

      if (!oPart)
      {
         // This is a single part message. Should we send the first part?
         if (iPartNo == 1)
            return pBody;
      }

      int iCurIdx = 1;

      String sResponse;
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
      std::vector<IMAPFetchParser::BodyPart> vecPartsToLookAt = parser_->GetPartsToLookAt();
      if (vecPartsToLookAt.size() == 1)
      {
         IMAPFetchParser::BodyPart oBodyPart = vecPartsToLookAt[0];

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
}

