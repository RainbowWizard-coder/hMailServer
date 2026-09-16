// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SealedMessage.h"

#include "../BO/MessageData.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
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
   SealedMessage::IsOpaque(std::shared_ptr<MessageData> messageData)
   {
      if (!messageData)
         return false;

      return IsOpaqueContentType(messageData->GetFieldValue(_T("Content-Type")));
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
   }
}
