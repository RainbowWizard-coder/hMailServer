// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "GatewayEncryption.h"
#include "SealedMessage.h"
#include "OpenPgpKey.h"
#include "OpenPgpWriter.h"
#include "File.h"
#include "FileUtilities.h"
#include "ByteBuffer.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "Parsing/StringParser.h"
#include "../Mime/Mime.h"

#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <new>
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
      AnsiString Utf8Of(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String WideOf(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Hex(const unsigned char *data, size_t length)
      {
         static const char digits[] = "0123456789abcdef";
         AnsiString out;
         for (size_t i = 0; i < length; i++)
         {
            out += digits[data[i] >> 4];
            out += digits[data[i] & 0x0F];
         }
         return out;
      }

      // Lets go of a string's memory, not only of its length: a message of
      // tens of megabytes is dropped as soon as the next form of it is made.
      void ReleaseText(AnsiString &text)
      {
         std::string().swap(text);
      }

      // Appends text to parts without copying it: AnsiString has no move
      // constructor, and the text is the size of the message.
      void MoveInto(std::vector<AnsiString> &parts, AnsiString &text)
      {
         parts.push_back(AnsiString());
         static_cast<std::string &>(parts.back()).swap(text);
      }

      // Where the header block ends: the length of the header including the line
      // end of its last field, and where the body starts. A message with no blank
      // line is all header.
      size_t HeaderEnd(const AnsiString &message, size_t &bodyStart)
      {
         const char *text = message.c_str();
         const size_t length = (size_t) message.GetLength();
         for (size_t i = 0; i < length; i++)
         {
            if (text[i] != '\n')
               continue;
            if (i + 2 < length && text[i + 1] == '\r' && text[i + 2] == '\n')
            {
               bodyStart = i + 3;
               return i + 1;
            }
            if (i + 1 < length && text[i + 1] == '\n')
            {
               bodyStart = i + 2;
               return i + 1;
            }
         }
         bodyStart = length;
         return length;
      }

      // The first field of the given lower-case name in the header block,
      // unfolded and in lower case, or empty. A field at the start of a line
      // only, so that a name that merely ends in it (X-Original-Content-Type)
      // is not read as the message's own.
      AnsiString TopLevelField(const AnsiString &message, const char *loweredName)
      {
         size_t bodyStart = 0;
         const size_t headerLength = HeaderEnd(message, bodyStart);
         AnsiString header = message.Mid(0, (int) headerLength);
         header.MakeLower();
         header.Replace("\r\n ", " ");
         header.Replace("\r\n\t", " ");
         header.Replace("\n ", " ");
         header.Replace("\n\t", " ");

         const AnsiString name = AnsiString(loweredName) + ":";
         int at = 0;
         if (!header.StartsWith(name.c_str()))
         {
            const AnsiString atLineStart = "\n" + name;
            at = header.Find(atLineStart.c_str());
            if (at < 0)
               return AnsiString();
            at += 1;
         }
         const int lineEnd = header.Find("\n", (size_t) at);
         return lineEnd < 0 ? header.Mid(at) : header.Mid(at, lineEnd - at);
      }

      bool IsBase64Character(char c)
      {
         return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '+' || c == '/' || c == '=';
      }

      // The first octets of base64 text starting at 'from', white space
      // skipped, up to 'wanted' octets; stops at the first character that is
      // neither base64 nor white space, and at a line that begins with '=' -
      // the checksum line of an OpenPGP armour, which is not data.
      void DecodeBase64Prefix(const AnsiString &text, size_t from, size_t wanted, std::vector<unsigned char> &out)
      {
         out.clear();

         const char *data = text.c_str();
         const size_t length = (size_t) text.GetLength();
         const size_t needed = (wanted + 2) / 3 * 4;

         std::string characters;
         for (size_t i = from; i < length && characters.size() < needed; i++)
         {
            const char c = data[i];
            if (c == '=' && i > from && data[i - 1] == '\n')
               break;
            if (IsBase64Character(c))
               characters += c;
            else if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
               continue;
            else
               break;
         }

         characters.resize(characters.size() - characters.size() % 4);
         if (characters.empty())
            return;

         const AnsiString decoded = Base64::Decode(characters.c_str(), (int) characters.size());
         out.assign(decoded.c_str(), decoded.c_str() + decoded.GetLength());
      }

      // The CMS content types that are under a key (RFC 5652 section 6, RFC
      // 5083): id-envelopedData 1.2.840.113549.1.7.3 and id-ct-authEnvelopedData
      // 1.2.840.113549.1.9.16.1.23, as their OIDs are encoded.
      const unsigned char OidEnvelopedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x03 };
      const unsigned char OidAuthEnvelopedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x10, 0x01, 0x17 };

      // id-signedData 1.2.840.113549.1.7.2: opaque S/MIME signed-data, whose
      // content is in the clear but inside CMS. Not under a key, so never
      // "already encrypted" here; SealedMessage asks for it separately.
      const unsigned char OidSignedData[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };

      // Where the content type OID of a ContentInfo (SEQUENCE { contentType
      // OID, [0] content }) starts in DER octets, and its length; false when
      // the octets do not begin one. The length of the SEQUENCE may be short,
      // long or indefinite (BER, which some clients write).
      bool ContentInfoOid(const std::vector<unsigned char> &der, size_t &oidAt, size_t &oidLength)
      {
         if (der.size() < 4 || der[0] != 0x30)
            return false;

         size_t at = 1;
         const unsigned char first = der[at++];
         if (first & 0x80)
         {
            const size_t count = first & 0x7F;
            if (count > 4)
               return false;
            at += count;
         }

         if (at + 2 > der.size() || der[at] != 0x06)
            return false;

         oidLength = der[at + 1];
         at += 2;
         if (at + oidLength > der.size())
            return false;

         oidAt = at;
         return true;
      }

      // Whether DER octets begin a ContentInfo whose content type is one of
      // the two above.
      bool IsEnvelopedContentInfo(const std::vector<unsigned char> &der)
      {
         size_t at = 0;
         size_t oidLength = 0;
         if (!ContentInfoOid(der, at, oidLength))
            return false;

         return (oidLength == sizeof(OidEnvelopedData) && memcmp(&der[at], OidEnvelopedData, oidLength) == 0) ||
                (oidLength == sizeof(OidAuthEnvelopedData) && memcmp(&der[at], OidAuthEnvelopedData, oidLength) == 0);
      }

      bool IsSignedContentInfo(const std::vector<unsigned char> &der)
      {
         size_t at = 0;
         size_t oidLength = 0;
         if (!ContentInfoOid(der, at, oidLength))
            return false;

         return oidLength == sizeof(OidSignedData) && memcmp(&der[at], OidSignedData, oidLength) == 0;
      }

      // An OpenPGP packet header (RFC 9580 section 4.2) at 'at': the tag, the
      // header's length, the body's length, and whether the body is framed in
      // partial lengths (or, old format, runs to the end). False when the
      // header runs past the octets there are.
      bool ReadPacketHeader(const std::vector<unsigned char> &data, size_t at, int &tag, size_t &headerLength, size_t &bodyLength, bool &partial)
      {
         partial = false;
         if (at >= data.size() || (data[at] & 0x80) == 0)
            return false;

         const unsigned char first = data[at];
         if (first & 0x40)
         {
            tag = first & 0x3F;
            if (at + 2 > data.size())
               return false;
            const unsigned char o1 = data[at + 1];
            if (o1 < 192)
            {
               headerLength = 2;
               bodyLength = o1;
            }
            else if (o1 < 224)
            {
               if (at + 3 > data.size())
                  return false;
               headerLength = 3;
               bodyLength = (((size_t) o1 - 192) << 8) + (size_t) data[at + 2] + 192;
            }
            else if (o1 == 255)
            {
               if (at + 6 > data.size())
                  return false;
               headerLength = 6;
               bodyLength = ((size_t) data[at + 2] << 24) | ((size_t) data[at + 3] << 16) | ((size_t) data[at + 4] << 8) | (size_t) data[at + 5];
            }
            else
            {
               headerLength = 2;
               bodyLength = (size_t) 1 << (o1 & 0x1F);
               partial = true;
            }
            return true;
         }

         tag = (first >> 2) & 0x0F;
         const int lengthType = first & 0x03;
         if (lengthType == 3)
         {
            headerLength = 1;
            bodyLength = 0;
            partial = true;
            return true;
         }

         const size_t octets = lengthType == 0 ? 1 : lengthType == 1 ? 2 : 4;
         if (at + 1 + octets > data.size())
            return false;
         headerLength = 1 + octets;
         bodyLength = 0;
         for (size_t i = 0; i < octets; i++)
            bodyLength = (bodyLength << 8) | (size_t) data[at + 1 + i];
         return true;
      }

      // Whether the body of a PGP/MIME message holds an OpenPGP message that is
      // under a key: an armoured PGP MESSAGE at the start of a line whose
      // packets are one or more session key packets (tag 1, public-key; tag 3,
      // symmetric) and then encrypted data (tag 18 SEIPD, tag 20 AEAD, tag 9
      // the old symmetrically encrypted kind). The first 12 KB are decoded,
      // which holds the session key packets of any ordinary number of
      // recipients; a message with more is taken for NOT encrypted, which is
      // the safe way round: it is encrypted a second time rather than sent.
      bool BeginsWithSessionKeysThenEncryptedData(const std::vector<unsigned char> &packets)
      {
         size_t offset = 0;
         bool sawSessionKey = false;
         while (offset < packets.size())
         {
            int tag = 0;
            size_t headerLength = 0;
            size_t bodyLength = 0;
            bool partial = false;
            if (!ReadPacketHeader(packets, offset, tag, headerLength, bodyLength, partial))
               return false;

            if (tag == 1 || tag == 3)
            {
               if (partial)
                  return false;
               sawSessionKey = true;
               offset += headerLength + bodyLength;
               continue;
            }

            return sawSessionKey && (tag == 18 || tag == 20 || tag == 9);
         }

         return false;
      }

      // The message, parsed, has the shape of one WHOLLY under a key
      // (SealedMessage::IsOpaque): PGP/MIME with exactly the version part and
      // the ciphertext, or S/MIME as one part named smime.p7m or nothing. The
      // byte tests below find ciphertext; this one says there is nothing
      // beside it - a third, readable part of a multipart/encrypted message
      // would otherwise go to a partner, or be stored in a zero-access
      // mailbox, in the clear because the ciphertext next to it was real. A
      // message that does not parse is not.
      bool HasTheShapeOfOneUnderAKey(const AnsiString &message)
      {
         std::shared_ptr<MimeBody> mime = std::make_shared<MimeBody>();
         if (mime->LoadFromMemory(std::make_shared<const std::string>(message.c_str(), (size_t) message.GetLength()),
                                  _T("GatewayEncryption"), false) != MimeLoadResult::Loaded)
            return false;
         return SealedMessage::IsOpaque(*mime);
      }

      bool HoldsEncryptedOpenPgpMessage(const AnsiString &message, size_t bodyStart)
      {
         static const char beginLine[] = "-----BEGIN PGP MESSAGE-----";

         int begin = message.Find(beginLine, (size_t) bodyStart);
         while (begin > 0 && message[begin - 1] != '\n')
            begin = message.Find(beginLine, (size_t) begin + 1);
         if (begin < 0)
            return false;

         // The armour headers (Version:, Comment:), then one blank line.
         int at = message.Find("\n", (size_t) begin);
         if (at < 0)
            return false;
         at++;

         while (true)
         {
            const int lineEnd = message.Find("\n", (size_t) at);
            if (lineEnd < 0)
               return false;

            AnsiString line = message.Mid(at, lineEnd - at);
            line.TrimRight();
            at = lineEnd + 1;

            if (line.IsEmpty())
               break;
            if (line.Find(":") < 0)
               return false;
         }

         std::vector<unsigned char> packets;
         DecodeBase64Prefix(message, (size_t) at, 12 * 1024, packets);

         return BeginsWithSessionKeysThenEncryptedData(packets);
      }

      // A field the partner's copy does not carry. The Content-* fields and
      // MIME-Version described the body that is now under the key. The DKIM and
      // ARC signatures covered that body, fail against the new one, and are
      // replaced by signing the copy (DKIMSigner, in ExternalDelivery); the
      // original carries all of them, inside, where they still verify. The
      // server's own mark is written afresh.
      bool IsDroppedField(const AnsiString &loweredName)
      {
         return loweredName.StartsWith("content-") ||
                loweredName == "mime-version" ||
                loweredName == "dkim-signature" ||
                loweredName == "arc-seal" ||
                loweredName == "arc-message-signature" ||
                loweredName == "arc-authentication-results" ||
                loweredName == "x-hmailserver-gateway-encrypted";
      }

      AnsiString Lowered(const AnsiString &value)
      {
         AnsiString lowered = value;
         lowered.MakeLower();
         return lowered;
      }

      AnsiString DomainOfAddress(const AnsiString &address)
      {
         const int at = address.Find("@");
         if (at < 0)
            return AnsiString();
         return Lowered(address.Mid(at + 1));
      }

      // Whether a domain some key names is the policy's: the pattern matches it,
      // or it is a host under the policy's own domain (gateway.partner.example
      // under partner.example), which is where a partner's gateway lives.
      bool DomainMatches(const String &domain, const String &pattern)
      {
         if (domain.IsEmpty() || pattern.IsEmpty())
            return false;

         if (StringParser::WildcardMatchNoCase(pattern, domain))
            return true;

         const String suffix = _T(".") + pattern;
         return domain.GetLength() > suffix.GetLength() &&
                domain.Right(suffix.GetLength()).CompareNoCase(suffix) == 0;
      }

      AnsiString TextOf(const ASN1_STRING *value)
      {
         if (value == nullptr)
            return AnsiString();

         const unsigned char *data = ASN1_STRING_get0_data(value);
         const int length = ASN1_STRING_length(value);
         if (data == nullptr || length <= 0)
            return AnsiString();

         std::string text((const char *) data, (size_t) length);

         // An X.509 string may legally contain a NUL; a domain never does, and
         // "partner.example\0.evil" read by a C string function is the classic
         // attack on certificate names. Nothing with one names anything here.
         if (text.find('\0') != std::string::npos)
            return AnsiString();

         return AnsiString(text);
      }

      bool UtcParts(__int64 seconds, struct tm &parts)
      {
         memset(&parts, 0, sizeof(parts));
#ifdef HM_PLATFORM_POSIX
         time_t value = (time_t) seconds;
         return ::gmtime_r(&value, &parts) != nullptr;
#else
         __time64_t value = seconds;
         return _gmtime64_s(&parts, &value) == 0;
#endif
      }

      AnsiString DateText(__int64 seconds)
      {
         struct tm parts;
         if (!UtcParts(seconds, parts))
            return AnsiString("an unreadable date");

         AnsiString text;
         text.Format("%04d-%02d-%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
         return text;
      }

      __int64 EpochOf(const ASN1_TIME *moment)
      {
         if (moment == nullptr)
            return 0;

         struct tm parts = {};
         if (ASN1_TIME_to_tm(moment, &parts) != 1)
            return 0;

#ifdef _WIN32
         return (__int64) _mkgmtime(&parts);
#else
         return (__int64) timegm(&parts);
#endif
      }

      // The bits of a big-endian magnitude, leading zero octets and bits not
      // counted: the size of an RSA modulus as an OpenPGP key carries it.
      int BitsOf(const std::vector<unsigned char> &magnitude)
      {
         size_t first = 0;
         while (first < magnitude.size() && magnitude[first] == 0)
            first++;
         if (first == magnitude.size())
            return 0;

         int bits = (int) (magnitude.size() - first - 1) * 8;
         unsigned char lead = magnitude[first];
         while (lead)
         {
            bits++;
            lead >>= 1;
         }
         return bits;
      }

      // Whether a certificate names the partner: an rfc822Name or a dNSName in
      // its subjectAltName, or its subject's emailAddress or common name, in
      // the policy's domain.
      bool CertificateNamesDomain(X509 *certificate, const String &pattern)
      {
         bool named = false;

         GENERAL_NAMES *names = (GENERAL_NAMES *) X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr);
         if (names != nullptr)
         {
            const int count = sk_GENERAL_NAME_num(names);
            for (int i = 0; i < count && !named; i++)
            {
               const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
               if (name == nullptr)
                  continue;

               if (name->type == GEN_EMAIL)
                  named = DomainMatches(WideOf(DomainOfAddress(TextOf(name->d.rfc822Name))), pattern);
               else if (name->type == GEN_DNS)
                  named = DomainMatches(WideOf(Lowered(TextOf(name->d.dNSName))), pattern);
            }

            GENERAL_NAMES_free(names);
         }

         if (named)
            return true;

         const X509_NAME *subject = X509_get_subject_name(certificate);
         if (subject == nullptr)
            return false;

         const int nids[] = { NID_pkcs9_emailAddress, NID_commonName };
         for (int nid : nids)
         {
            int index = -1;
            while (!named && (index = X509_NAME_get_index_by_NID(subject, nid, index)) >= 0)
            {
               const X509_NAME_ENTRY *entry = X509_NAME_get_entry(subject, index);
               if (entry == nullptr)
                  continue;

               const AnsiString value = TextOf(X509_NAME_ENTRY_get_data(entry));
               const AnsiString domain = value.Find("@") >= 0 ? DomainOfAddress(value) : Lowered(value);
               named = DomainMatches(WideOf(domain), pattern);
            }
         }

         return named;
      }

      void AddWarning(GatewayEncryption::KeyFacts &facts, const AnsiString &sentence)
      {
         if (facts.warning.IsEmpty())
            facts.warning = sentence;
         else
            facts.warning += "; " + sentence;
      }

      // What one certificate is, in the terms the save and the listing use.
      void DescribeCertificate(X509 *leaf, const String &domainPattern, __int64 now, GatewayEncryption::KeyFacts &facts)
      {
         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int digestLength = 0;
         if (X509_digest(leaf, EVP_sha256(), digest, &digestLength) == 1)
            facts.fingerprint = Hex(digest, digestLength);

         char name[512];
         name[0] = 0;
         X509_NAME_oneline(X509_get_subject_name(leaf), name, sizeof(name));
         facts.subject = name;

         const __int64 notBefore = EpochOf(X509_get0_notBefore(leaf));
         const __int64 notAfter = EpochOf(X509_get0_notAfter(leaf));
         facts.expires = notAfter;

         if (notAfter > 0 && notAfter < now)
         {
            facts.problem = "the certificate expired on " + DateText(notAfter) + ". Ask the partner for its current certificate";
            return;
         }

         if (notBefore > now)
         {
            facts.problem = "the certificate is not valid until " + DateText(notBefore);
            return;
         }

         EVP_PKEY *publicKey = X509_get0_pubkey(leaf);
         const int type = publicKey != nullptr ? EVP_PKEY_get_base_id(publicKey) : 0;
         if (type != EVP_PKEY_RSA && type != EVP_PKEY_EC)
         {
            facts.problem = "the certificate's key cannot be encrypted to: S/MIME encryption needs an RSA or an elliptic-curve key";
            return;
         }

         const int bits = EVP_PKEY_get_bits(publicKey);
         if (type == EVP_PKEY_RSA && bits < GatewayEncryption::MinimumRsaBits)
         {
            facts.problem.Format("the certificate's key is RSA of %d bits, and this server encrypts to RSA of %d bits or more", bits, (int) GatewayEncryption::MinimumRsaBits);
            return;
         }

         if (type == EVP_PKEY_EC && bits < GatewayEncryption::MinimumEcBits)
         {
            facts.problem.Format("the certificate's key is an elliptic curve of %d bits, and this server encrypts to curves of %d bits or more", bits, (int) GatewayEncryption::MinimumEcBits);
            return;
         }

         // No key usage extension is no restriction (RFC 5280 section 4.2.1.3).
         const uint32_t usage = X509_get_key_usage(leaf);
         if (usage != UINT32_MAX)
         {
            const uint32_t needed = type == EVP_PKEY_RSA ? (uint32_t) KU_KEY_ENCIPHERMENT : (uint32_t) KU_KEY_AGREEMENT;
            if ((usage & needed) == 0)
            {
               facts.problem = type == EVP_PKEY_RSA
                  ? "the certificate's key usage does not allow encryption: an RSA certificate that is encrypted to must carry keyEncipherment"
                  : "the certificate's key usage does not allow encryption: an elliptic-curve certificate that is encrypted to must carry keyAgreement";
               return;
            }
         }

         facts.usable = true;

         const uint32_t extended = X509_get_extended_key_usage(leaf);
         if (extended != UINT32_MAX && (extended & (XKU_SMIME | XKU_ANYEKU)) == 0)
            AddWarning(facts, "the certificate's extended key usage does not include e-mail protection, and some gateways refuse a message encrypted to such a certificate");

         if (!domainPattern.IsEmpty() && !CertificateNamesDomain(leaf, domainPattern))
            AddWarning(facts, "the certificate names no address or host in " + Utf8Of(domainPattern) +
                              "; it was accepted, since a partner's gateway certificate often names the gateway rather than the domain, but check its fingerprint with the partner");
      }

      GatewayEncryption::KeyFacts InspectSmime(const AnsiString &text, const String &domainPattern, __int64 now)
      {
         GatewayEncryption::KeyFacts facts;

         if (text.Find("-----BEGIN CERTIFICATE-----") < 0)
         {
            facts.problem = text.Find("-----BEGIN PGP") >= 0
               ? "this is an OpenPGP key, not a certificate: choose OpenPGP for it"
               : "this is not a PEM certificate: an S/MIME gateway certificate is pasted as the block that begins -----BEGIN CERTIFICATE-----";
            return facts;
         }

         std::vector<X509 *> certificates;
         BIO *bio = BIO_new_mem_buf(text.c_str(), text.GetLength());
         if (bio != nullptr)
         {
            while (true)
            {
               X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
               if (certificate == nullptr)
                  break;
               certificates.push_back(certificate);
            }
            BIO_free(bio);
         }

         // The read that ends the loop leaves "no start line" behind it, and a
         // failed one its own reason; neither is anybody else's business.
         ERR_clear_error();

         if (certificates.empty())
         {
            facts.problem = "the certificate could not be read: what is between -----BEGIN CERTIFICATE----- and -----END CERTIFICATE----- is not an X.509 certificate";
            return facts;
         }

         // The first certificate is the one encrypted to; any after it are taken
         // as its chain, kept with it, and not encrypted to.
         DescribeCertificate(certificates[0], domainPattern, now, facts);

         for (X509 *certificate : certificates)
            X509_free(certificate);

         return facts;
      }

      GatewayEncryption::KeyFacts InspectOpenPgp(const AnsiString &text, const String &domainPattern, __int64 now)
      {
         GatewayEncryption::KeyFacts facts;

         if (text.Find("-----BEGIN CERTIFICATE-----") >= 0)
         {
            facts.problem = "this is an X.509 certificate, not an OpenPGP key: choose S/MIME for it";
            return facts;
         }

         const OpenPgpKey::Parsed parsed = OpenPgpKey::ParseText(text, 0);
         if (!parsed.ok)
         {
            facts.problem = parsed.error;
            return facts;
         }

         facts.fingerprint = parsed.fingerprint;
         facts.subject = parsed.primary_user_id;
         facts.expires = parsed.expires;

         if (parsed.bytes.empty())
         {
            facts.problem = "the key is empty";
            return facts;
         }

         // The subkey is chosen only among those whose binding signature
         // verifies against the primary (OpenPgpWriter.h), so the refusal of a
         // key with none is this sentence.
         const OpenPgpWriter::EncryptionKey chosen = OpenPgpWriter::ChooseEncryptionKey(&parsed.bytes[0], parsed.bytes.size(), now);
         if (!chosen.ok)
         {
            facts.problem = "no message can be encrypted to this key: " + chosen.error;
            return facts;
         }

         if (chosen.fingerprint != chosen.primary_fingerprint)
            facts.subkey_fingerprint = chosen.fingerprint;

         facts.expires = chosen.expires;

         if (chosen.algorithm == 1 || chosen.algorithm == 2)
         {
            const int bits = BitsOf(chosen.rsa_n);
            if (bits < GatewayEncryption::MinimumRsaBits)
            {
               facts.problem.Format("the key's encryption key is RSA of %d bits, and this server encrypts to RSA of %d bits or more", bits, (int) GatewayEncryption::MinimumRsaBits);
               return facts;
            }
         }
         else if (chosen.algorithm == 18)
         {
            // The KDF parameters the Curve25519 subkey names: SHA-256, -384 or
            // -512, and an AES key wrap - what OpenPgpWriter::Encrypt writes.
            const bool hashKnown = chosen.kdf_hash == 8 || chosen.kdf_hash == 9 || chosen.kdf_hash == 10;
            const bool cipherKnown = chosen.kdf_cipher == 7 || chosen.kdf_cipher == 8 || chosen.kdf_cipher == 9;
            if (!hashKnown || !cipherKnown)
            {
               facts.problem = "the key's Curve25519 subkey asks for a KDF hash or a key-wrap cipher this server does not have (it has SHA-256, SHA-384 and SHA-512, and AES)";
               return facts;
            }
         }

         facts.usable = true;

         if (domainPattern.IsEmpty())
            return facts;

         bool named = false;
         for (const AnsiString &userId : parsed.user_ids)
         {
            if (DomainMatches(WideOf(DomainOfAddress(OpenPgpKey::AddressOfUserId(userId))), domainPattern))
            {
               named = true;
               break;
            }
         }

         if (!named)
            AddWarning(facts, "the key names no address in " + Utf8Of(domainPattern) +
                              "; it was accepted, since a partner's gateway key often names the gateway rather than the domain, but check its fingerprint with the partner");

         return facts;
      }

      // PGP/MIME, RFC 3156: the version part and the armoured message. The
      // original is let go of once it is encrypted; the body comes back in
      // three parts - the opening, the armour, the closing boundary - so that
      // the armour is never copied into a larger string.
      GatewayEncryption::Outcome SealOpenPgp(const AnsiString &keyText, AnsiString &original, __int64 now,
                                             AnsiString &contentFields, std::vector<AnsiString> &body, AnsiString &problem)
      {
         const OpenPgpKey::Parsed parsed = OpenPgpKey::ParseText(keyText, 0);
         if (!parsed.ok || parsed.bytes.empty())
         {
            problem = parsed.ok ? AnsiString("the key is empty") : parsed.error;
            return GatewayEncryption::Failed;
         }

         const OpenPgpWriter::EncryptionKey to = OpenPgpWriter::ChooseEncryptionKey(&parsed.bytes[0], parsed.bytes.size(), now);
         if (!to.ok)
         {
            problem = "no message can be encrypted to this key: " + to.error;
            return GatewayEncryption::Failed;
         }

         unsigned char boundaryBytes[16];
         memset(boundaryBytes, 0, sizeof(boundaryBytes));
         if (RAND_bytes(boundaryBytes, sizeof(boundaryBytes)) != 1)
         {
            problem = "no random bytes could be drawn for the MIME boundary";
            return GatewayEncryption::Deferred;
         }
         const AnsiString boundary = "hmailserver-gateway-" + Hex(boundaryBytes, sizeof(boundaryBytes));

         // The key was judged usable a moment ago, by the same reader, so what
         // fails from here is this server's machinery: a retry, not a refusal.
         std::vector<unsigned char> message;
         if (!OpenPgpWriter::Encrypt(to, (const unsigned char *) original.c_str(), (size_t) original.GetLength(), message, problem))
            return GatewayEncryption::Deferred;

         ReleaseText(original);

         contentFields = "MIME-Version: 1.0\r\n";
         contentFields += "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n boundary=\"" + boundary + "\"\r\n";

         AnsiString opening = "This is an OpenPGP/MIME encrypted message (RFC 3156). It was encrypted by the sender's mail server to the key of the recipient's domain.\r\n";
         opening += "\r\n--" + boundary + "\r\n";
         opening += "Content-Type: application/pgp-encrypted\r\nContent-Description: PGP/MIME version identification\r\n\r\nVersion: 1\r\n";
         opening += "\r\n--" + boundary + "\r\n";
         opening += "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\nContent-Description: OpenPGP encrypted message\r\nContent-Disposition: inline; filename=\"encrypted.asc\"\r\n\r\n";

         body.push_back(opening);
         {
            AnsiString armour = OpenPgpWriter::Armour(message);
            std::vector<unsigned char>().swap(message);
            MoveInto(body, armour);
         }
         body.push_back("\r\n--" + boundary + "--\r\n");
         return GatewayEncryption::Sealed;
      }

      // S/MIME enveloped-data, RFC 8551: CMS, AES-256-CBC, base64 in lines of
      // 76. The recipient is added with its own parameters: RSAES-OAEP for an
      // RSA key (RFC 3560, the default SHA-1 parameters every client reads),
      // and the X9.63 KDF over SHA-256 for an elliptic-curve one (RFC 5753;
      // OpenSSL's own default is SHA-1). The original is let go of once CMS has
      // it.
      GatewayEncryption::Outcome SealSmime(const AnsiString &keyText, AnsiString &original,
                                           AnsiString &contentFields, std::vector<AnsiString> &body, AnsiString &problem)
      {
         BIO *certificates = BIO_new_mem_buf(keyText.c_str(), keyText.GetLength());
         X509 *leaf = certificates != nullptr ? PEM_read_bio_X509(certificates, nullptr, nullptr, nullptr) : nullptr;
         if (certificates != nullptr)
            BIO_free(certificates);
         ERR_clear_error();

         if (leaf == nullptr)
         {
            problem = "the certificate could not be read";
            return GatewayEncryption::Failed;
         }

         EVP_PKEY *publicKey = X509_get0_pubkey(leaf);
         const int type = publicKey != nullptr ? EVP_PKEY_get_base_id(publicKey) : 0;

         BIO *in = BIO_new_mem_buf(original.c_str(), original.GetLength());
         CMS_ContentInfo *cms = nullptr;
         bool ok = false;

         if (in != nullptr)
         {
            // CMS_PARTIAL: the recipient is added below with its parameters,
            // and CMS_final writes the content after that. CMS_BINARY: the
            // message is already CRLF lines and goes under the key byte for
            // byte, as the PGP/MIME copy's does.
            cms = CMS_encrypt(nullptr, in, EVP_aes_256_cbc(), CMS_BINARY | CMS_PARTIAL);

            CMS_RecipientInfo *recipient = cms != nullptr ? CMS_add1_recipient_cert(cms, leaf, CMS_KEY_PARAM) : nullptr;
            EVP_PKEY_CTX *parameters = recipient != nullptr ? CMS_RecipientInfo_get0_pkey_ctx(recipient) : nullptr;

            bool configured = false;
            if (parameters != nullptr)
            {
               if (type == EVP_PKEY_RSA)
                  configured = EVP_PKEY_CTX_set_rsa_padding(parameters, RSA_PKCS1_OAEP_PADDING) > 0;
               else if (type == EVP_PKEY_EC)
                  configured = EVP_PKEY_CTX_set_ecdh_kdf_md(parameters, EVP_sha256()) > 0;
            }

            ok = configured && CMS_final(cms, in, nullptr, CMS_BINARY) == 1;
            BIO_free(in);
         }

         // CMS holds the encrypted content now; the plaintext is not needed.
         ReleaseText(original);

         BIO *out = nullptr;
         if (ok)
         {
            out = BIO_new(BIO_s_mem());
            ok = out != nullptr && i2d_CMS_bio(out, cms) == 1;
         }

         if (cms != nullptr)
            CMS_ContentInfo_free(cms);
         X509_free(leaf);

         // Base64 in lines of 76, encoded a block at a time straight from the
         // DER - 1,024 lines of 57 octets, a multiple of three - so that the
         // whole object's base64 is never held beside its lines.
         AnsiString lines;
         if (ok)
         {
            char *data = nullptr;
            const long length = BIO_get_mem_data(out, &data);
            if (data != nullptr && length > 0)
            {
               const size_t total = (size_t) length;
               lines.reserve(total / 57 * 78 + 80);

               const size_t block = 57 * 1024;
               for (size_t start = 0; start < total; start += block)
               {
                  const size_t count = total - start < block ? total - start : block;
                  const AnsiString encoded = Base64::Encode(data + start, (int) count);
                  const size_t characters = (size_t) encoded.GetLength();
                  for (size_t at = 0; at < characters; at += 76)
                  {
                     lines.append(encoded.c_str() + at, characters - at < 76 ? characters - at : 76);
                     lines += "\r\n";
                  }
               }
            }
            else
               ok = false;
         }

         if (out != nullptr)
            BIO_free(out);

         char reason[256];
         reason[0] = 0;
         if (!ok)
         {
            const unsigned long code = ERR_get_error();
            if (code != 0)
               ERR_error_string_n(code, reason, sizeof(reason));
         }
         ERR_clear_error();

         if (!ok)
         {
            // The certificate was judged usable a moment ago, so what failed
            // is this server's machinery: a retry, not a refusal.
            problem = "the message could not be enveloped to the certificate";
            if (reason[0] != 0)
               problem += AnsiString(" (") + reason + ")";
            return GatewayEncryption::Deferred;
         }

         contentFields = "MIME-Version: 1.0\r\n";
         contentFields += "Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"\r\n";
         contentFields += "Content-Transfer-Encoding: base64\r\n";
         contentFields += "Content-Disposition: attachment; filename=\"smime.p7m\"\r\n";
         contentFields += "Content-Description: S/MIME encrypted message\r\n";

         MoveInto(body, lines);
         return GatewayEncryption::Sealed;
      }

      // The partner's copy in two parts, the header block (with its blank line)
      // and the body's pieces, so that SealFile can write them one after the
      // other. original is consumed.
      GatewayEncryption::Outcome SealParts(RemoteEncryption method, const String &key, AnsiString &original, __int64 now,
                                           AnsiString &head, std::vector<AnsiString> &body,
                                           GatewayEncryption::KeyFacts &facts, AnsiString &problem)
      {
         head.Empty();
         body.clear();

         // Room for every part up front: a vector that grew would copy the
         // parts already in it, the armour among them.
         body.reserve(4);

         // Judged again at the moment of sending, and not only at the save: a key
         // that was good on the day it was pasted may have expired since.
         facts = GatewayEncryption::Inspect(method, key, String(), now);
         if (!facts.usable)
         {
            problem = facts.problem;
            return GatewayEncryption::Failed;
         }

         AnsiString keyText = Utf8Of(key);
         keyText.Trim();

         // Read from the original before the sealing lets go of it.
         head = GatewayEncryption::OuterHeaders(original);
         head += GatewayEncryption::MarkField();
         head += ": ";
         head += GatewayEncryption::MethodName(method);
         head += " to key ";
         head += GatewayEncryption::KeyName(facts);
         head += "\r\n";

         AnsiString contentFields;
         GatewayEncryption::Outcome outcome = GatewayEncryption::Failed;

         if (method == RemoteEncryptOpenPgp)
            outcome = SealOpenPgp(keyText, original, now, contentFields, body, problem);
         else if (method == RemoteEncryptSmime)
            outcome = SealSmime(keyText, original, contentFields, body, problem);
         else
            problem = "the policy names no encryption method";

         if (outcome != GatewayEncryption::Sealed)
         {
            head.Empty();
            body.clear();
            return outcome;
         }

         head += contentFields;
         head += "\r\n";
         return GatewayEncryption::Sealed;
      }

      void Require(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }
   }

   bool
   GatewayEncryption::ContainsPrivateKey(const AnsiString &text)
   {
      // Every PEM label of a private key ends in PRIVATE KEY (PKCS#8, ENCRYPTED,
      // RSA, EC, DSA, OPENSSH), and OpenPGP's are PRIVATE KEY BLOCK and the
      // older SECRET KEY BLOCK. Upper case, as the labels are written; a
      // public key's or a certificate's text never carries either phrase.
      return text.Find("PRIVATE KEY") >= 0 || text.Find("SECRET KEY") >= 0;
   }

   bool
   GatewayEncryption::AcceptableKeyText(const AnsiString &keyText, AnsiString &problem)
   {
      AnsiString text = keyText;
      text.Trim();

      if (text.IsEmpty())
         return true;

      if (ContainsPrivateKey(text))
      {
         const bool openPgp = text.Find("PGP PRIVATE KEY") >= 0 || text.Find("SECRET KEY") >= 0;
         problem = openPgp
            ? "this paste carries an OpenPGP SECRET key, and this server never holds a partner's - not beside its public key either. Paste the partner's public key alone: gpg --armor --export, not --export-secret-keys"
            : "this paste carries a PRIVATE key, and this server never holds a partner's - not beside its certificate or public key either. Paste the partner's certificate (the block that begins -----BEGIN CERTIFICATE-----) or its public key alone";
         return false;
      }

      if (text.GetLength() > (int) MaximumKeyCharacters)
      {
         problem.Format("the key is %d characters, and a policy holds one gateway key of at most %d: a paste this long is a keyring or a certificate bundle",
            text.GetLength(), (int) MaximumKeyCharacters);
         return false;
      }

      return true;
   }

   GatewayEncryption::KeyFacts
   GatewayEncryption::Inspect(RemoteEncryption method, const String &key, const String &domainPattern, __int64 now)
   {
      KeyFacts facts;

      AnsiString text = Utf8Of(key);
      text.Trim();

      if (text.IsEmpty())
      {
         facts.problem = "no key is held: paste the partner's OpenPGP public key or its PEM certificate";
         return facts;
      }

      // Before anything reads the text as a key: a private key beside a public
      // one is refused whole, whichever reader the method would hand it to.
      if (!AcceptableKeyText(text, facts.problem))
         return facts;

      switch (method)
      {
      case RemoteEncryptOpenPgp:
         return InspectOpenPgp(text, domainPattern, now);
      case RemoteEncryptSmime:
         return InspectSmime(text, domainPattern, now);
      default:
         facts.problem = "the policy names no encryption method";
         return facts;
      }
   }

   AnsiString
   GatewayEncryption::KeyName(const KeyFacts &facts)
   {
      AnsiString name = facts.fingerprint;
      if (!facts.subkey_fingerprint.IsEmpty())
         name += " subkey " + facts.subkey_fingerprint;
      return name;
   }

   AnsiString
   GatewayEncryption::Summary(RemoteEncryption method, const String &key, const String &domainPattern, __int64 now)
   {
      if (key.IsEmpty())
      {
         return method == RemoteEncryptNone
            ? AnsiString("No key is held.")
            : AnsiString("No key is held, so no message to this domain can be encrypted.");
      }

      // A key kept while encryption is off is still described, as what it
      // looks like.
      const RemoteEncryption judged = method != RemoteEncryptNone ? method : MethodOfKey(key);
      const KeyFacts facts = Inspect(judged, key, domainPattern, now);

      AnsiString text = AnsiString(MethodName(judged)) + " key";
      if (!facts.fingerprint.IsEmpty())
         text += " " + facts.fingerprint;
      if (!facts.subkey_fingerprint.IsEmpty())
         text += " (messages are encrypted to its subkey " + facts.subkey_fingerprint + "; confirm both with the partner)";
      if (!facts.subject.IsEmpty())
         text += ", " + facts.subject;
      if (!facts.fingerprint.IsEmpty())
         text += facts.expires > 0 ? AnsiString(", expires ") + ExpiryText(facts.expires) : AnsiString(", does not expire");
      text += ".";

      if (!facts.usable)
      {
         AnsiString why = facts.problem;
         why.TrimRight('.');
         text += " No message can be encrypted to it: " + why + ".";
      }

      if (!facts.warning.IsEmpty())
         text += " Note: " + facts.warning + ".";

      if (method == RemoteEncryptNone)
         text += " Gateway encryption is off; the key is kept for when it is turned on.";

      return text;
   }

   RemoteEncryption
   GatewayEncryption::MethodOfKey(const String &key)
   {
      if (key.IsEmpty())
         return RemoteEncryptNone;

      if (key.Find(_T("-----BEGIN CERTIFICATE-----")) >= 0)
         return RemoteEncryptSmime;

      return RemoteEncryptOpenPgp;
   }

   bool
   GatewayEncryption::IsEncryptedEndToEnd(const AnsiString &message)
   {
      size_t bodyStart = 0;
      HeaderEnd(message, bodyStart);

      // The label says where to look; what is there decides. An S/MIME object
      // is encrypted when its CMS content type is enveloped-data or
      // authenticated-enveloped-data, whatever smime-type (or a name=
      // parameter) says, and whether or not there is one: S/MIME v2 clients
      // labelled opaque SIGNED data application/pkcs7-mime with no smime-type.
      const AnsiString type = TopLevelField(message, "content-type");
      if (type.Find("application/pkcs7-mime") >= 0 || type.Find("application/x-pkcs7-mime") >= 0)
      {
         std::vector<unsigned char> prefix;
         const AnsiString encoding = TopLevelField(message, "content-transfer-encoding");

         if (encoding.Find("base64") >= 0)
            DecodeBase64Prefix(message, bodyStart, 32, prefix);
         else if (bodyStart < (size_t) message.GetLength())
         {
            const size_t end = std::min((size_t) message.GetLength(), bodyStart + 32);
            prefix.assign(message.c_str() + bodyStart, message.c_str() + end);
         }

         return IsEnvelopedContentInfo(prefix) && HasTheShapeOfOneUnderAKey(message);
      }

      // PGP/MIME: the top-level multipart/encrypted with the OpenPGP protocol,
      // AND an OpenPGP message in it that is under a key, AND nothing else in
      // it but the version part.
      if (OpenPgpWriter::IsPgpMimeEncrypted(message))
         return HoldsEncryptedOpenPgpMessage(message, bodyStart) && HasTheShapeOfOneUnderAKey(message);

      return false;
   }

   bool
   GatewayEncryption::IsEncryptedOpenPgpPackets(const std::vector<unsigned char> &packets)
   {
      return BeginsWithSessionKeysThenEncryptedData(packets);
   }

   bool
   GatewayEncryption::IsEnvelopedCms(const std::vector<unsigned char> &der)
   {
      return IsEnvelopedContentInfo(der);
   }

   bool
   GatewayEncryption::IsSignedCms(const std::vector<unsigned char> &der)
   {
      return IsSignedContentInfo(der);
   }

   AnsiString
   GatewayEncryption::OuterHeaders(const AnsiString &original)
   {
      size_t bodyStart = 0;
      const size_t headerLength = HeaderEnd(original, bodyStart);
      const AnsiString header = original.Mid(0, (int) headerLength);
      const char *text = header.c_str();
      const size_t length = (size_t) header.GetLength();

      AnsiString outer;
      size_t at = 0;
      while (at < length)
      {
         // One field: up to a line end that is not followed by white space.
         size_t end = at;
         while (end < length)
         {
            size_t lineEnd = end;
            while (lineEnd < length && text[lineEnd] != '\n')
               lineEnd++;
            end = lineEnd < length ? lineEnd + 1 : length;
            if (end < length && (text[end] == ' ' || text[end] == '\t'))
               continue;
            break;
         }

         const AnsiString field(std::string(text + at, end - at));
         at = end;

         const int colon = field.Find(":");
         if (colon <= 0)
            continue;

         AnsiString name = field.Mid(0, colon);
         name.Trim();
         name.MakeLower();
         if (IsDroppedField(name))
            continue;

         // CRLF line ends throughout, whatever the file had.
         AnsiString normalised = field;
         normalised.Replace("\r\n", "\n");
         normalised.Replace("\n", "\r\n");
         if (!normalised.EndsWith("\r\n"))
            normalised += "\r\n";
         outer += normalised;
      }

      return outer;
   }

   GatewayEncryption::Outcome
   GatewayEncryption::Seal(RemoteEncryption method, const String &key, const AnsiString &original, __int64 now,
                           AnsiString &sealed, KeyFacts &facts, AnsiString &problem)
   {
      sealed.Empty();
      problem.Empty();
      facts = KeyFacts();

      // The caller's text is left as it was; the sealing consumes its own copy.
      AnsiString consumed = original;
      AnsiString head;
      std::vector<AnsiString> body;

      const Outcome outcome = SealParts(method, key, consumed, now, head, body, facts, problem);
      if (outcome != Sealed)
         return outcome;

      sealed = head;
      for (const AnsiString &part : body)
         sealed += part;
      return Sealed;
   }

   GatewayEncryption::Outcome
   GatewayEncryption::SealFile(RemoteEncryption method, const String &key, const String &messageFile, const String &copyFile,
                               __int64 now, KeyFacts &facts, AnsiString &problem)
   {
      facts = KeyFacts();
      problem.Empty();

      try
      {
         unsigned __int64 size = 0;
         if (!FileUtilities::FileSize64(messageFile, size))
         {
            problem = "the message file could not be read";
            return Deferred;
         }

         if (size > (unsigned __int64) MaximumMessageBytes)
         {
            problem.Format("the message is %d KB, and a message is encrypted at the gateway in memory up to %d KB",
               (int) (size / 1024), (int) MaximumMessageBytes / 1024);
            return Failed;
         }

         AnsiString original;
         {
            File file;
            if (!file.Open(messageFile, File::OTReadOnly))
            {
               problem = "the message file could not be read";
               return Deferred;
            }

            std::shared_ptr<ByteBuffer> buffer = file.ReadFile();
            file.Close();

            if (!buffer)
            {
               problem = "the message file could not be read";
               return Deferred;
            }

            // One copy, and the buffer let go of at the end of this block.
            original.assign((const char *) buffer->GetBuffer(), buffer->GetSize());
         }

         // Under a key already: it goes as it is.
         if (IsEncryptedEndToEnd(original))
            return AlreadyEncrypted;

         AnsiString head;
         std::vector<AnsiString> body;
         const Outcome outcome = SealParts(method, key, original, now, head, body, facts, problem);
         if (outcome != Sealed)
            return outcome;

         // The parts one after the other, never joined: the armour or the
         // base64 is the size of the message, and joining it would copy it.
         File out;
         bool written = out.Open(copyFile, File::OTCreate) && out.Write(head);
         for (size_t i = 0; written && i < body.size(); i++)
         {
            written = out.Write(body[i]);
            ReleaseText(body[i]);
         }
         // The close writes the encrypted copy's last bytes (File::Close).
         written = out.Close() && written;

         if (!written)
         {
            FileUtilities::DeleteFile(copyFile);
            problem = "the encrypted copy could not be written to the temporary directory";
            return Deferred;
         }

         return Sealed;
      }
      catch (const std::bad_alloc &)
      {
         FileUtilities::DeleteFile(copyFile);
         problem = "the server ran out of memory encrypting the message";
         return Deferred;
      }
   }

   const char *
   GatewayEncryption::MethodName(RemoteEncryption method)
   {
      switch (method)
      {
      case RemoteEncryptOpenPgp:
         return "OpenPGP";
      case RemoteEncryptSmime:
         return "S/MIME";
      case RemoteEncryptNone:
      default:
         return "no";
      }
   }

   const char *
   GatewayEncryption::MethodWord(RemoteEncryption method)
   {
      switch (method)
      {
      case RemoteEncryptOpenPgp:
         return "openpgp";
      case RemoteEncryptSmime:
         return "smime";
      case RemoteEncryptNone:
      default:
         return "none";
      }
   }

   AnsiString
   GatewayEncryption::ExpiryText(__int64 expires)
   {
      if (expires <= 0)
         return AnsiString();

      struct tm parts;
      if (!UtcParts(expires, parts))
         return AnsiString();

      AnsiString text;
      text.Format("%04d-%02d-%02dT%02d:%02d:%02dZ", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
         parts.tm_hour, parts.tm_min, parts.tm_sec);
      return text;
   }

   // ---------------------------------------------------------------------------
   // The tests. Every input is a value; the clock is passed in. The OpenPGP keys
   // are the fixtures GnuPG 2.4 made for build/check-portal-openpgp.js (alice,
   // Ed25519 with a cv25519 subkey, the same bytes the regression suite's
   // OpenPgpTestSupport.RingOwnPublicBase64 carries; bob, RSA 3072 with an RSA
   // subkey, build/fixtures/openpgp/bob.pub.asc) and two GnuPG 2.4.7 made for
   // this test on 24 September 2026 (an ECDSA P-256 key with an ECDH P-256
   // subkey, and an RSA 1024 key); every one of their subkey bindings was
   // checked outside this file with Python's cryptography package. The
   // certificates were made with openssl req for this test - gateway@partner.test
   // in each, e-mail protection - and their private keys thrown away, because
   // nothing here decrypts: the regression fixture SMTP/GatewayEncryption.cs
   // opens what this writes, with .NET and with gpg. What the tests read of an
   // S/MIME copy they read with OpenSSL's CMS decoder: the RecipientInfo's
   // algorithm, RSAES-OAEP or the SHA-256 KDF.
   // ---------------------------------------------------------------------------

   namespace
   {
      // The algorithm of the first RecipientInfo in an S/MIME copy's CMS object
      // - the key transport's or the key agreement's - as a NID; 0 when it
      // could not be read.
      int RecipientAlgorithmOf(const AnsiString &sealed)
      {
         size_t bodyStart = 0;
         HeaderEnd(sealed, bodyStart);

         std::string characters;
         for (size_t i = bodyStart; i < (size_t) sealed.GetLength(); i++)
         {
            if (IsBase64Character(sealed[(int) i]))
               characters += sealed[(int) i];
         }

         const AnsiString der = Base64::Decode(characters.c_str(), (int) characters.size());
         BIO *bio = BIO_new_mem_buf(der.c_str(), der.GetLength());
         CMS_ContentInfo *cms = bio != nullptr ? d2i_CMS_bio(bio, nullptr) : nullptr;

         int nid = 0;
         if (cms != nullptr)
         {
            STACK_OF(CMS_RecipientInfo) *infos = CMS_get0_RecipientInfos(cms);
            CMS_RecipientInfo *info = infos != nullptr && sk_CMS_RecipientInfo_num(infos) > 0 ? sk_CMS_RecipientInfo_value(infos, 0) : nullptr;

            X509_ALGOR *algorithm = nullptr;
            if (info != nullptr && CMS_RecipientInfo_type(info) == CMS_RECIPINFO_TRANS)
               CMS_RecipientInfo_ktri_get0_algs(info, nullptr, nullptr, &algorithm);
            else if (info != nullptr && CMS_RecipientInfo_type(info) == CMS_RECIPINFO_AGREE)
            {
               ASN1_OCTET_STRING *ukm = nullptr;
               CMS_RecipientInfo_kari_get0_alg(info, &algorithm, &ukm);
            }

            if (algorithm != nullptr)
            {
               const ASN1_OBJECT *object = nullptr;
               X509_ALGOR_get0(&object, nullptr, nullptr, algorithm);
               nid = object != nullptr ? OBJ_obj2nid(object) : 0;
            }

            CMS_ContentInfo_free(cms);
         }

         if (bio != nullptr)
            BIO_free(bio);
         ERR_clear_error();
         return nid;
      }

      // A PGP/MIME message around an armoured body, for the recognition tests.
      AnsiString PgpMime(const char *armouredBase64)
      {
         AnsiString message = "Subject: s\r\nMIME-Version: 1.0\r\nContent-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\"; boundary=x\r\n\r\n";
         message += "--x\r\nContent-Type: application/pgp-encrypted\r\n\r\nVersion: 1\r\n\r\n";
         message += "--x\r\nContent-Type: application/octet-stream\r\n\r\n";
         if (armouredBase64 != nullptr)
         {
            message += "-----BEGIN PGP MESSAGE-----\r\n\r\n";
            message += armouredBase64;
            message += "\r\n-----END PGP MESSAGE-----\r\n";
         }
         message += "\r\n--x--\r\n";
         return message;
      }
   }

   void
   GatewayEncryptionTester::Test()
   {
      const __int64 now = 1800000000;   // 2027-01-15T08:00:00Z

      const AnsiString original =
         "Return-Path: <ann@example.test>\r\n"
         "DKIM-Signature: v=1; a=rsa-sha256; d=example.test; s=one;\r\n"
         "\tb=c2lnbmF0dXJl\r\n"
         "ARC-Seal: i=1; cv=none; d=example.test\r\n"
         "ARC-Message-Signature: i=1; bh=abc\r\n"
         "ARC-Authentication-Results: i=1; example.test\r\n"
         "From: Ann Example <ann@example.test>\r\n"
         "To: bob@partner.test\r\n"
         "Cc: carol@partner.test\r\n"
         "Date: Fri, 15 Jan 2027 08:00:00 +0000\r\n"
         "Subject: The subject stays\r\n"
         "Message-ID: <one@example.test>\r\n"
         "X-Original-Content-Type: application/pkcs7-mime; smime-type=enveloped-data\r\n"
         "MIME-Version: 1.0\r\n"
         "Content-Type: text/plain;\r\n"
         " charset=utf-8\r\n"
         "Content-Transfer-Encoding: 8bit\r\n"
         "\r\n"
         "The body goes under the key.\r\n";

      // ---- the outer header: the original's, less what described or signed the old body
      {
         const AnsiString outer = GatewayEncryption::OuterHeaders(original);
         Require(outer.Find("From: Ann Example <ann@example.test>\r\n") >= 0 && outer.Find("To: bob@partner.test\r\n") >= 0 &&
                 outer.Find("Cc: carol@partner.test\r\n") >= 0 && outer.Find("Date: Fri, 15 Jan 2027") >= 0 &&
                 outer.Find("Subject: The subject stays\r\n") >= 0 && outer.Find("Message-ID: <one@example.test>\r\n") >= 0 &&
                 outer.Find("Return-Path: <ann@example.test>\r\n") >= 0,
                 "The kept header fields are not all in the partner's copy.");
         Require(outer.Find("DKIM-Signature") < 0 && outer.Find("c2lnbmF0dXJl") < 0 && outer.Find("ARC-") < 0,
                 "A signature over the old body was kept on the partner's copy.");
         // A field is looked for where a field starts - the start of a line -
         // so that X-Original-Content-Type, kept on purpose, does not count.
         Require(!outer.StartsWith("MIME-Version:") && outer.Find("\r\nMIME-Version:") < 0 &&
                 !outer.StartsWith("Content-Type:") && outer.Find("\r\nContent-Type:") < 0 &&
                 !outer.StartsWith("Content-Transfer-Encoding:") && outer.Find("\r\nContent-Transfer-Encoding:") < 0 &&
                 outer.Find("charset") < 0,
                 "A field that described the old body was kept on the partner's copy.");
         Require(outer.Find("\r\nX-Original-Content-Type: application/pkcs7-mime") >= 0, "A field whose name merely ends in Content-Type was taken off.");
         Require(outer.Find("The body") < 0, "The body leaked into the header block.");
         Require(outer.EndsWith("\r\n") && outer.Find("\r\n\r\n") < 0, "The header block is not CRLF fields with no blank line.");
      }

      // ---- what is encrypted end to end already, and what is not: decided by what the body holds
      {
         Require(!GatewayEncryption::IsEncryptedEndToEnd(original), "A plain message, with a lookalike field above its own, was taken for encrypted.");

         // PGP/MIME: session key packets, then encrypted data.
         Require(GatewayEncryption::IsEncryptedEndToEnd(PgpMime("wQoDAAAAAAAAAAAB0gUBqrvM3Q==")),
                 "PGP/MIME holding a session key packet and encrypted data was not recognised.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd(PgpMime("ywhiAAAAAABoaQ==")),
                 "PGP/MIME whose PGP MESSAGE is a literal data packet - readable - was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd(PgpMime("wQoDAAAAAAAAAAABywJiAA==")),
                 "PGP/MIME whose session key packet is followed by readable data was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd(PgpMime(nullptr)),
                 "A multipart/encrypted label with no OpenPGP message under it was taken for encrypted.");

         // Real ciphertext with a readable part beside it is not a message
         // under a key: the third part would travel, or be stored, in the
         // clear. Nor is a ciphertext part that calls itself invoice.exe.
         {
            AnsiString passenger = PgpMime("wQoDAAAAAAAAAAAB0gUBqrvM3Q==");
            passenger.Replace("\r\n--x--\r\n", "\r\n--x\r\nContent-Type: text/plain\r\n\r\nReadable text beside the ciphertext.\r\n--x--\r\n");
            Require(passenger.Find("Readable text") > 0, "The passenger fixture was not built.");
            Require(!GatewayEncryption::IsEncryptedEndToEnd(passenger),
                    "PGP/MIME with real ciphertext and a readable third part was taken for wholly encrypted.");

            AnsiString misnamed = PgpMime("wQoDAAAAAAAAAAAB0gUBqrvM3Q==");
            misnamed.Replace("Content-Type: application/octet-stream\r\n", "Content-Type: application/octet-stream; name=\"invoice.exe\"\r\n");
            Require(misnamed.Find("invoice.exe") > 0, "The misnamed fixture was not built.");
            Require(!GatewayEncryption::IsEncryptedEndToEnd(misnamed),
                    "PGP/MIME whose ciphertext part is named invoice.exe was taken for encrypted.");
         }

         // S/MIME: the CMS content type, whatever the label says.
         Require(GatewayEncryption::IsEncryptedEndToEnd("Subject: s\r\nContent-Type: application/pkcs7-mime; smime-type=enveloped-data; name=smime.p7m\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA=\r\n"),
                 "S/MIME enveloped-data was not recognised.");
         Require(GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/pkcs7-mime;\r\n smime-type=authEnveloped-data\r\nContent-Transfer-Encoding: base64\r\n\r\nMIID6AYLKoZIhvcNAQkQARegggPZAAAAAAAA\r\n"),
                 "S/MIME authEnveloped-data, folded, with a long-form length, was not recognised.");
         Require(GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/x-pkcs7-mime; name=smime.p7m\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA=\r\n"),
                 "An older client's enveloped-data, with no smime-type, was not recognised.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/x-pkcs7-mime; name=smime.p7m\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHAqCAMIACAQEAAAAAAAA=\r\n"),
                 "An S/MIME v2 opaque SIGNED message, labelled with no smime-type, was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/pkcs7-mime; smime-type=signed-data; name=\"enveloped-data.p7m\"\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHAqCAMIACAQEAAAAAAAA=\r\n"),
                 "Signed data named enveloped-data in a parameter was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=smime.p7m\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHAqCAMIACAQEAAAAAAAA=\r\n"),
                 "Signed data labelled enveloped-data was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=smime.p7m\r\n\r\nMIIB\r\n"),
                 "A label with no CMS object under it was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=x\r\n\r\n--x\r\n"),
                 "A clear-signed message was taken for encrypted.");
         Require(!GatewayEncryption::IsEncryptedEndToEnd("Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=invoice.exe\r\nContent-Transfer-Encoding: base64\r\n\r\nMIAGCSqGSIb3DQEHA6CAMIACAQAAAAAAAAA=\r\n"),
                 "Enveloped-data named invoice.exe was taken for a message wholly under a key.");
      }

      // ---- what Inspect refuses without needing a key, private-key material first
      {
         Require(!GatewayEncryption::Inspect(RemoteEncryptOpenPgp, _T(""), _T("partner.test"), now).usable, "No key at all was usable.");

         const GatewayEncryption::KeyFacts secret = GatewayEncryption::Inspect(RemoteEncryptOpenPgp,
            _T("-----BEGIN PGP PRIVATE KEY BLOCK-----\n\nAAAA\n-----END PGP PRIVATE KEY BLOCK-----\n"), _T("partner.test"), now);
         Require(!secret.usable && secret.problem.Find("SECRET") >= 0, "An OpenPGP private key was not refused by name.");

         const GatewayEncryption::KeyFacts privatePem = GatewayEncryption::Inspect(RemoteEncryptSmime,
            _T("-----BEGIN PRIVATE KEY-----\nMIIB\n-----END PRIVATE KEY-----\n"), _T("partner.test"), now);
         Require(!privatePem.usable && privatePem.problem.Find("PRIVATE") >= 0, "A PEM private key was not refused by name.");

         // Whatever the method, and whatever the kind of the key beside it.
         const GatewayEncryption::KeyFacts privateUnderNone = GatewayEncryption::Inspect(RemoteEncryptOpenPgp,
            _T("-----BEGIN EC PRIVATE KEY-----\nMHcCAQEE\n-----END EC PRIVATE KEY-----\n"), _T("partner.test"), now);
         Require(!privateUnderNone.usable && privateUnderNone.problem.Find("PRIVATE") >= 0, "A PEM EC private key given as an OpenPGP key was not refused as private.");

         const char *labels[] = { "-----BEGIN RSA PRIVATE KEY-----", "-----BEGIN ENCRYPTED PRIVATE KEY-----", "-----BEGIN EC PRIVATE KEY-----",
                                  "-----BEGIN OPENSSH PRIVATE KEY-----", "-----BEGIN PGP SECRET KEY BLOCK-----", "-----BEGIN PGP PRIVATE KEY BLOCK-----" };
         for (const char *label : labels)
            Require(GatewayEncryption::ContainsPrivateKey(AnsiString("junk\n") + label + "\nAAAA\n"), "A private-key label was not recognised.");
         Require(!GatewayEncryption::ContainsPrivateKey("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n") &&
                 !GatewayEncryption::ContainsPrivateKey("-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nmDME\n-----END PGP PUBLIC KEY BLOCK-----\n"),
                 "A certificate or a public key was taken for a private key.");

         AnsiString problem;
         Require(GatewayEncryption::AcceptableKeyText("", problem) && GatewayEncryption::AcceptableKeyText("  \r\n ", problem),
                 "No key at all was refused as key text.");
         const AnsiString tooLong(std::string((size_t) GatewayEncryption::MaximumKeyCharacters + 1, 'A'));
         Require(!GatewayEncryption::AcceptableKeyText(tooLong, problem) && problem.Find("characters") >= 0, "A paste over the ceiling was not refused.");

         Require(!GatewayEncryption::Inspect(RemoteEncryptSmime, _T("hello"), _T("partner.test"), now).usable, "Plain words were taken for a certificate.");

         const GatewayEncryption::KeyFacts garbage = GatewayEncryption::Inspect(RemoteEncryptSmime,
            _T("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"), _T("partner.test"), now);
         Require(!garbage.usable && garbage.problem.Find("could not be read") >= 0, "A PEM block that is not a certificate was not refused.");

         const GatewayEncryption::KeyFacts crossed = GatewayEncryption::Inspect(RemoteEncryptOpenPgp,
            _T("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"), _T("partner.test"), now);
         Require(!crossed.usable && crossed.problem.Find("S/MIME") >= 0, "A certificate given as an OpenPGP key did not say which method it is for.");

         Require(GatewayEncryption::MethodOfKey(_T("-----BEGIN CERTIFICATE-----\nMIIB")) == RemoteEncryptSmime &&
                 GatewayEncryption::MethodOfKey(_T("mDMEaq0hphYJ")) == RemoteEncryptOpenPgp &&
                 GatewayEncryption::MethodOfKey(_T("")) == RemoteEncryptNone,
                 "A held key's method was not read from its shape.");

         Require(GatewayEncryption::ExpiryText(0).IsEmpty() && GatewayEncryption::ExpiryText(now) == "2027-01-15T08:00:00Z",
                 "An expiry is not written as ISO 8601 in UTC.");
      }

      // ---- OpenPGP: the fixture keys, usable, named, and a message sealed to them
      {
         const String alice =
            _T("mDMEaq0hphYJKwYBBAHaRw8BAQdAogB/YdOhOFS5ajaz62eLUepoNXY/3nTQgUXElQCO+fy0I0Fs")
            _T("aWNlIEZpeHR1cmUgPGFsaWNlQGZpeHR1cmVzLnRlc3Q+iJAEExYKADgWIQQ8b/XjRYFz3V2z9KSh")
            _T("QmPJfeaFfQUCaq0hpgIbAwULCQgHAgYVCgkICwIEFgIDAQIeAQIXgAAKCRChQmPJfeaFfaSaAP4z")
            _T("1aPIosKBxbw2o6g4StHegDSHti6i6BaakoCP3DqqoAEAvvu1HHt590Ku50WUoHSMaO9EdiPdNpDF")
            _T("lOZv3UVyfAi4OARqrSGmEgorBgEEAZdVAQUBAQdAGQu/CHDJ97RxQz9zk4Jk8BUAvFvLbNRtnTzZ")
            _T("ll8CQzEDAQgHiHgEGBYKACAWIQQ8b/XjRYFz3V2z9KShQmPJfeaFfQUCaq0hpgIbDAAKCRChQmPJ")
            _T("feaFfTkvAP9vpnBYjBI3tYEJqpJteokGrh4/EWyM3WNO/DUJtc62aAEAukeUO3aWgpkHejO2t5W8")
            _T("4vVO3MeX4UFOng31q2kkOA0=");

         const GatewayEncryption::KeyFacts named = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, alice, _T("fixtures.test"), now);
         Require(named.usable, (AnsiString("The fixture key was refused: ") + named.problem).c_str());
         Require(named.fingerprint == "3c6ff5e3458173dd5db3f4a4a14263c97de6857d", "The OpenPGP fingerprint is not the primary key's as gpg printed it.");
         Require(named.subkey_fingerprint == "4a144aada87aad8cce8a0659eaa03821629d0473", "The subkey encrypted to is not named, or is not the one gpg encrypts to.");
         Require(GatewayEncryption::KeyName(named) == "3c6ff5e3458173dd5db3f4a4a14263c97de6857d subkey 4a144aada87aad8cce8a0659eaa03821629d0473",
                 "The key's name does not carry both fingerprints.");
         Require(named.subject.Find("alice@fixtures.test") >= 0, "The OpenPGP subject is not the primary user id.");
         Require(named.expires == 0 && named.warning.IsEmpty(), "A key that does not expire and names the domain carried an expiry or a warning.");
         Require(GatewayEncryption::Summary(RemoteEncryptOpenPgp, alice, _T("fixtures.test"), now).Find("subkey 4a144aada87aad8cce8a0659eaa03821629d0473") >= 0,
                 "The summary does not name the subkey messages are encrypted to.");

         const GatewayEncryption::KeyFacts elsewhere = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, alice, _T("partner.test"), now);
         Require(elsewhere.usable && elsewhere.warning.Find("partner.test") >= 0, "A key naming another domain was refused, or accepted without the warning.");

         AnsiString sealed, problem;
         GatewayEncryption::KeyFacts facts;
         Require(GatewayEncryption::Seal(RemoteEncryptOpenPgp, alice, original, now, sealed, facts, problem) == GatewayEncryption::Sealed,
                 (AnsiString("The message was not sealed to the OpenPGP key: ") + problem).c_str());
         Require(facts.fingerprint == named.fingerprint && facts.subkey_fingerprint == named.subkey_fingerprint, "The seal did not name the key it was made to.");
         Require(OpenPgpWriter::IsPgpMimeEncrypted(sealed) && GatewayEncryption::IsEncryptedEndToEnd(sealed), "The OpenPGP copy is not PGP/MIME under a key.");
         Require(sealed.Find("-----BEGIN PGP MESSAGE-----") >= 0 && sealed.Find("Version: 1\r\n") >= 0, "The OpenPGP copy is not shaped as RFC 3156 says.");
         Require(sealed.Find("Subject: The subject stays\r\n") >= 0 && sealed.Find("Message-ID: <one@example.test>\r\n") >= 0, "The OpenPGP copy lost a kept header.");
         Require(sealed.Find("The body goes under the key.") < 0 && sealed.Find("DKIM-Signature") < 0, "The OpenPGP copy carries the plaintext or the old signature.");
         Require(sealed.Find("X-hMailServer-Gateway-Encrypted: OpenPGP to key 3c6ff5e3458173dd5db3f4a4a14263c97de6857d subkey 4a144aada87aad8cce8a0659eaa03821629d0473\r\n") >= 0,
                 "The OpenPGP copy does not carry the mark with both fingerprints.");
         {
            const int first = sealed.Find("MIME-Version:");
            Require(first >= 0 && sealed.Find("MIME-Version:", (size_t) first + 1) < 0, "MIME-Version does not appear exactly once in the OpenPGP copy.");
         }

         // The key cut off after its primary packet: not a key the reader takes.
         const GatewayEncryption::KeyFacts truncated = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, _T("mDMEaq0hphYJKwYBBAHaRw8BAQdAogB/YdOhOFS5ajaz62eLUepoNXY/3nTQgUXElQCO+fy0I0Fs"), _T("fixtures.test"), now);
         Require(!truncated.usable && !truncated.problem.IsEmpty(), "A truncated key was usable, or refused without a sentence.");

         // alice's public key with a PEM private key after it: refused whole.
         const GatewayEncryption::KeyFacts mixed = GatewayEncryption::Inspect(RemoteEncryptOpenPgp,
            alice + _T("\n-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIA==\n-----END PRIVATE KEY-----\n"), _T("fixtures.test"), now);
         Require(!mixed.usable && mixed.problem.Find("PRIVATE") >= 0, "A public key with a PEM private key beside it was accepted.");

         // bob: RSA 3072, his RSA subkey bound by an RSA signature that verifies.
         const String bob =
            _T("-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n")
            _T("mQGNBGqtIagBDADQQirACtpvaYmaRYWH9UnVjqAkN5BvOWSu6ycCTLIq3wX41lfk\n")
            _T("zZH7L4mqYjoF0TokjiRCXcedUmZnhRttrcoh5xXU8qY+rKrfvJhE0TJP+h/JlkQz\n")
            _T("DEmX4ahbFWbprkVR4FBLxF6+VyrgQCmn57WSu2H89T6MwESbA5gYhqGcdELkVdz/\n")
            _T("22LxDu1arHNm2ITPpnfmcfC4RFXOwfILs4vhxFuLL2rp53o3S7qtjHCjgHMKGEQb\n")
            _T("ci/lGKmjvPUIOIbJCL8gWSWDDftP5QLoB/5DrTu+dGBn+Dw/BkOQ9JEerFbEyBlC\n")
            _T("IOghF02g0YYJSo9yrPVDyhUmq+p3kWu6sUYQU73qnEXDNFSJBnzJlbdrAc7lsFQn\n")
            _T("+GecDIH4vH4STIaVIiY3XPMnhLz/OBEL3ykR2JW7csETTIS9GeZmzsxOuh15y0xf\n")
            _T("ofPP5LMxF3+ybz/x22DBSfOyuQmVx4Jea84JXYn5aNIRj+TI/FY76AYLfigJ8ksy\n")
            _T("SVbJ7B4iL6FyKjEAEQEAAbQfQm9iIEZpeHR1cmUgPGJvYkBmaXh0dXJlcy50ZXN0\n")
            _T("PokBzgQTAQoAOBYhBDSyrH5SFDoaggMj0c9odaLsQOzVBQJqrSGoAhsDBQsJCAcC\n")
            _T("BhUKCQgLAgQWAgMBAh4BAheAAAoJEM9odaLsQOzVMB8MAKwDCFVLGT0e51tXgIGd\n")
            _T("Acpm+zfwrnfqYCaI4kb5s/nG9VGBWYH70WUcZ3oHA/Kygs7iE8pkRnv0LvTTJf2V\n")
            _T("Lul/3jkFtsdz7uha0MSczWh1SyFV7JESoVDpR70GcGypK38gz5K76UDdSuAsgsjl\n")
            _T("LcyBPhJ8LuCCtEpJk55fk8/FOv6YmzzUfVfVDdAZQJrGRatlMJAi3iAQGKyiDEF+\n")
            _T("KtoScZYW9RBbD4tiYEYQE04x0D6dWVJBEtkI6mmyzsWmhp8JgqIjUALrn/kyRpQ/\n")
            _T("3Ke7MtoBSwSn4WH1FlmTjjJjNLVPF5b1sVD42FKTLeT3zCZwn3kJary9YYrJA0ed\n")
            _T("kB5wsBmbLYvL8nSAwLH0hp6k2FZUinbycykCH4dOqSF1hNbiERhnxTYGCoBkK8Xc\n")
            _T("q025WhwrBdl3GBlDTsAvsEQ+hi3RpKvZQ4LD/+yQGdMpixxsdN9QcsN8NM2BX0aA\n")
            _T("lCMc+kQndKaK1UmMN2OlKyGbxK7EWfFU57PzP3DQ/zJtOrkBjQRqrSGoAQwA2v/q\n")
            _T("1OAO+5X31FS7TpOGprn5jWKJTskq9rBTLLh4eMCDiM8+iE0EQx0RF21x0sgVoHxg\n")
            _T("AZyAap0/bMVa9zyqli3XbEcNC+BeTFju6TkrW4E61+xnU1UNZE5zHIRyBbPlN5nF\n")
            _T("zQ8bH1+6ZLTtDk7GHYZX/emxbuXb14pfMjO7l+xNOghWnw15EuxBoUUxt9C2sR4D\n")
            _T("LR6oDIKnbjtSdjKMsyn++JaYht9s3gfHP1wfOEVP7pHzRhj5synpDzEyV7sLjI/v\n")
            _T("Rov78AUWixAcHT9OpLEKZYXy0P9CfBmEJMABxTNi4c+NNYSel6yPDEGOFC6hhBiK\n")
            _T("GdjBqJVx1iuoutPY09PDOINKiLUpHpAaXJxn6zMakjNhQhEO/FJNIIKFDkPHopR8\n")
            _T("xh9pF9UggjGKsO8+ihlVjR7wN5NTzEb1rB4PNlILlELY2C22Av6TjYIv2aOv8/h7\n")
            _T("OKOcIMnUsLsdD0Gtz8aL/IrXVYM8xN1jwMBNpvaLsq3tNbCkkSvItGUFmiVbABEB\n")
            _T("AAGJAbYEGAEKACAWIQQ0sqx+UhQ6GoIDI9HPaHWi7EDs1QUCaq0hqAIbDAAKCRDP\n")
            _T("aHWi7EDs1bXmC/9UMYtYAJN8UOBcNebJ1J180tMi5NjrwqyxLPY3myFPjrrmDSct\n")
            _T("AS5xNT9267CEUQ0mxEiGlP758Ym33kYkc66JiIwyhOEHEdRmAJAH+kqJ7NcsUSHv\n")
            _T("TU1eX/l34lwFCCcSDr762rg+rcU/pkFZoryh45f7koZHP5qXWdTTzKMV+MMwdmuq\n")
            _T("9ZnLHsaUqSn5SFdYHmtRW/cZKg1vGlQLtZIEzr7vvOIhfhh6uJT1cnk773Q087Mj\n")
            _T("eU9m0ImEFcEX7rmqfMcKBtZ2hZ6uktN0CHwwmS3+Jd4Pkp8mNCtTe+ATy49zY2dm\n")
            _T("xvygUVgdyuHjCHZjtW5+j5xRha57t7UjEqfk03x5RrP8RgmrrAkYcY2a+765ky1X\n")
            _T("cJ3d/e3coSCUpE80GPyMmbdAvokBMuzPsH9Ph+JbBVFwBMpqdTaiZ1CzR7xlukLI\n")
            _T("ijHc9lnf2i3EBrRSKruJefv467zl0tsnIU09ABppSQrVBZKl4woqqSL3mK3tie3O\n")
            _T("FliobdOY2FyvU7w=\n")
            _T("=0+ys\n")
            _T("-----END PGP PUBLIC KEY BLOCK-----\n");

         const GatewayEncryption::KeyFacts rsa = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, bob, _T("fixtures.test"), now);
         Require(rsa.usable, (AnsiString("The RSA fixture key was refused: ") + rsa.problem).c_str());
         Require(rsa.fingerprint == "34b2ac7e52143a1a820323d1cf6875a2ec40ecd5" && rsa.subkey_fingerprint == "fb3f4769432e95ea8aab059b1e3a18b77792547c",
                 "The RSA key's fingerprints are not the ones gpg printed.");
         AnsiString rsaSealed;
         Require(GatewayEncryption::Seal(RemoteEncryptOpenPgp, bob, original, now, rsaSealed, facts, problem) == GatewayEncryption::Sealed &&
                 GatewayEncryption::IsEncryptedEndToEnd(rsaSealed),
                 (AnsiString("The message was not sealed to the RSA key: ") + problem).c_str());

         // ECDSA P-256 with an ECDH P-256 subkey. The binding is an ECDSA
         // signature that verifies, so the refusal is the subkey's KIND - and
         // not "not bound", which is what it would be if ECDSA were not checked.
         const String p256 =
            _T("mFIEarScrxMIKoZIzj0DAQcCAwQPZIbahcAMs4wrTfgLRcZxXVPn24/PrIXAGBchmfgiGz9RoV2/")
            _T("0FW1L214CDQsPoXY687LMzBN+irhFN56MCI2tCFQMjU2IEZpeHR1cmUgPHAyNTZAZml4dHVyZXMu")
            _T("dGVzdD6IkwQTEwgAOxYhBP1SrVFv9URDN/6AZfEZzMEIVjm8BQJqtJyvAhsDBQsJCAcCAiICBhUK")
            _T("CQgLAgQWAgMBAh4HAheAAAoJEPEZzMEIVjm8ckIA/0d/Bn4jwjoU4ZRJZgJP5qapVFSPAB9jn4/c")
            _T("giuc7dCiAP4kBOOC4ZV/bMdGQT20Duk/xfIWlVYFCeuQYpCMFOMlWbhWBGq0nK8SCCqGSM49AwEH")
            _T("AgMEKwOFiyvMqEx/8oGNlrwMqj0zMho2g/h1b+8ZipO/PLtnVG8qW+/RXpOAaJrmqBVII95w8xab")
            _T("E1rZrBZcJyfpjgMBCAeIeAQYEwgAIBYhBP1SrVFv9URDN/6AZfEZzMEIVjm8BQJqtJyvAhsMAAoJ")
            _T("EPEZzMEIVjm8vDUA/Ru+W2GSJbIhRvH82UgqHaJGy96lZmJ2NKuNivCzjnRLAQCFq2OhMWkEQJEK")
            _T("eEKHzxGOEyZWTTBw0Ez5ps6iJLeEFQ==");
         const GatewayEncryption::KeyFacts nist = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, p256, _T("fixtures.test"), now);
         Require(!nist.usable && nist.problem.Find("of a kind this server does not write to") >= 0 && nist.fingerprint == "fd52ad516ff5444337fe8065f119ccc1085639bc",
                 (AnsiString("A P-256 key was not refused for its subkey's kind: ") + nist.problem).c_str());

         // RSA 1024, bound properly, and too short to encrypt to.
         const String smallKey =
            _T("mI0EarScrwEEANLzM35NkWDcpW3wNwytwo0pIDkDO0pcD1+Yi/uGZeFzkrljs36UyucRPYMXb6En")
            _T("DURGHywkmAxl8dGtxMIuq3H//Qfyg1OUqAIYBT4ClPSbLl66mXmXipt/KMsmytQjLd9B2A43oWge")
            _T("LgcKjEdgbtYVmC7n9t2ggOnNde+fv+yhABEBAAG0I1NtYWxsIEZpeHR1cmUgPHNtYWxsQGZpeHR1")
            _T("cmVzLnRlc3Q+iNEEEwEIADsWIQRPaGcnKcGcJtMSSCMnREfZHQdWDQUCarScrwIbAwULCQgHAgIi")
            _T("AgYVCgkICwIEFgIDAQIeBwIXgAAKCRAnREfZHQdWDbYwBAC3FqTONz9R7zVh3L16gWaVmeWVU9ap")
            _T("7uIOgHtetF2ZILhpZPzG8rLqrUCJ93g/0l7yFM55s8bRJiTlENhyXe6CjGh0S3PWOMy7O4+HP0l6")
            _T("HIeJRr15GhI94BWT+yTKKEv6umJNR6PCbnwvXcnpAiyrHq0qb+mEvKNsEtmoSY10U7iNBGq0nK8B")
            _T("BAD1kN0GUwV5oslGi7zZjtg2RtUF4oBdBQjraJyM9eA2AVSA5OQzkBHew83PAKUdU3kpUehoP3jp")
            _T("Ewgp6gLFerV5p8E7Dxx2yn54BFrUa6xcdwfhB1TAdxo74HIYud1uCzM9q/1UKXwREB2aWPio4/Er")
            _T("xELCFSntXvAEskhWKIOnqwARAQABiLYEGAEIACAWIQRPaGcnKcGcJtMSSCMnREfZHQdWDQUCarSc")
            _T("rwIbDAAKCRAnREfZHQdWDVn+A/9iKWhbUkEGOYczIHAakvHQ+bPGWY3V1+3ibD7KA6hn6FoD5Chs")
            _T("VLftb6PeZZDnRjdmUJoL1vHx87bQi15AG9LedGEmWeiu3nXE2/+PvWcrPr6yzQi/MvglilhXSDK4")
            _T("6sJpfygJdQbW/dO4FEc8ThW+W/ME5DYxwbw1IWuqF7JDjg==");
         const GatewayEncryption::KeyFacts weak = GatewayEncryption::Inspect(RemoteEncryptOpenPgp, smallKey, _T("fixtures.test"), now);
         Require(!weak.usable && weak.problem.Find("RSA of 1024 bits") >= 0,
                 (AnsiString("An RSA 1024 OpenPGP key was not refused for its size: ") + weak.problem).c_str());
      }

      // ---- S/MIME: the test certificate, usable, named, and a message enveloped to it by RSAES-OAEP
      {
         const String certificate =
            _T("-----BEGIN CERTIFICATE-----\n")
            _T("MIIDfzCCAmegAwIBAgIUdZNi4gbXpNnptmO9rZ7UP0n7z/swDQYJKoZIhvcNAQEL\n")
            _T("BQAwPjEdMBsGA1UEAwwUZ2F0ZXdheS5wYXJ0bmVyLnRlc3QxHTAbBgNVBAoMFFBh\n")
            _T("cnRuZXIgVGVzdCBHYXRld2F5MB4XDTI2MDkyNDAyMzEzMVoXDTM2MDkyMTAyMzEz\n")
            _T("MVowPjEdMBsGA1UEAwwUZ2F0ZXdheS5wYXJ0bmVyLnRlc3QxHTAbBgNVBAoMFFBh\n")
            _T("cnRuZXIgVGVzdCBHYXRld2F5MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC\n")
            _T("AQEAysGmvzJaryE5Hi/rgTW1sU3D34G8RAxqj1R9Xm+R2lW7rc2bCP1hOFRJkM98\n")
            _T("IcE77v/kQNWfL1N4NolmJgHo7kXmcpdLdtCsocVoYUNyCmJsYlJcEW9WQUyHgRk4\n")
            _T("4t4vNsJ8wOyUX1sjxI5b0PZVIU65N7PV85KV2bouMUw4kTH3/GyTz1qPvIivh3qx\n")
            _T("kjLvMSJj3fUAKbdOc3z0tuzhQtWjXxs3nvfBH9mhYDO63K6ZoIZR3jv7XxTf0WrN\n")
            _T("gxji69qN/jWDaF3Az9Ek8cdHEW8PbGRubTMjibS7NWLYasUViYQC2mDGEg3dPI/c\n")
            _T("p8AL/KRw6uzvvSwbnl2pgZOjxwIDAQABo3UwczAMBgNVHRMBAf8EAjAAMA4GA1Ud\n")
            _T("DwEB/wQEAwIFoDATBgNVHSUEDDAKBggrBgEFBQcDBDAfBgNVHREEGDAWgRRnYXRl\n")
            _T("d2F5QHBhcnRuZXIudGVzdDAdBgNVHQ4EFgQUBmb+23eZH0T92DTpsCRnalp0g9Ew\n")
            _T("DQYJKoZIhvcNAQELBQADggEBAGgowXY/KyGO6MIZpc+IgACM0ldUevSIaUij1HsJ\n")
            _T("j9hoW6P2t7SOhfP7gIR1J3rXcat0WxbztM3h9lHV9DhMh4nPX0CBY0jlscM0MczB\n")
            _T("/pFdF02rmqAwGi4jGsms0+lnukWbjxKYZ4XZmaQLwRDfE0zgwhWgdP41240k1v/I\n")
            _T("JHBKLFeE2HDBjsgMvl8TRhe/wW6S14F/THf1jBbgSPBVoITxea6GuJF99YBua51o\n")
            _T("fVgiF6C4gnuGHJP39qOLJ3EQ1s9UbS3PASBk4PStUW0HC8B2vbJCEZ3E/Q0Swc2J\n")
            _T("GpDKXEh0AcHXp9qKHAsIvhcKU+ziQwCVitKolPHkxFvY8Y0=\n")
            _T("-----END CERTIFICATE-----\n");

         const GatewayEncryption::KeyFacts named = GatewayEncryption::Inspect(RemoteEncryptSmime, certificate, _T("partner.test"), now);
         Require(named.usable, (AnsiString("The test certificate was refused: ") + named.problem).c_str());
         Require(named.fingerprint == "f94894af1dea80795af02d9e52e6fad0d436dc318c18ac4c8250842714a412d5", "The S/MIME fingerprint is not the certificate's SHA-256 as openssl printed it.");
         Require(named.subkey_fingerprint.IsEmpty() && GatewayEncryption::KeyName(named) == named.fingerprint, "A certificate was named with a subkey.");
         Require(named.expires == 2105577091, "The certificate's expiry is not its notAfter.");
         Require(named.subject.Find("gateway.partner.test") >= 0, "The certificate's subject was not read.");
         Require(named.warning.IsEmpty(), (AnsiString("A certificate for the partner's own domain carried a warning: ") + named.warning).c_str());

         const GatewayEncryption::KeyFacts elsewhere = GatewayEncryption::Inspect(RemoteEncryptSmime, certificate, _T("other.test"), now);
         Require(elsewhere.usable && elsewhere.warning.Find("other.test") >= 0, "A certificate for another domain was refused, or accepted without the warning.");

         const GatewayEncryption::KeyFacts expired = GatewayEncryption::Inspect(RemoteEncryptSmime, certificate, _T("partner.test"), 2105577091 + 1);
         Require(!expired.usable && expired.problem.Find("expired") >= 0, "An expired certificate was usable.");

         const GatewayEncryption::KeyFacts early = GatewayEncryption::Inspect(RemoteEncryptSmime, certificate, _T("partner.test"), 1700000000);
         Require(!early.usable && early.problem.Find("not valid until") >= 0, "A certificate not yet valid was usable.");

         // The certificate with an OpenPGP secret key block after it: refused whole.
         const GatewayEncryption::KeyFacts mixed = GatewayEncryption::Inspect(RemoteEncryptSmime,
            certificate + _T("-----BEGIN PGP PRIVATE KEY BLOCK-----\n\nlFgE\n-----END PGP PRIVATE KEY BLOCK-----\n"), _T("partner.test"), now);
         Require(!mixed.usable && mixed.problem.Find("SECRET") >= 0, "A certificate with an OpenPGP secret key beside it was accepted.");

         AnsiString sealed, problem;
         GatewayEncryption::KeyFacts facts;
         Require(GatewayEncryption::Seal(RemoteEncryptSmime, certificate, original, now, sealed, facts, problem) == GatewayEncryption::Sealed,
                 (AnsiString("The message was not enveloped to the certificate: ") + problem).c_str());
         Require(sealed.Find("Content-Type: application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"\r\n") >= 0 &&
                 sealed.Find("Content-Disposition: attachment; filename=\"smime.p7m\"\r\n") >= 0 &&
                 sealed.Find("Content-Transfer-Encoding: base64\r\n") >= 0,
                 "The S/MIME copy is not shaped as RFC 8551 says.");
         Require(GatewayEncryption::IsEncryptedEndToEnd(sealed), "The S/MIME copy is not recognised as enveloped.");
         Require(RecipientAlgorithmOf(sealed) == NID_rsaesOaep, "The content key was not sent to the RSA certificate by RSAES-OAEP.");
         Require(sealed.Find("Subject: The subject stays\r\n") >= 0 && sealed.Find("The body goes under the key.") < 0, "The S/MIME copy lost the subject or carries the plaintext.");
         Require(sealed.Find("X-hMailServer-Gateway-Encrypted: S/MIME to key f94894af1dea80795af02d9e52e6fad0d436dc318c18ac4c8250842714a412d5\r\n") >= 0, "The S/MIME copy does not carry the mark.");

         AnsiString refused, why;
         Require(GatewayEncryption::Seal(RemoteEncryptSmime, certificate, original, 2105577091 + 1, refused, facts, why) == GatewayEncryption::Failed &&
                 why.Find("expired") >= 0 && refused.IsEmpty(),
                 "A certificate that expired after it was saved was still enveloped to, or its refusal was not a permanent one.");
      }

      // ---- S/MIME: the kinds of certificate that are refused, and the elliptic-curve one that is not
      {
         // RSA 2048 with digitalSignature only: a signing certificate.
         const String signingOnly =
            _T("-----BEGIN CERTIFICATE-----\n")
            _T("MIIDQzCCAiugAwIBAgIUKrMESlR1AUPGTn7uhub+lSw+o48wDQYJKoZIhvcNAQEL\n")
            _T("BQAwIDEeMBwGA1UEAwwVc2lnbm9ubHkucGFydG5lci50ZXN0MB4XDTI2MDkyNDAz\n")
            _T("NDUwNVoXDTM2MDkyMTAzNDUwNVowIDEeMBwGA1UEAwwVc2lnbm9ubHkucGFydG5l\n")
            _T("ci50ZXN0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtl34dAYf8tsn\n")
            _T("F6WrcEkfcidBUA5HA+hWuma2XcnJ0PnI4sWCCL2Ariafi797ITx8RK5jUDDK5ZkA\n")
            _T("O4eXQEodhvKQJ02Bh1BWJPrgLev4hcwjpProCNLV5e78olVBTWs4iaJ6X6zOPe26\n")
            _T("qhr6tNIwM71NlX4F3IufB+/iYdiBTwNyQHl8I7N+1uR5pt9Rn/vNvySL7/pJIIiW\n")
            _T("pQXVDB6+PmMze69nuvtMnhW/6FDKzvb314crA1pmZ55Hm0NIvGzRQHNry0x74oTe\n")
            _T("chFVe0RwlO10u551uDLrQVIaoBj/VwFxV+W+waBZHCwDyS0S3zFzMJf+R6o6a4gO\n")
            _T("ZLvdhYK9/QIDAQABo3UwczAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIHgDAT\n")
            _T("BgNVHSUEDDAKBggrBgEFBQcDBDAfBgNVHREEGDAWgRRnYXRld2F5QHBhcnRuZXIu\n")
            _T("dGVzdDAdBgNVHQ4EFgQU52sqs3/iZhNdlvAMYSxa9nhi18kwDQYJKoZIhvcNAQEL\n")
            _T("BQADggEBADtVGiLnzESSWLSwg3XhWpIZ/9TVG4vLOa3LNYKlBWaIHQaTSXpN7cGF\n")
            _T("dZ76N/FSyHXuIsAGaAZc5zWIXUrS1xboAI0bhdgz9SZjdOdzcklpTc8cv017eL+I\n")
            _T("++1WMaHHAGGjb/lFspLXoHirYmxpW2mMotAkpb+sCQ28PJmxHTTd//n5UcwOKg0a\n")
            _T("jNJotBsFeDi9+ehOKwe1xnRkoKIM0/O06pNTPTVMFbTFJCXo/qFhVoL1tNOX4t0s\n")
            _T("nIyNxao/8ZcFgu/R1QkOSpbmA+CX45RFAmNqhU2GNyriQRbe4t8FXFFFCmxDut5l\n")
            _T("vOKt/UHOqeGZh4sNr1g1odpjG2zAiuk=\n")
            _T("-----END CERTIFICATE-----\n");
         const GatewayEncryption::KeyFacts signing = GatewayEncryption::Inspect(RemoteEncryptSmime, signingOnly, _T("partner.test"), now);
         Require(!signing.usable && signing.problem.Find("keyEncipherment") >= 0 &&
                 signing.fingerprint == "a0a958aa5d0cb4cfe09552cc14406708f70afe1ac8a55f2c904cdf12b2102d2a",
                 "An RSA certificate without keyEncipherment was not refused for its key usage.");

         // Ed25519: a key that signs and cannot be encrypted to.
         const String edwards =
            _T("-----BEGIN CERTIFICATE-----\n")
            _T("MIIBeTCCASugAwIBAgIUG6f6lTd9C5GYOQzHPwOrrttyr3UwBQYDK2VwMCExHzAd\n")
            _T("BgNVBAMMFmVkZ2F0ZXdheS5wYXJ0bmVyLnRlc3QwHhcNMjYwOTI0MDM0NTA1WhcN\n")
            _T("MzYwOTIxMDM0NTA1WjAhMR8wHQYDVQQDDBZlZGdhdGV3YXkucGFydG5lci50ZXN0\n")
            _T("MCowBQYDK2VwAyEAfQ+MkOm3LYCz3kvx6jP2Fb0faMYCxiXjM9jOjiX8H62jdTBz\n")
            _T("MAwGA1UdEwEB/wQCMAAwDgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUF\n")
            _T("BwMEMB8GA1UdEQQYMBaBFGdhdGV3YXlAcGFydG5lci50ZXN0MB0GA1UdDgQWBBT+\n")
            _T("1Ksekd9XPZvSSikoFxxGt8tKjjAFBgMrZXADQQCv6vaGXkw0a/JoY0fiUFXQmVWk\n")
            _T("pps3hupUP3mwnQ6CFmr30dQ9rnD60ACa87ghNS9tySyCQYr7Dah9JlGceq4N\n")
            _T("-----END CERTIFICATE-----\n");
         const GatewayEncryption::KeyFacts ed = GatewayEncryption::Inspect(RemoteEncryptSmime, edwards, _T("partner.test"), now);
         Require(!ed.usable && ed.problem.Find("RSA or an elliptic-curve key") >= 0, "An Ed25519 certificate was not refused for its key's kind.");

         // RSA 1024 with keyEncipherment: the right usage, too short a key.
         const String shortRsa =
            _T("-----BEGIN CERTIFICATE-----\n")
            _T("MIICPjCCAaegAwIBAgIUPHtqD0STXe9cDUoQ7STmKOd9dOIwDQYJKoZIhvcNAQEL\n")
            _T("BQAwIDEeMBwGA1UEAwwVc21hbGxyc2EucGFydG5lci50ZXN0MB4XDTI2MDkyNDAz\n")
            _T("NDUwNloXDTM2MDkyMTAzNDUwNlowIDEeMBwGA1UEAwwVc21hbGxyc2EucGFydG5l\n")
            _T("ci50ZXN0MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDEss21kounH0+xmYca\n")
            _T("YHW8KkM4wz7aQg+gIS421Yb11lJ9jrIpvGSAwCIz4e9T2ttE9qybwNc246k5xnWg\n")
            _T("PTs9Ie4ORBaTcyLlloXV0/x0qTkqWXu9c7ThUysAk2Y3pz+M+lIjt89ZbSK+oE8A\n")
            _T("He1/kbM2CZGuYVpwtCi/6EtkDwIDAQABo3UwczAMBgNVHRMBAf8EAjAAMA4GA1Ud\n")
            _T("DwEB/wQEAwIFIDATBgNVHSUEDDAKBggrBgEFBQcDBDAfBgNVHREEGDAWgRRnYXRl\n")
            _T("d2F5QHBhcnRuZXIudGVzdDAdBgNVHQ4EFgQUaUd76bNYBTbwbvo5p4KcgCWFRT4w\n")
            _T("DQYJKoZIhvcNAQELBQADgYEAI26tTkPPZLVsdsxAZv4/93yN/k1xtlUehIcjmsgl\n")
            _T("yUuFkdjk9En3VVJhfi7fbP+P5SnljEmP8k7EcP8fpuCqICdD9cWz1MA3E4tcHr2R\n")
            _T("jrvV4ekI+kljX/FFH8Pe0h8+blh+Sqc/ldM/4uHWFGpsZvXyKiZrtSGXeoiloy+/\n")
            _T("GKY=\n")
            _T("-----END CERTIFICATE-----\n");
         const GatewayEncryption::KeyFacts shortKey = GatewayEncryption::Inspect(RemoteEncryptSmime, shortRsa, _T("partner.test"), now);
         Require(!shortKey.usable && shortKey.problem.Find("RSA of 1024 bits") >= 0, "An RSA 1024 certificate was not refused for its size.");

         // P-256 with keyAgreement: enveloped by ECDH, the KDF over SHA-256.
         const String curve =
            _T("-----BEGIN CERTIFICATE-----\n")
            _T("MIIBuTCCAV+gAwIBAgIUa+XfUc0vtMTyDGaSLp5JmN1ubl8wCgYIKoZIzj0EAwIw\n")
            _T("ITEfMB0GA1UEAwwWZWNnYXRld2F5LnBhcnRuZXIudGVzdDAeFw0yNjA5MjQwMzQ1\n")
            _T("MDVaFw0zNjA5MjEwMzQ1MDVaMCExHzAdBgNVBAMMFmVjZ2F0ZXdheS5wYXJ0bmVy\n")
            _T("LnRlc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQm+ZJsV+m85dQ73SPOBssb\n")
            _T("qk7FOQpdnQFTpRWUISknJygUhL55yRwpRM93hAdPzpPSWQtTVbimLeL9e3bz/0LJ\n")
            _T("o3UwczAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIDCDATBgNVHSUEDDAKBggr\n")
            _T("BgEFBQcDBDAfBgNVHREEGDAWgRRnYXRld2F5QHBhcnRuZXIudGVzdDAdBgNVHQ4E\n")
            _T("FgQUAsTCBCEu7xbgHPlMiQ3W6qYZwl4wCgYIKoZIzj0EAwIDSAAwRQIgQJwqXctw\n")
            _T("Ha/AypaRi0SicNbnFNQ2FwtbbQfbUrsFWboCIQDWYr46dSXBOvc9bftmkha2kxnM\n")
            _T("jVPjRFRincbjlrByMw==\n")
            _T("-----END CERTIFICATE-----\n");
         const GatewayEncryption::KeyFacts ec = GatewayEncryption::Inspect(RemoteEncryptSmime, curve, _T("partner.test"), now);
         Require(ec.usable && ec.fingerprint == "522f3a467670663ef2ea05c7df45455ba76ca00d099bdec602dcf56797ad7557",
                 (AnsiString("The P-256 certificate was refused: ") + ec.problem).c_str());

         AnsiString sealed, problem;
         GatewayEncryption::KeyFacts facts;
         Require(GatewayEncryption::Seal(RemoteEncryptSmime, curve, original, now, sealed, facts, problem) == GatewayEncryption::Sealed &&
                 GatewayEncryption::IsEncryptedEndToEnd(sealed),
                 (AnsiString("The message was not enveloped to the P-256 certificate: ") + problem).c_str());
         Require(RecipientAlgorithmOf(sealed) == NID_dhSinglePass_stdDH_sha256kdf_scheme,
                 "The content key was not agreed with the P-256 certificate by ECDH with the SHA-256 KDF.");
      }
   }
}
