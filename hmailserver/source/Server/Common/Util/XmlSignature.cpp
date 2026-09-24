// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "XmlSignature.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <stdexcept>
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
      const char *Ds = "http://www.w3.org/2000/09/xmldsig#";
      const char *EnvelopedSignature = "http://www.w3.org/2000/09/xmldsig#enveloped-signature";
      const char *RsaSha256 = "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256";
      const char *EcdsaSha256 = "http://www.w3.org/2001/04/xmldsig-more#ecdsa-sha256";
      const char *DigestSha256 = "http://www.w3.org/2001/04/xmlenc#sha256";

      // The smallest RSA key a signature is taken from.
      const int MinimumRsaBits = 2048;

      // A value from the document, for a sentence: cut short, and a control
      // character shown as a question mark, since the sentence is logged.
      std::string Shown(const std::string &text)
      {
         std::string shown = text.size() > 80 ? text.substr(0, 80) + "..." : text;
         for (char &c : shown)
         {
            if ((unsigned char) c < 0x20 || c == 0x7F)
               c = '?';
         }
         return shown;
      }

      // Whether the element holds text that is not white space. The structural
      // elements of a signature hold elements and nothing else.
      bool HoldsText(const XmlNode &node)
      {
         for (const std::shared_ptr<XmlNode> &child : node.children)
         {
            if (child->kind != XmlNode::KindText)
               continue;
            for (char c : child->text)
            {
               if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                  return true;
            }
         }
         return false;
      }

      // The element's children, which must be exactly these ds: elements in
      // this order - the optional ones last and marked with a question mark.
      bool ChildrenAre(const XmlNode &node, const std::vector<std::string> &names, std::vector<const XmlNode *> &found)
      {
         found = node.ElementChildren();
         if (HoldsText(node))
            return false;

         size_t required = 0;
         for (const std::string &name : names)
         {
            if (name.empty() || name[name.size() - 1] != '?')
               required++;
         }

         if (found.size() < required || found.size() > names.size())
            return false;

         for (size_t i = 0; i < found.size(); i++)
         {
            std::string name = names[i];
            if (!name.empty() && name[name.size() - 1] == '?')
               name = name.substr(0, name.size() - 1);
            if (!found[i]->Is(Ds, name.c_str()))
               return false;
         }

         return true;
      }

      std::string Algorithm(const XmlNode &node)
      {
         std::string algorithm;
         node.GetAttribute("Algorithm", algorithm);
         return algorithm;
      }

      // The PrefixList of an exc-c14n method's InclusiveNamespaces child. Refused
      // when the method holds anything else, when the InclusiveNamespaces
      // element holds anything at all - it is empty by definition, and whatever
      // were put in it would be canonicalised with SignedInfo before any key is
      // looked at - and when the list names more than MaximumPrefixes.
      bool InclusivePrefixes(const XmlNode &method, const char *what, std::vector<std::string> &prefixes, std::string &problem)
      {
         prefixes.clear();

         const std::vector<const XmlNode *> children = method.ElementChildren();
         if (HoldsText(method) || children.size() > 1 ||
             (children.size() == 1 && !children[0]->Is(ExclusiveCanonicalizer::Namespace(), "InclusiveNamespaces")))
         {
            problem = std::string("the signature's ") + what + " carries something other than an InclusiveNamespaces";
            return false;
         }

         if (children.empty())
            return true;

         if (!children[0]->ElementChildren().empty() || HoldsText(*children[0]))
         {
            problem = std::string("the signature's ") + what + " has an InclusiveNamespaces that is not empty";
            return false;
         }

         std::string list;
         children[0]->GetAttribute("PrefixList", list);
         if (!ExclusiveCanonicalizer::PrefixList(list, prefixes))
         {
            problem = std::string("the signature's ") + what + " names more than 32 prefixes in its PrefixList, which is refused";
            return false;
         }

         return true;
      }

      // Why a canonicalisation method is not the one accepted.
      std::string CanonicalizationRefusal(const std::string &algorithm)
      {
         if (algorithm == std::string(ExclusiveCanonicalizer::Algorithm()) + "WithComments")
            return "the signature is canonicalised with comments, which is refused: a comment the signature covers is text this server would not read";
         return "the signature's canonicalisation is not exclusive XML canonicalisation without comments (it is " + Shown(algorithm) + ")";
      }

      std::string SignatureMethodRefusal(const std::string &algorithm)
      {
         if (algorithm.find("hmac") != std::string::npos)
            return "the signature is an HMAC, which is refused: it would be one anybody holding the shared key could write";
         if (algorithm.find("sha1") != std::string::npos)
            return "the signature is made with SHA-1, which is refused";
         return "the signature's method is not rsa-sha256 or ecdsa-sha256 (it is " + Shown(algorithm) + ")";
      }

      std::string DigestRefusal(const std::string &algorithm)
      {
         if (algorithm.find("sha1") != std::string::npos)
            return "the signed element's digest is SHA-1, which is refused";
         return "the signed element's digest is not SHA-256 (it is " + Shown(algorithm) + ")";
      }

      std::string TransformRefusal(const std::string &algorithm)
      {
         std::string named = algorithm;
         if (algorithm.find("xslt") != std::string::npos)
            named = "XSLT";
         else if (algorithm.find("xpath") != std::string::npos || algorithm.find("filter2") != std::string::npos)
            named = "XPath";
         else if (algorithm.find("#base64") != std::string::npos)
            named = "base64";
         return "the signature uses a transform this server does not take (" + Shown(named) +
                "): only enveloped-signature and then exclusive canonicalisation";
      }

      // How many attributes in the whole document, named ID, Id or id and in no
      // namespace, carry this value: the three names XML-DSig tools resolve a
      // same-document reference by.
      int CountId(const XmlNode &node, const std::string &value)
      {
         int count = 0;
         for (const XmlAttribute &attribute : node.attributes)
         {
            if (attribute.uri.empty() && (attribute.local == "ID" || attribute.local == "Id" || attribute.local == "id") &&
                attribute.value == value)
               count++;
         }

         for (const std::shared_ptr<XmlNode> &child : node.children)
         {
            if (child->kind == XmlNode::KindElement)
               count += CountId(*child, value);
         }

         return count;
      }

      std::string Sha256(const std::string &bytes)
      {
         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int length = 0;
         if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1)
            return std::string();
         return std::string((const char *) digest, length);
      }

      // XML-DSig's ECDSA value is the raw r||s, each half the width of the
      // curve's order; OpenSSL verifies the X9.62 DER SEQUENCE of two INTEGERs.
      bool RawEcdsaToDer(const std::string &raw, std::string &der)
      {
         if (raw.empty() || raw.size() % 2 != 0 || raw.size() > 264)
            return false;

         const size_t half = raw.size() / 2;
         ECDSA_SIG *signature = ECDSA_SIG_new();
         if (signature == nullptr)
            return false;

         BIGNUM *r = BN_bin2bn((const unsigned char *) raw.data(), (int) half, nullptr);
         BIGNUM *s = BN_bin2bn((const unsigned char *) raw.data() + half, (int) half, nullptr);

         bool made = false;
         if (r != nullptr && s != nullptr && ECDSA_SIG_set0(signature, r, s) == 1)
         {
            // signature owns r and s now.
            const int length = i2d_ECDSA_SIG(signature, nullptr);
            if (length > 0)
            {
               std::vector<unsigned char> buffer((size_t) length);
               unsigned char *write = buffer.data();
               i2d_ECDSA_SIG(signature, &write);
               der.assign((const char *) buffer.data(), buffer.size());
               made = true;
            }
         }
         else
         {
            BN_free(r);
            BN_free(s);
         }

         ECDSA_SIG_free(signature);
         return made;
      }

      bool VerifyWithKey(EVP_PKEY *key, const std::string &data, const std::string &signature)
      {
         bool verified = false;
         EVP_MD_CTX *context = EVP_MD_CTX_new();
         if (context != nullptr)
         {
            if (EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                EVP_DigestVerifyUpdate(context, data.data(), data.size()) == 1)
            {
               verified = EVP_DigestVerifyFinal(context, (const unsigned char *) signature.data(), signature.size()) == 1;
            }
            EVP_MD_CTX_free(context);
         }

         ERR_clear_error();
         return verified;
      }

      // The public keys of the PEM certificates, in the order written. The
      // caller frees them.
      std::vector<EVP_PKEY *> ReadKeys(const std::string &pem)
      {
         std::vector<EVP_PKEY *> keys;

         BIO *bio = BIO_new_mem_buf(pem.data(), (int) pem.size());
         if (bio == nullptr)
            return keys;

         while (true)
         {
            X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            if (certificate == nullptr)
               break;

            EVP_PKEY *key = X509_get_pubkey(certificate);
            X509_free(certificate);

            // A null keeps the index: certificateIndex names the certificate
            // as the administrator pasted them.
            keys.push_back(key);
         }

         BIO_free(bio);

         // The read that ends the loop leaves "no start line" behind it.
         ERR_clear_error();
         return keys;
      }
   }

   bool
   XmlSignature::Base64Decode(const std::string &text, std::string &bytes)
   {
      bytes.clear();

      unsigned long buffer = 0;
      int bits = 0;
      int padding = 0;
      size_t symbols = 0;

      for (char c : text)
      {
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;

         symbols++;

         if (c == '=')
         {
            padding++;
            continue;
         }

         // Nothing but padding after padding.
         if (padding > 0)
            return false;

         int value = -1;
         if (c >= 'A' && c <= 'Z')
            value = c - 'A';
         else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 26;
         else if (c >= '0' && c <= '9')
            value = c - '0' + 52;
         else if (c == '+')
            value = 62;
         else if (c == '/')
            value = 63;
         else
            return false;

         buffer = (buffer << 6) | (unsigned long) value;
         bits += 6;

         if (bits >= 8)
         {
            bits -= 8;
            bytes += (char) ((buffer >> bits) & 0xFF);
         }
      }

      // Whole quads, at most two padding characters, and the bits after the
      // last byte - two after one padding character, four after two - zero,
      // so that no two spellings mean one value.
      if (symbols % 4 != 0 || padding > 2 || bits != padding * 2)
         return false;

      if (bits > 0 && (buffer & ((1UL << bits) - 1)) != 0)
         return false;

      return true;
   }

   std::string
   XmlSignature::Base64Encode(const std::string &bytes)
   {
      static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve((bytes.size() + 2) / 3 * 4);

      size_t i = 0;
      for (; i + 2 < bytes.size(); i += 3)
      {
         const unsigned long group = ((unsigned long) (unsigned char) bytes[i] << 16) |
                                     ((unsigned long) (unsigned char) bytes[i + 1] << 8) |
                                     (unsigned long) (unsigned char) bytes[i + 2];
         out += alphabet[(group >> 18) & 0x3F];
         out += alphabet[(group >> 12) & 0x3F];
         out += alphabet[(group >> 6) & 0x3F];
         out += alphabet[group & 0x3F];
      }

      const size_t left = bytes.size() - i;
      if (left == 1)
      {
         const unsigned long group = (unsigned long) (unsigned char) bytes[i] << 16;
         out += alphabet[(group >> 18) & 0x3F];
         out += alphabet[(group >> 12) & 0x3F];
         out += "==";
      }
      else if (left == 2)
      {
         const unsigned long group = ((unsigned long) (unsigned char) bytes[i] << 16) |
                                     ((unsigned long) (unsigned char) bytes[i + 1] << 8);
         out += alphabet[(group >> 18) & 0x3F];
         out += alphabet[(group >> 12) & 0x3F];
         out += alphabet[(group >> 6) & 0x3F];
         out += '=';
      }

      return out;
   }

   bool
   XmlSignature::Verify(const XmlNode &element, const char *idAttribute, const std::string &certificatesPem,
                        XmlDocument &verified, int &certificateIndex, std::string &problem)
   {
      certificateIndex = -1;

      const std::vector<const XmlNode *> signatures = element.ChildElements(Ds, "Signature");
      if (signatures.empty())
      {
         problem = "the " + element.local + " is not signed";
         return false;
      }

      if (signatures.size() > 1)
      {
         problem = "the " + element.local + " carries more than one signature";
         return false;
      }

      const XmlNode &signature = *signatures[0];

      // The element the Reference must name, named once in the whole document.
      std::string id;
      if (!element.GetAttribute(idAttribute, id) || id.empty())
      {
         problem = "the signed " + element.local + " has no " + idAttribute + " for its signature to name";
         return false;
      }

      const XmlNode *root = &element;
      while (root->parent != nullptr)
         root = root->parent;

      if (CountId(*root, id) != 1)
      {
         problem = "the " + element.local + "'s ID is written on more than one element of the document, which is the shape of a wrapping attack";
         return false;
      }

      std::vector<const XmlNode *> parts;
      if (!ChildrenAre(signature, { "SignedInfo", "SignatureValue", "KeyInfo?" }, parts))
      {
         problem = "the signature is not SignedInfo, SignatureValue and at most a KeyInfo (an Object, or anything else, is refused)";
         return false;
      }

      const XmlNode &signedInfo = *parts[0];
      const XmlNode &signatureValue = *parts[1];

      std::vector<const XmlNode *> info;
      if (!ChildrenAre(signedInfo, { "CanonicalizationMethod", "SignatureMethod", "Reference" }, info))
      {
         problem = "the signature's SignedInfo is not one canonicalisation method, one signature method and exactly one Reference";
         return false;
      }

      const std::string canonicalization = Algorithm(*info[0]);
      std::vector<std::string> signedInfoPrefixes;
      if (canonicalization != ExclusiveCanonicalizer::Algorithm())
      {
         problem = CanonicalizationRefusal(canonicalization);
         return false;
      }

      if (!InclusivePrefixes(*info[0], "canonicalisation method", signedInfoPrefixes, problem))
         return false;

      const std::string method = Algorithm(*info[1]);
      const bool ecdsa = method == EcdsaSha256;
      if (method != RsaSha256 && !ecdsa)
      {
         problem = SignatureMethodRefusal(method);
         return false;
      }

      if (!info[1]->ElementChildren().empty() || HoldsText(*info[1]))
      {
         problem = "the signature method carries parameters, which are refused";
         return false;
      }

      const XmlNode &reference = *info[2];
      std::string uri;
      if (!reference.GetAttribute("URI", uri) || uri != "#" + id)
      {
         problem = uri.empty()
            ? "the signature's Reference names no element (an empty URI is the whole document), which is refused"
            : "the signature's Reference names another element than the " + element.local + " it is in";
         return false;
      }

      std::vector<const XmlNode *> referenceParts;
      if (!ChildrenAre(reference, { "Transforms", "DigestMethod", "DigestValue" }, referenceParts))
      {
         problem = "the signature's Reference is not Transforms, DigestMethod and DigestValue";
         return false;
      }

      const std::vector<const XmlNode *> transforms = referenceParts[0]->ElementChildren();
      for (const XmlNode *transform : transforms)
      {
         if (!transform->Is(Ds, "Transform"))
         {
            problem = "the signature's Transforms holds something other than a Transform";
            return false;
         }

         const std::string algorithm = Algorithm(*transform);
         if (algorithm != EnvelopedSignature && algorithm != ExclusiveCanonicalizer::Algorithm())
         {
            problem = TransformRefusal(algorithm);
            return false;
         }
      }

      if (HoldsText(*referenceParts[0]) || transforms.size() != 2 ||
          Algorithm(*transforms[0]) != EnvelopedSignature || Algorithm(*transforms[1]) != ExclusiveCanonicalizer::Algorithm() ||
          !transforms[0]->ElementChildren().empty() || HoldsText(*transforms[0]))
      {
         problem = "the signature's transforms are not exactly enveloped-signature and then exclusive canonicalisation";
         return false;
      }

      std::vector<std::string> referencePrefixes;
      if (!InclusivePrefixes(*transforms[1], "canonicalisation transform", referencePrefixes, problem))
         return false;

      const std::string digestMethod = Algorithm(*referenceParts[1]);
      if (digestMethod != DigestSha256)
      {
         problem = DigestRefusal(digestMethod);
         return false;
      }

      if (!referenceParts[1]->ElementChildren().empty() || HoldsText(*referenceParts[1]))
      {
         problem = "the signature's digest method carries parameters, which are refused";
         return false;
      }

      std::string expectedDigest;
      if (!Base64Decode(referenceParts[2]->TextContent(), expectedDigest) || expectedDigest.size() != 32 ||
          !referenceParts[2]->ElementChildren().empty())
      {
         problem = "the signature's DigestValue is not the base64 of a SHA-256 digest";
         return false;
      }

      std::string signatureBytes;
      if (!Base64Decode(signatureValue.TextContent(), signatureBytes) || signatureBytes.empty() ||
          !signatureValue.ElementChildren().empty())
      {
         problem = "the signature's SignatureValue is not base64";
         return false;
      }

      // The signature first: over SignedInfo as its own canonicalisation writes
      // it, with the certificates the administrator stored and nothing else.
      // SignedInfo is small - its shape was held to three children above, and
      // each of theirs - so what a stranger can make this server do before a
      // key has spoken is bounded by that; the element the Reference names,
      // which can be most of the document, is canonicalised only for a
      // signature that verified.
      const std::string canonicalSignedInfo = ExclusiveCanonicalizer::Canonicalize(signedInfo, signedInfoPrefixes, nullptr);

      std::string ecdsaDer;
      if (ecdsa && !RawEcdsaToDer(signatureBytes, ecdsaDer))
      {
         problem = "the signature's ECDSA value is not the r and s halves XML-DSig writes";
         return false;
      }

      std::vector<EVP_PKEY *> keys = ReadKeys(certificatesPem);
      if (keys.empty())
      {
         problem = "no certificate of the identity provider's is stored to verify the signature with";
         return false;
      }

      bool anyUsable = false;
      for (size_t i = 0; i < keys.size() && certificateIndex < 0; i++)
      {
         EVP_PKEY *key = keys[i];
         if (key == nullptr)
            continue;

         const int type = EVP_PKEY_get_base_id(key);
         if (ecdsa ? type != EVP_PKEY_EC : (type != EVP_PKEY_RSA || EVP_PKEY_get_bits(key) < MinimumRsaBits))
            continue;

         anyUsable = true;
         if (VerifyWithKey(key, canonicalSignedInfo, ecdsa ? ecdsaDer : signatureBytes))
            certificateIndex = (int) i;
      }

      for (EVP_PKEY *key : keys)
      {
         if (key != nullptr)
            EVP_PKEY_free(key);
      }

      if (certificateIndex < 0)
      {
         problem = anyUsable
            ? "the " + element.local + "'s signature does not verify against the identity provider's certificate"
            : std::string(ecdsa ? "the signature is ECDSA and no stored certificate holds an elliptic-curve key"
                                : "the signature is RSA and no stored certificate holds an RSA key of 2048 bits or more");
         return false;
      }

      // Then the digest: the element as exc-c14n writes it, the signature left
      // out - the bytes the signature, through SignedInfo's DigestValue, speaks
      // for.
      const std::string canonical = ExclusiveCanonicalizer::Canonicalize(element, referencePrefixes, &signature);
      const std::string digest = Sha256(canonical);
      if (digest.size() != expectedDigest.size() ||
          CRYPTO_memcmp(digest.data(), expectedDigest.data(), digest.size()) != 0)
      {
         certificateIndex = -1;
         problem = "the signed " + element.local + " was changed after it was signed: its digest does not match";
         return false;
      }

      // What the caller uses from here on: the bytes the digest covered, read
      // again, and not the element it passed in.
      std::string again;
      if (!verified.Parse(canonical, again) || verified.Root() == nullptr ||
          verified.Root()->local != element.local || verified.Root()->uri != element.uri)
      {
         problem = "the signed " + element.local + " could not be read again from the bytes its signature covers";
         return false;
      }

      return true;
   }

   // ------------------------------------------------------------ the tests

   namespace
   {
      void Require(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      // The vectors, made on 24 September 2026 with .NET Framework 4.8's
      // SignedXml (RSA 2048, enveloped-signature and exc-c14n, SHA-256, the
      // signature inserted after the Issuer as SAML's schema puts it) and, for
      // the ECDSA one, the same document with the method changed and a P-256
      // signature over the SignedInfo .NET's XmlDsigExcC14NTransform wrote.
      // .NET's own CheckSignature took the RSA one. No KeyInfo and no white
      // space between elements, so that no byte of either is one a change to
      // could leave the meaning alone - which is what the mutation test needs.
      // The certificates are self-signed and hold public keys only.
      const char *RsaSigned =
         "<saml:Assertion xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\" ID=\"_vector1\" IssueInstant=\"2026-"
         "09-24T12:00:00Z\" Version=\"2.0\"><saml:Issuer>https://idp.example.test/saml</saml:Issuer><Signature xm"
         "lns=\"http://www.w3.org/2000/09/xmldsig#\"><SignedInfo><CanonicalizationMethod Algorithm=\"http://www.w"
         "3.org/2001/10/xml-exc-c14n#\" /><SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rs"
         "a-sha256\" /><Reference URI=\"#_vector1\"><Transforms><Transform Algorithm=\"http://www.w3.org/2000/09/x"
         "mldsig#enveloped-signature\" /><Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" /></Tra"
         "nsforms><DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\" /><DigestValue>cKuVAHrPIvJ"
         "JXPPtb2SzZymIlcBEIe+YU6sr/didtXs=</DigestValue></Reference></SignedInfo><SignatureValue>UqMEmx16WSP7"
         "oXRxu+Jd9S4XUrx09SqAFhc8qxBLwjFN3DOGUkLcZHxPYZW2IHei10AJzpud/JXzAKwFO2tRhZvFueAeKgvHr/vD55p525jv3EHe"
         "qq5SienSDHxwouc9fDI0QNDsbYHiHfy0/q/k+TFcGf4kKreLS3Dkjrs7W3V3KdwW7wRj7IlIf0RGWwyX6gXzD8iZnFlnBgvdY61r"
         "j4Fr+NFmx7RG2XT62le2SissF3wMzMTD21fAsnOMmO7bd+d1wz0SLsqESarLrbwddKpjqqRA9Fu8ueKA261tKV38SnsTbBFCfg4Z"
         "FPgWAaqefgPUdzt+gOe9jkUZ8kbnGQ==</SignatureValue></Signature><saml:Subject><saml:NameID Format=\"urn:"
         "oasis:names:tc:SAML:1.1:nameid-format:emailAddress\">user@example.test</saml:NameID></saml:Subject></"
         "saml:Assertion>";

      const char *RsaCertificate =
         "-----BEGIN CERTIFICATE-----\n"
         "MIICqjCCAZKgAwIBAgIIHwQJfF3EpbwwDQYJKoZIhvcNAQELBQAwFTETMBEGA1UE\n"
         "AxMKVmVjdG9yIElkUDAeFw0yNjAxMDEwMDAwMDBaFw00NjAxMDEwMDAwMDBaMBUx\n"
         "EzARBgNVBAMTClZlY3RvciBJZFAwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n"
         "AoIBAQCmggYA1KsWQgiMSFKjE9SZWSF7xg/IZE6RI98uYT0H+AvA/qY56CtwQhmu\n"
         "7kGjViy61WebB1z7bq94MHIhyr9+rvpiemD5Rx0w+Hp6wrpuER3g+NE1gC0ry9NG\n"
         "m1BONRjo2e2k7JcjOKqLThytj3NB0w6k3cEzskTFm+wjAPowp3sgEcxOkYFCDeYl\n"
         "Y65/6365TOkAwmGIfoQ1htsB8PCb2XoHY8ACwyNgaGfsYN4GmWG37EzA7vUfCqRq\n"
         "PmTZqNTwMv4GFQqnYixyavC7drpxJDAY7drTBkbtCyO1m7AuKjj+1ODjaZFUkh4+\n"
         "T9Vya//LpSKVkGQok69lU9vzpuV5AgMBAAEwDQYJKoZIhvcNAQELBQADggEBABc3\n"
         "ElminjxE9arDIv4yTPl2JVR1BJenpj3kJ62DLWYhDa8x5HE3gQr7zkNzG9zu8gbg\n"
         "JTfXC20lob+1vyKpnFM3LVSjCftQ3FqFbViEZfwPPHYBbYxx9vRt6Zpi3bH3tOtu\n"
         "azs6wVLG7qNbEPOW5Dk5Z+r7TkBFR1Cjri5CsC/20GJNcz5EkzsRUEqHSrTpBElK\n"
         "EAn+M8q6Pscad2O0hxaHA4zX7c6N+xU6uS2Q6pIZp3XTTGSs40inLiHZbntGlDCS\n"
         "vR26jom216ozTlTtS89b52SROp4PQ8vRGrGw1nVOZfn9yI/n43BsZlvjmXPOsoT5\n"
         "qyF2JYO3c3zrbKkFFAQ=\n"
         "-----END CERTIFICATE-----\n";

      const char *OtherCertificate =
         "-----BEGIN CERTIFICATE-----\n"
         "MIICrTCCAZWgAwIBAgIJAPYQ+wrdD/9OMA0GCSqGSIb3DQEBCwUAMBYxFDASBgNV\n"
         "BAMTC0Fub3RoZXIgSWRQMB4XDTI2MDEwMTAwMDAwMFoXDTQ2MDEwMTAwMDAwMFow\n"
         "FjEUMBIGA1UEAxMLQW5vdGhlciBJZFAwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAw\n"
         "ggEKAoIBAQC2RJOpC0/thQLWcjaslpEY856jg6gYuLcNLimSpN8rQonEc7xxo0ox\n"
         "BBQVQXiEdJXZ7xZpycD22UghmYOOzo75Wigp5Y/i+XSt7y3Y6xyHCEPsLipoTEkj\n"
         "WsQfpeYqs+dDHzNXz/NoQZ+UwEUmCxNoF6UoUbj0pMaCUH7wgbg3vjWti3583QE6\n"
         "sAOES+8m0WBCffItVB0gikrowSGz9vlRNKaRQP8y/GD+wTQs88ja8MkOGkUT+gWK\n"
         "GCu9CMNKGm5q5zkhyRDhx5efJiafyJ9eewrIsGKlhTexY8q8OifU3dG1q2lLm/og\n"
         "wY+MFvatau9F/A9fclp9keypIxZqs6hZAgMBAAEwDQYJKoZIhvcNAQELBQADggEB\n"
         "AFzFTezP9EsN+HpPIZdNIYPu5rKnk63z+RNbDsfhvuPyw28GV9gsoigF3NQYmNR2\n"
         "of4+uN7NCU78uPd4fs5pFKr2MlHXeXxuJOXjcOiVcWbwOzuPYnd1Mjd79T1HnK2n\n"
         "/DjN8E4CoyUKDOEVM0If0LW5P+AH9wxfzenXCCf1rPwRhHTd3FUaa1Bt1UDvWYnp\n"
         "MAedVlqmSaq6T+oOuOGhSEcGNYtFI/kT8W17vDHFNBgNfAGGMTpLm9O65k4SavYA\n"
         "2gZTjebLdvRXpD+0bB36XXzLTJjROnG5BthVPlqoIMj88As1cPxwg6J6wIpUUy7G\n"
         "PoqZnIf7m23+R9r+RM8NoNI=\n"
         "-----END CERTIFICATE-----\n";

      const char *EcSigned =
         "<saml:Assertion xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\" ID=\"_vector1\" IssueInstant=\"2026-"
         "09-24T12:00:00Z\" Version=\"2.0\"><saml:Issuer>https://idp.example.test/saml</saml:Issuer><Signature xm"
         "lns=\"http://www.w3.org/2000/09/xmldsig#\"><SignedInfo><CanonicalizationMethod Algorithm=\"http://www.w"
         "3.org/2001/10/xml-exc-c14n#\" /><SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#ec"
         "dsa-sha256\" /><Reference URI=\"#_vector1\"><Transforms><Transform Algorithm=\"http://www.w3.org/2000/09"
         "/xmldsig#enveloped-signature\" /><Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" /></T"
         "ransforms><DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\" /><DigestValue>cKuVAHrPI"
         "vJJXPPtb2SzZymIlcBEIe+YU6sr/didtXs=</DigestValue></Reference></SignedInfo><SignatureValue>ts19ROOKem"
         "K0AuIuhKKoLbjmkjtxlpkTTPXby5RtyOGts+Bp0lWebr6fNPhIygUpZJBycFx60lQ42Wsad8Nydw==</SignatureValue></Sig"
         "nature><saml:Subject><saml:NameID Format=\"urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress\">us"
         "er@example.test</saml:NameID></saml:Subject></saml:Assertion>";

      const char *EcCertificate =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBJTCBy6ADAgECAgkAwsTfeUElnt4wCgYIKoZIzj0EAwIwGDEWMBQGA1UEAxMN\n"
         "VmVjdG9yIEVDIElkUDAeFw0yNjAxMDEwMDAwMDBaFw00NjAxMDEwMDAwMDBaMBgx\n"
         "FjAUBgNVBAMTDVZlY3RvciBFQyBJZFAwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNC\n"
         "AAQNr16GtivVIDCAtbzd+tKZIv9jIhvRlTyIAyNWxiyungXdKKrAQDMx2vu/HPxx\n"
         "xQljyE0dxAQBp0O+C28msgnaMAoGCCqGSM49BAMCA0kAMEYCIQDNIF6GCy+btYr0\n"
         "XvnlJIBjxOPxY83V+YUUijoUJgH8SgIhAKnRyGN4CP52XzOmWh2e0HBXDStABb35\n"
         "nPKeV4HLdi44\n"
         "-----END CERTIFICATE-----\n";

      // The same assertion signed by SignedXml with an RSA key of 1024 bits: a
      // signature that verifies mathematically, and that the verifier must not
      // take from a key that size.
      const char *WeakSigned =
         "<saml:Assertion xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\" ID=\"_vector1\" IssueInstant=\"2026-"
         "09-24T12:00:00Z\" Version=\"2.0\"><saml:Issuer>https://idp.example.test/saml</saml:Issuer><Signature xm"
         "lns=\"http://www.w3.org/2000/09/xmldsig#\"><SignedInfo><CanonicalizationMethod Algorithm=\"http://www.w"
         "3.org/2001/10/xml-exc-c14n#\" /><SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rs"
         "a-sha256\" /><Reference URI=\"#_vector1\"><Transforms><Transform Algorithm=\"http://www.w3.org/2000/09/x"
         "mldsig#enveloped-signature\" /><Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" /></Tra"
         "nsforms><DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\" /><DigestValue>cKuVAHrPIvJ"
         "JXPPtb2SzZymIlcBEIe+YU6sr/didtXs=</DigestValue></Reference></SignedInfo><SignatureValue>JO2JVIgyodtn"
         "1T/0fI77usQSnbu0Eo7SoAMQooUD2JnhKHCGvDtEAqnf5xUz9Ru0jFHwZr3xtBBlPmIGoq2ljzJiNRqSEAhod/RXOLk1o+LHEaLr"
         "fKlqeLMklET8gd4Ri+5rJSKM1CAZf+DDl/1J2elxrLRxx3AGOXABrqGJ/7A=</SignatureValue></Signature><saml:Subje"
         "ct><saml:NameID Format=\"urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress\">user@example.test</s"
         "aml:NameID></saml:Subject></saml:Assertion>";

      const char *WeakCertificate =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBoTCCAQqgAwIBAgIIL5V4y55Iu3swDQYJKoZIhvcNAQELBQAwEzERMA8GA1UE\n"
         "AxMIV2VhayBJZFAwHhcNMjYwMTAxMDAwMDAwWhcNNDYwMTAxMDAwMDAwWjATMREw\n"
         "DwYDVQQDEwhXZWFrIElkUDCBnzANBgkqhkiG9w0BAQEFAAOBjQAwgYkCgYEA23vN\n"
         "5uTVIfXuRsB+u45HtK/11Q256321Go4pXoKISE1iglIyMWYR9RyRGnopqy9OmBNI\n"
         "oJj6cNusulhea5Mc4MQLO7h0pg5LQYqeph1eb6Cop0+Co5RPfg7nh6OJ17YyuOwN\n"
         "WxeMQ10bAzhbs0LdSr829ECedvulVXoEeDzZo5kCAwEAATANBgkqhkiG9w0BAQsF\n"
         "AAOBgQAE+WOJlC7Xdq9+zo4NRzmmTS7/0scdJ7FN9MalSA0aWPGUPGsmfRNh1tya\n"
         "2CTNvIfMaS1Rbyrdo6yqiuDMuVIhZrWoEmfBziwwIgh6gmU4lEk2AhR5igpt5f4B\n"
         "ZA17badQdvdl5ICqlU4UkKVYjcVgsNh660+QyGLblUPinEiUDQ==\n"
         "-----END CERTIFICATE-----\n";

      std::string Replaced(const std::string &text, const std::string &from, const std::string &to)
      {
         const size_t at = text.find(from);
         Require(at != std::string::npos, "The tester's vector does not hold " + from.substr(0, 60));
         return text.substr(0, at) + to + text.substr(at + from.size());
      }

      bool Verifies(const std::string &text, const std::string &certificates, std::string &problem)
      {
         XmlDocument document;
         if (!document.Parse(text, problem))
            return false;

         XmlDocument verified;
         int index = -1;
         return XmlSignature::Verify(*document.Root(), "ID", certificates, verified, index, problem);
      }

      void RefusedWith(const char *what, const std::string &text, const std::string &certificates, const char *sentence)
      {
         std::string problem;
         Require(!Verifies(text, certificates, problem), std::string("A signature verified with ") + what + ".");
         Require(problem.find(sentence) != std::string::npos,
                 std::string("A signature with ") + what + " was refused with another sentence: " + problem);
      }

      // Every single-byte change to the signed document, refused.
      void EveryMutationRefused(const char *vector, const std::string &text, const std::string &certificates)
      {
         for (size_t i = 0; i < text.size(); i++)
         {
            std::string mutated = text;
            mutated[i] = (char) (mutated[i] ^ 0x01);

            std::string problem;
            if (Verifies(mutated, certificates, problem))
            {
               throw std::logic_error(std::string(vector) + ": a change to byte " + std::to_string(i) + " ('" + text.substr(i, 1) +
                                      "' to '" + mutated.substr(i, 1) + "') still verified");
            }
         }
      }
   }

   void
   XmlSignatureTester::Test()
   {
      // ---- base64, strictly
      {
         std::string bytes;
         Require(XmlSignature::Base64Decode("Zm9v", bytes) && bytes == "foo", "base64 of foo was not read.");
         Require(XmlSignature::Base64Decode("Zm8=", bytes) && bytes == "fo", "base64 with one padding character was not read.");
         Require(XmlSignature::Base64Decode("Zg==", bytes) && bytes == "f", "base64 with two padding characters was not read.");
         Require(XmlSignature::Base64Decode(" Zm9v\r\n Zg== ", bytes) && bytes == "foof", "base64 with white space in it was not read.");
         Require(!XmlSignature::Base64Decode("Zh==", bytes), "base64 whose bits after the last byte are not zero was read.");
         Require(!XmlSignature::Base64Decode("Zm9", bytes), "base64 that is not whole quads was read.");
         Require(!XmlSignature::Base64Decode("Zg==Zg==", bytes), "base64 with data after its padding was read.");
         Require(!XmlSignature::Base64Decode("Zm9-", bytes), "base64url's alphabet was read as base64.");
         Require(XmlSignature::Base64Encode("foo") == "Zm9v" && XmlSignature::Base64Encode("fo") == "Zm8=" &&
                 XmlSignature::Base64Encode("f") == "Zg==" && XmlSignature::Base64Encode("") == "",
                 "base64 was written wrongly.");
      }

      const std::string rsa = RsaSigned;
      const std::string ec = EcSigned;

      // ---- the fixed vectors verify, and what the caller is handed is what was signed
      {
         std::string problem;
         XmlDocument document;
         Require(document.Parse(rsa, problem), "The RSA vector was not read: " + problem);

         XmlDocument verified;
         int index = -1;
         Require(XmlSignature::Verify(*document.Root(), "ID", RsaCertificate, verified, index, problem),
                 "The RSA vector .NET's SignedXml signed did not verify: " + problem);
         Require(index == 0, "The RSA vector verified against the wrong certificate.");
         Require(verified.Root() != nullptr && verified.Root()->local == "Assertion" &&
                 verified.Root()->ChildElement("http://www.w3.org/2000/09/xmldsig#", "Signature") == nullptr,
                 "What the verified RSA vector handed back is not the assertion without its signature.");

         const XmlNode *subject = verified.Root()->ChildElement("urn:oasis:names:tc:SAML:2.0:assertion", "Subject");
         const XmlNode *nameId = subject == nullptr ? nullptr : subject->ChildElement("urn:oasis:names:tc:SAML:2.0:assertion", "NameID");
         Require(nameId != nullptr && nameId->TextContent() == "user@example.test", "The verified RSA vector's NameID was not read.");

         // Rotation: the new certificate stored beside the old one.
         Require(XmlSignature::Verify(*document.Root(), "ID", std::string(OtherCertificate) + RsaCertificate, verified, index, problem) && index == 1,
                 "The RSA vector did not verify against the second of two stored certificates.");

         Require(Verifies(ec, EcCertificate, problem), "The ECDSA P-256 vector did not verify: " + problem);
      }

      // ---- against another key, nothing verifies
      RefusedWith("another certificate", rsa, OtherCertificate, "does not verify against");
      RefusedWith("an RSA certificate for an ECDSA signature", ec, RsaCertificate, "no stored certificate holds an elliptic-curve key");
      RefusedWith("no certificate", rsa, "", "no certificate");

      // ---- the key the message carries is ignored: a KeyInfo naming another
      // certificate changes nothing, either way
      {
         std::string other = OtherCertificate;
         other = Replaced(other, "-----BEGIN CERTIFICATE-----\n", "");
         other = Replaced(other, "-----END CERTIFICATE-----\n", "");
         const std::string carried = Replaced(rsa, "</SignatureValue>",
            "</SignatureValue><KeyInfo><X509Data><X509Certificate>" + other + "</X509Certificate></X509Data></KeyInfo>");

         std::string problem;
         Require(Verifies(carried, RsaCertificate, problem), "A KeyInfo stopped a good signature verifying: " + problem);
         RefusedWith("a KeyInfo carrying the certificate that is not stored", carried, OtherCertificate, "does not verify against");
      }

      // ---- every single-byte change refused
      EveryMutationRefused("The RSA vector", rsa, RsaCertificate);
      EveryMutationRefused("The ECDSA vector", ec, EcCertificate);

      // ---- each refusal, by name
      RefusedWith("no signature", Replaced(rsa, rsa.substr(rsa.find("<Signature "), rsa.find("</Signature>") + 12 - rsa.find("<Signature ")), ""),
                  RsaCertificate, "is not signed");
      {
         const std::string whole = rsa.substr(rsa.find("<Signature "), rsa.find("</Signature>") + 12 - rsa.find("<Signature "));
         RefusedWith("two signatures", Replaced(rsa, whole, whole + whole), RsaCertificate, "more than one signature");
      }
      RefusedWith("a SHA-1 digest", Replaced(rsa, "http://www.w3.org/2001/04/xmlenc#sha256", "http://www.w3.org/2000/09/xmldsig#sha1"),
                  RsaCertificate, "SHA-1");
      RefusedWith("an rsa-sha1 signature", Replaced(rsa, RsaSha256, "http://www.w3.org/2000/09/xmldsig#rsa-sha1"),
                  RsaCertificate, "SHA-1");
      RefusedWith("an HMAC signature", Replaced(rsa, RsaSha256, "http://www.w3.org/2000/09/xmldsig#hmac-sha1"),
                  RsaCertificate, "HMAC");
      RefusedWith("an XSLT transform",
                  Replaced(rsa, "<Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\" />",
                           "<Transform Algorithm=\"http://www.w3.org/TR/1999/REC-xslt-19991116\" />"),
                  RsaCertificate, "XSLT");
      RefusedWith("a third transform",
                  Replaced(rsa, "</Transforms>", "<Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" /></Transforms>"),
                  RsaCertificate, "not exactly enveloped-signature");
      RefusedWith("a Reference with an empty URI", Replaced(rsa, "URI=\"#_vector1\"", "URI=\"\""), RsaCertificate, "empty URI");
      RefusedWith("a Reference to another element", Replaced(rsa, "URI=\"#_vector1\"", "URI=\"#_other\""), RsaCertificate,
                  "names another element");
      RefusedWith("canonicalisation with comments",
                  Replaced(rsa, "<CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"",
                           "<CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#WithComments\""),
                  RsaCertificate, "with comments");
      RefusedWith("inclusive canonicalisation",
                  Replaced(rsa, "<CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"",
                           "<CanonicalizationMethod Algorithm=\"http://www.w3.org/TR/2001/REC-xml-c14n-20010315\""),
                  RsaCertificate, "not exclusive XML canonicalisation");
      RefusedWith("an Object in the signature", Replaced(rsa, "</SignatureValue>", "</SignatureValue><Object>x</Object>"),
                  RsaCertificate, "an Object");
      RefusedWith("the ID written twice", Replaced(rsa, "<saml:Subject>", "<saml:Subject ID=\"_vector1\">"), RsaCertificate,
                  "wrapping attack");
      RefusedWith("a changed NameID", Replaced(rsa, "user@example.test", "user@example.tesu"), RsaCertificate, "digest does not match");

      // An ECDSA value that is good base64 of an odd number of bytes - 63 -
      // reaches the transcode, which cannot split it into r and s. (The digest
      // leaves the signature out, so it is not what refuses it.)
      {
         const size_t open = ec.find("<SignatureValue>") + 16;
         const size_t close = ec.find("</SignatureValue>");
         const std::string odd = ec.substr(0, open) + XmlSignature::Base64Encode(std::string(63, '\x01')) + ec.substr(close);
         RefusedWith("an ECDSA value that is not r and s", odd, EcCertificate, "r and s halves");
      }

      // An RSA key under 2048 bits is not used, though the signature it made is good.
      RefusedWith("an RSA key of 1024 bits", WeakSigned, WeakCertificate, "no stored certificate holds an RSA key of 2048 bits or more");

      // The other transforms the refusal names.
      RefusedWith("an XPath transform",
                  Replaced(rsa, "<Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\" />",
                           "<Transform Algorithm=\"http://www.w3.org/TR/1999/REC-xpath-19991116\" />"),
                  RsaCertificate, "(XPath)");
      RefusedWith("a base64 transform",
                  Replaced(rsa, "<Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\" />",
                           "<Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#base64\" />"),
                  RsaCertificate, "(base64)");

      // The canonicaliser's bounds, before any key is read: a PrefixList of
      // more than 32, and an InclusiveNamespaces with anything inside it.
      {
         std::string many = "p0";
         for (int i = 1; i <= ExclusiveCanonicalizer::MaximumPrefixes; i++)
            many += " p" + std::to_string(i);
         RefusedWith("a PrefixList of 33 prefixes",
                     Replaced(rsa, "<CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" />",
                              "<CanonicalizationMethod Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"><InclusiveNamespaces "
                              "xmlns=\"http://www.w3.org/2001/10/xml-exc-c14n#\" PrefixList=\"" + many + "\" /></CanonicalizationMethod>"),
                     RsaCertificate, "more than 32 prefixes");
         RefusedWith("an InclusiveNamespaces with elements inside it",
                     Replaced(rsa, "<Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\" />",
                              "<Transform Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"><InclusiveNamespaces "
                              "xmlns=\"http://www.w3.org/2001/10/xml-exc-c14n#\" PrefixList=\"saml\"><x/><x/></InclusiveNamespaces></Transform>"),
                     RsaCertificate, "not empty");
      }
   }
}
