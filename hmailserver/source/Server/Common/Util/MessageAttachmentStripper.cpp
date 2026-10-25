// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "MessageAttachmentStripper.h"
#include "SealedMessage.h"
#include "../Mime/Mime.h"
#include "../BO/Message.h"
#include "../BO/ServerMessages.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentServerMessage.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The stripped message could not be written over the message's file: it
      // could not be opened for writing - another program holds it, letting it
      // be read but not written - or did not take all of what was written. What
      // the caller delivers is that file, which now holds the message as it
      // arrived or a part of the rewrite, so no part of it can be kept, and the
      // caller deletes it as it deletes a message that cannot be read
      // (SMTPDeliverer::HandleInfectedMessage_). This line was not written, and
      // the caller said that the attachments had been stripped and delivered the
      // file as it was.
      MessageAttachmentStripper::Result ReportRewriteFailed(const String &fileName)
      {
         LOG_APPLICATION(Formatter::Format("The message {0} could not be rewritten, so its attachments could not be stripped and no part of it can be kept.", fileName));

         return MessageAttachmentStripper::Result::RewriteFailed;
      }
   }

   MessageAttachmentStripper::MessageAttachmentStripper()
   {

   }

   MessageAttachmentStripper::~MessageAttachmentStripper()
   {

   }

   MessageAttachmentStripper::Result
   MessageAttachmentStripper::Strip(std::shared_ptr<Message> pMessage)
   {
      const String fileName = MessageStore::WorkingFile(pMessage);

      // Read message, extract attachments.
      //
      // The result was discarded, and this function goes on to REWRITE the message from
      // what it read. A message it could not read parses as no parts, FindFirstPart
      // returns nothing and it returns below without touching the file - so the outcome
      // was accidentally safe rather than deliberately so, and it depended on
      // FindFirstPart's behaviour rather than on the load having worked. Say so instead.
      //
      // A message that cannot be read has no text that can be picked out of it, so
      // nothing of it can be kept: the file is not rewritten, and the caller deletes
      // the message (SMTPDeliverer::HandleInfectedMessage_) as the "delete the
      // message" action does. The file is unreadable here because it could not be
      // opened - another program holds it, or has moved it away - or because its
      // parse failed.
      MimeBody oMimeMessage;

      if (oMimeMessage.LoadFromFile(fileName) != MimeLoadResult::Loaded)
      {
         LOG_APPLICATION(Formatter::Format("The message {0} could not be read, so its attachments could not be stripped and no part of it can be kept.", fileName));
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
      //
      // And "encrypted" is decided from the body, not the label: a
      // multipart/encrypted header over ordinary parts is one line any sender
      // can write, and declining on it would deliver the virus the scanner has
      // just found whole. SealedMessage::IsEncrypted says encrypted only for
      // the exact shape of a real encrypted message, which has no part to
      // strip.
      //
      // IsEncrypted and not IsOpaque: S/MIME's opaque signed-data is opaque to
      // this server's readers, its content inside the CMS, but it is not under
      // a key, and the recipient's client shows that content, a file in it
      // included, as it shows a multipart/signed message's. It is one part
      // that is not text, so below it is replaced by the notice, as any
      // message that is one attachment is.
      const char *topLevelContentType = oMimeMessage.GetContentType();
      const String contentType = topLevelContentType != nullptr ? String(topLevelContentType) : String();

      if (SealedMessage::IsEncrypted(oMimeMessage))
      {
         LOG_APPLICATION(Formatter::Format("Message {0} is encrypted ({1}), so its attachments have NOT been stripped: keeping one part of an encrypted message destroys it and leaves the recipient a fragment of ciphertext. The message has been left exactly as it was. Set the anti-virus action to delete the message if such a message must not be delivered.",
            pMessage->GetID(), contentType));
         return Result::LeftAloneEncrypted;
      }

      if (!oMimeMessage.IsMultiPart())
      {
         // A single-part message is either its text or something attached, and
         // the same question as for a part of a multipart message tells which
         // (IsGoodTextPart_, which asks MimeBody::IsAttachment): a part whose
         // disposition is "attachment" - or "inline" with a file name - or whose
         // main type is not text, is an attachment wherever it stands.
         //
         // Text has no attachment to remove: nothing is rewritten, and the
         // caller says so. The text of a multipart message is kept in the same
         // way below.
         if (IsGoodTextPart_(oMimeMessage))
            return Result::NothingToStrip;

         // The one part is the attachment, so removing every attachment leaves
         // the header and the notice, as for a multipart message with no text
         // part. The header's Content- fields describe the part removed - its
         // encoding, its file name - and none of them describes the notice, so
         // they go too: a Content-Transfer-Encoding left behind would have the
         // notice decoded as that encoding, and a Content-Disposition would show
         // it as the file.
         //
         // What is delivered is the file, so a rewrite that did not reach it
         // has stripped nothing (ReportRewriteFailed, above).
         if (!WriteToDisk_(pMessage, oMimeMessage, std::shared_ptr<MimeBody>(), true))
            return ReportRewriteFailed(fileName);

         // As for a multipart message below: a signed message is said to have
         // lost its signature, and a label the body does not bear out is said
         // to have been disbelieved.
         if (SealedMessage::IsOpaque(oMimeMessage))
         {
            LOG_APPLICATION(Formatter::Format("Message {0} is signed in S/MIME's opaque form ({1}), which is not encrypted, so it has been replaced by the notice as any other message that is one attachment is: the signed content, and the signature, are not delivered.",
               pMessage->GetID(), contentType));
         }
         else if (SealedMessage::IsOpaqueContentType(contentType))
         {
            LOG_APPLICATION(Formatter::Format("Message {0} is labelled encrypted ({1}) but its body is not an encrypted message, so it has been replaced by the notice as any other message that is one attachment is.",
               pMessage->GetID(), contentType));
         }

         return Result::ReplacedByNotice;
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
      //
      // And as for a single part: a rewrite that did not reach the file has
      // stripped nothing.
      if (!WriteToDisk_(pMessage, oMimeMessage, pBody, false))
         return ReportRewriteFailed(fileName);

      if (SealedMessage::IsOpaqueContentType(contentType))
      {
         LOG_APPLICATION(Formatter::Format("Message {0} is labelled encrypted ({1}) but its body is not an encrypted message, so its attachments have been stripped as for any other message.",
            pMessage->GetID(), contentType));
      }
      else if (SealedMessage::IsSealedContentType(contentType))
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

         if (!IsGoodTextPart_(*part))
            continue;

         if (subType.IsEmpty() || String(part->GetSubType()).CompareNoCase(subType) == 0)
            return part;
      }

      return none;
   }

   bool
   MessageAttachmentStripper::WriteToDisk_(std::shared_ptr<Message> pMessage, MimeBody &oMainMessage, std::shared_ptr<MimeBody> pBody, bool bodyWasThePart)
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

         // Every Content- field of a single-part message - its type, encoding,
         // disposition, description, ID, digest - is about the part this
         // replaces with the notice. For a multipart message the type alone is
         // left out, as before: the rest describe the whole message.
         // (AnsiString::StartsWith compares without case.)
         const bool describesTheRemovedPart = bodyWasThePart && sName.StartsWith("content-");

         if (sName.CompareNoCase("subject") == 0)
         {
            String sTextVirusFound = Configuration::Instance()->GetServerMessages()->GetMessage("VIRUS_FOUND");

            sHeader += sName;
            sHeader += ": " + sTextVirusFound +": ";
            sHeader += oField.GetValue();
            sHeader += "\r\n";
         }
         else if (sName.CompareNoCase("content-type") != 0 && !describesTheRemovedPart)
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

      String fileName = MessageStore::WorkingFile(pMessage);

      // Its result was thrown away, and the caller went on as though the file
      // held the stripped message whatever the write had done. The size is the
      // file's once the file holds it, and not before.
      if (!FileUtilities::WriteToFile(fileName, sData, false))
         return false;

      pMessage->SetSize(sData.GetLength());

      return true;
   }


   bool
   MessageAttachmentStripper::IsGoodTextPart_(const MimeBody &body)
   {
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
      if (body.IsAttachment())
         return false;

      String sMainType = body.GetMainType();

      return sMainType.CompareNoCase(_T("text")) == 0;
   }
}
