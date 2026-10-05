// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SamlSignIn.h"
#include "GatewayEncryption.h"
#include "HttpsClient.h"
#include "SignInState.h"
#include "Unicode.h"
#include "WebAuthn.h"
#include "XmlDocument.h"
#include "XmlSignature.h"
#include "../Application/Configuration.h"
// zlib: on Windows the copy vendored under Server/zlib, which hMailServer.exe
// compiles in; on POSIX the distribution's, whose library CMakeLists.txt links,
// so the header and the library are one version (and the source tarball can
// leave Server/zlib out: build/source-exclusions.txt).
#ifdef HM_PLATFORM_POSIX
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
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
      const char *ProtocolNamespace = "urn:oasis:names:tc:SAML:2.0:protocol";
      const char *AssertionNamespace = "urn:oasis:names:tc:SAML:2.0:assertion";
      const char *StatusSuccess = "urn:oasis:names:tc:SAML:2.0:status:Success";
      const char *BearerMethod = "urn:oasis:names:tc:SAML:2.0:cm:bearer";
      const char *EmailAddressFormat = "urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress";
      const char *DefaultEmailAttribute = "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress";

      // The error this unit reports (7320-7329 are SAML's).
      const int ErrorCertificateUnreadable = 7320;

      // The sign-ins the ACS verified, waiting for their browsers: made only
      // after a signed answer, dropped oldest first as they always were.
      const size_t MostFinished = 200;
      const size_t MostSeen = 5000;
      const int MinimumRsaBits = 2048;
      const int MinimumEcBits = 256;
      const int MostCertificates = 2;

      // A sign-in the ACS verified, waiting for its browser at the finish: the
      // ID of the request it answered, which the hmsaml cookie of the browser
      // that started it holds, sealed.
      struct Finished
      {
         OidcSignIn::Result result;
         std::string request_id;
         time_t created = 0;
         unsigned __int64 order = 0;
      };

      struct Seen
      {
         time_t until = 0;
         unsigned __int64 order = 0;
      };

      boost::mutex stateLock;
      std::map<std::string, Finished> finished;
      std::map<std::string, Seen> seen;
      unsigned __int64 nextOrder = 1;

      const char *DefaultButtonLabel = "Sign in with SAML";

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
      }

      String FromUtf8(const std::string &bytes)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(bytes.c_str()), text);
         return text;
      }

      AnsiString Setting(const TCHAR *name)
      {
         AnsiString value = Utf8(Configuration::Instance()->GetSignInString(name));
         value.Trim();
         return value;
      }

      std::string Text(const AnsiString &value)
      {
         return std::string(value.c_str(), (size_t) value.GetLength());
      }

      AnsiString Trimmed(AnsiString text)
      {
         text.Trim();
         while (text.EndsWith("/"))
            text = text.Mid(0, text.GetLength() - 1);
         return text;
      }

      std::string Stripped(const std::string &text)
      {
         size_t first = 0;
         size_t last = text.size();
         while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\n' || text[first] == '\r'))
            first++;
         while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\n' || text[last - 1] == '\r'))
            last--;
         return text.substr(first, last - first);
      }

      // A value from the answer, for a sentence the log and the page show: cut
      // short, and a control character - a line break a character reference
      // put in an attribute - shown as a question mark.
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

      // Whether an element holds another element: a NameID or an attribute
      // value is text, and one with markup inside it is not an address.
      bool HoldsElements(const XmlNode &node)
      {
         return !node.ElementChildren().empty();
      }

      bool SameText(const std::string &a, const std::string &b)
      {
         return a.size() == b.size() && !a.empty() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
      }

      std::string Hex(const unsigned char *bytes, size_t length, bool upper, const char *separator)
      {
         static const char *lowerDigits = "0123456789abcdef";
         static const char *upperDigits = "0123456789ABCDEF";
         const char *digits = upper ? upperDigits : lowerDigits;

         std::string out;
         for (size_t i = 0; i < length; i++)
         {
            if (i > 0 && separator != nullptr)
               out += separator;
            out += digits[bytes[i] >> 4];
            out += digits[bytes[i] & 0x0F];
         }
         return out;
      }

      std::string XmlEscape(const std::string &text)
      {
         std::string out;
         for (char c : text)
         {
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
            }
         }
         return out;
      }

      __int64 DaysFromCivil(int year, int month, int day)
      {
         year -= month <= 2 ? 1 : 0;
         const int era = (year >= 0 ? year : year - 399) / 400;
         const int yearOfEra = year - era * 400;
         const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
         const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
         return (__int64) era * 146097 + (__int64) dayOfEra - 719468;
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

      // An instant as SAML writes one: xs:dateTime, UTC, to the second.
      std::string TimeText(__int64 seconds)
      {
         struct tm parts;
         if (!UtcParts(seconds, parts))
            return std::string();

         char text[32];
         snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", parts.tm_year + 1900, parts.tm_mon + 1,
                  parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
         return std::string(text);
      }

      __int64 EpochOf(const ASN1_TIME *moment)
      {
         struct tm parts = {};
         if (moment == nullptr || ASN1_TIME_to_tm(moment, &parts) != 1)
            return 0;

         return DaysFromCivil(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday) * 86400 +
                parts.tm_hour * 3600 + parts.tm_min * 60 + parts.tm_sec;
      }

      int CountText(const std::string &text, const char *needle)
      {
         int count = 0;
         for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            count++;
         return count;
      }

      // The certificates in pasted text, found by their PEM markers and read
      // whatever the line breaks between the markers became. The Deck draws a
      // write-only setting as a password box, which takes a paste on one line -
      // Chrome drops the line breaks and Firefox makes them spaces - and
      // OpenSSL's PEM reader wants them; so what is between the markers is read
      // as base64 with its white space skipped, strictly, and then as DER, the
      // whole of it. A block whose inside is not a certificate is left out.
      std::vector<X509 *> ReadCertificates(const std::string &text)
      {
         static const char *BeginMarker = "-----BEGIN CERTIFICATE-----";
         static const char *EndMarker = "-----END CERTIFICATE-----";
         const size_t beginLength = strlen(BeginMarker);
         const size_t endLength = strlen(EndMarker);

         std::vector<X509 *> certificates;
         size_t at = 0;

         while (true)
         {
            const size_t start = text.find(BeginMarker, at);
            if (start == std::string::npos)
               break;

            const size_t stop = text.find(EndMarker, start + beginLength);
            if (stop == std::string::npos)
               break;

            std::string der;
            if (XmlSignature::Base64Decode(text.substr(start + beginLength, stop - start - beginLength), der) && !der.empty())
            {
               const unsigned char *read = (const unsigned char *) der.data();
               X509 *certificate = d2i_X509(nullptr, &read, (long) der.size());
               if (certificate != nullptr && read == (const unsigned char *) der.data() + der.size())
                  certificates.push_back(certificate);
               else if (certificate != nullptr)
                  X509_free(certificate);
            }

            at = stop + endLength;
         }

         ERR_clear_error();
         return certificates;
      }

      void FreeCertificates(std::vector<X509 *> &certificates)
      {
         for (X509 *certificate : certificates)
            X509_free(certificate);
         certificates.clear();
      }

      // Every element {namespaceUri}localName in the subtree, the node itself included.
      int CountElements(const XmlNode &node, const char *namespaceUri, const char *localName)
      {
         int count = node.Is(namespaceUri, localName) ? 1 : 0;
         for (const std::shared_ptr<XmlNode> &child : node.children)
         {
            if (child->kind == XmlNode::KindElement)
               count += CountElements(*child, namespaceUri, localName);
         }
         return count;
      }

      // How many attributes named ID, Id or id carry this value in the subtree.
      int CountIds(const XmlNode &node, const std::string &value)
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
               count += CountIds(*child, value);
         }

         return count;
      }

      std::string Attribute(const XmlNode &node, const char *name)
      {
         std::string value;
         node.GetAttribute(name, value);
         return value;
      }

      // The last segment of a URN, for a sentence: ...:status:AuthnFailed is
      // AuthnFailed.
      std::string LastSegment(const std::string &urn)
      {
         const size_t colon = urn.rfind(':');
         return Shown(colon == std::string::npos ? urn : urn.substr(colon + 1));
      }

      void PruneLocked(time_t now)
      {
         for (auto it = finished.begin(); it != finished.end();)
         {
            if (now - it->second.created > SamlSignIn::FinishSeconds)
               it = finished.erase(it);
            else
               ++it;
         }

         for (auto it = seen.begin(); it != seen.end();)
         {
            if (it->second.until < now)
               it = seen.erase(it);
            else
               ++it;
         }
      }

      bool RandomText(std::string &text)
      {
         unsigned char bytes[32];
         if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return false;
         text = Text(WebAuthn::Base64Url(bytes, sizeof(bytes)));
         return true;
      }

      // A full table of answers seen or sign-ins finished drops its oldest entry
      // and takes the new one; both are written only after a signed answer.
      template <typename Table>
      void MakeRoomLocked(Table &table, size_t most)
      {
         while (table.size() >= most)
         {
            auto oldest = table.begin();
            for (auto it = table.begin(); it != table.end(); ++it)
            {
               if (it->second.order < oldest->second.order)
                  oldest = it;
            }
            table.erase(oldest);
         }
      }

      // A request this server issued, as its ID says (SignInState.h): nothing of
      // it is held here, because /portal/saml/start answers anybody and a table
      // of starts is one a stranger can fill - until 6.3.6 one was, and a few
      // thousand addresses pushed every person's sign-in out of it. The ID is
      //
      //    _<32 hex digits, random>.<w|d>.<when it ends, seconds since the epoch>.<tag>
      //
      // an xs:ID (a letter or an underscore first, then letters, digits, '.',
      // '-' and '_'), and the tag is this process's over everything before it,
      // so the identity provider echoes back, in a Response it signs, a request
      // the ACS can tell is this server's own, for which page and until when.
      // The RelayState sent beside it is the ID too: under the 80 bytes the
      // bindings allow, and a Response that comes back with another sign-in's
      // RelayState is still refused.
      struct IssuedRequest
      {
         bool deck = false;
         time_t expires = 0;
      };

      std::string RequestId(const std::string &random, bool deck, time_t expires)
      {
         const std::string body = "_" + random + "." + (deck ? "d" : "w") + "." + std::to_string((long long) expires);
         return body + "." + Text(SignInState::Tag(SignInState::SamlRequest, body));
      }

      bool ReadRequestId(const std::string &id, IssuedRequest &issued)
      {
         if (id.size() > 120)
            return false;

         const size_t lastDot = id.rfind('.');
         if (lastDot == std::string::npos || lastDot == 0)
            return false;

         const std::string body = id.substr(0, lastDot);
         if (!SignInState::Verify(SignInState::SamlRequest, body, AnsiString(id.substr(lastDot + 1).c_str())))
            return false;

         // Tagged, so written here: _<hex>.<w|d>.<digits>.
         const size_t first = body.find('.');
         const size_t second = first == std::string::npos ? std::string::npos : body.find('.', first + 1);
         if (second == std::string::npos || second != first + 2)
            return false;

         const std::string expires = body.substr(second + 1);
         if (expires.empty() || expires.size() > 18 || expires.find_first_not_of("0123456789") != std::string::npos)
            return false;

         issued.deck = body[first + 1] == 'd';
         issued.expires = (time_t) std::stoll(expires);
         return true;
      }

      bool SamlAutoProvision()
      {
         return Configuration::Instance()->GetSamlAutoProvision();
      }

      // An address as far as this unit judges one; the mapping judges the rest.
      bool PlausibleAddress(const std::string &address)
      {
         if (address.empty() || address.size() > 255)
            return false;

         const size_t at = address.find('@');
         if (at == std::string::npos || at == 0 || at + 1 == address.size() || address.find('@', at + 1) != std::string::npos)
            return false;

         for (char c : address)
         {
            if ((unsigned char) c <= 0x20 || c == 0x7F)
               return false;
         }

         return true;
      }
   }

   bool
   SamlSignIn::Configured()
   {
      return !Setting(PROPERTY_SAML_IDP_ENTITY_ID).IsEmpty() &&
             !Setting(PROPERTY_SAML_IDP_SSO_URL).IsEmpty() &&
             !Setting(PROPERTY_SAML_IDP_CERTIFICATE).IsEmpty() &&
             !BaseUrl().IsEmpty();
   }

   AnsiString
   SamlSignIn::Label()
   {
      const AnsiString label = Setting(PROPERTY_SAML_BUTTON_LABEL);
      return label.IsEmpty() ? AnsiString(DefaultButtonLabel) : label;
   }

   AnsiString
   SamlSignIn::BaseUrl()
   {
      AnsiString base = Trimmed(Setting(PROPERTY_OIDC_REDIRECT_BASE_URL));
      if (base.IsEmpty())
         base = Trimmed(Utf8(Configuration::Instance()->GetImportOAuthRedirectBaseUrl()));
      return base;
   }

   AnsiString
   SamlSignIn::AcsUrl()
   {
      const AnsiString base = BaseUrl();
      return base.IsEmpty() ? AnsiString() : base + "/portal/saml/acs";
   }

   AnsiString
   SamlSignIn::ServiceProviderEntityId()
   {
      const AnsiString named = Setting(PROPERTY_SAML_SP_ENTITY_ID);
      if (!named.IsEmpty())
         return named;

      const AnsiString base = BaseUrl();
      return base.IsEmpty() ? AnsiString() : base + "/saml";
   }

   bool
   SamlSignIn::Deflate(const std::string &bytes, std::string &deflated)
   {
      z_stream stream;
      memset(&stream, 0, sizeof(stream));

      // -15: raw DEFLATE (RFC 1951), no zlib header and no checksum, which is
      // what the HTTP-Redirect binding carries (SAML bindings, section 3.4.4.1).
      if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
         return false;

      const uLong bound = deflateBound(&stream, (uLong) bytes.size());
      std::vector<unsigned char> out((size_t) bound + 16);

      stream.next_in = (Bytef *) bytes.data();
      stream.avail_in = (uInt) bytes.size();
      stream.next_out = out.data();
      stream.avail_out = (uInt) out.size();

      const int code = deflate(&stream, Z_FINISH);
      const size_t produced = out.size() - stream.avail_out;
      deflateEnd(&stream);

      if (code != Z_STREAM_END)
         return false;

      deflated.assign((const char *) out.data(), produced);
      return true;
   }

   bool
   SamlSignIn::ReadTime(const std::string &text, __int64 &seconds)
   {
      size_t at = 0;

      auto digits = [&text, &at](size_t count, int &value) -> bool
      {
         if (at + count > text.size())
            return false;

         value = 0;
         for (size_t i = 0; i < count; i++)
         {
            const char c = text[at + i];
            if (c < '0' || c > '9')
               return false;
            value = value * 10 + (c - '0');
         }

         at += count;
         return true;
      };

      auto literal = [&text, &at](char c) -> bool
      {
         if (at >= text.size() || text[at] != c)
            return false;
         at++;
         return true;
      };

      int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
      if (!digits(4, year) || !literal('-') || !digits(2, month) || !literal('-') || !digits(2, day) || !literal('T') ||
          !digits(2, hour) || !literal(':') || !digits(2, minute) || !literal(':') || !digits(2, second))
         return false;

      // A fraction of a second: read and dropped.
      if (at < text.size() && text[at] == '.')
      {
         at++;
         const size_t start = at;
         while (at < text.size() && text[at] >= '0' && text[at] <= '9')
            at++;
         if (at == start)
            return false;
      }

      // SAML's times are UTC and written with Z; an offset is read as one.
      int offset = 0;
      if (at < text.size())
      {
         if (text[at] == 'Z')
         {
            at++;
         }
         else if (text[at] == '+' || text[at] == '-')
         {
            const int sign = text[at] == '-' ? -1 : 1;
            at++;

            int offsetHours = 0, offsetMinutes = 0;
            if (!digits(2, offsetHours) || !literal(':') || !digits(2, offsetMinutes) || offsetHours > 14 || offsetMinutes > 59)
               return false;
            offset = sign * (offsetHours * 3600 + offsetMinutes * 60);
         }
         else
         {
            return false;
         }
      }

      if (at != text.size() || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
         return false;

      seconds = DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset;
      return true;
   }

   bool
   SamlSignIn::Begin(bool deck, AnsiString &url, AnsiString &cookie, AnsiString &problem)
   {
      if (!Configured())
      {
         problem = "signing in with a SAML identity provider is not set up on this server";
         return false;
      }

      unsigned char idBytes[16];
      if (RAND_bytes(idBytes, sizeof(idBytes)) != 1)
      {
         problem = "the random generator failed";
         return false;
      }

      // An xs:ID begins with a letter or an underscore, never a digit; this one
      // says, under a tag, which page the sign-in is for and when it ends
      // (RequestId). The RelayState is the same ID, and the browser's hmsaml
      // cookie holds it sealed for the finish. Nothing is kept here.
      const std::string id = RequestId(Hex(idBytes, sizeof(idBytes), false, nullptr), deck, time(nullptr) + PendingSeconds);
      const std::string relay = id;
      const AnsiString browser = SignInState::Seal(SignInState::SamlCookie, id);
      if (browser.IsEmpty() || id.size() > 80)
      {
         problem = "the sign-in could not be sealed";
         return false;
      }

      // The URL goes into a Location header as it is, so a control character
      // in it - a line break above all - is refused rather than sent.
      const std::string ssoUrl = Text(Setting(PROPERTY_SAML_IDP_SSO_URL));
      for (char c : ssoUrl)
      {
         if ((unsigned char) c < 0x20 || c == 0x7F)
         {
            problem = "the identity provider's sign-in URL holds a control character; an administrator has to set it again";
            return false;
         }
      }

      const std::string request =
         "<samlp:AuthnRequest xmlns:samlp=\"urn:oasis:names:tc:SAML:2.0:protocol\""
         " xmlns:saml=\"urn:oasis:names:tc:SAML:2.0:assertion\""
         " ID=\"" + id + "\" Version=\"2.0\" IssueInstant=\"" + TimeText((__int64) time(nullptr)) + "\"" +
         " Destination=\"" + XmlEscape(ssoUrl) + "\"" +
         " AssertionConsumerServiceURL=\"" + XmlEscape(Text(AcsUrl())) + "\"" +
         " ProtocolBinding=\"urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST\">" +
         "<saml:Issuer>" + XmlEscape(Text(ServiceProviderEntityId())) + "</saml:Issuer>" +
         "</samlp:AuthnRequest>";

      std::string deflated;
      if (!Deflate(request, deflated))
      {
         problem = "the sign-in request could not be compressed";
         return false;
      }

      const AnsiString encoded = AnsiString(XmlSignature::Base64Encode(deflated).c_str());
      url = AnsiString(ssoUrl.c_str()) + (ssoUrl.find('?') != std::string::npos ? "&" : "?") +
            "SAMLRequest=" + HttpsClient::FormEncode(encoded) +
            "&RelayState=" + HttpsClient::FormEncode(AnsiString(relay.c_str()));
      cookie = browser;
      return true;
   }

   bool
   SamlSignIn::Finish(const AnsiString &finishToken, const AnsiString &cookie, OidcSignIn::Result &result, AnsiString &problem)
   {
      result = OidcSignIn::Result();

      Finished entry;
      bool found = false;
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         auto it = finishToken.IsEmpty() ? finished.end() : finished.find(Text(finishToken));
         if (it != finished.end())
         {
            entry = it->second;
            finished.erase(it);
            found = true;
         }
      }

      if (!found)
      {
         problem = "this sign-in was finished already, or was not verified here";
         return false;
      }

      // The page is the sign-in's own, from here on.
      result.deck = entry.result.deck;

      if (time(nullptr) - entry.created > FinishSeconds)
      {
         problem = "the sign-in took too long to come back; start it again";
         return false;
      }

      // The browser binding: the cookie the start gave for this very request,
      // which holds its ID sealed. A Response somebody forwarded to another
      // browser ends here, whatever the ACS made of it.
      std::string cookieRequest;
      if (!SignInState::Open(SignInState::SamlCookie, cookie, cookieRequest) || !SameText(entry.request_id, cookieRequest))
      {
         problem = "this sign-in was started in another browser; start it again from the sign-in page";
         return false;
      }

      // Then who the verified address is here: OIDC's rules, with SAML's list
      // and SAML's switch.
      OidcSignIn::Mapping mapping;
      mapping.administrators = PROPERTY_SAML_ADMINISTRATORS;
      mapping.auto_provision = &SamlAutoProvision;
      mapping.auto_provision_name = PROPERTY_SAML_AUTO_PROVISION;
      mapping.component = _T("SamlSignIn");

      return OidcSignIn::MapAddress(entry.result.deck, entry.result.address, mapping, result, problem);
   }

   bool
   SamlSignIn::Complete(const AnsiString &samlResponse, const AnsiString &relayState,
                        OidcSignIn::Result &result, AnsiString &finishToken, AnsiString &problem)
   {
      result = OidcSignIn::Result();
      finishToken.clear();
      const std::string relay = Text(relayState);
      const time_t now = time(nullptr);

      // Which page the answer goes back to, whenever the RelayState names a
      // sign-in started here - decided by the sign-in, never by the answer.
      {
         IssuedRequest named;
         if (ReadRequestId(relay, named))
            result.deck = named.deck;
      }

      if (!Configured())
      {
         problem = "signing in with a SAML identity provider is not set up on this server";
         return false;
      }

      std::string xml;
      if (samlResponse.IsEmpty() || !XmlSignature::Base64Decode(Text(samlResponse), xml))
      {
         problem = "the identity provider's answer is not a base64 SAMLResponse";
         return false;
      }

      XmlDocument document;
      std::string readProblem;
      if (!document.Parse(xml, readProblem))
      {
         problem = AnsiString(("the identity provider's answer could not be read: " + readProblem).c_str());
         return false;
      }

      const XmlNode &response = *document.Root();
      if (!response.Is(ProtocolNamespace, "Response") || Attribute(response, "Version") != "2.0")
      {
         problem = "the identity provider's answer is not a SAML 2.0 Response";
         return false;
      }

      // Refused before anything else is judged, so the sentence says what to
      // change at the identity provider.
      if (CountElements(response, AssertionNamespace, "EncryptedAssertion") > 0)
      {
         problem = "the identity provider encrypted the assertion, and this server reads only signed, unencrypted assertions: "
                   "set the application at the identity provider to sign the assertion and not to encrypt it";
         return false;
      }

      const XmlNode *status = response.ChildElement(ProtocolNamespace, "Status");
      const XmlNode *statusCode = status == nullptr ? nullptr : status->ChildElement(ProtocolNamespace, "StatusCode");
      const std::string code = statusCode == nullptr ? std::string() : Attribute(*statusCode, "Value");
      if (code != StatusSuccess)
      {
         const XmlNode *second = statusCode == nullptr ? nullptr : statusCode->ChildElement(ProtocolNamespace, "StatusCode");
         const std::string why = second == nullptr ? code : Attribute(*second, "Value");
         problem = AnsiString(("the identity provider did not sign the person in (" + LastSegment(why.empty() ? "no status" : why) + ")").c_str());
         return false;
      }

      // The shape: one Assertion, where SAML puts it, and nothing signed
      // anywhere a verifier would not look.
      const int assertions = CountElements(response, AssertionNamespace, "Assertion");
      if (assertions == 0)
      {
         problem = "the identity provider's answer carries no assertion";
         return false;
      }

      if (assertions > 1)
      {
         problem = "the identity provider's answer carries more than one assertion, which is refused";
         return false;
      }

      const XmlNode *assertion = response.ChildElement(AssertionNamespace, "Assertion");
      if (assertion == nullptr)
      {
         problem = "the assertion is not directly under the Response, where SAML puts it - which is the shape of a wrapping attack";
         return false;
      }

      const size_t responseSignatures = response.ChildElements(XmlSignature::Namespace(), "Signature").size();
      const size_t assertionSignatures = assertion->ChildElements(XmlSignature::Namespace(), "Signature").size();
      if ((size_t) CountElements(response, XmlSignature::Namespace(), "Signature") != responseSignatures + assertionSignatures)
      {
         problem = "the answer carries a signature that is neither the Response's nor the Assertion's own, which is refused";
         return false;
      }

      const std::string responseId = Attribute(response, "ID");
      const std::string assertionId = Attribute(*assertion, "ID");
      if (responseId.empty() || assertionId.empty() || CountIds(response, responseId) != 1 || CountIds(response, assertionId) != 1)
      {
         problem = "an ID of the answer is missing or written on more than one element, which is the shape of a wrapping attack";
         return false;
      }

      if (responseSignatures == 0 && assertionSignatures == 0)
      {
         problem = "the identity provider's answer is not signed";
         return false;
      }

      if (assertionSignatures == 0 && Configuration::Instance()->GetSamlRequireSignedAssertion())
      {
         problem = "the assertion itself is not signed, and SamlRequireSignedAssertion requires that it is: set the identity provider "
                   "to sign the assertion, or switch the setting off to take a signature over the whole Response";
         return false;
      }

      // As stored they are written PEM already (NormalisedCertificates); read
      // through it again, so that a value put in the database some other way
      // is judged as the settings would have judged it.
      const std::string certificates =
         Text(Utf8(NormalisedCertificates(Configuration::Instance()->GetSignInString(PROPERTY_SAML_IDP_CERTIFICATE))));
      {
         std::vector<X509 *> stored = ReadCertificates(certificates);
         const bool readable = !stored.empty();
         FreeCertificates(stored);

         if (!readable)
         {
            // Reached only through a value the settings did not check - the
            // database written by hand: every surface that stores it reads it.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, ErrorCertificateUnreadable, "SamlSignIn::Complete",
               "The SAML identity provider's certificate stored in SamlIdpCertificate cannot be read as a PEM certificate, so no SAML sign-in "
               "can be verified. Paste the identity provider's signing certificate again (saml_idp_certificate).");
            problem = "the identity provider's certificate stored here cannot be read; an administrator has to paste it again";
            return false;
         }
      }

      // The signatures. Whichever verified it, what is used from here on is
      // what a signature covered, re-read - never the document it came in.
      XmlDocument signedResponse;
      XmlDocument signedAssertion;
      const XmlNode *useResponse = &response;
      const XmlNode *useAssertion = nullptr;
      int certificateIndex = -1;
      std::string signatureProblem;

      if (responseSignatures > 0)
      {
         if (!XmlSignature::Verify(response, "ID", certificates, signedResponse, certificateIndex, signatureProblem))
         {
            problem = AnsiString(signatureProblem.c_str());
            return false;
         }

         useResponse = signedResponse.Root();
         const std::vector<const XmlNode *> within = useResponse->ChildElements(AssertionNamespace, "Assertion");
         if (within.size() != 1)
         {
            problem = "the signed Response does not hold exactly one assertion";
            return false;
         }
         useAssertion = within[0];
      }

      if (assertionSignatures > 0)
      {
         if (!XmlSignature::Verify(*assertion, "ID", certificates, signedAssertion, certificateIndex, signatureProblem))
         {
            problem = AnsiString(signatureProblem.c_str());
            return false;
         }

         useAssertion = signedAssertion.Root();
      }

      if (useAssertion == nullptr)
      {
         problem = "no signature covers the assertion";
         return false;
      }

      // ---- the Response
      const std::string acs = Text(AcsUrl());
      const std::string idpEntityId = Text(Setting(PROPERTY_SAML_IDP_ENTITY_ID));

      const std::string destination = Attribute(*useResponse, "Destination");
      if (destination != acs)
      {
         problem = AnsiString((destination.empty()
            ? std::string("the Response names no Destination, so nothing says it was meant for this server")
            : "the Response was sent to another address (" + Shown(destination) + ") than this server's " + acs).c_str());
         return false;
      }

      const std::string inResponseTo = Attribute(*useResponse, "InResponseTo");
      if (inResponseTo.empty())
      {
         problem = "the identity provider started this sign-in itself (an IdP-initiated sign-in answers no request), and this "
                   "server takes only the sign-ins it started: sign in from the sign-in page";
         return false;
      }

      const XmlNode *responseIssuer = useResponse->ChildElement(AssertionNamespace, "Issuer");
      if (responseIssuer != nullptr && Stripped(responseIssuer->TextContent()) != idpEntityId)
      {
         problem = "the Response was issued by another identity provider than the one set up here";
         return false;
      }

      // The request it answers: one this server issued (its ID's tag), not
      // answered before (the spent set, which only a verified answer below adds
      // to), coming back with the RelayState it went out with - the same ID.
      IssuedRequest entry;
      if (!ReadRequestId(inResponseTo, entry) || SignInState::IsSpent(SignInState::SamlRequest, inResponseTo))
      {
         problem = "this answer is to a sign-in that was not started here, or that was finished already";
         return false;
      }

      // The page is decided by the server: until the RelayState has matched,
      // it is the one the posted RelayState's own sign-in named (above), and a
      // Response naming another sign-in in its InResponseTo does not choose it.
      if (!SameText(inResponseTo, relay))
      {
         problem = "the answer came back with the RelayState of another sign-in";
         return false;
      }

      result.deck = entry.deck;

      if (now > entry.expires)
      {
         problem = "the sign-in took longer than ten minutes; start it again";
         return false;
      }

      // ---- the Assertion
      if (Attribute(*useAssertion, "Version") != "2.0")
      {
         problem = "the assertion is not SAML 2.0";
         return false;
      }

      const XmlNode *issuer = useAssertion->ChildElement(AssertionNamespace, "Issuer");
      if (issuer == nullptr || Stripped(issuer->TextContent()) != idpEntityId)
      {
         problem = "the assertion was issued by another identity provider than the one set up here";
         return false;
      }

      const XmlNode *subject = useAssertion->ChildElement(AssertionNamespace, "Subject");
      if (subject == nullptr)
      {
         problem = "the assertion names no subject";
         return false;
      }

      // A bearer confirmation for this server, this request, and now.
      std::string confirmationProblem = "the assertion has no bearer SubjectConfirmation";
      __int64 expires = 0;
      bool confirmed = false;

      for (const XmlNode *confirmation : subject->ChildElements(AssertionNamespace, "SubjectConfirmation"))
      {
         if (Attribute(*confirmation, "Method") != BearerMethod)
            continue;

         const XmlNode *data = confirmation->ChildElement(AssertionNamespace, "SubjectConfirmationData");
         if (data == nullptr)
         {
            confirmationProblem = "the assertion's bearer SubjectConfirmation carries no SubjectConfirmationData";
            continue;
         }

         if (Attribute(*data, "Recipient") != acs)
         {
            confirmationProblem = "the assertion is for another recipient than this server's " + acs;
            continue;
         }

         if (Attribute(*data, "InResponseTo") != inResponseTo)
         {
            confirmationProblem = "the assertion answers another sign-in than the Response it came in";
            continue;
         }

         __int64 notOnOrAfter = 0;
         if (!ReadTime(Attribute(*data, "NotOnOrAfter"), notOnOrAfter))
         {
            confirmationProblem = "the assertion's SubjectConfirmationData has no NotOnOrAfter";
            continue;
         }

         if (notOnOrAfter + ClockSkewSeconds <= (__int64) now)
         {
            confirmationProblem = "the assertion's bearer confirmation has expired";
            continue;
         }

         const std::string notBeforeText = Attribute(*data, "NotBefore");
         __int64 notBefore = 0;
         if (!notBeforeText.empty() && (!ReadTime(notBeforeText, notBefore) || notBefore - ClockSkewSeconds > (__int64) now))
         {
            confirmationProblem = "the assertion's bearer confirmation is not valid yet";
            continue;
         }

         confirmed = true;
         expires = notOnOrAfter;
         break;
      }

      if (!confirmed)
      {
         problem = AnsiString(confirmationProblem.c_str());
         return false;
      }

      const XmlNode *conditions = useAssertion->ChildElement(AssertionNamespace, "Conditions");
      if (conditions == nullptr)
      {
         problem = "the assertion carries no Conditions, so it names no audience";
         return false;
      }

      const std::string conditionsNotBefore = Attribute(*conditions, "NotBefore");
      if (!conditionsNotBefore.empty())
      {
         __int64 notBefore = 0;
         if (!ReadTime(conditionsNotBefore, notBefore) || notBefore - ClockSkewSeconds > (__int64) now)
         {
            problem = "the assertion's Conditions are not valid yet";
            return false;
         }
      }

      const std::string conditionsNotOnOrAfter = Attribute(*conditions, "NotOnOrAfter");
      if (!conditionsNotOnOrAfter.empty())
      {
         __int64 notOnOrAfter = 0;
         if (!ReadTime(conditionsNotOnOrAfter, notOnOrAfter) || notOnOrAfter + ClockSkewSeconds <= (__int64) now)
         {
            problem = "the assertion's Conditions have expired";
            return false;
         }

         if (notOnOrAfter > expires)
            expires = notOnOrAfter;
      }

      // Every AudienceRestriction must name this service provider (SAML 2.0
      // core, section 2.5.1.4), and there has to be one.
      const std::string spEntityId = Text(ServiceProviderEntityId());
      const std::vector<const XmlNode *> restrictions = conditions->ChildElements(AssertionNamespace, "AudienceRestriction");
      if (restrictions.empty())
      {
         problem = "the assertion names no audience, so nothing says it was meant for this server";
         return false;
      }

      for (const XmlNode *restriction : restrictions)
      {
         bool ours = false;
         for (const XmlNode *audience : restriction->ChildElements(AssertionNamespace, "Audience"))
         {
            if (Stripped(audience->TextContent()) == spEntityId)
               ours = true;
         }

         if (!ours)
         {
            problem = AnsiString(("the assertion is for another service provider: its audience is not this server's entity ID, " + spEntityId).c_str());
            return false;
         }
      }

      if (useAssertion->ChildElements(AssertionNamespace, "AuthnStatement").empty())
      {
         problem = "the assertion does not say the person signed in (it has no AuthnStatement)";
         return false;
      }

      // Replay: an assertion is taken once, and remembered until it expires.
      const std::string usedId = Attribute(*useAssertion, "ID");
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         PruneLocked(now);
         if (usedId.empty() || seen.find(usedId) != seen.end())
         {
            problem = "this assertion has been used to sign in already";
            return false;
         }

         Seen record;
         record.until = (time_t) (expires + ClockSkewSeconds);
         record.order = nextOrder++;
         MakeRoomLocked(seen, MostSeen);
         seen[usedId] = record;
      }

      // And the request is answered: spent, until its ID could no longer be
      // shown anyway. Only a Response that got this far - signed by the
      // identity provider - adds to the spent set.
      if (!SignInState::Spend(SignInState::SamlRequest, inResponseTo, entry.expires + ClockSkewSeconds))
      {
         problem = "this answer is to a sign-in that was not started here, or that was finished already";
         return false;
      }

      // ---- who it is
      std::string address;
      const XmlNode *nameId = subject->ChildElement(AssertionNamespace, "NameID");
      if (nameId != nullptr && Attribute(*nameId, "Format") == EmailAddressFormat)
      {
         if (HoldsElements(*nameId))
         {
            problem = "the assertion's NameID holds markup, not an address";
            return false;
         }
         address = Stripped(nameId->TextContent());
      }
      else
      {
         std::string wanted = Text(Setting(PROPERTY_SAML_EMAIL_ATTRIBUTE));
         if (wanted.empty())
            wanted = DefaultEmailAttribute;

         int named = 0;
         std::vector<std::string> values;
         for (const XmlNode *statement : useAssertion->ChildElements(AssertionNamespace, "AttributeStatement"))
         {
            for (const XmlNode *attribute : statement->ChildElements(AssertionNamespace, "Attribute"))
            {
               if (Attribute(*attribute, "Name") != wanted)
                  continue;

               named++;
               for (const XmlNode *value : attribute->ChildElements(AssertionNamespace, "AttributeValue"))
               {
                  if (HoldsElements(*value))
                  {
                     problem = AnsiString(("the assertion's attribute " + Shown(wanted) + " holds markup, not an address").c_str());
                     return false;
                  }
                  values.push_back(Stripped(value->TextContent()));
               }
            }
         }

         if (named == 0 || values.empty())
         {
            problem = AnsiString(("the assertion names the person neither by an emailAddress NameID nor by the attribute " + Shown(wanted)).c_str());
            return false;
         }

         if (named > 1 || values.size() > 1)
         {
            problem = AnsiString(("the assertion gives the attribute " + Shown(wanted) + " more than one value, so it does not say which address signed in").c_str());
            return false;
         }

         address = values[0];
      }

      if (!PlausibleAddress(address))
      {
         problem = "the identity provider did not say which address signed in";
         return false;
      }

      // Verified. Kept for the finish, where the browser that started the
      // sign-in has to show its cookie before anything is done with it - the
      // mapping included, so that no mailbox is made for a Response that ends
      // in the wrong browser.
      std::string token;
      if (!RandomText(token))
      {
         problem = "the random generator failed";
         return false;
      }

      Finished waiting;
      waiting.result.deck = entry.deck;
      waiting.result.address = FromUtf8(address);
      waiting.request_id = inResponseTo;
      waiting.created = time(nullptr);

      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         waiting.order = nextOrder++;
         PruneLocked(waiting.created);
         MakeRoomLocked(finished, MostFinished);
         finished[token] = waiting;
      }

      result.address = waiting.result.address;
      finishToken = AnsiString(token.c_str());
      return true;
   }

   AnsiString
   SamlSignIn::Metadata()
   {
      const std::string acs = Text(AcsUrl());
      const std::string entityId = Text(ServiceProviderEntityId());
      if (acs.empty() || entityId.empty())
         return AnsiString();

      const bool wantSigned = Configuration::Instance()->GetSamlRequireSignedAssertion();

      // AuthnRequestsSigned is false because they are not: this server holds no
      // signing key for SAML, so there is no KeyDescriptor to publish either.
      const std::string metadata =
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<md:EntityDescriptor xmlns:md=\"urn:oasis:names:tc:SAML:2.0:metadata\" entityID=\"" + XmlEscape(entityId) + "\">\n" +
         "  <md:SPSSODescriptor AuthnRequestsSigned=\"false\" WantAssertionsSigned=\"" + (wantSigned ? "true" : "false") + "\"" +
         " protocolSupportEnumeration=\"urn:oasis:names:tc:SAML:2.0:protocol\">\n" +
         "    <md:NameIDFormat>" + EmailAddressFormat + "</md:NameIDFormat>\n" +
         "    <md:AssertionConsumerService Binding=\"urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST\" Location=\"" + XmlEscape(acs) +
         "\" index=\"0\" isDefault=\"true\"/>\n" +
         "  </md:SPSSODescriptor>\n" +
         "</md:EntityDescriptor>\n";

      return AnsiString(metadata.c_str());
   }

   String
   SamlSignIn::CertificateProblem(const String &pem)
   {
      AnsiString text = Utf8(pem);
      text.Trim();
      if (text.IsEmpty())
         return String();

      // The gateway-encryption row's gate for pasted key text: no private-key
      // material of either kind anywhere in it, and no more than 60,000
      // characters. Its sentences name a partner's key; the reasons are the
      // same, said for this setting.
      AnsiString refusal;
      if (!GatewayEncryption::AcceptableKeyText(text, refusal))
      {
         if (GatewayEncryption::ContainsPrivateKey(text))
            return String(_T("saml_idp_certificate carries a PRIVATE key, and this server never holds the identity provider's. Paste its signing certificate alone: the block that begins -----BEGIN CERTIFICATE-----."));

         String sentence;
         sentence.Format(_T("saml_idp_certificate is %d characters, and holds at most %d: one or two certificates are a few kilobytes."),
                         text.GetLength(), (int) GatewayEncryption::MaximumKeyCharacters);
         return sentence;
      }

      const std::string pemText = Text(text);
      std::vector<X509 *> certificates = ReadCertificates(pemText);
      const int blocks = CountText(pemText, "-----BEGIN ");

      String sentence;
      if (certificates.empty())
      {
         sentence = _T("saml_idp_certificate is the identity provider's signing certificate in PEM - the block that begins -----BEGIN CERTIFICATE----- - and what was pasted could not be read as one.");
      }
      else if ((int) certificates.size() != blocks)
      {
         sentence = _T("saml_idp_certificate holds certificates and nothing else: a PEM block that is not a certificate, or one that could not be read, was pasted with them.");
      }
      else if ((int) certificates.size() > MostCertificates)
      {
         sentence = _T("saml_idp_certificate holds at most two certificates: the identity provider's signing certificate, and the one replacing it while a key is rolled over.");
      }
      else
      {
         for (X509 *certificate : certificates)
         {
            EVP_PKEY *key = X509_get0_pubkey(certificate);
            const int type = key == nullptr ? 0 : EVP_PKEY_get_base_id(key);
            const int bits = key == nullptr ? 0 : EVP_PKEY_get_bits(key);

            if (type == EVP_PKEY_RSA && bits < MinimumRsaBits)
               sentence.Format(_T("saml_idp_certificate holds an RSA key of %d bits, and a signature is taken from RSA of %d bits or more."), bits, MinimumRsaBits);
            else if (type == EVP_PKEY_EC && bits < MinimumEcBits)
               sentence.Format(_T("saml_idp_certificate holds an elliptic-curve key of %d bits, and a signature is taken from curves of %d bits or more."), bits, MinimumEcBits);
            else if (type != EVP_PKEY_RSA && type != EVP_PKEY_EC)
               sentence = _T("saml_idp_certificate holds a key that is neither RSA nor elliptic-curve, and SAML signatures are verified with one of the two.");

            if (!sentence.IsEmpty())
               break;
         }
      }

      FreeCertificates(certificates);

      if (sentence.IsEmpty())
      {
         const int stored = NormalisedCertificates(pem).GetLength();
         if (stored > MaximumStoredCharacters)
            sentence.Format(_T("saml_idp_certificate would be %d characters stored, and the setting holds %d: paste the certificate in use alone."),
                            stored, (int) MaximumStoredCharacters);
      }

      return sentence;
   }

   String
   SamlSignIn::NormalisedCertificates(const String &pem)
   {
      AnsiString text = Utf8(pem);
      text.Trim();

      std::vector<X509 *> certificates = ReadCertificates(Text(text));
      if (certificates.empty())
         return FromUtf8(Text(text));

      std::string written;
      for (X509 *certificate : certificates)
      {
         BIO *bio = BIO_new(BIO_s_mem());
         if (bio == nullptr)
            continue;

         if (PEM_write_bio_X509(bio, certificate) == 1)
         {
            char *data = nullptr;
            const long length = BIO_get_mem_data(bio, &data);
            if (length > 0 && data != nullptr)
               written.append(data, (size_t) length);
         }

         BIO_free(bio);
      }

      FreeCertificates(certificates);
      ERR_clear_error();
      return FromUtf8(written);
   }

   String
   SamlSignIn::CertificateSummary(const String &pem)
   {
      std::vector<X509 *> certificates = ReadCertificates(Text(Utf8(pem)));

      std::string summary;
      for (X509 *certificate : certificates)
      {
         unsigned char sha256[EVP_MAX_MD_SIZE];
         unsigned char sha1[EVP_MAX_MD_SIZE];
         unsigned int sha256Length = 0;
         unsigned int sha1Length = 0;

         if (X509_digest(certificate, EVP_sha256(), sha256, &sha256Length) != 1 ||
             X509_digest(certificate, EVP_sha1(), sha1, &sha1Length) != 1)
            continue;

         if (!summary.empty())
            summary += "; ";

         summary += "SHA-256 " + Hex(sha256, sha256Length, true, ":");
         summary += ", SHA-1 thumbprint " + Hex(sha1, sha1Length, true, nullptr);

         const __int64 notAfter = EpochOf(X509_get0_notAfter(certificate));
         if (notAfter > 0)
            summary += ", expires " + TimeText(notAfter);
      }

      FreeCertificates(certificates);
      ERR_clear_error();
      return FromUtf8(summary);
   }

   void
   SamlSignIn::Clear()
   {
      boost::lock_guard<boost::mutex> guard(stateLock);
      finished.clear();
      seen.clear();
   }
}
