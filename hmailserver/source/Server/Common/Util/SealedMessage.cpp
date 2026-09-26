// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SealedMessage.h"

#include "GatewayEncryption.h"
#include "Encoding/Base64.h"
#include "../BO/MessageData.h"
#include "../Mime/Mime.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The file-name extensions each part of a real encrypted message carries,
      // when it carries one at all: what every client that writes these shapes
      // calls them (encrypted.asc, msg.asc, smime.p7m).
      const TCHAR *const OpenPgpExtensions[] = { _T(".asc"), _T(".pgp"), _T(".gpg"), nullptr };
      const TCHAR *const SmimeEnvelopeExtensions[] = { _T(".p7m"), nullptr };
      const TCHAR *const SmimeSignatureExtensions[] = { _T(".p7s"), nullptr };
      const TCHAR *const OpenPgpSignatureExtensions[] = { _T(".asc"), _T(".sig"), _T(".pgp"), _T(".gpg"), nullptr };

      bool EndsInOneOf(const String &name, const TCHAR *const *extensions)
      {
         String lowered = name;
         // As the blocked-attachment list reads it: trailing dots and white
         // space are dropped by Windows when the file is saved.
         lowered.TrimRight(_T(". \t"));
         lowered.ToLower();

         for (const TCHAR *const *extension = extensions; *extension; extension++)
         {
            if (lowered.EndsWith(*extension))
               return true;
         }

         return false;
      }

      // Whether every name a part gives itself - the one the blocked-attachment
      // list reads and a client saves it under, and the Content-Type name=
      // beside it - is absent or ends in one of the extensions. A part of an
      // encrypted message that is called invoice.exe is not one, whatever the
      // bytes in it are: a client that does not decrypt shows it as a file of
      // that name.
      bool CarriesOnlyNamesEndingIn(MimeBody &part, const TCHAR *const *extensions)
      {
         // An RFC 2231 name (filename*=, name*0=) is a second spelling a client
         // may prefer to the one read below. No client writes one on these
         // parts, so a part that has one is not taken for one of them.
         const char *fields[] = { CMimeConst::ContentDisposition(), CMimeConst::ContentType() };
         for (const char *field : fields)
         {
            const char *raw = part.GetRawFieldValue(field);
            if (raw != nullptr && strchr(raw, '*') != nullptr)
               return false;
         }

         const String names[] = {
            part.MimeHeader::GetUnicodeFilename(),
            String(AnsiString(part.GetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename()))),
            String(AnsiString(part.GetParameter(CMimeConst::ContentType(), CMimeConst::Name()))) };

         for (const String &name : names)
         {
            if (!name.IsEmpty() && !EndsInOneOf(name, extensions))
               return false;
         }

         return true;
      }

      String MediaTypeOfPart(MimeBody &part)
      {
         const char *contentType = part.GetContentType();
         return SealedMessage::MediaTypeOf(contentType != nullptr ? String(contentType) : String());
      }

      bool IsBase64Character(char c)
      {
         return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '+' || c == '/' || c == '=';
      }

      std::vector<unsigned char> DecodeBase64Characters(std::string characters)
      {
         std::vector<unsigned char> out;
         characters.resize(characters.size() - characters.size() % 4);
         if (characters.empty())
            return out;

         const AnsiString decoded = Base64::Decode(characters.c_str(), (int) characters.size());
         out.assign(decoded.c_str(), decoded.c_str() + decoded.GetLength());
         return out;
      }

      // The first octets of a part's content with its transfer encoding
      // removed: enough to read the start of a CMS object without decoding a
      // message of fifty megabytes to do it.
      std::vector<unsigned char> DecodedPrefix(MimeBody &part, size_t wanted)
      {
         const AnsiString &content = part.GetContent();
         const char *encodingField = part.GetTransferEncoding();
         AnsiString encoding = encodingField != nullptr ? AnsiString(encodingField) : AnsiString();
         encoding.Trim();
         encoding.MakeLower();

         if (encoding == "base64")
         {
            const size_t needed = (wanted + 2) / 3 * 4;
            std::string characters;
            for (size_t i = 0; i < (size_t) content.GetLength() && characters.size() < needed; i++)
            {
               const char c = content.c_str()[i];
               if (IsBase64Character(c))
                  characters += c;
               else if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
                  break;
            }

            return DecodeBase64Characters(characters);
         }

         AnsiString decoded;
         const AnsiString *source = &content;
         if (encoding == "quoted-printable")
         {
            if (!part.GetDecodedContent(decoded))
               return std::vector<unsigned char>();
            source = &decoded;
         }

         const size_t length = std::min((size_t) source->GetLength(), wanted);
         return std::vector<unsigned char>(source->c_str(), source->c_str() + length);
      }

      // One line of a text at a time, in place: where it starts and how long it
      // is without its line end and trailing white space. The text can be the
      // size of the message and is walked once for every rule that asks, so
      // nothing of it is copied.
      class LineReader
      {
      public:
         LineReader(const char *text, size_t length) :
            text_(text),
            length_(length),
            at_(0)
         {
         }

         bool Next(const char *&line, size_t &lineLength)
         {
            if (at_ >= length_)
               return false;

            const void *newline = memchr(text_ + at_, '\n', length_ - at_);
            const size_t end = newline != nullptr ? (size_t) ((const char *) newline - text_) : length_;

            line = text_ + at_;
            lineLength = end - at_;
            while (lineLength > 0 && (line[lineLength - 1] == '\r' || line[lineLength - 1] == ' ' || line[lineLength - 1] == '\t'))
               lineLength--;

            at_ = end + 1;
            return true;
         }

      private:
         const char *text_;
         size_t length_;
         size_t at_;
      };

      bool LineIs(const char *line, size_t length, const char *literal)
      {
         const size_t literalLength = strlen(literal);
         return length == literalLength && memcmp(line, literal, literalLength) == 0;
      }

      // A "Name: value" line of printable ASCII, as an armour header or the
      // control part's fields are.
      bool IsFieldLine(const char *line, size_t length)
      {
         const void *colon = memchr(line, ':', length);
         if (colon == nullptr || colon == line)
            return false;

         for (size_t i = 0; i < length; i++)
         {
            const unsigned char c = (unsigned char) line[i];
            if ((c < 0x20 || c > 0x7E) && c != '\t')
               return false;
         }

         return true;
      }

      // A part's content with its transfer encoding removed: in place when it
      // has none to remove, which is how the armour of a PGP/MIME message is
      // always sent, and otherwise decoded into 'decoded'.
      bool ContentOf(MimeBody &part, AnsiString &decoded, const char *&text, size_t &length)
      {
         const char *encodingField = part.GetTransferEncoding();
         AnsiString encoding = encodingField != nullptr ? AnsiString(encodingField) : AnsiString();
         encoding.Trim();
         encoding.MakeLower();

         if (encoding.IsEmpty() || encoding == "7bit" || encoding == "8bit" || encoding == "binary")
         {
            text = part.GetContent().c_str();
            length = (size_t) part.GetContent().GetLength();
            return true;
         }

         if (!part.GetDecodedContent(decoded))
            return false;

         text = decoded.c_str();
         length = (size_t) decoded.GetLength();
         return true;
      }

      // An ASCII-armoured OpenPGP message (RFC 9580 section 6.2) and nothing
      // else: the BEGIN line, armour headers, a blank line, base64 lines, an
      // optional checksum, the END line - and whose packets are session keys
      // and then encrypted data. Text around the armour, a line that is not
      // base64, or packets that are not under a key, and it is not one: what
      // is being decided is that there is nothing in this part but ciphertext.
      bool IsArmouredEncryptedMessage(const char *text, size_t textLength)
      {
         LineReader reader(text, textLength);
         const char *line = nullptr;
         size_t length = 0;

         bool more = reader.Next(line, length);
         while (more && length == 0)
            more = reader.Next(line, length);

         if (!more || !LineIs(line, length, "-----BEGIN PGP MESSAGE-----"))
            return false;

         // Armour headers (Version:, Comment:), then one blank line.
         while (true)
         {
            if (!reader.Next(line, length))
               return false;
            if (length == 0)
               break;
            if (!IsFieldLine(line, length))
               return false;
         }

         // 12 KB of packets is what GatewayEncryption reads, and holds the
         // session key packets of any ordinary number of recipients.
         const size_t wantedCharacters = 12 * 1024 / 3 * 4;
         std::string characters;
         bool sawData = false;
         bool sawPadding = false;
         bool sawChecksum = false;
         bool sawEnd = false;

         while (reader.Next(line, length))
         {
            if (LineIs(line, length, "-----END PGP MESSAGE-----"))
            {
               sawEnd = true;
               break;
            }

            if (length == 0 || sawChecksum)
               return false;

            if (line[0] == '=')
            {
               // The checksum: '=' and four base64 characters.
               if (!sawData || length != 5)
                  return false;
               for (size_t i = 1; i < 5; i++)
               {
                  if (!IsBase64Character(line[i]) || line[i] == '=')
                     return false;
               }
               sawChecksum = true;
               continue;
            }

            if (sawPadding)
               return false;

            for (size_t i = 0; i < length; i++)
            {
               const char c = line[i];
               if (!IsBase64Character(c))
                  return false;
               if (c == '=')
                  sawPadding = true;
               else if (sawPadding)
                  return false;
            }

            sawData = true;
            if (characters.size() < wantedCharacters)
               characters.append(line, std::min(length, wantedCharacters - characters.size()));
         }

         if (!sawEnd || !sawData)
            return false;

         // Nothing after the armour but blank lines.
         while (reader.Next(line, length))
         {
            if (length != 0)
               return false;
         }

         return GatewayEncryption::IsEncryptedOpenPgpPackets(DecodeBase64Characters(characters));
      }

      // The first part of a PGP/MIME message (RFC 3156 section 4): the control
      // information, "Version: 1", and nothing else but more fields of the
      // same form.
      bool IsPgpVersionPart(MimeBody &part)
      {
         AnsiString decoded;
         const char *text = nullptr;
         size_t textLength = 0;
         if (!ContentOf(part, decoded, text, textLength) || textLength > 1024)
            return false;

         LineReader reader(text, textLength);
         const char *line = nullptr;
         size_t length = 0;
         bool sawVersion = false;
         while (reader.Next(line, length))
         {
            if (length == 0)
               continue;
            if (!IsFieldLine(line, length))
               return false;

            AnsiString compact(std::string(line, length));
            compact.Replace(" ", "");
            compact.Replace("\t", "");
            compact.MakeLower();
            if (compact == "version:1")
               sawVersion = true;
         }

         return sawVersion;
      }

      // The ciphertext part of a PGP/MIME message, or a bare
      // application/pgp-encrypted body: an armoured encrypted message and
      // nothing else, under a name an encrypted part carries.
      bool IsOpenPgpCiphertextPart(MimeBody &part)
      {
         if (part.IsMultiPart() || !CarriesOnlyNamesEndingIn(part, OpenPgpExtensions))
            return false;

         AnsiString decoded;
         const char *text = nullptr;
         size_t length = 0;
         if (!ContentOf(part, decoded, text, length))
            return false;

         return IsArmouredEncryptedMessage(text, length);
      }

      String ProtocolOf(MimeBody &message)
      {
         String protocol = String(AnsiString(message.GetParameter(CMimeConst::ContentType(), "protocol")));
         protocol.Trim();
         protocol.ToLower();
         return protocol;
      }
   }

   String
   SealedMessage::MediaTypeOf(const String &contentType)
   {
      String result = contentType;

      int semicolon = result.Find(_T(";"));
      if (semicolon >= 0)
         result = result.Mid(0, semicolon);

      result.TrimLeft();
      result.TrimRight();
      result.ToLower();
      return result;
   }

   bool
   SealedMessage::IsSealedContentType(const String &contentType)
   {
      const String mediaType = MediaTypeOf(contentType);

      return mediaType == _T("multipart/signed") ||
             mediaType == _T("multipart/encrypted") ||
             mediaType == _T("application/pkcs7-mime") ||
             mediaType == _T("application/x-pkcs7-mime") ||
             mediaType == _T("application/pgp-encrypted") ||
             mediaType == _T("application/pgp-signature");
   }

   bool
   SealedMessage::IsSealed(std::shared_ptr<MessageData> messageData)
   {
      if (!messageData)
         return false;

      return IsSealedContentType(messageData->GetFieldValue(_T("Content-Type")));
   }

   bool
   SealedMessage::IsOpaqueContentType(const String &contentType)
   {
      const String mediaType = MediaTypeOf(contentType);

      // application/pgp-signature is deliberately not here either: a detached
      // signature at the top level is a malformed message, but what it carries
      // is a signature over text, not a text nobody can read.
      return mediaType == _T("multipart/encrypted") ||
             mediaType == _T("application/pkcs7-mime") ||
             mediaType == _T("application/x-pkcs7-mime") ||
             mediaType == _T("application/pgp-encrypted");
   }

   bool
   SealedMessage::IsOpaque(MimeBody &message)
   {
      const String mediaType = MediaTypeOfPart(message);

      // S/MIME: one part, whose content is a CMS object this server does not
      // unpack - enveloped-data or authenticated-enveloped-data, which are under
      // a key, or opaque signed-data, whose text is inside the CMS.
      if (mediaType == _T("application/pkcs7-mime") || mediaType == _T("application/x-pkcs7-mime"))
      {
         if (message.IsMultiPart() || !CarriesOnlyNamesEndingIn(message, SmimeEnvelopeExtensions))
            return false;

         const std::vector<unsigned char> prefix = DecodedPrefix(message, 32);
         return GatewayEncryption::IsEnvelopedCms(prefix) || GatewayEncryption::IsSignedCms(prefix);
      }

      // A bare application/pgp-encrypted body: malformed, but when what it
      // holds is an encrypted OpenPGP message it is as unreadable as the
      // PGP/MIME it was cut out of.
      if (mediaType == _T("application/pgp-encrypted"))
         return IsOpenPgpCiphertextPart(message);

      // PGP/MIME (RFC 3156 section 4): exactly two parts, the control
      // information and then the ciphertext, under the OpenPGP protocol. With
      // exactly those two there is no other part for a blocked file to hide in.
      if (mediaType == _T("multipart/encrypted"))
      {
         if (!message.IsMultiPart() || message.GetNumberOfParts() != 2)
            return false;

         if (ProtocolOf(message) != _T("application/pgp-encrypted"))
            return false;

         // No caller is part of the way through this message's parts when it
         // asks, so the part walk's own position can be used here.
         std::shared_ptr<MimeBody> control = message.FindFirstPart();
         std::shared_ptr<MimeBody> ciphertext = message.FindNextPart();
         if (!control || !ciphertext)
            return false;

         if (MediaTypeOfPart(*control) != _T("application/pgp-encrypted") || control->IsMultiPart() ||
             !CarriesOnlyNamesEndingIn(*control, OpenPgpExtensions) || !IsPgpVersionPart(*control))
            return false;

         if (MediaTypeOfPart(*ciphertext) != _T("application/octet-stream"))
            return false;

         return IsOpenPgpCiphertextPart(*ciphertext);
      }

      return false;
   }

   bool
   SealedMessage::IsOpaque(std::shared_ptr<MimeBody> message)
   {
      return message ? IsOpaque(*message) : false;
   }

   std::shared_ptr<MimeBody>
   SealedMessage::DetachedSignaturePart(MimeBody &message)
   {
      std::shared_ptr<MimeBody> none;

      // RFC 1847: multipart/signed has exactly two parts, the signed content
      // and then the signature, whose type is the protocol parameter's.
      if (MediaTypeOfPart(message) != _T("multipart/signed") || !message.IsMultiPart() || message.GetNumberOfParts() != 2)
         return none;

      const String protocol = ProtocolOf(message);
      const bool smime = protocol == _T("application/pkcs7-signature") || protocol == _T("application/x-pkcs7-signature");
      const bool openPgp = protocol == _T("application/pgp-signature");
      if (!smime && !openPgp)
         return none;

      // No caller is part of the way through this message's parts when it asks.
      message.FindFirstPart();
      std::shared_ptr<MimeBody> signature = message.FindNextPart();
      if (!signature || signature->IsMultiPart())
         return none;

      const String type = MediaTypeOfPart(*signature);
      if (smime && type != _T("application/pkcs7-signature") && type != _T("application/x-pkcs7-signature"))
         return none;
      if (openPgp && type != _T("application/pgp-signature"))
         return none;

      if (!CarriesOnlyNamesEndingIn(*signature, smime ? SmimeSignatureExtensions : OpenPgpSignatureExtensions))
         return none;

      // And what it holds is a signature: CMS signed-data, or an armoured
      // OpenPGP signature.
      if (smime)
         return GatewayEncryption::IsSignedCms(DecodedPrefix(*signature, 32)) ? signature : none;

      AnsiString decoded;
      const char *text = nullptr;
      size_t textLength = 0;
      if (!ContentOf(*signature, decoded, text, textLength))
         return none;

      LineReader reader(text, textLength);
      const char *line = nullptr;
      size_t length = 0;
      bool more = reader.Next(line, length);
      while (more && length == 0)
         more = reader.Next(line, length);

      return more && LineIs(line, length, "-----BEGIN PGP SIGNATURE-----") ? signature : none;
   }

   std::shared_ptr<MimeBody>
   SealedMessage::DetachedSignaturePart(std::shared_ptr<MimeBody> message)
   {
      return message ? DetachedSignaturePart(*message) : std::shared_ptr<MimeBody>();
   }

   bool
   SealedMessage::IsOpaque(std::shared_ptr<MessageData> messageData)
   {
      if (!messageData)
         return false;

      return IsOpaque(messageData->GetMimeMessage());
   }

   void
   SealedMessageTester::Test()
   {
      if (SealedMessage::MediaTypeOf(_T("  Application/PKCS7-Mime; smime-type=enveloped-data")) != _T("application/pkcs7-mime"))
         throw std::logic_error("The media type was not separated from its parameters and lower-cased.");

      if (SealedMessage::MediaTypeOf(_T("text/plain")) != _T("text/plain"))
         throw std::logic_error("A Content-Type with no parameters was not returned as itself.");

      if (SealedMessage::MediaTypeOf(_T("")) != _T(""))
         throw std::logic_error("An absent Content-Type did not stay absent.");

      // The six, each in a shape a real client sends.
      if (!SealedMessage::IsSealedContentType(_T("multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"x\"")))
         throw std::logic_error("An S/MIME detached signature was not recognised as sealed.");

      if (!SealedMessage::IsSealedContentType(_T("multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"x\"")))
         throw std::logic_error("A PGP/MIME message was not recognised as sealed.");

      if (!SealedMessage::IsSealedContentType(_T("application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"")))
         throw std::logic_error("An S/MIME enveloped message was not recognised as sealed.");

      if (!SealedMessage::IsSealedContentType(_T("application/x-pkcs7-mime; name=\"smime.p7m\"")))
         throw std::logic_error("The older spelling of an S/MIME enveloped message was not recognised as sealed.");

      if (!SealedMessage::IsSealedContentType(_T("application/pgp-encrypted")))
         throw std::logic_error("A bare application/pgp-encrypted was not recognised as sealed.");

      if (!SealedMessage::IsSealedContentType(_T("APPLICATION/PGP-SIGNATURE")))
         throw std::logic_error("An upper-case application/pgp-signature was not recognised as sealed.");

      // And the messages that are not sealed, which is nearly all of them. A
      // false positive here is a message that stops being scanned, stops being
      // filtered and stops having its blocked attachments removed, so these
      // matter at least as much as the six above.
      if (SealedMessage::IsSealedContentType(_T("multipart/mixed; boundary=\"x\"")))
         throw std::logic_error("An ordinary message with an attachment was called sealed.");

      if (SealedMessage::IsSealedContentType(_T("multipart/alternative; boundary=\"x\"")))
         throw std::logic_error("An ordinary plain-and-HTML message was called sealed.");

      if (SealedMessage::IsSealedContentType(_T("multipart/related; boundary=\"x\"")))
         throw std::logic_error("A message with an inline image was called sealed.");

      if (SealedMessage::IsSealedContentType(_T("text/plain; charset=utf-8")))
         throw std::logic_error("A plain-text message was called sealed.");

      if (SealedMessage::IsSealedContentType(_T("")))
         throw std::logic_error("A message with no Content-Type at all was called sealed.");

      // A signed-looking name that is not the media type. "multipart/signed"
      // appearing as a parameter value must not seal a mixed message, or a
      // sender could opt out of attachment blocking by writing a boundary.
      if (SealedMessage::IsSealedContentType(_T("multipart/mixed; boundary=\"multipart/signed\"")))
         throw std::logic_error("A boundary that spells a sealed type made an ordinary message look sealed.");

      // The one that is not a MIME question at all: an inline OpenPGP message
      // is text/plain and this deliberately says so. MessageIndexer and the
      // documentation both depend on that being the answer rather than an
      // oversight.
      if (SealedMessage::IsSealedContentType(_T("text/plain; charset=us-ascii; x-action=pgp-encrypted")))
         throw std::logic_error("Inline OpenPGP is text/plain and must not be recognised here.");

      // Null: the direction that changes nothing for the caller.
      std::shared_ptr<MessageData> noMessage;
      if (SealedMessage::IsSealed(noMessage))
         throw std::logic_error("A message that could not be loaded was called sealed.");

      if (SealedMessage::IsOpaque(noMessage))
         throw std::logic_error("A message that could not be loaded was called opaque.");

      // Opaque is the subset of sealed whose content cannot be read here. The
      // four that hide their content...
      if (!SealedMessage::IsOpaqueContentType(_T("multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=\"x\"")))
         throw std::logic_error("A PGP/MIME message was not recognised as opaque.");

      if (!SealedMessage::IsOpaqueContentType(_T("application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"")))
         throw std::logic_error("An S/MIME enveloped message was not recognised as opaque.");

      if (!SealedMessage::IsOpaqueContentType(_T("Application/X-PKCS7-Mime; smime-type=signed-data; name=\"smime.p7m\"")))
         throw std::logic_error("An S/MIME opaque-signed message was not recognised as opaque: its text is inside CMS this server does not unpack.");

      if (!SealedMessage::IsOpaqueContentType(_T("application/pgp-encrypted")))
         throw std::logic_error("A bare application/pgp-encrypted was not recognised as opaque.");

      // ...and the two that are sealed but readable. A false positive here is a
      // rule that stops filtering an organisation's own signed mail and a virus
      // verdict that calls an examined message unexamined.
      if (SealedMessage::IsOpaqueContentType(_T("multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"x\"")))
         throw std::logic_error("A multipart/signed message was called opaque, but its first part is the text as written and can be read.");

      if (SealedMessage::IsOpaqueContentType(_T("application/pgp-signature")))
         throw std::logic_error("A detached signature was called opaque.");

      if (SealedMessage::IsOpaqueContentType(_T("multipart/mixed; boundary=\"x\"")))
         throw std::logic_error("An ordinary message with an attachment was called opaque.");

      // Every opaque type is a sealed type; the reverse is what the two tests
      // above pin. Checked as a rule rather than a list so that a type added to
      // one set and not the other is caught.
      const TCHAR *everyType[] = {
         _T("multipart/signed"), _T("multipart/encrypted"), _T("application/pkcs7-mime"),
         _T("application/x-pkcs7-mime"), _T("application/pgp-encrypted"), _T("application/pgp-signature"),
         _T("multipart/mixed"), _T("text/plain"), _T("text/html"), _T("multipart/alternative"), _T("") };

      for (const TCHAR *type : everyType)
      {
         if (SealedMessage::IsOpaqueContentType(type) && !SealedMessage::IsSealedContentType(type))
            throw std::logic_error("A content type was called opaque but not sealed; opaque is a subset of sealed.");
      }

      TestStructure_();
   }

   namespace
   {
      std::shared_ptr<MimeBody> ParsedForTest(const std::string &text)
      {
         std::shared_ptr<MimeBody> body = std::make_shared<MimeBody>();
         if (body->LoadFromMemory(std::make_shared<const std::string>(text), _T("SealedMessageTester"), false) != MimeLoadResult::Loaded)
            throw std::logic_error("A test message did not parse.");
         return body;
      }

      // One public-key session key packet and then SEIPD, armoured: what
      // IsEncryptedEndToEnd, and now IsOpaque, take for a message under a key.
      const char EncryptedPackets[] = "wQwDAAAAAAAAAAABAAHSGQFzZWFsZWQtY2lwaGVydGV4dC1tYXJrZXI=";
      // A literal data packet: an OpenPGP message that is not under any key.
      const char LiteralPackets[] = "ywhiAAAAAABoaQ==";
      // The first octets of a Windows executable.
      const char ExecutableBase64[] = "TVqQAAMAAAAEAAAA//8AALgAAAAAAAAAQAAAAAAAAAA=";

      std::string PgpMime(const std::string &controlPart, const std::string &ciphertextPart, const std::string &extraPart = std::string(),
                          const std::string &protocol = "; protocol=\"application/pgp-encrypted\"")
      {
         std::string text =
            "From: a@example.com\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/encrypted" + protocol + "; boundary=\"b\"\r\n"
            "\r\n"
            "--b\r\n" + controlPart + "\r\n"
            "--b\r\n" + ciphertextPart + "\r\n";
         if (!extraPart.empty())
            text += "--b\r\n" + extraPart + "\r\n";
         text += "--b--\r\n";
         return text;
      }

      const std::string ControlPart =
         "Content-Type: application/pgp-encrypted\r\n"
         "Content-Description: PGP/MIME version identification\r\n"
         "\r\n"
         "Version: 1\r\n";

      std::string CiphertextPart(const std::string &name, const std::string &packets, const std::string &after = std::string())
      {
         return "Content-Type: application/octet-stream; name=\"" + name + "\"\r\n"
                "Content-Disposition: inline; filename=\"" + name + "\"\r\n"
                "\r\n"
                "-----BEGIN PGP MESSAGE-----\r\n"
                "\r\n" + packets + "\r\n"
                "=abcd\r\n"
                "-----END PGP MESSAGE-----\r\n" + after;
      }

      const std::string ExecutablePart =
         "Content-Type: application/octet-stream; name=\"invoice.exe\"\r\n"
         "Content-Transfer-Encoding: base64\r\n"
         "Content-Disposition: attachment; filename=\"invoice.exe\"\r\n"
         "\r\n" + std::string(ExecutableBase64) + "\r\n";

      std::string Smime(const std::string &name, const std::string &base64)
      {
         return "From: a@example.com\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=\"" + name + "\"\r\n"
                "Content-Transfer-Encoding: base64\r\n"
                "Content-Disposition: attachment; filename=\"" + name + "\"\r\n"
                "\r\n" + base64 + "\r\n";
      }

      void Require(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }
   }

   void
   SealedMessageTester::TestStructure_()
   {
      // Opaque is decided from what the body holds. The label is a claim any
      // sender can write, and every caller of IsOpaque then skips a check -
      // the blocked-attachment list, the stripper, the rule engine's body and
      // content criteria - so a label alone must never be enough.
      Require(SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", EncryptedPackets)))),
         "A real PGP/MIME message, its ciphertext under a key, was not opaque.");

      // As Mutt writes it: no name on either part.
      Require(SealedMessage::IsOpaque(ParsedForTest(PgpMime("Content-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n",
         std::string("Content-Type: application/octet-stream\r\n\r\n-----BEGIN PGP MESSAGE-----\r\n\r\n") + EncryptedPackets + "\r\n-----END PGP MESSAGE-----\r\n"))),
         "A PGP/MIME message whose parts carry no names was not opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, ExecutablePart))),
         "A multipart/encrypted label around invoice.exe was called opaque, which switches off the blocked-attachment list for it.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", EncryptedPackets), ExecutablePart))),
         "A real PGP/MIME pair with invoice.exe as a third part was called opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("invoice.exe", EncryptedPackets)))),
         "A ciphertext part named invoice.exe was called opaque: a client that does not decrypt shows it as that file.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart,
         std::string("Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
                     "Content-Disposition: attachment; filename*=utf-8''invoice.exe\r\n\r\n"
                     "-----BEGIN PGP MESSAGE-----\r\n\r\n") + EncryptedPackets + "\r\n-----END PGP MESSAGE-----\r\n"))),
         "A ciphertext part with an RFC 2231 name beside its plain one was called opaque; a client may save it as invoice.exe.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", LiteralPackets)))),
         "An OpenPGP message under no key was called opaque, but anybody can read it.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", EncryptedPackets, "calc\r\n")))),
         "A ciphertext part with text after its armour was called opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", EncryptedPackets), std::string(), std::string()))),
         "A multipart/encrypted with no OpenPGP protocol parameter was called opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(
         "Content-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\nstart calc.exe\r\n", CiphertextPart("encrypted.asc", EncryptedPackets)))),
         "A control part that carries more than its fields was called opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(PgpMime(ControlPart,
         "Content-Type: text/plain; charset=us-ascii\r\n\r\nCard 4111 1111 1111 1111\r\n"))),
         "A multipart/encrypted label around readable text was called opaque, which hides it from the content rules.");

      // S/MIME: the CMS object's own content type decides, and its name.
      Require(SealedMessage::IsOpaque(ParsedForTest(Smime("smime.p7m", "MIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA="))),
         "An S/MIME enveloped-data message was not opaque.");

      Require(SealedMessage::IsOpaque(ParsedForTest(Smime("smime.p7m", "MIAGCSqGSIb3DQEHAqCAMIACAQE="))),
         "An S/MIME opaque signed-data message was not opaque: its text is inside CMS this server does not unpack.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(Smime("smime.p7m", ExecutableBase64))),
         "An application/pkcs7-mime label on an executable was called opaque.");

      Require(!SealedMessage::IsOpaque(ParsedForTest(Smime("invoice.exe", "MIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA="))),
         "An S/MIME part named invoice.exe was called opaque.");

      // The ordinary shapes, which were never opaque.
      Require(!SealedMessage::IsOpaque(ParsedForTest("Content-Type: text/plain\r\n\r\nhello\r\n")),
         "A plain-text message was called opaque.");

      std::shared_ptr<MimeBody> none;
      Require(!SealedMessage::IsOpaque(none), "A message that was not there was called opaque.");

      // The detached signature: the second of two parts, of the protocol's
      // type, under a signature's name, holding a signature.
      auto signedMessage = [](const std::string &protocol, const std::string &signaturePart, const std::string &extraPart)
      {
         std::string text =
            "From: a@example.com\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/signed; protocol=\"" + protocol + "\"; micalg=sha-256; boundary=\"s\"\r\n"
            "\r\n"
            "--s\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "The text as written.\r\n"
            "\r\n"
            "--s\r\n" + signaturePart + "\r\n";
         if (!extraPart.empty())
            text += "--s\r\n" + extraPart + "\r\n";
         text += "--s--\r\n";
         return text;
      };

      auto smimeSignature = [](const std::string &type, const std::string &name, const std::string &base64)
      {
         return "Content-Type: " + type + "; name=\"" + name + "\"\r\n"
                "Content-Transfer-Encoding: base64\r\n"
                "Content-Disposition: attachment; filename=\"" + name + "\"\r\n"
                "\r\n" + base64 + "\r\n";
      };

      const std::string signedData = "MIAGCSqGSIb3DQEHAqCAMIACAQE=";

      std::shared_ptr<MimeBody> genuine = ParsedForTest(signedMessage("application/pkcs7-signature",
         smimeSignature("application/pkcs7-signature", "smime.p7s", signedData), std::string()));
      std::shared_ptr<MimeBody> signaturePart = SealedMessage::DetachedSignaturePart(genuine);
      Require(signaturePart != nullptr && SealedMessage::MediaTypeOf(String(signaturePart->GetContentType())) == _T("application/pkcs7-signature"),
         "The smime.p7s of a real S/MIME signed message was not recognised as its signature.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(signedMessage("application/pgp-signature",
         "Content-Type: application/pgp-signature; name=\"signature.asc\"\r\n\r\n-----BEGIN PGP SIGNATURE-----\r\n\r\niQEzBAEBCAAdFiEE\r\n-----END PGP SIGNATURE-----\r\n",
         std::string()))) != nullptr,
         "The signature.asc of a real PGP/MIME signed message was not recognised as its signature.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(signedMessage("application/pkcs7-signature",
         smimeSignature("application/pkcs7-signature", "invoice.exe", signedData), std::string()))) == nullptr,
         "A signature-typed part called invoice.exe was spared the blocked-attachment list.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(signedMessage("application/pkcs7-signature",
         smimeSignature("application/pkcs7-signature", "smime.p7s", ExecutableBase64), std::string()))) == nullptr,
         "A signature-typed part holding an executable was spared the blocked-attachment list.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(signedMessage("application/pkcs7-signature",
         smimeSignature("application/pkcs7-signature", "smime.p7s", signedData), ExecutablePart))) == nullptr,
         "A multipart/signed with a third part was taken for a signed message.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(signedMessage("application/pgp-signature",
         smimeSignature("application/pkcs7-signature", "smime.p7s", signedData), std::string()))) == nullptr,
         "A signature of another type than the protocol names was spared.");

      Require(SealedMessage::DetachedSignaturePart(ParsedForTest(PgpMime(ControlPart, CiphertextPart("encrypted.asc", EncryptedPackets)))) == nullptr,
         "An encrypted message was said to have a detached signature.");
   }
}
