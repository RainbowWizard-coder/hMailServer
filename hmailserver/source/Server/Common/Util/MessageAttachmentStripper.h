// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Message;
   class MimeBody;
   class CMimeMessage;
   
   
   class MessageAttachmentStripper  
   {
   public:
      MessageAttachmentStripper();
      virtual ~MessageAttachmentStripper();

      // What Strip did, so that the caller can log what happened rather than
      // what it asked for, and so that it can decide what happens next. Strip
      // is called after a scanner has found a virus. NothingToStrip and
      // LeftAloneEncrypted mean the message goes on still carrying it;
      // LeftAloneUnreadable and RewriteFailed mean nothing in it could be
      // kept, and the caller does not deliver it.
      enum class Result
      {
         Stripped,             // rewritten as its text part plus the notice
         ReplacedByNotice,     // a single part that is not the message's text: rewritten as the notice alone
         NothingToStrip,       // a single-part message that is its text: there is no attachment to remove
         LeftAloneEncrypted,   // encrypted end to end (SealedMessage::IsEncrypted), so there is no part to pick out
         LeftAloneUnreadable,  // the file could not be read, so it was not rewritten: the caller deletes it
         RewriteFailed         // read, but what was to replace it could not be written: the file holds the message as it arrived, or part of the rewrite, and the caller deletes it
      };

      // Rewrites the message as its text part plus a notice, which is what the
      // anti-virus "delete attachments" action means. The text part is searched
      // for through the whole MIME tree (text/plain first, then any text part),
      // so a multipart/mixed > multipart/related > multipart/alternative message
      // keeps its text. A message with no text part anywhere is stripped to the
      // header and the notice, and so is a message of one part that is not its
      // text - a file, an encapsulated message, a text file declared an
      // attachment - since that part is the attachment.
      //
      // A message of one part that IS its text has nothing to remove, and is
      // left as it is (NothingToStrip), as an infected text part of a multipart
      // message is kept. A message that cannot be read is not rewritten either
      // (LeftAloneUnreadable): nothing of it can be kept, so the caller deletes
      // it rather than deliver what the scanner found. Nor can anything of one
      // whose rewrite could not be written (RewriteFailed): its file is the
      // message as it arrived, or a part of what was being written over it,
      // and the caller deletes it too.
      //
      // An ENCRYPTED message (SealedMessage::IsEncrypted) is left exactly as it
      // arrived and the reason is logged: keeping one part of it hands the
      // recipient a fragment of ciphertext. A SIGNED message is stripped like
      // any other, because nothing here verifies a signature and a sender who
      // could opt out of stripping by adding a multipart/signed wrapper would;
      // the log says the signature has been invalidated. S/MIME's opaque
      // signed-data, one part that is not text, is replaced by the notice as
      // any message that is one attachment is. See
      // hmailserver/docs/SealedMail.md.
      static Result Strip(std::shared_ptr<Message> pMessage);
   private:

      // pBody may be null, meaning "there is no text part to keep": the header
      // and the notice are written and the body is empty. bodyWasThePart says
      // that the message's own header described the part removed - a
      // single-part message - so its Content- fields, which describe that part
      // and not the notice, are left out with Content-Type. False when the
      // file could not be written - not opened for writing, or not all of it
      // taken - and then the message's size is left as it was.
      static bool WriteToDisk_(std::shared_ptr<Message> pMessage, MimeBody &oMainMessage, std::shared_ptr<MimeBody> pBody, bool bodyWasThePart);

      // The first part under parent, at any depth up to the cap, that
      // IsGoodTextPart_ accepts and whose sub type is subType (any text sub
      // type when subType is empty). Null when there is none.
      static std::shared_ptr<MimeBody> FindTextPart_(MimeBody &parent, const String &subType, int depth);

      // True when this part is the text of the message rather than something
      // attached to it: main type text, and not declared an attachment by its
      // Content-Disposition (an attached .txt file is not the message's text).
      // It compared the whole media type against "text", with the sense of
      // CompareNoCase inverted, until 16 September 2026; the two mistakes
      // cancelled into "true for every part". Asked of a single-part message
      // too, whose one part is the message itself.
      static bool IsGoodTextPart_(const MimeBody &body);
   };

}