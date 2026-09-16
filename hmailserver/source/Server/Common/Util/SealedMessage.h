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
//               blocked-attachment list, the attachment stripper, the virus
//               verdict line, the rule engine's body criterion and the Sieve
//               log line.
//
// The rewriters ask the NARROWER question on purpose. Nothing on this server
// verifies a signature, so "multipart/signed" at the top of a message is a
// claim any sender can make; if the blocked-attachment list or the stripper
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

      // The narrower question: is the CONTENT of this message unreadable here?
      //
      // Sealed and opaque are different sets, and a caller that confuses them
      // tells a lie in one direction or the other. Every sealed message must be
      // left byte for byte as it arrived - that is the six above. But only four
      // of the six hide their content:
      //
      //   multipart/encrypted, application/pkcs7-mime, application/x-pkcs7-mime
      //   and application/pgp-encrypted
      //
      // are ciphertext (or, for an S/MIME opaque-signed message, CMS this
      // server does not unpack) from the first byte of the body to the last. A
      // multipart/signed message is NOT: its first part is the text exactly as
      // the author wrote it, readable by the virus scanner, the rule engine and
      // a Sieve script alike, and a body criterion on it evaluates honestly.
      // Declining to read it would break every rule an organisation has on its
      // own signed mail, and calling a clean scan of it "unexamined" would be
      // false. So the verdict line, the rule engine, the Sieve log, the
      // blocked-attachment list and the stripper ask this (see the note at the
      // top of the file on why the last two do), and the disclaimer asks
      // IsSealed.
      static bool IsOpaqueContentType(const String &contentType);

      // IsOpaqueContentType asked of a loaded message. False for a null one.
      static bool IsOpaque(std::shared_ptr<MessageData> messageData);
   };

   // Run from ClassTester::DoTests.
   class SealedMessageTester
   {
   public:
      void Test();
   };
}
