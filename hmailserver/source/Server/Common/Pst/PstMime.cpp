// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstMime.h.

#include "StdAfx.h"

#include "PstMime.h"
#include "PstCodePages.h"
#include "PstRtf.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace HM
{
   namespace
   {
      bool IsAsciiPrintable(const std::string &text)
      {
         for (size_t i = 0; i < text.size(); i++)
         {
            const unsigned char c = (unsigned char) text[i];
            if (c < 0x20 || c >= 0x7F)
               return false;
         }
         return true;
      }

      // RFC 5322 atext, and the space between words of a phrase.
      bool IsAtextOrSpace(unsigned char c)
      {
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')
            return true;
         return strchr("!#$%&'*+-/=?^_`{|}~", (int) c) != nullptr && c != 0;
      }

      bool LowerEquals(const std::string &a, const std::string &b)
      {
         if (a.size() != b.size())
            return false;
         for (size_t i = 0; i < a.size(); i++)
         {
            char x = a[i];
            char y = b[i];
            if (x >= 'A' && x <= 'Z') x = (char) (x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (char) (y - 'A' + 'a');
            if (x != y)
               return false;
         }
         return true;
      }

      bool StartsWithNoCase(const std::string &text, const char *prefix)
      {
         const size_t length = strlen(prefix);
         return text.size() >= length && LowerEquals(text.substr(0, length), prefix);
      }

      std::string Trim(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t");
         if (first == std::string::npos)
            return std::string();
         const size_t last = text.find_last_not_of(" \t");
         return text.substr(first, last - first + 1);
      }

      // An address fit for a header: nothing that would end it or start another.
      bool UsableAddress(const std::string &address)
      {
         if (address.empty() || address.size() > 320 || address.find('@') == std::string::npos)
            return false;
         for (size_t i = 0; i < address.size(); i++)
         {
            const unsigned char c = (unsigned char) address[i];
            if (c <= 0x20 || c == 0x7F || strchr("<>,;:\"\\()", (int) c) != nullptr)
               return false;
         }
         return true;
      }

      // A value that goes into a header as it is (a Message-ID, a references
      // list): printable ASCII, with long ones folded at their spaces.
      std::string Folded(const std::string &value)
      {
         if (value.size() <= 900)
            return value;

         std::string out;
         size_t lineLength = 0;
         size_t i = 0;
         while (i < value.size())
         {
            size_t space = value.find(' ', i);
            if (space == std::string::npos)
               space = value.size();
            const std::string word = value.substr(i, space - i);
            if (!out.empty() && lineLength + word.size() + 1 > 76)
            {
               out += "\r\n ";
               lineLength = 1;
            }
            else if (!out.empty())
            {
               out += ' ';
               lineLength++;
            }
            out += word;
            lineLength += word.size();
            i = space + 1;
         }
         return out;
      }

      // A MIME type from PR_ATTACH_MIME_TAG: type/subtype of token characters,
      // else application/octet-stream.
      std::string ContentType(const std::string &tag)
      {
         const std::string value = Trim(tag);
         const size_t slash = value.find('/');
         if (value.empty() || value.size() > 100 || slash == std::string::npos || slash == 0 || slash + 1 >= value.size())
            return "application/octet-stream";

         for (size_t i = 0; i < value.size(); i++)
         {
            const unsigned char c = (unsigned char) value[i];
            if (i == slash)
               continue;
            if (c <= 0x20 || c >= 0x7F || strchr("()<>@,;:\\\"/[]?=", (int) c) != nullptr)
               return "application/octet-stream";
         }
         return value;
      }

      struct Part
      {
         std::string headers;   // each ends in CRLF
         std::string body;
      };

      std::string Entity(const Part &part)
      {
         return part.headers + "\r\n" + part.body;
      }

      class Builder
      {
      public:
         Builder(const std::string &stem, std::vector<PstWarning> &warnings) :
            stem_(stem),
            warnings_(warnings),
            counter_(0)
         {
         }

         std::string Boundary()
         {
            // "=_" can never appear in quoted-printable or base64 output, and
            // the stem and the counter keep one message's boundaries apart from
            // another's and from each other; the "_" after the counter keeps
            // one from being the start of another ("_1_" is not the start of
            // "_10_"), which RFC 2046's prefix match would take for it.
            char number[16];
            snprintf(number, sizeof(number), "%d", ++counter_);
            return "=_hmPST_" + stem_ + "_" + number + "_";
         }

         Part Multipart(const char *subtype, const std::vector<Part> &parts, const std::string &extra = std::string())
         {
            std::vector<std::string> entities;
            for (const Part &part : parts)
               entities.push_back(Entity(part));

            // An embedded message and a clear-signed entity go in as they are,
            // bytes from the file, and the stem comes from the file too: every
            // line of the parts that begins as a boundary of this reader's
            // does, and a boundary is taken that none of them begins with.
            // Each line is looked at once, and each boundary passed over is
            // the start of one of them, so this ends.
            const std::string lead = "--=_hmPST_";
            std::set<std::string> taken;
            for (const std::string &entity : entities)
            {
               size_t at = entity.find(lead);
               while (at != std::string::npos)
               {
                  if (at == 0 || entity[at - 1] == '\n')
                  {
                     const size_t end = entity.find_first_of("\r\n", at);
                     const size_t length = (end == std::string::npos ? entity.size() : end) - at;
                     taken.insert(entity.substr(at, length < 256 ? length : 256));
                  }
                  at = entity.find(lead, at + 1);
               }
            }

            std::string boundary = Boundary();
            for (;;)
            {
               const std::string delimiter = "--" + boundary;
               std::set<std::string>::const_iterator next = taken.lower_bound(delimiter);
               if (next == taken.end() || next->compare(0, delimiter.size(), delimiter) != 0)
                  break;
               boundary = Boundary();
            }

            Part out;
            out.headers = std::string("Content-Type: multipart/") + subtype + ";" + extra + "\r\n\tboundary=\"" + boundary + "\"\r\n";
            for (const std::string &entity : entities)
               out.body += "--" + boundary + "\r\n" + entity + "\r\n";
            out.body += "--" + boundary + "--\r\n";
            return out;
         }

         const std::string &Stem() const { return stem_; }
         std::vector<PstWarning> &Warnings() { return warnings_; }

      private:
         std::string stem_;
         std::vector<PstWarning> &warnings_;
         int counter_;
      };

      Part TextPart(const char *subtype, const std::string &bytes, const std::string &charset)
      {
         Part part;
         part.headers = std::string("Content-Type: text/") + subtype + (charset.empty() ? std::string() : "; charset=\"" + charset + "\"") + "\r\n" +
                        "Content-Transfer-Encoding: quoted-printable\r\n";
         part.body = PstMime::QuotedPrintable(PstMime::Crlf(bytes));
         return part;
      }

      Part AttachmentPart(const std::string &name, const std::string &type, const std::string &data, const std::string &contentId, bool inlined)
      {
         Part part;
         part.headers = "Content-Type: " + type + ";\r\n\t" + PstMime::FileNameParameter("name", name) + "\r\n" +
                        "Content-Disposition: " + (inlined ? "inline" : "attachment") + ";\r\n\t" + PstMime::FileNameParameter("filename", name) + "\r\n";
         if (!contentId.empty())
            part.headers += "Content-ID: <" + contentId + ">\r\n";
         part.headers += "Content-Transfer-Encoding: base64\r\n";
         part.body = PstMime::Base64(data);
         return part;
      }

      std::string AddressList(const char *field, const std::vector<PstRecipient> &recipients, int type)
      {
         std::string out;
         for (const PstRecipient &recipient : recipients)
         {
            if (recipient.type != type)
               continue;
            out += out.empty() ? std::string(field) + ": " : std::string(",\r\n ");
            out += PstMime::Mailbox(recipient.name, recipient.address);
         }
         return out.empty() ? out : out + "\r\n";
      }

      std::string CleanContentId(const std::string &value)
      {
         std::string id = Trim(PstMime::HeaderSafe(value));
         if (id.size() >= 2 && id[0] == '<' && id[id.size() - 1] == '>')
            id = id.substr(1, id.size() - 2);
         if (id.empty() || id.size() > 250 || !IsAsciiPrintable(id) || id.find_first_of("<> \"\\") != std::string::npos)
            return std::string();
         return id;
      }

      std::string Headers(const PstMessage &message, std::vector<PstWarning> &warnings)
      {
         if (message.hasTransportHeaders && !Trim(message.transportHeaders).empty())
            return PstMime::WithoutContentFields(message.transportHeaders);

         std::string headers;

         const bool hasRepresenting = !message.representingName.empty() || !message.representingAddress.empty();
         const std::string fromName = hasRepresenting ? message.representingName : message.senderName;
         const std::string fromAddress = hasRepresenting ? message.representingAddress : message.senderAddress;
         if (!fromName.empty() || !fromAddress.empty())
            headers += "From: " + PstMime::Mailbox(fromName, fromAddress) + "\r\n";

         if (hasRepresenting && UsableAddress(message.senderAddress) && !LowerEquals(message.senderAddress, message.representingAddress))
            headers += "Sender: " + PstMime::Mailbox(message.senderName, message.senderAddress) + "\r\n";

         headers += AddressList("To", message.recipients, 1);
         headers += AddressList("Cc", message.recipients, 2);
         headers += AddressList("Bcc", message.recipients, 3);

         const std::string subject = PstMime::HeaderSafe(message.subject);
         if (!subject.empty())
         {
            if (IsAsciiPrintable(subject) && subject.size() <= 900)
               headers += "Subject: " + subject + "\r\n";
            else
               headers += "Subject: " + PstMime::EncodedWords(subject) + "\r\n";
         }

         const uint64_t times[] = { message.submitTime, message.deliveryTime, message.creationTime };
         for (uint64_t fileTime : times)
         {
            int64_t seconds = 0;
            if (PstFormat::FileTimeToUnix(fileTime, seconds))
            {
               headers += "Date: " + PstMime::DateText(seconds) + "\r\n";
               break;
            }
         }

         const struct { const char *field; const std::string *value; } identifiers[] =
         {
            { "Message-ID", &message.messageId }, { "In-Reply-To", &message.inReplyTo }, { "References", &message.references }
         };
         for (const auto &identifier : identifiers)
         {
            const std::string value = Trim(PstMime::HeaderSafe(*identifier.value));
            if (value.empty() || !IsAsciiPrintable(value))
               continue;

            // Folded only at its spaces: an identifier longer than a line may
            // be (RFC 5322's 998 characters, less the field's name) cannot be
            // written, and is left out.
            size_t longest = 0;
            size_t start = 0;
            while (start <= value.size())
            {
               size_t space = value.find(' ', start);
               if (space == std::string::npos)
                  space = value.size();
               if (space - start > longest)
                  longest = space - start;
               start = space + 1;
            }
            if (longest > 900)
            {
               warnings.push_back(PstWarning("identifier_too_long", std::string("its ") + identifier.field +
                  " holds an identifier longer than a header line may be, and was left out"));
               continue;
            }

            headers += std::string(identifier.field) + ": " + Folded(value) + "\r\n";
         }

         return headers;
      }

      std::string BuildMessage(const PstMessage &message, const std::string &stem, std::vector<PstWarning> &warnings);

      Part Body(const PstMessage &message, Builder &builder)
      {
         std::vector<PstWarning> &warnings = builder.Warnings();

         bool hasText = message.hasBody;
         std::string text = message.body;

         bool hasHtml = false;
         std::string html;
         std::string htmlCharset;

         std::vector<Part> extra;

         if (message.hasHtml && !message.html.empty())
         {
            hasHtml = true;
            html = message.html;
            if (message.htmlCodePage > 0)
            {
               htmlCharset = PstCodePages::CharsetName(message.htmlCodePage);
               if (htmlCharset.empty())
                  warnings.push_back(PstWarning("html_charset_unknown",
                     "the HTML body is in code page " + std::to_string(message.htmlCodePage) + ", which this reader does not name; it is kept as it is, with no charset"));
            }
         }
         else if (message.hasRtf && !message.rtfCompressed.empty())
         {
            std::string rtf;
            std::string problem;
            if (!PstRtf::Decompress(message.rtfCompressed, PstStore::MaximumMessageBytes, rtf, problem))
            {
               warnings.push_back(PstWarning("rtf_unreadable", "the RTF body was not read: " + problem));
            }
            else
            {
               const PstRtf::Kind kind = PstRtf::KindOf(rtf);
               if (kind == PstRtf::KindHtml)
               {
                  hasHtml = true;
                  html = PstRtf::Deencapsulate(rtf, true);
                  htmlCharset = "utf-8";
               }
               else if (kind == PstRtf::KindText)
               {
                  if (!hasText)
                  {
                     hasText = true;
                     text = PstRtf::Deencapsulate(rtf, false);
                  }
               }
               else
               {
                  // A body written in RTF: kept as the file Outlook would save,
                  // beside the text Outlook keeps with it.
                  if (!hasText)
                  {
                     hasText = true;
                     text = PstRtf::Deencapsulate(rtf, false);
                  }
                  extra.push_back(AttachmentPart("body.rtf", "application/rtf", rtf, std::string(), false));
                  warnings.push_back(PstWarning("rtf_body_kept", "the body was written in RTF; it is kept as body.rtf beside its text"));
               }
            }
         }

         Part textPart = TextPart("plain", text, "utf-8");
         Part core;
         if (hasText && hasHtml)
         {
            std::vector<Part> alternatives;
            alternatives.push_back(textPart);
            alternatives.push_back(TextPart("html", html, htmlCharset));
            core = builder.Multipart("alternative", alternatives);
         }
         else if (hasHtml)
            core = TextPart("html", html, htmlCharset);
         else
            core = textPart;

         // The attachments: those with a Content-ID go with an HTML body, as the
         // pictures it shows; the rest after it.
         std::vector<Part> related;
         std::vector<Part> attached;
         size_t index = 0;
         for (const PstAttachment &attachment : message.attachments)
         {
            index++;
            if (attachment.method == 5 && attachment.embedded)
            {
               char suffix[16];
               snprintf(suffix, sizeof(suffix), "e%u", (unsigned int) index);
               std::vector<PstWarning> innerWarnings;
               const std::string inner = BuildMessage(*attachment.embedded, builder.Stem() + suffix, innerWarnings);

               // What the embedded message was written without is this one's
               // too, as the reader's warnings about it are (PstStore); at most
               // as many as a message keeps.
               for (const PstWarning &warning : innerWarnings)
               {
                  if (warnings.size() >= PstStore::MaximumWarnings * 2)
                     break;
                  warnings.push_back(PstWarning(warning.word, PstFormat::CutUtf8("in the embedded message: " + warning.detail,
                                                                                 PstStore::MaximumWarningBytes)));
               }

               bool eightBit = false;
               for (size_t i = 0; i < inner.size() && !eightBit; i++)
                  eightBit = (unsigned char) inner[i] >= 0x80;

               std::string name = PstMime::CleanFileName(attachment.embedded->subject.empty() ? attachment.fileName : attachment.embedded->subject);
               if (name == "attachment")
                  name = "message";
               Part part;
               part.headers = "Content-Type: message/rfc822\r\n"
                              "Content-Disposition: attachment;\r\n\t" + PstMime::FileNameParameter("filename", name + ".eml") + "\r\n";
               if (eightBit)
                  part.headers += "Content-Transfer-Encoding: 8bit\r\n";
               part.body = inner;
               attached.push_back(part);
               continue;
            }

            if (attachment.method != 1)
               continue;

            const std::string name = PstMime::CleanFileName(attachment.fileName);
            const std::string type = ContentType(attachment.mimeType);
            const std::string contentId = CleanContentId(attachment.contentId);

            if (hasHtml && !contentId.empty())
               related.push_back(AttachmentPart(name, type, attachment.data, contentId, true));
            else
               attached.push_back(AttachmentPart(name, type, attachment.data, contentId, false));
         }

         if (!related.empty())
         {
            std::vector<Part> parts;
            parts.push_back(core);
            parts.insert(parts.end(), related.begin(), related.end());
            core = builder.Multipart("related", parts, hasText ? " type=\"multipart/alternative\";" : " type=\"text/html\";");
         }

         attached.insert(attached.end(), extra.begin(), extra.end());
         if (!attached.empty())
         {
            std::vector<Part> parts;
            parts.push_back(core);
            parts.insert(parts.end(), attached.begin(), attached.end());
            core = builder.Multipart("mixed", parts);
         }

         return core;
      }

      std::string BuildMessage(const PstMessage &message, const std::string &stem, std::vector<PstWarning> &warnings)
      {
         Builder builder(stem, warnings);
         const std::string headers = Headers(message, warnings);

         // S/MIME, kept as it was. A clear-signed message's attachment is the
         // signed entity whole - its own Content-Type first - and follows the
         // headers unchanged; an opaque one is its body.
         if (StartsWithNoCase(message.messageClass, "IPM.Note.SMIME"))
         {
            const PstAttachment *signedEntity = nullptr;
            for (const PstAttachment &attachment : message.attachments)
            {
               if (attachment.method == 1 && !attachment.data.empty())
               {
                  signedEntity = &attachment;
                  break;
               }
            }

            if (signedEntity && StartsWithNoCase(message.messageClass, "IPM.Note.SMIME.MultipartSigned"))
            {
               if (StartsWithNoCase(signedEntity->data, "Content-Type:") || StartsWithNoCase(signedEntity->data, "MIME-Version:"))
               {
                  std::string entity = signedEntity->data;
                  if (StartsWithNoCase(entity, "MIME-Version:"))
                  {
                     const size_t end = entity.find("\r\n");
                     entity = end == std::string::npos ? std::string() : entity.substr(end + 2);
                  }
                  return headers + "MIME-Version: 1.0\r\n" + entity;
               }

               warnings.push_back(PstWarning("smime_not_kept", "the signed message's signed part is not a MIME entity; it is kept as an attachment"));
            }
            else if (signedEntity)
            {
               // IPM.Note.SMIME is enveloped or opaque-signed data alike, and
               // which is not read here: no smime-type, rather than a wrong one.
               return headers + "MIME-Version: 1.0\r\n"
                      "Content-Type: application/pkcs7-mime;\r\n\tname=\"smime.p7m\"\r\n"
                      "Content-Disposition: attachment; filename=\"smime.p7m\"\r\n"
                      "Content-Transfer-Encoding: base64\r\n\r\n" + PstMime::Base64(signedEntity->data);
            }
         }

         const Part body = Body(message, builder);
         return headers + "MIME-Version: 1.0\r\n" + Entity(body);
      }
   }

   std::string
   PstMime::Build(const PstMessage &message, const std::string &stem, std::vector<PstWarning> &warnings)
   {
      return BuildMessage(message, stem, warnings);
   }

   std::string
   PstMime::HeaderSafe(const std::string &utf8)
   {
      std::string out = utf8;
      for (size_t i = 0; i < out.size(); i++)
      {
         const unsigned char c = (unsigned char) out[i];
         if (c < 0x20 || c == 0x7F)
            out[i] = ' ';
      }
      return out;
   }

   std::string
   PstMime::EncodedWords(const std::string &utf8, size_t firstLineUsed)
   {
      // RFC 2047 section 2: a word is at most 75 characters and a line holding
      // one at most 76. "=?UTF-8?B?" and "?=" are twelve characters, so a word
      // after the folding space has 63 of base64: 45 bytes of text. The first
      // word shares its line with the firstLineUsed characters before it and
      // carries less - never under six bytes, which a character of four still
      // fits in whole. Each word is cut before a byte that continues a
      // character.
      std::string out;
      size_t i = 0;
      size_t used = firstLineUsed;
      while (i < utf8.size())
      {
         size_t room = used < 76 ? 76 - used : 0;
         if (room > 75)
            room = 75;
         size_t maximum = room > 12 ? (room - 12) / 4 * 3 : 0;
         if (maximum < 6)
            maximum = 6;

         size_t length = utf8.size() - i < maximum ? utf8.size() - i : maximum;
         while (length > 1 && i + length < utf8.size() && ((unsigned char) utf8[i + length] & 0xC0) == 0x80)
            length--;

         std::string encoded = Base64(utf8.substr(i, length));
         // Base64 above is folded into lines; one short word never is.
         const size_t end = encoded.find("\r\n");
         if (end != std::string::npos)
            encoded = encoded.substr(0, end);

         if (!out.empty())
            out += "\r\n ";
         out += "=?UTF-8?B?" + encoded + "?=";
         i += length;
         used = 1;
      }
      return out;
   }

   std::string
   PstMime::Phrase(const std::string &utf8)
   {
      const std::string name = Trim(HeaderSafe(utf8));
      if (name.empty())
         return name;

      if (!IsAsciiPrintable(name))
         return EncodedWords(name);

      bool plain = name.size() <= 200;
      for (size_t i = 0; i < name.size() && plain; i++)
         plain = IsAtextOrSpace((unsigned char) name[i]);
      if (plain)
         return name;

      if (name.size() > 200)
         return EncodedWords(name);

      std::string quoted = "\"";
      for (size_t i = 0; i < name.size(); i++)
      {
         if (name[i] == '"' || name[i] == '\\')
            quoted += '\\';
         quoted += name[i];
      }
      return quoted + "\"";
   }

   std::string
   PstMime::Mailbox(const std::string &name, const std::string &address)
   {
      const std::string phrase = Phrase(name);
      const std::string cleanAddress = Trim(HeaderSafe(address));

      if (UsableAddress(cleanAddress))
      {
         if (phrase.empty() || LowerEquals(Trim(HeaderSafe(name)), cleanAddress))
            return cleanAddress;
         return phrase + " <" + cleanAddress + ">";
      }

      // No address mail can use - an Exchange recipient the PST kept no SMTP
      // address for: the name alone, as an empty group.
      return (phrase.empty() ? std::string("undisclosed-recipients") : phrase) + ":;";
   }

   std::string
   PstMime::Crlf(const std::string &text)
   {
      std::string out;
      out.reserve(text.size() + text.size() / 32);
      for (size_t i = 0; i < text.size(); i++)
      {
         const char c = text[i];
         if (c == '\r')
         {
            out += "\r\n";
            if (i + 1 < text.size() && text[i + 1] == '\n')
               i++;
         }
         else if (c == '\n')
            out += "\r\n";
         else
            out += c;
      }
      return out;
   }

   std::string
   PstMime::QuotedPrintable(const std::string &bytes)
   {
      // RFC 2045 6.7, over text whose lines already end in CRLF: those are
      // kept, everything else that is not printable ASCII - "=" among it - is
      // encoded, a space or tab before a line break is encoded, and a line is
      // broken softly before it passes 76 characters.
      static const char hex[] = "0123456789ABCDEF";
      std::string out;
      out.reserve(bytes.size() + bytes.size() / 8);
      size_t lineLength = 0;

      for (size_t i = 0; i < bytes.size(); i++)
      {
         const unsigned char c = (unsigned char) bytes[i];

         if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
         {
            out += "\r\n";
            lineLength = 0;
            i++;
            continue;
         }

         const bool lineEnds = i + 1 >= bytes.size() || (bytes[i + 1] == '\r' && i + 2 < bytes.size() && bytes[i + 2] == '\n');
         std::string piece;
         if ((c >= 33 && c <= 126 && c != '=') || ((c == ' ' || c == '\t') && !lineEnds))
            piece = std::string(1, (char) c);
         else
         {
            piece = "=";
            piece += hex[c >> 4];
            piece += hex[c & 0x0F];
         }

         if (lineLength + piece.size() > 75)
         {
            out += "=\r\n";
            lineLength = 0;
         }

         out += piece;
         lineLength += piece.size();
      }

      return out;
   }

   std::string
   PstMime::Base64(const std::string &bytes)
   {
      static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(bytes.size() * 4 / 3 + bytes.size() / 38 + 8);

      size_t lineLength = 0;
      for (size_t i = 0; i < bytes.size(); i += 3)
      {
         const unsigned int a = (unsigned char) bytes[i];
         const unsigned int b = i + 1 < bytes.size() ? (unsigned char) bytes[i + 1] : 0;
         const unsigned int c = i + 2 < bytes.size() ? (unsigned char) bytes[i + 2] : 0;
         const unsigned int triple = (a << 16) | (b << 8) | c;

         out += alphabet[(triple >> 18) & 0x3F];
         out += alphabet[(triple >> 12) & 0x3F];
         out += i + 1 < bytes.size() ? alphabet[(triple >> 6) & 0x3F] : '=';
         out += i + 2 < bytes.size() ? alphabet[triple & 0x3F] : '=';

         lineLength += 4;
         if (lineLength >= 76)
         {
            out += "\r\n";
            lineLength = 0;
         }
      }

      if (lineLength > 0)
         out += "\r\n";
      return out;
   }

   std::string
   PstMime::DateText(int64_t unixSeconds)
   {
      static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
      static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

      int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, weekday = 0;
      PstFormat::UnixToCivil(unixSeconds, year, month, day, hour, minute, second, weekday);

      char text[64];
      snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d +0000", days[weekday % 7], day, months[(month - 1) % 12], year, hour, minute, second);
      return text;
   }

   std::string
   PstMime::CleanFileName(const std::string &utf8)
   {
      std::string name = utf8;
      const size_t slash = name.find_last_of("\\/");
      if (slash != std::string::npos)
         name = name.substr(slash + 1);

      std::string out;
      for (size_t i = 0; i < name.size(); i++)
      {
         const unsigned char c = (unsigned char) name[i];
         if (c >= 0x20 && c != 0x7F)
            out += (char) c;
      }

      out = Trim(out);
      if (out.size() > 200)
      {
         size_t cut = 200;
         while (cut > 0 && ((unsigned char) out[cut] & 0xC0) == 0x80)
            cut--;
         out = out.substr(0, cut);
      }

      return out.empty() ? std::string("attachment") : out;
   }

   std::string
   PstMime::FileNameParameter(const std::string &parameter, const std::string &utf8)
   {
      if (IsAsciiPrintable(utf8))
      {
         std::string quoted = parameter + "=\"";
         for (size_t i = 0; i < utf8.size(); i++)
         {
            if (utf8[i] == '"' || utf8[i] == '\\')
               quoted += '\\';
            quoted += utf8[i];
         }
         return quoted + "\"";
      }

      // RFC 2231: charset'language'percent-encoded octets.
      static const char hex[] = "0123456789ABCDEF";
      std::string out = parameter + "*=UTF-8''";
      for (size_t i = 0; i < utf8.size(); i++)
      {
         const unsigned char c = (unsigned char) utf8[i];
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$&+-.^_`|~", (int) c) != nullptr)
            out += (char) c;
         else
         {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
         }
      }
      return out;
   }

   std::string
   PstMime::WithoutContentFields(const std::string &headers)
   {
      const std::string text = Crlf(headers);

      std::string out;
      bool keeping = false;
      size_t at = 0;
      while (at < text.size())
      {
         size_t end = text.find("\r\n", at);
         if (end == std::string::npos)
            end = text.size();
         const std::string line = text.substr(at, end - at);
         at = end + 2;

         // The end of the header, and anything a store kept after it.
         if (line.empty())
            break;

         if (line[0] == ' ' || line[0] == '\t')
         {
            if (keeping)
               out += line + "\r\n";
            continue;
         }

         const size_t colon = line.find(':');
         if (colon == std::string::npos || colon == 0)
         {
            // Not a field: nothing to keep, and not a continuation of one.
            keeping = false;
            continue;
         }

         const std::string name = Trim(line.substr(0, colon));
         keeping = !LowerEquals(name, "MIME-Version") && !StartsWithNoCase(name, "Content-");
         if (keeping)
            out += line + "\r\n";
      }

      return out;
   }
}
