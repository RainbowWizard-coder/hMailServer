// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a sealed message is, said once.
//
// A sealed message is one the two people writing to each other signed or
// encrypted end to end - S/MIME or PGP/MIME - so that this server is a courier
// for bytes it holds no key to. The webmail has been able to send and read them
// since 6.3.1, and a mail client has always been able to.
//
// The list of media types below is the list DisclaimerAdder has always used to
// decide that appending a legal footer to a message would break it, and it is
// deliberately the SAME list rather than a second one: every part of the server
// that reads or rewrites a message asks this class, or the product tells its
// administrator two different stories about one message.
// DisclaimerAdder::IsProtectedContentType now asks this.
//
// Two questions are answered, and which one a caller asks matters:
//
//   IsSealed  - "would rewriting this break a signature or the encryption?"
//               Asked by the domain disclaimer and the domain signature, where
//               declining costs nothing but a footer.
//   IsOpaque  - "is the content of this message unreadable here?" Asked by the
//               blocked-attachment list, the virus verdict line, the rule
//               engine's body and content criteria and eDiscovery. It is
//               answered from the body, never from the label: see IsOpaque
//               below. The attachment stripper asks the narrower IsEncrypted,
//               which S/MIME's opaque signed-data is not.
//
// The rewriters ask the NARROWER question on purpose. Nothing on this server
// verifies a signature, so "multipart/signed" at the top of a message is a
// claim any sender can make - and so is "multipart/encrypted", which is why
// IsOpaque looks for ciphertext rather than for the words. If the blocked-attachment list or the stripper
// declined a signed message, a multipart/signed wrapper around invoice.exe
// would walk past both, and the stripper's case is worse still because a
// signed message is the one sealed shape where the scanner can actually see a
// virus. A signed message is therefore rewritten like any other when the
// administrator's list or the scanner says so, and the log says the signature
// has been invalidated - the honest outcome. An encrypted message is left
// alone, because there is nothing to rewrite but the ciphertext.
//
// What this class is NOT: a decision about what to DO with a sealed message.
// Each caller keeps that. It answers one question - "is this a message we
// cannot see inside" - so that every caller can then say so in the log instead
// of quietly treating ciphertext as an empty body or as a blockable file.
//
// See hmailserver/docs/SealedMail.md for what a sealed message does and does
// not get on this server, which is the operator-facing half of this file.

#pragma once

namespace HM
{
   class MessageData;
   class MimeBody;

   class SealedMessage
   {
   public:

      // The media type of a Content-Type value: lower case, parameters removed,
      // surrounding white space trimmed. "  Application/PKCS7-Mime;
      // smime-type=enveloped-data" is "application/pkcs7-mime".
      static String MediaTypeOf(const String &contentType);

      // True when a message whose top-level Content-Type is this one is signed
      // or encrypted end to end.
      //
      // The six types, and why each is here:
      //
      //   multipart/signed            RFC 1847 - S/MIME detached and PGP/MIME
      //                               signatures. The first part is the signed
      //                               text; changing one byte of it invalidates
      //                               the signature in every client that checks.
      //   multipart/encrypted         RFC 3156 - PGP/MIME. The parts are a
      //                               version marker and the ciphertext.
      //   application/pkcs7-mime      RFC 8551 - an opaque S/MIME message,
      //   application/x-pkcs7-mime    encrypted or signed. The whole message is
      //                               one blob, usually named smime.p7m.
      //   application/pgp-encrypted   RFC 3156 - the parts of a PGP/MIME
      //   application/pgp-signature   message, seen at the top only in a
      //                               malformed or hand-built message, and
      //                               still not ours to rewrite.
      //
      // NOT here, deliberately: inline ("clearsigned" or armoured-in-the-body)
      // OpenPGP, which is a text/plain message with a BEGIN PGP block in it.
      // The MIME layer cannot tell one of those from ordinary text, so nothing
      // here can either; the places where that matters say so where they are.
      static bool IsSealedContentType(const String &contentType);

      // The same question asked of a loaded message, from its top-level
      // Content-Type. False for a null messageData, which is the direction that
      // leaves the caller's existing behaviour unchanged.
      static bool IsSealed(std::shared_ptr<MessageData> messageData);

