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
      // what it asked for. Strip is called after a scanner has found a virus,
      // and every value here except Stripped means the message was delivered
      // still carrying it.
      enum class Result
      {
         Stripped,             // rewritten as its text part plus the notice
         NothingToStrip,       // a single-part message: there is no attachment to remove
         LeftAloneEncrypted,   // encrypted end to end, so there is no part to pick out
         LeftAloneUnreadable   // the file could not be parsed, so it was not rewritten
      };

      // Rewrites the message as its text part plus a notice, which is what the
      // anti-virus "delete attachments" action means. The text part is searched
      // for through the whole MIME tree (text/plain first, then any text part),
      // so a multipart/mixed > multipart/related > multipart/alternative message
      // keeps its text. A message with no text part anywhere is stripped to the
      // header and the notice.
      //
      // An ENCRYPTED message (SealedMessage::IsOpaque) is left exactly as it
      // arrived and the reason is logged: keeping one part of it hands the
      // recipient a fragment of ciphertext. A SIGNED message is stripped like
      // any other, because nothing here verifies a signature and a sender who
      // could opt out of stripping by adding a multipart/signed wrapper would;
      // the log says the signature has been invalidated. See
      // hmailserver/docs/SealedMail.md.
      static Result Strip(std::shared_ptr<Message> pMessage);
   private:

      // pBody may be null, meaning "there is no text part to keep": the header
      // and the notice are written and the body is empty.
      static void WriteToDisk_(std::shared_ptr<Message> pMessage, MimeBody &oMainMessage, std::shared_ptr<MimeBody> pBody);

      // The first part under parent, at any depth up to the cap, that
      // IsGoodTextPart_ accepts and whose sub type is subType (any text sub
      // type when subType is empty). Null when there is none.
      static std::shared_ptr<MimeBody> FindTextPart_(MimeBody &parent, const String &subType, int depth);

      // True when this part is the text of the message rather than something
      // attached to it: main type text, and not declared an attachment by its
      // Content-Disposition (an attached .txt file is not the message's text).
      // It compared the whole media type against "text", with the sense of
      // CompareNoCase inverted, until 16 September 2026; the two mistakes
      // cancelled into "true for every part".
      static bool IsGoodTextPart_(std::shared_ptr<MimeBody> pBody);
   };

}