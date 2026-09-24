// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// XML-Signature verification (XML Signature Syntax and Processing, the W3C
// recommendation), of the one shape SAML 2.0 identity providers sign with and
// nothing else: an enveloped signature over its own parent element. It is the
// half of SAML sign-in that decides whether anything in an identity provider's
// answer is true (SamlSignIn.h); the reader and the canonicaliser it stands on
// are XmlDocument.h's.
//
// WHAT IS ACCEPTED - ALL OF IT, OR NOTHING
//
//   * Exactly one ds:Signature among the element's children. A second one, or
//     none, is refused; a signature anywhere else in the document is the
//     caller's to refuse (SamlSignIn refuses one that is not the Response's or
//     the Assertion's own).
//   * ds:SignedInfo, then ds:SignatureValue, then at most a ds:KeyInfo - which
//     is IGNORED: the key is the certificate the administrator stored, never
//     one the message carries. A ds:Object, or anything else, is refused.
//   * SignedInfo's CanonicalizationMethod is exc-c14n without comments (its
//     InclusiveNamespaces PrefixList honoured); anything else is refused -
//     inclusive c14n, and exc-c14n WithComments above all, since a comment the
//     signature covered is text this server would not see.
//   * SignatureMethod rsa-sha256 (RSA PKCS#1 v1.5 with SHA-256) or
//     ecdsa-sha256 (the value the raw r||s of RFC 4050 and RFC 6931). SHA-1,
//     HMAC and everything else refused.
//   * Exactly one Reference, whose URI is "#" and the element's own ID - an
//     empty URI (the whole document) or any other element is refused - and
//     whose Transforms are exactly enveloped-signature and then exc-c14n. An
//     XSLT, an XPath, a base64 or any other transform is refused. The digest
//     is SHA-256; SHA-1 is refused.
//   * The ID must name one element in the whole document: an ID written twice
//     - on the element and on a copy of it somewhere else - is refused,
//     because that is the shape of a wrapping attack.
//   * Base64 is read strictly: white space is skipped, and anything else that
//     is not the alphabet, bad padding, or bits after the last byte that are
//     not zero, is refused - so a value has one spelling.
//   * An InclusiveNamespaces PrefixList of more than 32 prefixes, or an
//     InclusiveNamespaces element with anything inside it, is refused.
//
// THE ORDER, AND WHAT A STRANGER CAN COST
//
// The shape and the algorithms are judged first, then SignedInfo is
// canonicalised and its signature verified, and only then is the element the
// Reference names - which can be most of the document - canonicalised and its
// digest compared. So a document nobody signed costs a parse, the checks above
// and one signature over a SignedInfo whose shape they bounded; the
// canonicaliser's own bounds (XmlDocument.h) hold the rest.
//
// THE ANTI-WRAPPING RULE
//
// A signature that verifies proves something about the bytes its digest was
// computed over and about nothing else. So Verify hands back those bytes read
// again as a document of their own - the signed element without its
// signature - and the caller uses THAT, never the element it passed in or
// anything looked up again in the original document. An attacker who moves
// the signed element somewhere the verifier looks and puts a forged one where
// the application looks gains nothing: the application only ever sees what
// was signed.
#pragma once

#include <string>

#include "XmlDocument.h"

namespace HM
{
   class XmlSignature
   {
   public:
      // Verifies the enveloped signature among element's children against the
      // certificates (one or more PEM certificates, any of which may have made
      // it: an identity provider rolling its key over signs with the new one
      // while the old one is still stored beside it). idAttribute is the
      // attribute the Reference's URI must name (SAML's is ID). On success,
      // verified holds the element as the digest saw it, re-read, and
      // certificateIndex which certificate verified it. False with a sentence
      // otherwise.
      static bool Verify(const XmlNode &element, const char *idAttribute, const std::string &certificatesPem,
                         XmlDocument &verified, int &certificateIndex, std::string &problem);

      // Base64 (RFC 4648 section 4), read strictly as described above; and
      // written, with no line breaks.
      static bool Base64Decode(const std::string &text, std::string &bytes);
      static std::string Base64Encode(const std::string &bytes);

      // The XML-DSig namespace.
      static const char *Namespace() { return "http://www.w3.org/2000/09/xmldsig#"; }
   };

   // Run from ClassTester::DoTests: fixed vectors .NET's SignedXml signed (RSA)
   // and a P-256 signature over the SignedInfo .NET canonicalised (ECDSA),
   // verifying against their certificates and not against another; every
   // single-byte change to the signed documents refused; a good signature by an
   // RSA key of 1024 bits refused; and each refusal above, by name.
   class XmlSignatureTester
   {
   public:
      void Test();
   };
}