      // Whether a message whose top-level Content-Type is this one CLAIMS to
      // be unreadable here: multipart/encrypted, application/pkcs7-mime,
      // application/x-pkcs7-mime or application/pgp-encrypted. A claim is all
      // it is - any sender can write the header - so nothing that skips a
      // check may act on it; it is for describing a message in a log line,
      // and for the Sieve log line that only reports. Every caller that
      // decides something asks IsOpaque below.
      static bool IsOpaqueContentType(const String &contentType);

      // The narrower question, answered from the body: is the CONTENT of this
      // message really unreadable here?
      //
      // Sealed and opaque are different sets, and a caller that confuses them
      // tells a lie in one direction or the other. A multipart/signed message
      // is sealed and NOT opaque: its first part is the text exactly as the
      // author wrote it, readable by the virus scanner, the rule engine and a
      // Sieve script alike. Only an encrypted message, or S/MIME's opaque
      // signed-data, hides its content - and the blocked-attachment list and
      // the rule engine's body and content criteria stand aside for one, the
      // attachment stripper for an encrypted one alone (IsEncrypted). So the
      // answer is never taken from the label, which the sender writes, but
      // from the shape the label promises:
      //
      //   application/pkcs7-mime or application/x-pkcs7-mime: one part whose
      //     content begins a CMS ContentInfo of enveloped-data,
      //     authenticated-enveloped-data or signed-data, named smime.p7m or
      //     nothing (GatewayEncryption::IsEnvelopedCms, IsSignedCms).
      //   multipart/encrypted: protocol="application/pgp-encrypted" and
      //     EXACTLY two parts, application/pgp-encrypted holding "Version: 1"
      //     and application/octet-stream holding one ASCII-armoured OpenPGP
      //     message and nothing else, whose packets are session keys and then
      //     encrypted data (GatewayEncryption::IsEncryptedOpenPgpPackets), each
      //     named *.asc, *.pgp, *.gpg or nothing.
      //   application/pgp-encrypted at the top: the second of those alone.
      //
      // Anything else - a multipart/encrypted around invoice.exe, around
      // text/plain, with a third part, a ciphertext part called invoice.exe,
      // an OpenPGP message under no key - is not opaque, and is blocked,
      // stripped and filtered like any other message. Before 6.3.4 shipped
      // this was the label alone, and one header switched all four off.
      //
      // It walks the top level's parts with FindFirstPart and FindNextPart, so
      // it is not to be asked part of the way through such a walk.
      static bool IsOpaque(MimeBody &message);
      static bool IsOpaque(std::shared_ptr<MimeBody> message);

      // IsOpaque asked of a loaded message's MIME tree. False for a null one.
      static bool IsOpaque(std::shared_ptr<MessageData> messageData);

      // IsOpaque less S/MIME's opaque signed-data: is the content of this
      // message under a key? The attachment stripper asks this. A message whose
      // text is inside a signature is as hidden from this server's readers as
      // an encrypted one, but not from its recipient's client, which shows the
      // content, a file in it included. The stripper, which cannot pick a part
      // out of it, replaces it with the notice, as it replaces any other
      // message that is one attachment, where it leaves an encrypted one as it
      // arrived.
      static bool IsEncrypted(MimeBody &message);

      // The detached signature of a signed message, or null: the part the
      // blocked-attachment list leaves alone, because replacing it removes
      // nothing and makes every client report the message as tampered with.
      //
      // Decided from the structure, like IsOpaque, because a content type is
      // a label the sender writes and the list is there to stop the sender:
      // the SECOND of exactly two parts of a top-level multipart/signed, of
      // the type its protocol parameter names (application/pkcs7-signature or
      // x-pkcs7-signature for S/MIME, application/pgp-signature for OpenPGP),
      // named *.p7s - or *.asc, *.sig, *.pgp, *.gpg - or not at all, and
      // holding CMS signed-data or an armoured OpenPGP signature. A part of
      // a signature's type anywhere else, or called invoice.exe, is checked
      // like any other attachment.
      static std::shared_ptr<MimeBody> DetachedSignaturePart(MimeBody &message);
      static std::shared_ptr<MimeBody> DetachedSignaturePart(std::shared_ptr<MimeBody> message);

   private:

      // IsOpaque's shape check, with S/MIME's opaque signed-data counted as
      // opaque or not (IsEncrypted).
      static bool IsOpaque_(MimeBody &message, bool signedDataIsOpaque);
   };

   // Run from ClassTester::DoTests.
   class SealedMessageTester
   {
   public:
      void Test();

   private:
      void TestStructure_();
   };
}
