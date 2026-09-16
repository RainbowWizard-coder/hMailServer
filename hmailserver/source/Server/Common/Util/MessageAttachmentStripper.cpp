// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "MessageAttachmentStripper.h"
#include "SealedMessage.h"
#include "../Mime/Mime.h"
#include "../BO/Message.h"
#include "../BO/ServerMessages.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentServerMessage.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   MessageAttachmentStripper::MessageAttachmentStripper()
   {

   }

   MessageAttachmentStripper::~MessageAttachmentStripper()
   {

   }

   MessageAttachmentStripper::Result
   MessageAttachmentStripper::Strip(std::shared_ptr<Message> pMessage)
   {
      const String fileName = PersistentMessage::GetFileName(pMessage);

      // Read message, extract attachments.
      //
      // The result was discarded, and this function goes on to REWRITE the message from
      // what it read. A message it could not read parses as no parts, FindFirstPart
      // returns nothing and it returns below without touching the file - so the outcome
      // was accidentally safe rather than deliberately so, and it depended on
      // FindFirstPart's behaviour rather than on the load having worked. Say so instead.
      MimeBody oMimeMessage;

      if (oMimeMessage.LoadFromFile(fileName) != MimeLoadResult::Loaded)
      {
         LOG_APPLICATION(Formatter::Format("The message {0} could not be read, so its attachments have not been stripped. The message has been left exactly as it was.", fileName));
         return Result::LeftAloneUnreadable;
      }

      // An encrypted message is not something a part can be picked out of.
      //
      // This function keeps one text part and throws the rest of the message
      // away. On an encrypted message the "rest of the message" is the whole
      // message: the ciphertext. What survived was whichever part the walk
      // reached first, under a header this function rebuilds without its
      // Content-Type - so the recipient got a fragment of base64 declared as
      // nothing in particular, and the original was gone.
      //
      // The message is left exactly as it arrived instead. It still carries the
      // virus the scanner found, so this is not a quiet pass: the log says what
      // was not done and why, the caller logs that the message was delivered
      // unstripped, and an installation that cannot accept that should set
      // AVAction to delete the message rather than to strip it, which is the
      // setting that still works on a message nobody can read.
      //
      // IsOpaque and not IsSealed, and the difference is a security boundary.
      // Nothing here verifies a signature, so "multipart/signed" at the top of
      // a message is a claim any sender can make, and this is the one shape of
      // sealed message where the scanner CAN see a virus - in the signed text
      // or in a file inside the signed part. Declining to strip a signed
      // message would deliver exactly the infected messages the scanner had
      // found, to anyone who added one header. So a signed message is stripped
      // like any other, and the log says that doing so has invalidated the
      // signature, which is the honest outcome when the scanner's verdict and a
      // sender's signature disagree.
      const char *topLevelContentType = oMimeMessage.GetContentType();
      const String contentType = topLevelContentType != nullptr ? String(topLevelContentType) : String();

      if (SealedMessage::IsOpaqueContentType(contentType))
      {
         LOG_APPLICATION(Formatter::Format("Message {0} is encrypted ({1}), so its attachments have NOT been stripped: keeping one part of an encrypted message destroys it and leaves the recipient a fragment of ciphertext. The message has been left exactly as it was. Set the anti-virus action to delete the message if such a message must not be delivered.",
            pMessage->GetID(), contentType));
         return Result::LeftAloneEncrypted;
      }

      if (!oMimeMessage.IsMultiPart())
      {
         // A single-part message has no attachment to remove: its one part is
         // the message. This returned silently before, and the caller then
         // logged that the attachments had been stripped. Nothing is rewritten;
         // the caller now says so.
         return Result::NothingToStrip;
      }

      // The text to keep. text/plain is preferred wherever it is in the tree,
      // because WriteToDisk_ writes the part's raw text under the message's own
      // headers minus Content-Type, and plain text is the one kind that reads
      // correctly that way. Only when there is no text/plain anywhere is any
      // other text part taken.
      //
      // The walk used to look two levels deep only - the top-level parts and
      // the children of a top-level multipart - which lost the whole text of
      // the most common HTML shape, multipart/mixed > multipart/related >
      // multipart/alternative > text/plain, the moment IsGoodTextPart_ started
      // telling the truth: neither multipart/related nor multipart/alternative
      // is text, and the text/plain is three levels down.
      std::shared_ptr<MimeBody> pBody = FindTextPart_(oMimeMessage, _T("plain"), 0);

      if (!pBody)
         pBody = FindTextPart_(oMimeMessage, String(), 0);

      // pBody is null when every part of this message is an attachment and one
      // of them is the virus the scanner just found. While IsGoodTextPart_
      // answered true for whatever part it was shown, this was unreachable -
      // such a message was "stripped" to its first attachment, which is the
      // infected one as often as not. Now that the comparison is right, no text
      // part is a real outcome, and returning here would deliver the message
      // whole. So it is stripped to nothing: the header, the notice, and no body.
      WriteToDisk_(pMessage, oMimeMessage, pBody);

      if (SealedMessage::IsSealedContentType(contentType))
      {
         LOG_APPLICATION(Formatter::Format("Message {0} is signed ({1}) and its attachments have been stripped, which invalidates the signature: the recipient's client will report the message as tampered with. The signed text has been kept; the parts the anti-virus action removes have been removed as for any other message.",
            pMessage->GetID(), contentType));
      }

      return Result::Stripped;
   }

   std::shared_ptr<MimeBody>
   MessageAttachmentStripper::FindTextPart_(MimeBody &parent, const String &subType, int depth)
   {
      std::shared_ptr<MimeBody> none;

      // A malformed or hostile message can nest parts without limit and this
      // walk is recursive on the delivery thread. 20 levels is the same cap the
      // Sieve body test uses, and far past anything a real client produces.
      if (depth > 20)
         return none;

      // Each MimeBody keeps its own part iterator, so walking a child's parts
      // does not disturb the parent's.
      for (std::shared_ptr<MimeBody> part = parent.FindFirstPart(); part; part = parent.FindNextPart())
      {
         if (part->IsMultiPart())
         {
            std::shared_ptr<MimeBody> found = FindTextPart_(*part, subType, depth + 1);
            if (found)
               return found;

            continue;
         }

         if (!IsGoodTextPart_(part))
            continue;

         if (subType.IsEmpty() || String(part->GetSubType()).CompareNoCase(subType) == 0)
            return part;
      }

      return none;
   }

   void
   MessageAttachmentStripper::WriteToDisk_(std::shared_ptr<Message> pMessage, MimeBody &oMainMessage, std::shared_ptr<MimeBody> pBody)
   {
      // A null body is now a caller's decision rather than a mistake: it means
      // "this message has no text part to keep", and what is written is the
      // header and the notice with nothing after it. Returning here instead
      // would leave the infected message on disk untouched.
      AnsiString sHeader;

      std::vector<MimeField> oFieldList = oMainMessage.Fields();

      auto iterField = oFieldList.begin();

      while (iterField != oFieldList.end())
      {
         MimeField oField = (*iterField);

         AnsiString sName = oField.GetName();
         if (sName.CompareNoCase("subject") == 0)
         {
            String sTextVirusFound = Configuration::Instance()->GetServerMessages()->GetMessage("VIRUS_FOUND");

            sHeader += sName;
            sHeader += ": " + sTextVirusFound +": ";
            sHeader += oField.GetValue();
            sHeader += "\r\n";
         }
         else
         if (sName.CompareNoCase("content-type") != 0)
         {
            sHeader += sName;
            sHeader += ": ";
            sHeader += oField.GetValue();
            sHeader += "\r\n";
         }

         iterField++;
      }

      String sBody = pBody ? pBody->GetRawText() : String();

      String sNotification = Configuration::Instance()->GetServerMessages()->GetMessage("VIRUS_ATTACHMENT_REMOVED");

      sBody = sNotification + sBody;

      String sData = sHeader;
      sData += "\r\n";
      sData += sBody + "\r\n";

      pMessage->SetSize(sData.GetLength());

      String fileName = PersistentMessage::GetFileName(pMessage);

      FileUtilities::WriteToFile(fileName, sData, false);
   }


   bool
   MessageAttachmentStripper::IsGoodTextPart_(std::shared_ptr<MimeBody> pBody)
   {
      if (!pBody)
         return false;

      // Two mistakes in one line, and they cancelled into "yes" for every part.
      //
      // CompareNoCase returns 0 when the strings are EQUAL, so `if (compare)`
      // was true when the part was NOT text - the test was inverted. And
      // GetContentType returns the whole "mediatype/subtype" (Mime.h:213), so
      // it can never equal "text" and the comparison was non-zero whatever the
      // part was. The function therefore returned true for every part, and Strip
      // kept whichever part it walked first: on an ordinary infected message
      // that is usually the text part by luck, and on one whose first part is
      // the virus it was the virus that survived and the text that was thrown
      // away.
      //
      // GetMainType is the half this was always asking about, and it is the
      // same call MimeBody::IsAttachment makes for the same question. A text
      // part that is declared an attachment - a .txt file, or an inline part
      // with a file name - is something attached, not the message's text, and
      // keeping it would make the stripped message read as that file.
      if (pBody->IsAttachment())
         return false;

      String sMainType = pBody->GetMainType();

      return sMainType.CompareNoCase(_T("text")) == 0;
   }
}
