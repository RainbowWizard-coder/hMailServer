// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DisclaimerAdder.h"
#include "InlineImageParts.h"
#include "SealedMessage.h"

#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Mime/Mime.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A message too large to be read whole (MessageData::MaxParseBytes) has only
      // its header read, and the text a footer goes after is in its body, which
      // is not read to append to it - finding the part to append to means parsing
      // the whole of it, and appending blind to a body whose structure is not
      // known breaks an encoded or a multipart one. So nothing is appended, and
      // the log says so, where an administrator missing a footer will look. Until
      // the large-message row the load failed and the footer was left off without
      // a word. True when the message is such a one and was left alone.
      bool LargeMessageSkipped_(std::shared_ptr<Message> message, std::shared_ptr<MessageData> data, const String &what)
      {
         if (!data->IsHeaderOnly())
            return false;

         LOG_APPLICATION(Formatter::Format("DisclaimerAdder - Message {0} from {1} is larger than the {2} MB a message is read whole at, so {3} was not appended: appending it needs the body, which was not read. The message was sent unchanged.",
            message ? message->GetID() : 0, message ? message->GetFromAddress() : String(), (int) (MessageData::MaxParseBytes / (1024 * 1024)), what));
         return true;
      }

      // What a quoted copy of the footer looks like once a mail client has been
      // at it: angle-bracket quote markers at the start of every line, tags
      // wrapped around it, white space folded differently. All of that is
      // removed from both sides before they are compared, so the comparison is
      // about the words rather than about the layout.
      String ForComparison(const String &value, bool isHtml)
      {
         String result;
         result.reserve(value.GetLength());

         bool atLineStart = true;
         bool inTag = false;
         bool pendingSpace = false;
         String tagName;
         bool tagNameDone = false;

         for (int i = 0; i < value.GetLength(); i++)
         {
            TCHAR c = value.GetAt(i);

            if (isHtml && c == '<')
            {
               inTag = true;
               tagName.Empty();
               tagNameDone = false;
               continue;
            }

            if (isHtml && c == '>' && inTag)
            {
               inTag = false;

               // A block element separates words and an inline one does not: a
               // client that bolds the last word of the footer - "may be
               // <b>privileged</b>." - must not turn it into "privileged ." and
               // stop matching the footer it quotes.
               tagName.ToLower();
               bool inlineTag = tagName == _T("b") || tagName == _T("i") || tagName == _T("u") || tagName == _T("s") ||
                                tagName == _T("em") || tagName == _T("strong") || tagName == _T("span") || tagName == _T("a") ||
                                tagName == _T("font") || tagName == _T("small") || tagName == _T("big") || tagName == _T("sub") ||
                                tagName == _T("sup") || tagName == _T("code") || tagName == _T("mark");
               if (!inlineTag)
                  pendingSpace = true;

               continue;
            }

            if (inTag)
            {
               if (!tagNameDone)
               {
                  if (c == '/' && tagName.IsEmpty())
                     continue;

                  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                     tagName += c;
                  else
                     tagNameDone = true;
               }

               continue;
            }

            if (c == '\r' || c == '\n')
            {
               atLineStart = true;
               pendingSpace = true;
               continue;
            }

            if (c == ' ' || c == '\t')
            {
               pendingSpace = true;
               continue;
            }

            // A plain-text reply prefixes every quoted line with '>', often
            // several deep, sometimes with spaces between.
            if (!isHtml && atLineStart && c == '>')
               continue;

            atLineStart = false;

            if (pendingSpace && !result.IsEmpty())
               result += _T(" ");

            pendingSpace = false;
            result += c;
         }

         result.ToLower();
         return result;
      }

      bool IsList(std::shared_ptr<MessageData> messageData)
      {
         if (!messageData->GetFieldValue(_T("List-Id")).IsEmpty())
            return true;

         if (!messageData->GetFieldValue(_T("List-Unsubscribe")).IsEmpty())
            return true;

         String precedence = messageData->GetFieldValue(_T("Precedence"));
         precedence.ToLower();
         precedence.TrimLeft();
         precedence.TrimRight();

         return precedence == _T("bulk") || precedence == _T("list") || precedence == _T("junk");
      }

      // True when at least one recipient is outside this server. A footer is a
      // statement to the outside world; putting it on a note between two
      // colleagues is the complaint every administrator who has deployed one
      // hears in the first week.
      bool HasExternalRecipient(std::shared_ptr<Message> message)
      {
         // owned by this call: the recipients of the message its caller holds - a session's, a delivery's or a task's own
         std::vector<std::shared_ptr<MessageRecipient> > &recipients = message->GetRecipients()->GetVector();

         for (auto recipient : recipients)
         {
            if (!recipient->GetIsLocalName())
               return true;
         }

         return false;
      }
   }

   bool
   DisclaimerAdder::IsProtectedContentType(const String &contentType)
   {
      // The six types and the parameter stripping moved to SealedMessage on 16
      // September 2026, unchanged, because five other parts of the server need
      // the same answer and two of them were not asking the question at all.
      // This stays as the name the footer and the signature have always called
      // it by.
      return SealedMessage::IsSealedContentType(contentType);
   }

   bool
   DisclaimerAdder::AlreadyCarries(const String &body, const String &disclaimer, bool bodyIsHtml)
   {
      const String needle = ForComparison(disclaimer, bodyIsHtml);

      // A footer of one or two characters would match almost anything; treat a
      // configuration that short as never already present rather than as always
      // present, so the administrator gets the footer they asked for.
      if (needle.GetLength() < 8)
         return false;

      return ForComparison(body, bodyIsHtml).Find(needle) >= 0;
   }

   bool
   DisclaimerAdder::Append(std::shared_ptr<Message> message,
                           std::shared_ptr<const Domain> sender_domain,
                           bool submission_is_ours,
                           std::shared_ptr<MessageData> &message_data)
   {
      if (!message)
      {
         HM_ASSERT(0);
         return false;
      }

      if (!sender_domain || !sender_domain->GetDisclaimerEnabled())
         return false;

      if (!submission_is_ours)
         return false;

      String plainText = sender_domain->GetDisclaimerPlainText();
      String html = sender_domain->GetDisclaimerHTML();

      if (plainText.IsEmpty() && html.IsEmpty())
         return false;

      if (html.IsEmpty())
      {
         // One box filled in is one footer in both forms, which is what an
         // administrator who filled in one box expects. The same rule the
         // signature follows.
         html = plainText;
         html.Replace(_T("\r\n"), _T("<br>\r\n"));
      }

      if (plainText.IsEmpty())
         plainText = html;

      // A bounce carries a null return path (RFC 5321 4.5.5) and is generated
      // by a mail system rather than written by a person. A footer on one is
      // noise on a message nobody chose to send.
      if (message->GetFromAddress().IsEmpty())
         return false;

      if (!HasExternalRecipient(message))
         return false;

      // Into a local, and handed back to the caller only when the footer is
      // actually added. Every guard below this line ends in "no footer", and a
      // MessageData left in the caller's variable by one of them would have the
      // caller rewrite the message file for nothing - re-serialising a message
      // this function decided not to touch.
      std::shared_ptr<MessageData> data = message_data;
      const bool loadedHere = !data;

      if (loadedHere)
      {
         // Header only when the message is too large to be read whole, so that
         // the guards below, which read the header, decide as for any message,
         // and the log can say why it got no footer.
         data = std::shared_ptr<MessageData>(new MessageData());
         std::shared_ptr<Account> emptyAccount;

         if (!data->LoadFromMessage(emptyAccount, message, MessageData::LargeMessage::HeaderOnly))
            return false;
      }

      // An automatic reply, a vacation message or a delivery report: RFC 3834
      // says what these are and MessageData already knows how to recognise one.
      if (data->IsAutoSubmitted())
         return false;

      if (IsList(data))
         return false;

      // This server has already footed this message. A message can reach this
      // point twice - a distribution list posting is re-submitted, a rule makes
      // a copy - and twice is exactly the failure this feature is judged on.
      if (!data->GetFieldValue(String(AppliedHeader())).IsEmpty())
         return false;

      if (IsProtectedContentType(data->GetFieldValue(_T("Content-Type"))))
      {
         // Said in the ordinary log rather than the error log: nothing has gone
         // wrong, but an administrator whose footer is missing from exactly the
         // messages their finance team signs needs to be able to find out why
         // without reading this file.
         String logText;
         logText.Format(_T("DisclaimerAdder - Message %I64d from %s is signed or encrypted (%s), so the domain's disclaimer was not appended: appending to it would break the signature or the encryption. The message was sent unchanged."),
            message->GetID(), message->GetFromAddress().c_str(), data->GetFieldValue(_T("Content-Type")).c_str());
         LOG_APPLICATION(logText);
         return false;
      }

      if (LargeMessageSkipped_(message, data, _T("the domain's disclaimer")))
         return false;

      auto plainPart = data->GetBodyTextPlainPart();
      auto htmlPart = data->GetBodyTextHtmlPart();

      if (!plainPart && !htmlPart)
      {
         // Nothing readable to append to - a message that is one attachment and
         // no text. Left alone rather than given a text part it never had,
         // which would change the message's shape for every client.
         return false;
      }

      // Decided once for the whole message: if the footer is already quoted in
      // either form, this is a reply to a message that carried it and neither
      // part gets a second copy.
      if (plainPart && AlreadyCarries(plainPart->GetUnicodeText(), plainText, false))
         return false;

      if (htmlPart && AlreadyCarries(htmlPart->GetUnicodeText(), html, true))
         return false;

      if (plainPart)
      {
         String content = plainPart->GetUnicodeText();
         content += "\r\n" + plainText;
         plainPart->SetUnicodeText(content);
      }

      if (htmlPart)
      {
         String content = htmlPart->GetUnicodeText();
         content += "<br/>\r\n" + html;
         htmlPart->SetUnicodeText(content);

         // A logo in the footer, as a data: URL, goes as a cid: part (InlineImageParts.h).
         if (InlineImageParts::HasDataImages(html))
            InlineImageParts::FromDataUrls(*data);
      }

      data->SetFieldValue(String(AppliedHeader()), sender_domain->GetName());

      if (loadedHere)
         message_data = data;

      return true;
   }

   bool
   DisclaimerAdder::AppendText(std::shared_ptr<Message> message,
                               const String &plainText,
                               std::shared_ptr<MessageData> &message_data,
                               const String &appliedBy)
   {
      if (!message || plainText.IsEmpty())
         return false;

      String html = plainText;
      html.Replace(_T("\r\n"), _T("<br>\r\n"));

      std::shared_ptr<MessageData> data = message_data;
      const bool loadedHere = !data;
      if (loadedHere)
      {
         data = std::shared_ptr<MessageData>(new MessageData());
         std::shared_ptr<Account> emptyAccount;
         if (!data->LoadFromMessage(emptyAccount, message, MessageData::LargeMessage::HeaderOnly))
            return false;
      }

      if (IsProtectedContentType(data->GetFieldValue(_T("Content-Type"))))
         return false;

      // A rule's disclaimer on a message read header only - the rules read one
      // too large to be read whole that way (RuleApplier::ApplyRules).
      if (LargeMessageSkipped_(message, data, _T("the disclaimer of ") + appliedBy))
         return false;

      auto plainPart = data->GetBodyTextPlainPart();
      auto htmlPart = data->GetBodyTextHtmlPart();
      if (!plainPart && !htmlPart)
         return false;

      if (plainPart && AlreadyCarries(plainPart->GetUnicodeText(), plainText, false))
         return false;
      if (htmlPart && AlreadyCarries(htmlPart->GetUnicodeText(), html, true))
         return false;

      if (plainPart)
      {
         String content = plainPart->GetUnicodeText();
         content += "\r\n" + plainText;
         plainPart->SetUnicodeText(content);
      }

      if (htmlPart)
      {
         String content = htmlPart->GetUnicodeText();
         content += "<br/>\r\n" + html;
         htmlPart->SetUnicodeText(content);
      }

      data->SetFieldValue(String(AppliedHeader()), appliedBy);

      if (loadedHere)
         message_data = data;

      return true;
   }

   void
   DisclaimerAdderTester::Test()
   {
      const String footer = _T("This message is confidential and may be privileged.");

      if (!DisclaimerAdder::AlreadyCarries(_T("Thanks.\r\n\r\nThis message is confidential and may be privileged."), footer, false))
         throw std::logic_error("A body carrying the disclaimer verbatim was not recognised.");

      // What a reply does to it: every line quoted.
      if (!DisclaimerAdder::AlreadyCarries(_T("Yes, agreed.\r\n\r\n> Thanks.\r\n>\r\n> This message is confidential and may\r\n> be privileged."), footer, false))
         throw std::logic_error("A quoted copy of the disclaimer was not recognised, so a reply chain would collect one per hop.");

      if (DisclaimerAdder::AlreadyCarries(_T("Yes, agreed."), footer, false))
         throw std::logic_error("A body that does not carry the disclaimer was said to carry it.");

      // The HTML form, wrapped in whatever tags the client used.
      const String htmlFooter = _T("<p>This message is confidential and may be privileged.</p>");
      if (!DisclaimerAdder::AlreadyCarries(_T("<div>Yes.</div><blockquote><div><p>This message is confidential\r\nand may be <b>privileged</b>.</p></div></blockquote>"), htmlFooter, true))
         throw std::logic_error("A quoted HTML copy of the disclaimer was not recognised.");

      if (DisclaimerAdder::AlreadyCarries(_T("<div>Yes.</div>"), htmlFooter, true))
         throw std::logic_error("An HTML body that does not carry the disclaimer was said to carry it.");

      // Too short to be a reliable needle.
      if (DisclaimerAdder::AlreadyCarries(_T("anything at all"), _T("at"), false))
         throw std::logic_error("A two-character disclaimer was matched against an arbitrary body.");

      if (!DisclaimerAdder::IsProtectedContentType(_T("multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"x\"")))
         throw std::logic_error("A multipart/signed message was not recognised as protected.");

      if (!DisclaimerAdder::IsProtectedContentType(_T("  Application/PKCS7-Mime; smime-type=enveloped-data")))
         throw std::logic_error("An S/MIME enveloped message was not recognised as protected.");

      if (!DisclaimerAdder::IsProtectedContentType(_T("multipart/encrypted; protocol=\"application/pgp-encrypted\"")))
         throw std::logic_error("A PGP/MIME message was not recognised as protected.");

      if (DisclaimerAdder::IsProtectedContentType(_T("multipart/alternative; boundary=\"x\"")))
         throw std::logic_error("An ordinary multipart/alternative message was refused as protected.");

      if (DisclaimerAdder::IsProtectedContentType(_T("")))
         throw std::logic_error("A message with no Content-Type was refused as protected.");
   }
}
