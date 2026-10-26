// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// ACME v2 (RFC 8555) client. See AcmeClient.h.

#include "StdAfx.h"

#include "AcmeClient.h"
#include "AuditTrail.h"
#include "FileUtilities.h"
#include "HttpsClient.h"
#include "OtelTracer.h"
#include "ServerStatus.h"
#include "Unicode.h"
#include "WebServicesServer.h"
#include "Encoding/Base64.h"

#include "../Application/NewInstallDefaults.h"
#include "../Application/Reinitializator.h"
#include "../Threading/WorkQueue.h"
#include "../BO/SSLCertificate.h"
#include "../BO/SSLCertificates.h"
#include "../BO/TCPIPPort.h"
#include "../BO/TCPIPPorts.h"
#include "../Persistence/PersistentSSLCertificate.h"
#include "../Persistence/PersistentTCPIPPort.h"
#include "../TCPIP/AcceptLoop.h"
#include "../TCPIP/SocketConstants.h"

#ifdef HM_PLATFORM_POSIX
namespace
{
   // _mkgmtime is Microsoft's name for the inverse of gmtime: a struct tm read
   // as UTC, where mktime would read it as local time. POSIX spells the same
   // function timegm. Giving it the Microsoft name once, here, keeps the call
   // sites below reading the way they do on Windows.
   inline time_t _mkgmtime(struct tm *parts)
   {
      return ::timegm(parts);
   }
}
#endif

#ifdef HM_PLATFORM_POSIX
// Winsock's half-close constant under the POSIX name for the same thing:
// SD_SEND is SHUT_WR. Same meaning, same value, different spelling.
static const int SD_SEND = SHUT_WR;
#endif

// <ws2tcpip.h> is Winsock's TCP/IP header. Everything this file takes from it -
// the address structures and the address-conversion calls - comes from
// <netinet/in.h>, <arpa/inet.h> and <netdb.h> on POSIX, which the platform layer
// has already included.
#ifdef _MSC_VER
#include <ws2tcpip.h>
#endif

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/core_names.h>
#include <openssl/sha.h>

#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // One ACME request, all of it: the lookup, the connection, the handshake
      // and the answer (HttpsClient's deadline).
      const int HttpsTimeoutSeconds = 30;
      const size_t MaxResponseSize = 1024 * 1024;
      const int PollIntervalMilliseconds = 2000;
      const int MaxPollAttempts = 30;

      // How close to expiry a failed renewal stops being a log line and becomes a
      // reported error. Renewal now begins a third of the certificate's lifetime
      // before it expires, so a single failure has weeks of slack on a 90-day
      // certificate and days on a 47-day one; an ERROR entry for that would teach an
      // administrator to ignore the error log. Inside this window the certificate is
      // about to stop working, which is a different thing entirely.
      //
      // Fixed rather than proportional, unlike the renewal decision above it: this is
      // about how long a human needs to react, and that does not shrink because
      // certificates did.
      const int ImminentExpiryDays = 7;

      // Reads the notAfter of the first certificate in a PEM file as a time_t.
      // False when there is no file, it does not parse, or the date does not
      // convert - all of which callers treat as "no usable certificate" rather than
      // as a certificate with a known expiry.
      // Both dates, because the renewal decision needs the certificate's LIFETIME
      // and not just its expiry. notBefore is returned as 0 when it cannot be read,
      // which the caller treats as "assume the historical 90-day lifetime" rather
      // than as a failure - a certificate with an unreadable notBefore still has a
      // perfectly good notAfter to renew against.
      bool ReadCertificateDates(const String &certificateFile, time_t &notBefore, time_t &notAfter)
      {
         notBefore = 0;
         notAfter = 0;

         if (!FileUtilities::Exists(certificateFile))
            return false;

         AnsiString narrowFileName = certificateFile;

         BIO *bio = BIO_new_file(narrowFileName.c_str(), "r");
         if (bio == nullptr)
            return false;

         X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
         BIO_free(bio);

         if (certificate == nullptr)
            return false;

         tm notAfterTm = {};
         bool parsed = false;

         if (ASN1_TIME_to_tm(X509_get0_notAfter(certificate), &notAfterTm) == 1)
         {
            time_t converted = _mkgmtime(&notAfterTm);

            if (converted != -1)
            {
               notAfter = converted;
               parsed = true;
            }
         }

         tm notBeforeTm = {};

         if (ASN1_TIME_to_tm(X509_get0_notBefore(certificate), &notBeforeTm) == 1)
         {
            time_t converted = _mkgmtime(&notBeforeTm);

            if (converted != -1)
               notBefore = converted;
         }

         X509_free(certificate);

         return parsed;
      }

      // Kept for the callers that only care when the certificate dies.
      bool ReadCertificateNotAfter(const String &certificateFile, time_t &notAfter)
      {
         time_t ignoredNotBefore = 0;

         return ReadCertificateDates(certificateFile, ignoredNotBefore, notAfter);
      }

      // Splits an ACME URL into components: https, or plain http to a loopback
      // address alone (localhost, 127.0.0.0/8, ::1), which is what HttpsClient
      // itself accepts and what lets a fixture stand a CA on this machine. RFC
      // 8555 section 6.1 has every request made over HTTPS so that nobody on the
      // path can read or change it; a request that never leaves the machine has
      // no path, and every URL the CA's directory and orders hand on is judged
      // here again, so a directory on the loopback address cannot send this
      // client in the clear to anywhere else.
      bool ParseAcmeUrl(const AnsiString &url, AnsiString &host, AnsiString &port, AnsiString &path)
      {
         bool https = false;

         if (!HttpsClient::ParseUrl(url, https, host, port, path))
            return false;

         if (host.IsEmpty())
            return false;

         return https || HttpsClient::IsLoopbackHost(host);
      }

      // Text a CA or a certificate supplied, made fit for a status document and
      // a log line: printable ASCII only, a run of anything else one space, cut
      // to at most maxLength characters.
      AnsiString CleanBytes(const unsigned char *data, int length, int maxLength)
      {
         AnsiString cleaned;
         bool spaced = false;

         for (int i = 0; i < length && cleaned.GetLength() < maxLength; i++)
         {
            const unsigned char character = data[i];

            if (character >= 0x20 && character < 0x7f)
            {
               cleaned += (char) character;
               spaced = false;
            }
            else if (!spaced)
            {
               cleaned += ' ';
               spaced = true;
            }
         }

         cleaned.Trim();
         return cleaned;
      }

      AnsiString CleanText(const AnsiString &value, int maxLength)
      {
         return CleanBytes((const unsigned char *) value.c_str(), value.GetLength(), maxLength);
      }

      // A name of AcmeDomains as written, for the status and the log: printable
      // ASCII as it is, and every other byte as \xHH, so that a name is never
      // shown empty. CleanText made a run of other bytes one space and trimmed
      // it, and a name of only such bytes - a Latin-1 character, which the
      // setting's narrowing keeps as its byte - was answered as "" in
      // unusable_domains and as nothing in the log line that names it. Cut to
      // at most maxLength characters, never inside an escape.
      AnsiString ShownName(const AnsiString &value, int maxLength)
      {
         AnsiString shown;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) value[i];

            AnsiString piece;
            if (character >= 0x20 && character < 0x7f)
               piece += (char) character;
            else
               piece.Format("\\x%02X", (unsigned int) character);

            if (shown.GetLength() + piece.GetLength() > maxLength)
               break;

            shown += piece;
         }

         return shown;
      }

      // A JSON string's content, as JsonStringValue_ gives it, read as text: \",
      // \\ and \/ as the character, \uXXXX of printable ASCII as that
      // character, and every other escape - a line break, a tab, a character
      // past ASCII - as a control character, which CleanText then makes one
      // space with the run it is in. A CA's sentence quotes the name it refuses
      // ("Cannot issue for \"...\"").
      AnsiString JsonUnescapeText(const AnsiString &value)
      {
         AnsiString result;
         const int length = value.GetLength();

         for (int i = 0; i < length; i++)
         {
            const char character = value[i];

            if (character != '\\' || i + 1 >= length)
            {
               result += character;
               continue;
            }

            const char escaped = value[++i];

            if (escaped == '"' || escaped == '\\' || escaped == '/')
            {
               result += escaped;
               continue;
            }

            if (escaped == 'u' && i + 4 < length)
            {
               int code = 0;
               bool hex = true;

               for (int digit = 1; digit <= 4 && hex; digit++)
               {
                  const char c = value[i + digit];
                  if (c >= '0' && c <= '9')
                     code = code * 16 + (c - '0');
                  else if (c >= 'a' && c <= 'f')
                     code = code * 16 + (c - 'a' + 10);
                  else if (c >= 'A' && c <= 'F')
                     code = code * 16 + (c - 'A' + 10);
                  else
                     hex = false;
               }

               if (hex)
               {
                  i += 4;
                  result += (code >= 0x20 && code < 0x7f) ? (char) code : '\n';
                  continue;
               }
            }

            result += '\n';
         }

         return result;
      }

      // The detail of a CA's problem document, from the raw string content: read,
      // cleaned and cut to 300 characters.
      String ProblemDetail(const AnsiString &raw)
      {
         return String(CleanText(JsonUnescapeText(raw), 300));
      }

      // A JSON string's content: the characters a JSON string may not hold as
      // they are escaped, everything else as it is. The caller gives UTF-8.
      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString result;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) value[i];

            switch (character)
            {
            case '\"':
               result += "\\\"";
               break;
            case '\\':
               result += "\\\\";
               break;
            case '\r':
               result += "\\r";
               break;
            case '\n':
               result += "\\n";
               break;
            case '\t':
               result += "\\t";
               break;
            default:
               if (character >= 0x20)
                  result += (char) character;
               break;
            }
         }

         return result;
      }

      AnsiString JsonString(const AnsiString &value)
      {
         return "\"" + JsonEscape(value) + "\"";
      }

      AnsiString JsonText(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return JsonString(utf8);
      }

      AnsiString JsonNumber(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      // A time as Unix seconds, or null when there is none.
      AnsiString JsonTime(__int64 value)
      {
         return value > 0 ? JsonNumber(value) : AnsiString("null");
      }

      // "a, b, c", for a log line or a sentence.
      AnsiString JoinNames(const std::vector<AnsiString> &names)
      {
         AnsiString joined;

         for (size_t i = 0; i < names.size(); i++)
         {
            if (i > 0)
               joined += ", ";

            joined += names[i];
         }

         return joined;
      }

      AnsiString JsonNames(const std::vector<AnsiString> &names)
      {
         AnsiString json = "[";

         for (size_t i = 0; i < names.size(); i++)
         {
            if (i > 0)
               json += ",";

            json += JsonString(names[i]);
         }

         return json + "]";
      }

      // An X509 name in the RFC 2253 form ("CN=R11,O=Let's Encrypt,C=US"),
      // cleaned; empty when it cannot be printed.
      AnsiString NameText(const X509_NAME *name)
      {
         if (name == nullptr)
            return "";

         BIO *bio = BIO_new(BIO_s_mem());
         if (bio == nullptr)
            return "";

         AnsiString text;

         if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) >= 0)
         {
            char *data = nullptr;
            long length = BIO_get_mem_data(bio, &data);

            if (data != nullptr && length > 0)
               text = CleanBytes((const unsigned char *) data, (int) length, 400);
         }

         BIO_free(bio);
         return text;
      }

      // The first certificate of a PEM text.
      X509 *ReadPemCertificate(const AnsiString &pem)
      {
         BIO *bio = BIO_new_mem_buf(pem.c_str(), pem.GetLength());
         if (bio == nullptr)
            return nullptr;

         X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
         BIO_free(bio);
         return certificate;
      }

      // The first certificate of a PEM file, through a BIO OpenSSL opens itself
      // (GetCertificateTlsa says why never a FILE* of ours).
      X509 *ReadPemCertificateFile(const String &file)
      {
         if (!FileUtilities::Exists(file))
            return nullptr;

         AnsiString narrowFileName = file;

         BIO *bio = BIO_new_file(narrowFileName.c_str(), "r");
         if (bio == nullptr)
            return nullptr;

         X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
         BIO_free(bio);
         return certificate;
      }

      // The dNSName entries of a certificate's subject alternative names in
      // lower case, or its common name when it has no such extension.
      void CertificateNames(X509 *certificate, std::vector<AnsiString> &names)
      {
         names.clear();

         GENERAL_NAMES *alternatives = (GENERAL_NAMES *) X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr);

         if (alternatives != nullptr)
         {
            for (int i = 0; i < sk_GENERAL_NAME_num(alternatives); i++)
            {
               const GENERAL_NAME *entry = sk_GENERAL_NAME_value(alternatives, i);
               if (entry == nullptr || entry->type != GEN_DNS)
                  continue;

               const ASN1_STRING *value = entry->d.dNSName;
               const unsigned char *data = ASN1_STRING_get0_data(value);
               const int length = ASN1_STRING_length(value);

               if (data == nullptr || length <= 0)
                  continue;

               // A NUL or any other byte no host name holds becomes a space,
               // so that a name carrying one matches nothing configured.
               AnsiString name = CleanBytes(data, length, 253);
               name.MakeLower();

               if (!name.IsEmpty())
                  names.push_back(name);
            }

            GENERAL_NAMES_free(alternatives);
            return;
         }

         // const: OpenSSL 4 hands the name back as one, and 3 takes it so.
         const X509_NAME *subject = X509_get_subject_name(certificate);
         const int position = subject != nullptr ? X509_NAME_get_index_by_NID(subject, NID_commonName, -1) : -1;

         if (position < 0)
            return;

         const ASN1_STRING *value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, position));
         const unsigned char *data = value != nullptr ? ASN1_STRING_get0_data(value) : nullptr;
         const int length = value != nullptr ? ASN1_STRING_length(value) : 0;

         if (data == nullptr || length <= 0)
            return;

         AnsiString name = CleanBytes(data, length, 253);
         name.MakeLower();

         if (!name.IsEmpty())
            names.push_back(name);
      }

      // An ASN1_TIME as Unix seconds; 0 when it does not convert.
      __int64 AsnTime(const ASN1_TIME *value)
      {
         tm parts = {};

         if (value == nullptr || ASN1_TIME_to_tm(value, &parts) != 1)
            return 0;

         time_t converted = _mkgmtime(&parts);
         return converted == -1 ? 0 : (__int64) converted;
      }

      // Extracts the JSON object that encloses the given position.
      AnsiString ExtractEnclosingObject(const AnsiString &json, int position)
      {
         int start = position;
         int depth = 0;

         while (start >= 0)
         {
            char character = json[start];
            if (character == '}')
               depth++;
            else if (character == '{')
            {
               if (depth == 0)
                  break;
               depth--;
            }

            start--;
         }

         if (start < 0)
            return "";

         int end = position;
         depth = 0;

         while (end < json.GetLength())
         {
            char character = json[end];
            if (character == '{')
               depth++;
            else if (character == '}')
            {
               if (depth == 0)
                  break;
               depth--;
            }

            end++;
         }

         if (end >= json.GetLength())
            return "";

         return json.Mid(start, end - start + 1);
      }
   }

   AcmeClient::AcmeClient() :
      account_key_(nullptr)
   {

   }

   AcmeClient::~AcmeClient()
   {
      if (account_key_ != nullptr)
      {
         EVP_PKEY_free(account_key_);
         account_key_ = nullptr;
      }
   }

   String
   AcmeClient::GetCertificateDirectory()
   {
      String directory = IniFileSettings::Instance()->GetAcmeCertificateDirectory();

      if (directory.IsEmpty())
         directory = IniFileSettings::Instance()->GetDataDirectory() + FileUtilities::PathSeparator + _T("ACME");

      return directory;
   }


   time_t
   AcmeClient::ParseRfc3339(const AnsiString &value)
   {
      // "2026-01-02T15:04:05Z", optionally with fractional seconds before the Z.
      // Deliberately strict about the shape rather than tolerant: this decides when a
      // certificate is renewed, and a timestamp half-understood is worse than one
      // rejected, because rejecting it falls back to a calculation that works.
      if (value.GetLength() < 20)
         return 0;

      tm parsed = {};

      int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

      if (sscanf_s(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                   &year, &month, &day, &hour, &minute, &second) != 6)
         return 0;

      if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
         return 0;

      parsed.tm_year = year - 1900;
      parsed.tm_mon = month - 1;
      parsed.tm_mday = day;
      parsed.tm_hour = hour;
      parsed.tm_min = minute;
      parsed.tm_sec = second;

      time_t converted = _mkgmtime(&parsed);

      return converted == -1 ? 0 : converted;
   }

   AnsiString
   AcmeClient::BuildAriId(const unsigned char *keyIdentifier, int keyIdentifierLength,
                          const unsigned char *serialDer, int serialDerLength)
   {
      if (keyIdentifier == nullptr || keyIdentifierLength <= 0 ||
          serialDer == nullptr || serialDerLength <= 0)
         return "";

      // RFC 9773 4.1: base64url of the AKI keyIdentifier, a dot, base64url of the
      // serial's DER content octets. Unpadded, which Base64Url_ already handles.
      return Base64Url_(keyIdentifier, keyIdentifierLength) + "." +
             Base64Url_(serialDer, serialDerLength);
   }

   bool
   AcmeClient::GetCertificateAriId(const String &certificateFile, AnsiString &ariId)
   {
      ariId = "";

      if (!FileUtilities::Exists(certificateFile))
         return false;

      AnsiString narrowFileName = certificateFile;

      BIO *bio = BIO_new_file(narrowFileName.c_str(), "r");
      if (bio == nullptr)
         return false;

      X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);

      if (certificate == nullptr)
         return false;

      bool built = false;

      const ASN1_OCTET_STRING *keyIdentifier = X509_get0_authority_key_id(certificate);
      const ASN1_INTEGER *serial = X509_get0_serialNumber(certificate);

      if (keyIdentifier != nullptr && serial != nullptr)
      {
         // The serial has to be its DER CONTENT octets, and that is not the same as
         // the bytes OpenSSL keeps in the ASN1_INTEGER. DER encodes an INTEGER in
         // two's complement, so a positive serial whose top bit is set carries a
         // leading zero byte to keep it positive - RFC 9773's own example is serial
         // 0x8765432101 encoding as 00 87 65 43 21 - while ASN1_STRING_get0_data
         // hands back the magnitude without it. Getting that wrong costs nothing
         // visible: the identifier is simply one the CA has never issued, it answers
         // 404, and this server quietly falls back to deciding for itself. Which is
         // to say it would have been wrong for about half of all certificates and
         // nobody would have noticed. So the rule is applied explicitly below.
         BIGNUM *serialNumber = ASN1_INTEGER_to_BN(serial, nullptr);

         if (serialNumber != nullptr)
         {
            std::vector<unsigned char> serialBytes;

            const int magnitudeLength = BN_num_bytes(serialNumber);

            if (magnitudeLength == 0)
            {
               // Serial zero. Not legal in a public certificate, but DER encodes it
               // as a single zero octet rather than as nothing, and an empty
               // component would produce an identifier with a dot and no serial.
               serialBytes.push_back(0x00);
            }
            else
            {
               serialBytes.resize(magnitudeLength);
               BN_bn2bin(serialNumber, &serialBytes[0]);

               // The rule this exists for: DER writes an INTEGER in two's
               // complement, so a positive value whose top bit is set needs a
               // leading zero octet or it would read as negative.
               if ((serialBytes[0] & 0x80) != 0)
                  serialBytes.insert(serialBytes.begin(), 0x00);
            }

            BN_free(serialNumber);

            ariId = BuildAriId(ASN1_STRING_get0_data(keyIdentifier), ASN1_STRING_length(keyIdentifier),
                               &serialBytes[0], (int) serialBytes.size());

            built = !ariId.IsEmpty();
         }
      }

      X509_free(certificate);

      return built;
   }

   time_t
   AcmeClient::GetRenewalTimeInWindow(const AnsiString &ariId, time_t windowStart, time_t windowEnd)
   {
      if (windowStart <= 0)
         return 0;

      if (windowEnd <= windowStart)
         return windowStart;

      // A stable point in the window, derived from the certificate's identifier.
      // Any cheap spreading function does; this one is FNV-1a because it is four
      // lines and has no dependencies.
      unsigned __int64 hash = 14695981039346656037ULL;

      for (int i = 0; i < ariId.GetLength(); i++)
      {
         hash ^= (unsigned char) ariId.GetAt(i);
         hash *= 1099511628211ULL;
      }

      const time_t span = windowEnd - windowStart;

      return windowStart + (time_t) (hash % (unsigned __int64) span);
   }

   bool
   AcmeClient::FetchSuggestedRenewalTime_(time_t &renewAt)
   {
      renewAt = 0;

      if (url_renewal_info_.IsEmpty())
         return false;

      AnsiString ariId;

      if (!GetCertificateAriId(GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem"), ariId))
         return false;

      AnsiString url = url_renewal_info_;

      if (url.Right(1) != "/")
         url += "/";

      url += ariId;

      HttpResponse response;

      if (!Transact_(url, "GET", "", response) || response.status_code != 200)
         return false;

      time_t windowStart = ParseRfc3339(JsonStringValue_(response.body, "start"));
      time_t windowEnd = ParseRfc3339(JsonStringValue_(response.body, "end"));

      if (windowStart <= 0)
         return false;

      renewAt = GetRenewalTimeInWindow(ariId, windowStart, windowEnd);

      return renewAt > 0;
   }

   time_t
   AcmeClient::GetRenewalTime(time_t notBefore, time_t notAfter)
   {
      if (notAfter <= 0)
         return 0;

      // An unreadable or nonsensical notBefore is treated as the 90-day lifetime
      // this server has always assumed, rather than as a reason to refuse to renew.
      time_t lifetime = (notBefore > 0 && notAfter > notBefore) ? (notAfter - notBefore)
                                                               : static_cast<time_t>(90) * 86400;

      // Two thirds through. The remaining third is the margin, and this server
      // escalates a failing renewal to the error log well inside it.
      time_t renewAt = notBefore > 0 ? notBefore + (lifetime / 3) * 2
                                     : notAfter - lifetime / 3;

      // A floor, for the very short lifetimes the industry is heading towards and
      // for anything experimental below them: never leave less than a day, because a
      // renewal that fails wants at least one more scheduled attempt before the
      // certificate dies, and the task runs hourly.
      time_t latest = notAfter - 86400;

      if (renewAt > latest)
         renewAt = latest;

      // ...and a certificate whose whole life is shorter than that floor is renewed
      // from the moment it is issued, which is the only honest answer.
      if (renewAt < notBefore)
         renewAt = notBefore;

      return renewAt;
   }

   // static
   bool
   AcmeClient::ReadCertificateNames(const String &certificateFile, std::vector<AnsiString> &names)
   {
      names.clear();

      X509 *certificate = ReadPemCertificateFile(certificateFile);
      if (certificate == nullptr)
         return false;

      CertificateNames(certificate, names);
      X509_free(certificate);
      return true;
   }

   // static
   bool
   AcmeClient::ReadCertificateNamesFromPem(const AnsiString &pem, std::vector<AnsiString> &names)
   {
      names.clear();

      X509 *certificate = ReadPemCertificate(pem);
      if (certificate == nullptr)
         return false;

      CertificateNames(certificate, names);
      X509_free(certificate);
      return true;
   }

   // static
   std::vector<AnsiString>
   AcmeClient::ParseDomainList(const AnsiString &list)
   {
      std::vector<AnsiString> domains;

      for (AnsiString domain : StringParser::SplitString(list, ","))
      {
         domain.Trim();
         domain.MakeLower();

         // The root's dot, which a host name written fully qualified ends with,
         // is no part of a name a certificate carries, and a CA refuses an
         // identifier that holds it.
         if (domain.EndsWith("."))
            domain = domain.Left(domain.GetLength() - 1);

         if (domain.IsEmpty())
            continue;

         // A name no certificate can carry would be missing from every one, so
         // that every check found the certificate due and every order - which
         // names every host - failed, the renewal for the other hosts with it.
         if (!IsUsableHostName(domain))
            continue;

         // Once each: a name written twice would be two identifiers of one
         // order, which a CA refuses (RFC 8555 section 7.1.3 has them distinct).
         if (std::find(domains.begin(), domains.end(), domain) == domains.end())
            domains.push_back(domain);
      }

      return domains;
   }

   // static
   std::vector<AnsiString>
   AcmeClient::UnusableDomains(const AnsiString &list)
   {
      std::vector<AnsiString> unusable;

      for (AnsiString domain : StringParser::SplitString(list, ","))
      {
         domain.Trim();

         AnsiString name = domain;
         name.MakeLower();
         if (name.EndsWith("."))
            name = name.Left(name.GetLength() - 1);

         if (name.IsEmpty() || IsUsableHostName(name))
            continue;

         // As written, for the administrator to find it in the list; printable
         // ASCII alone, since the status is JSON and the log a line, any other
         // byte as \xHH (ShownName), so that no name is shown empty.
         const AnsiString shown = ShownName(domain, 300);
         if (std::find(unusable.begin(), unusable.end(), shown) == unusable.end())
            unusable.push_back(shown);
      }

      return unusable;
   }

   // static
   bool
   AcmeClient::IsUsableHostName(const AnsiString &name)
   {
      const int length = name.GetLength();
      if (length < 1 || length > 253)
         return false;

      int labelStart = 0;
      bool lastLabelAllDigits = true;

      for (int i = 0; i <= length; i++)
      {
         const bool end = i == length;
         const char character = end ? '.' : name[i];

         if (character == '.')
         {
            const int labelLength = i - labelStart;
            if (labelLength < 1 || labelLength > 63)
               return false;

            if (name[labelStart] == '-' || name[i - 1] == '-')
               return false;

            if (!end)
            {
               labelStart = i + 1;
               lastLabelAllDigits = true;
            }

            continue;
         }

         const bool letter = character >= 'a' && character <= 'z';
         const bool digit = character >= '0' && character <= '9';

         if (!letter && !digit && character != '-')
            return false;

         if (!digit)
            lastLabelAllDigits = false;
      }

      // 192.0.2.1 is four labels of digits: an address, which an order of type
      // dns does not name, whatever a CA does with type ip.
      return !lastLabelAllDigits;
   }

   // static
   std::vector<AnsiString>
   AcmeClient::MissingNames(const std::vector<AnsiString> &certificateNames, const std::vector<AnsiString> &configured)
   {
      std::vector<AnsiString> missing;

      for (size_t i = 0; i < configured.size(); i++)
      {
         AnsiString wanted = configured[i];
         wanted.MakeLower();

         bool covered = false;

         for (size_t c = 0; c < certificateNames.size() && !covered; c++)
         {
            AnsiString name = certificateNames[c];
            name.MakeLower();

            if (name == wanted)
            {
               covered = true;
               break;
            }

            // "*.example.com" stands for one label: a.example.com, never
            // example.com itself or a.b.example.com (RFC 6125 section 6.4.3).
            if (name.StartsWith("*.") && name.GetLength() > 2)
            {
               const AnsiString suffix = name.Mid(1);
               const int dot = wanted.Find(".");

               if (dot > 0 && wanted.Mid(dot) == suffix)
                  covered = true;
            }
         }

         if (!covered)
            missing.push_back(configured[i]);
      }

      return missing;
   }

   bool
   AcmeClient::ShouldRenewNow(AnsiString &reason)
   {
      reason = "";

      time_t notBefore = 0;
      time_t notAfter = 0;

      const String certificateFile = GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");

      const std::vector<AnsiString> unusable = UnusableDomains(IniFileSettings::Instance()->GetAcmeDomains());
      if (!unusable.empty())
         LOG_APPLICATION("ACME: AcmeDomains holds " + String(JoinNames(unusable)) +
            ", which no certificate this server orders can name - a host name of letters, digits and hyphens is needed, an internationalised one in its xn-- form, and no wildcard or IP address - so it is left out of the check and of the order.");

      if (!ReadCertificateDates(certificateFile, notBefore, notAfter))
      {
         reason = "missing";
         return true;
      }

      // Every configured name, or due now: from the file alone, before the CA is
      // asked anything, since the answer does not depend on it.
      std::vector<AnsiString> names;
      if (ReadCertificateNames(certificateFile, names))
      {
         const std::vector<AnsiString> missing = MissingNames(names, ParseDomainList(IniFileSettings::Instance()->GetAcmeDomains()));

         if (!missing.empty())
         {
            LOG_APPLICATION("ACME: The certificate in use does not name " + String(JoinNames(missing)) +
               ", which AcmeDomains lists, so a certificate that names every one is due now.");
            reason = "names";
            return true;
         }
      }

      const time_t now = time(nullptr);

      // The CA's opinion, if it has one and will tell us.
      if (FetchDirectory_())
      {
         time_t suggested = 0;

         if (FetchSuggestedRenewalTime_(suggested))
         {
            // Never later than a day before expiry, whatever the CA says. ARI is
            // advisory and the certificate is not: RFC 9773 has the client respect
            // the window but still renew in time, and a CA that returns a window
            // past this certificate's own expiry - through a bug, a stale record or
            // an identifier collision - must not be able to take TLS down here.
            const time_t latest = notAfter - 86400;

            if (suggested > latest)
               suggested = latest;

            if (now < suggested)
               return false;

            reason = "ari";
            return true;
         }
      }

      if (now < GetRenewalTime(notBefore, notAfter))
         return false;

      reason = "lifetime";
      return true;
   }

   bool
   AcmeClient::Transact_(const AnsiString &url, const AnsiString &method, const AnsiString &payload, HttpResponse &response)
   {
      // OpenTelemetry: a client span for the outbound request, whose traceparent
      // goes out with the request's headers below. No-op unless OtelEndpoint is
      // configured.
      OtelSpanScope otelSpan("http.request", OtelSpanKindClient, AnsiString());

      // https, or plain http to a loopback address alone (ParseAcmeUrl says why).
      AnsiString host;
      AnsiString port;
      AnsiString path;

      if (!ParseAcmeUrl(url, host, port, path))
      {
         LOG_DEBUG("ACME: " + String(method) + " " + String(url) + " refused: an ACME URL must be https, or http to a loopback address.");
         return false;
      }

      otelSpan.AddAttribute("http.host", host);

      // Through HttpsClient, whose deadline bounds the whole request - the name
      // lookup, the connection, the handshake, sending and reading the answer to
      // its end - on every platform. The synchronous client this replaces handed
      // SO_RCVTIMEO a DWORD, which Linux refuses (it takes a timeval), and Asio's
      // synchronous calls on POSIX wait with no timeout anyway: a CA that took the
      // connection and never answered held the thread - the hourly renewal task's -
      // for good. Certificate verification against the machine's store with the
      // host name checked, SNI, the TLS 1.2 floor and the shared client settings
      // (SslContextInitializer::InitClient) are HttpsClient's, as they were this
      // client's.
      //
      // Accept: */* because HttpsClient asks for JSON unless told otherwise, and a
      // certificate comes back as application/pem-certificate-chain; the old client
      // named no type at all.
      std::vector<AnsiString> headers;
      headers.push_back("Accept: */*");

      // W3C trace context: propagate this request's span. Empty (and no header at
      // all) when tracing is disabled.
      const AnsiString otelTraceparent = otelSpan.GetTraceparentValue();
      if (!otelTraceparent.IsEmpty())
         headers.push_back("traceparent: " + otelTraceparent);

      HttpsClient::Response httpsResponse;
      String error;
      if (!HttpsClient::Request(method, url, headers, "application/jose+json", payload, httpsResponse, error,
                                HttpsTimeoutSeconds, MaxResponseSize))
      {
         LOG_DEBUG("ACME: " + String(method) + " " + String(url) + " failed: " + error);
         return false;
      }

      response.status_code = httpsResponse.status_code;

      const std::string headerBlock(httpsResponse.headers.c_str());
      response.nonce = GetHeaderValue(headerBlock, "replay-nonce");
      response.location = GetHeaderValue(headerBlock, "location");
      response.retry_after = GetHeaderValue(headerBlock, "retry-after");
      response.body = httpsResponse.body;

      if (!response.nonce.IsEmpty())
         nonce_ = response.nonce;

      NoteProblem_(response);

      return true;
   }

   void
   AcmeClient::NoteProblem_(const HttpResponse &response)
   {
      if (response.status_code >= 200 && response.status_code < 300)
      {
         failure_.problem_type = "";
         failure_.problem_detail = _T("");
         return;
      }

      // A badNonce is answered by SignedPost_ asking again, and is not what an
      // attempt failed of; anything else the CA explains is (ReadProblem).
      Problem problem;
      if (!ReadProblem(response.body, problem))
         return;

      failure_.problem_type = problem.type;
      failure_.problem_detail = problem.detail;

      // RFC 8555 section 6.6: a rate limit is answered with this type and, as a
      // rule, a Retry-After saying when to come back. Kept once seen in this
      // attempt, whatever the CA says after it.
      if (problem.rate_limited)
      {
         failure_.rate_limited = true;
         failure_.retry_after_seconds = ParseRetryAfter(response.retry_after, time(nullptr));
      }
   }

   void
   AcmeClient::Fail_(const AnsiString &step, const String &sentence)
   {
      if (!failure_.step.IsEmpty())
         return;

      failure_.step = step;
      failure_.sentence = sentence;
   }

   // static
   bool
   AcmeClient::ReadProblem(const AnsiString &body, Problem &problem)
   {
      problem = Problem();

      const AnsiString type = JsonStringValue_(body, "type");

      if (!type.StartsWith("urn:ietf:params:acme:error:") || type == "urn:ietf:params:acme:error:badNonce")
         return false;

      problem.type = CleanText(type, 100);
      problem.detail = ProblemDetail(JsonStringValue_(body, "detail"));
      problem.rate_limited = type == "urn:ietf:params:acme:error:rateLimited";
      return true;
   }

   // static
   int
   AcmeClient::ParseRetryAfter(const AnsiString &value, time_t now)
   {
      AnsiString text = value;
      text.Trim();

      if (text.IsEmpty())
         return -1;

      // delay-seconds: one or more digits and nothing else.
      bool digits = text.GetLength() <= 9;
      for (int i = 0; i < text.GetLength() && digits; i++)
         digits = text[i] >= '0' && text[i] <= '9';

      if (digits)
         return (int) strtol(text.c_str(), nullptr, 10);

      // IMF-fixdate, which is fixed-width: "Sun, 06 Nov 1994 08:49:37 GMT". Read
      // by position rather than with a %s scan, whose buffer-size arguments the
      // POSIX shim of sscanf_s does not take.
      if (text.GetLength() != 29 || text.Mid(3, 2) != ", " || text[7] != ' ' || text[11] != ' ' ||
          text[16] != ' ' || text[19] != ':' || text[22] != ':' || text.Mid(25) != " GMT")
         return -1;

      auto number = [&text](int start, int length) -> int
      {
         int result = 0;
         for (int i = start; i < start + length; i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return -1;
            result = result * 10 + (text[i] - '0');
         }
         return result;
      };

      static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

      const AnsiString month = text.Mid(8, 3);
      int monthIndex = -1;
      for (int m = 0; m < 12; m++)
      {
         if (month == months[m])
            monthIndex = m;
      }

      const int day = number(5, 2);
      const int year = number(12, 4);
      const int hour = number(17, 2);
      const int minute = number(20, 2);
      const int second = number(23, 2);

      if (monthIndex < 0 || day < 1 || day > 31 || year < 1970 || hour < 0 || hour > 23 ||
          minute < 0 || minute > 59 || second < 0 || second > 60)
         return -1;

      tm parts = {};
      parts.tm_year = year - 1900;
      parts.tm_mon = monthIndex;
      parts.tm_mday = day;
      parts.tm_hour = hour;
      parts.tm_min = minute;
      parts.tm_sec = second;

      const time_t at = _mkgmtime(&parts);
      if (at == -1)
         return -1;

      if (at <= now)
         return 0;

      const time_t seconds = at - now;
      return seconds > 0x7fffffff ? 0x7fffffff : (int) seconds;
   }

   AnsiString
   AcmeClient::Base64Url_(const unsigned char *data, int length)
   {
      AnsiString encoded = Base64::Encode((const char*) data, length);

      AnsiString result;
      for (int i = 0; i < encoded.GetLength(); i++)
      {
         char character = encoded[i];

         switch (character)
         {
         case '+':
            result += "-";
            break;
         case '/':
            result += "_";
            break;
         case '=':
         case '\r':
         case '\n':
            break;
         default:
            result += character;
            break;
         }
      }

      return result;
   }

   AnsiString
   AcmeClient::Base64Url_(const AnsiString &data)
   {
      return Base64Url_((const unsigned char*) data.c_str(), data.GetLength());
   }

   // static
   AnsiString
   AcmeClient::GetHeaderValue(const std::string &headers, const std::string &headerName)
   {
      std::string lowerHeaders = headers;
      for (size_t i = 0; i < lowerHeaders.size(); i++)
         lowerHeaders[i] = (char) tolower((unsigned char) lowerHeaders[i]);

      std::string needle = "\r\n" + headerName + ":";

      size_t position = lowerHeaders.find(needle);
      if (position == std::string::npos)
         return "";

      // The block ends with the last header's value and no line break after it
      // (HttpsClient cuts it at the blank line, as Transact_ used to), so the
      // header that comes last runs to the end of the block. Field order carries
      // no meaning (RFC 9110 section 5.3): a CA that sends Replay-Nonce or Location
      // last must not have it read as absent, which it was until 6.3.6.
      size_t valueStart = position + needle.size();
      size_t lineEnd = headers.find("\r\n", valueStart);
      if (lineEnd == std::string::npos)
         lineEnd = headers.size();

      AnsiString value(headers.substr(valueStart, lineEnd - valueStart));
      value.Trim();
      return value;
   }

   // static
   int
   AcmeClient::FindChallengeOfType(const AnsiString &json, const AnsiString &challengeType)
   {
      // Accepts '"type":"http-01"', '"type": "http-01"' and any other amount
      // of whitespace around the colon. A bare mention of the type string
      // elsewhere - an error detail, some other key's value - does not count:
      // the value must be preceded, across whitespace, by a colon and the
      // "type" key.
      AnsiString typeValue = "\"" + challengeType + "\"";
      const AnsiString typeKey = "\"type\"";

      int searchFrom = 0;
      for (;;)
      {
         int valuePosition = json.Find(typeValue, searchFrom);
         if (valuePosition < 0)
            return -1;

         int position = valuePosition - 1;

         while (position >= 0 && (json[position] == ' ' || json[position] == '\t' ||
                                  json[position] == '\r' || json[position] == '\n'))
            position--;

         if (position >= 0 && json[position] == ':')
         {
            position--;

            while (position >= 0 && (json[position] == ' ' || json[position] == '\t' ||
                                     json[position] == '\r' || json[position] == '\n'))
               position--;

            int keyStart = position - typeKey.GetLength() + 1;
            if (keyStart >= 0 && json.Mid(keyStart, typeKey.GetLength()) == typeKey)
               return valuePosition;
         }

         searchFrom = valuePosition + 1;
      }
   }

   AnsiString
   AcmeClient::JsonStringValue_(const AnsiString &json, const AnsiString &key, int searchFrom)
   {
      AnsiString needle = "\"" + key + "\"";

      int keyPosition = json.Find(needle, searchFrom);
      if (keyPosition < 0)
         return "";

      int colonPosition = json.Find(":", keyPosition + needle.GetLength());
      if (colonPosition < 0)
         return "";

      int valueStart = json.Find("\"", colonPosition);
      if (valueStart < 0)
         return "";

      valueStart++;

      // The quote that ends the string: one an odd run of backslashes comes
      // before is a quote inside it.
      int valueEnd = valueStart;
      for (;;)
      {
         valueEnd = json.Find("\"", valueEnd);
         if (valueEnd < 0)
            return "";

         int backslashes = 0;
         while (valueEnd - backslashes > valueStart && json[valueEnd - backslashes - 1] == '\\')
            backslashes++;

         if (backslashes % 2 == 0)
            break;

         valueEnd++;
      }

      return json.Mid(valueStart, valueEnd - valueStart);
   }

   bool
   AcmeClient::LoadOrCreateAccountKey_()
   {
      String directory = GetCertificateDirectory();
      FileUtilities::CreateDirectory(directory);

      String keyFile = directory + FileUtilities::PathSeparator + _T("account.key");
      AnsiString narrowKeyFile = keyFile;

      if (FileUtilities::Exists(keyFile))
      {
         BIO *bio = BIO_new_file(narrowKeyFile.c_str(), "r");
         if (bio != nullptr)
         {
            account_key_ = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
         }

         if (account_key_ != nullptr)
            return true;
      }

      account_key_ = EVP_RSA_gen(2048);
      if (account_key_ == nullptr)
         return false;

      BIO *bio = BIO_new_file(narrowKeyFile.c_str(), "w");
      if (bio == nullptr)
         return false;

      bool written = PEM_write_bio_PrivateKey(bio, account_key_, nullptr, nullptr, 0, nullptr, nullptr) == 1;
      BIO_free(bio);

      return written;
   }

   AnsiString
   AcmeClient::BuildJwk_() const
   {
      // RFC 7638: members in lexicographic order - e, kty, n.
      BIGNUM *modulus = nullptr;
      BIGNUM *exponent = nullptr;

      if (EVP_PKEY_get_bn_param(account_key_, OSSL_PKEY_PARAM_RSA_N, &modulus) != 1 ||
          EVP_PKEY_get_bn_param(account_key_, OSSL_PKEY_PARAM_RSA_E, &exponent) != 1)
      {
         if (modulus != nullptr)
            BN_free(modulus);
         if (exponent != nullptr)
            BN_free(exponent);

         return "";
      }

      std::vector<unsigned char> modulusBytes(BN_num_bytes(modulus));
      std::vector<unsigned char> exponentBytes(BN_num_bytes(exponent));

      BN_bn2bin(modulus, modulusBytes.data());
      BN_bn2bin(exponent, exponentBytes.data());

      BN_free(modulus);
      BN_free(exponent);

      AnsiString jwk;
      jwk += "{\"e\":\"" + Base64Url_(exponentBytes.data(), (int) exponentBytes.size()) + "\",";
      jwk += "\"kty\":\"RSA\",";
      jwk += "\"n\":\"" + Base64Url_(modulusBytes.data(), (int) modulusBytes.size()) + "\"}";

      return jwk;
   }

   AnsiString
   AcmeClient::GetJwkThumbprint_() const
   {
      AnsiString jwk = BuildJwk_();

      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256((const unsigned char*) jwk.c_str(), jwk.GetLength(), digest);

      return Base64Url_(digest, SHA256_DIGEST_LENGTH);
   }

   AnsiString
   AcmeClient::SignJws_(const AnsiString &url, const AnsiString &payload, bool useJwk)
   {
      AnsiString protectedHeader;
      protectedHeader += "{\"alg\":\"RS256\",";

      if (useJwk)
         protectedHeader += "\"jwk\":" + BuildJwk_() + ",";
      else
         protectedHeader += "\"kid\":\"" + account_url_ + "\",";

      protectedHeader += "\"nonce\":\"" + nonce_ + "\",";
      protectedHeader += "\"url\":\"" + url + "\"}";

      AnsiString encodedHeader = Base64Url_(protectedHeader);
      AnsiString encodedPayload = Base64Url_(payload);

      AnsiString signingInput = encodedHeader + "." + encodedPayload;

      AnsiString signature;

      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (context == nullptr)
         return "";

      if (EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, account_key_) == 1)
      {
         size_t signatureLength = 0;
         if (EVP_DigestSign(context, nullptr, &signatureLength,
                (const unsigned char*) signingInput.c_str(), signingInput.GetLength()) == 1)
         {
            std::vector<unsigned char> signatureBytes(signatureLength);

            if (EVP_DigestSign(context, signatureBytes.data(), &signatureLength,
                   (const unsigned char*) signingInput.c_str(), signingInput.GetLength()) == 1)
            {
               signature = Base64Url_(signatureBytes.data(), (int) signatureLength);
            }
         }
      }

      EVP_MD_CTX_free(context);

      if (signature.IsEmpty())
         return "";

      AnsiString jws;
      jws += "{\"protected\":\"" + encodedHeader + "\",";
      jws += "\"payload\":\"" + encodedPayload + "\",";
      jws += "\"signature\":\"" + signature + "\"}";

      return jws;
   }

   bool
   AcmeClient::SignedPost_(const AnsiString &url, const AnsiString &payload, HttpResponse &response)
   {
      for (int attempt = 0; attempt < 2; attempt++)
      {
         if (nonce_.IsEmpty() && !FetchNonce_())
            return false;

         bool useJwk = account_url_.IsEmpty();

         AnsiString jws = SignJws_(url, payload, useJwk);
         nonce_ = ""; // A nonce may only be used once.

         if (jws.IsEmpty())
            return false;

         if (!Transact_(url, "POST", jws, response))
            return false;

         // Retry once with a fresh nonce if the server rejected ours.
         if (response.status_code == 400 && response.body.Find("urn:ietf:params:acme:error:badNonce") >= 0)
            continue;

         return true;
      }

      return false;
   }

   bool
   AcmeClient::FetchDirectory_()
   {
      AnsiString directoryUrl = IniFileSettings::Instance()->GetAcmeDirectoryUrl();

      HttpResponse response;
      if (!Transact_(directoryUrl, "GET", "", response) || response.status_code != 200)
         return false;

      url_new_nonce_ = JsonStringValue_(response.body, "newNonce");
      url_new_account_ = JsonStringValue_(response.body, "newAccount");
      url_new_order_ = JsonStringValue_(response.body, "newOrder");

      // RFC 9773. Optional, and its absence is not an error: a CA that does not
      // implement ARI simply does not advertise it, and this server then decides for
      // itself when to renew exactly as it did before.
      url_renewal_info_ = JsonStringValue_(response.body, "renewalInfo");

      return !url_new_nonce_.IsEmpty() && !url_new_account_.IsEmpty() && !url_new_order_.IsEmpty();
   }

   bool
   AcmeClient::FetchNonce_()
   {
      HttpResponse response;
      if (!Transact_(url_new_nonce_, "GET", "", response))
         return false;

      return !nonce_.IsEmpty();
   }

   bool
   AcmeClient::RegisterAccount_()
   {
      AnsiString contactEmail = IniFileSettings::Instance()->GetAcmeContactEmail();

      AnsiString payload = "{\"termsOfServiceAgreed\":true";
      if (!contactEmail.IsEmpty())
         payload += ",\"contact\":[\"mailto:" + contactEmail + "\"]";
      payload += "}";

      HttpResponse response;
      if (!SignedPost_(url_new_account_, payload, response))
         return false;

      if (response.status_code != 200 && response.status_code != 201)
         return false;

      account_url_ = response.location;

      return !account_url_.IsEmpty();
   }

   bool
   AcmeClient::CreateOrder_(const std::vector<AnsiString> &domains, HttpResponse &orderResponse)
   {
      AnsiString payload = "{\"identifiers\":[";

      for (size_t i = 0; i < domains.size(); i++)
      {
         if (i > 0)
            payload += ",";

         payload += "{\"type\":\"dns\",\"value\":\"" + domains[i] + "\"}";
      }

      payload += "]}";

      if (!SignedPost_(url_new_order_, payload, orderResponse))
         return false;

      return orderResponse.status_code == 201;
   }

   bool
   AcmeClient::CompleteAuthorization_(const AnsiString &authorizationUrl)
   {
      // Every failure below says which step failed. They used to return false in
      // silence, so a renewal could fail with nothing at all in the log between
      // "Requesting a new certificate" and the next hourly attempt saying the same
      // thing - and no way for an administrator to tell a DNS problem from a
      // firewalled port 80 from a CA rate limit.
      //
      // Application log rather than reported errors: each of these is one step of
      // one attempt, the attempt is retried in an hour, and the outcome that
      // actually matters - a renewal still failing while the certificate in use is
      // about to expire - is reported once, by AcmeRenewalTask::DoWork.

      // POST-as-GET: empty string payload.
      HttpResponse authorizationResponse;
      if (!SignedPost_(authorizationUrl, "", authorizationResponse) || authorizationResponse.status_code != 200)
      {
         String message;
         message.Format(_T("ACME: Could not read the authorization (HTTP status %d). URL: %s"),
            authorizationResponse.status_code, String(authorizationUrl).c_str());
         LOG_APPLICATION(message);
         Fail_("authorization", _T("An authorization of the order could not be read from the CA."));

         return false;
      }

      // The identifier this authorization is for, so every message below can
      // say WHICH domain failed - the log used to leave an administrator with
      // three configured hostnames and no way to tell them apart (issue #34).
      // The identifier object is the first thing in the authorization, so the
      // first "value" key is its value.
      String domain = String(JsonStringValue_(authorizationResponse.body, "value"));
      if (domain.IsEmpty())
         domain = _T("(unknown)");

      AnsiString status = JsonStringValue_(authorizationResponse.body, "status");
      if (status == "valid")
         return true;

      // Locate the http-01 challenge object. Whitespace-tolerantly: boulder
      // pretty-prints its JSON ('"type": "http-01"', with a space), so the
      // compact-form search this replaces never matched a real Let's Encrypt
      // response - every issuance failed right here with a message that blamed
      // the CA (issue #34).
      int challengePosition = FindChallengeOfType(authorizationResponse.body, "http-01");
      if (challengePosition < 0)
      {
         // The raw response is the only evidence of what the CA actually
         // offered; the reporter of issue #34 could not root-cause from the log
         // precisely because it was not included.
         LOG_APPLICATION(Formatter::Format("ACME: Authorization for {0} offers no http-01 challenge. Response: {1}",
            domain, String(authorizationResponse.body.Mid(0, 500))));
         Fail_("authorization", Formatter::Format("The CA offers no http-01 challenge for {0}, the only kind this server answers.", domain));
         return false;
      }

      AnsiString challengeObject = ExtractEnclosingObject(authorizationResponse.body, challengePosition);

      AnsiString challengeUrl = JsonStringValue_(challengeObject, "url");
      AnsiString token = JsonStringValue_(challengeObject, "token");

      if (challengeUrl.IsEmpty() || token.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("ACME: The http-01 challenge for {0} carried no url or no token, so it cannot be answered.", domain));
         Fail_("authorization", Formatter::Format("The CA's http-01 challenge for {0} carried no URL or no token.", domain));
         return false;
      }

      AnsiString keyAuthorization = token + "." + GetJwkThumbprint_();

      // Make the challenge available to whichever listener answers port 80.
      AcmeChallengeStore::Set(token, keyAuthorization);

      // Serve the challenge on the configured HTTP port. When the web
      // services server already listens there, it serves the challenge
      // from the store and no transient listener is needed.
      AcmeChallengeServer challengeServer;

      bool sharedListener = WebServicesServer::IsListeningOnPort(IniFileSettings::Instance()->GetAcmeHttpPort());

      if (!sharedListener)
      {
         if (!challengeServer.Start(IniFileSettings::Instance()->GetAcmeHttpPort()))
         {
            LOG_APPLICATION("ACME: Failed to start the http-01 challenge listener. Is the port in use?");
            Fail_("authorization", _T("The http-01 challenge listener could not be started on AcmeHttpPort: another program may hold the port."));
            return false;
         }

         challengeServer.SetChallenge(token, keyAuthorization);
      }

      // Tell the CA to validate.
      HttpResponse challengeResponse;
      if (!SignedPost_(challengeUrl, "{}", challengeResponse))
      {
         LOG_APPLICATION(Formatter::Format("ACME: Could not ask the CA to validate the http-01 challenge for {0}.", domain));
         Fail_("authorization", Formatter::Format("The CA could not be asked to validate the http-01 challenge for {0}.", domain));
         return false;
      }

      // Poll the authorization until it leaves the pending state.
      for (int attempt = 0; attempt < MaxPollAttempts; attempt++)
      {
         Sleep(PollIntervalMilliseconds);

         HttpResponse pollResponse;
         if (!SignedPost_(authorizationUrl, "", pollResponse) || pollResponse.status_code != 200)
         {
            String message;
            message.Format(_T("ACME: Could not read the authorization while waiting for validation (HTTP status %d)."),
               pollResponse.status_code);
            LOG_APPLICATION(message);
            Fail_("authorization", Formatter::Format("The authorization for {0} could not be read from the CA while it was being validated.", domain));

            return false;
         }

         status = JsonStringValue_(pollResponse.body, "status");

         if (status == "valid")
            return true;

         if (status == "invalid")
         {
            LOG_APPLICATION(Formatter::Format("ACME: Challenge validation failed for {0}: {1}", domain, String(pollResponse.body.Mid(0, 500))));

            // The reason is the authorization's challenge error, which is a
            // problem document inside it, not the answer's own: read it so that
            // the status says what the CA saw (no DNS record, a 404, a timeout).
            const int errorAt = pollResponse.body.Find("\"error\"");
            if (errorAt >= 0)
            {
               failure_.problem_type = CleanText(JsonStringValue_(pollResponse.body, "type", errorAt), 100);
               failure_.problem_detail = ProblemDetail(JsonStringValue_(pollResponse.body, "detail", errorAt));
            }

            Fail_("authorization", Formatter::Format("The CA could not validate the http-01 challenge for {0}: it could not fetch the answer from this server on port 80.", domain));
            return false;
         }
      }

      LOG_APPLICATION(Formatter::Format("ACME: Timed out waiting for challenge validation of {0}.", domain));
      Fail_("authorization", Formatter::Format("The CA had not validated the http-01 challenge for {0} after a minute.", domain));
      return false;
   }

   bool
   AcmeClient::FinalizeOrder_(const AnsiString &finalizeUrl, const AnsiString &orderUrl, const std::vector<AnsiString> &domains)
   {
      // Reuse the existing certificate key when configured (the default):
      // a stable key keeps published DANE TLSA "3 1 1" records valid
      // across renewals.
      EVP_PKEY *domainKey = nullptr;

      if (IniFileSettings::Instance()->GetAcmeReuseKey())
      {
         AnsiString keyPath = AnsiString(GetCertificateDirectory() + FileUtilities::PathSeparator + _T("privkey.pem"));

         // A BIO, not a FILE*: see GetCertificateTlsa for why a FILE* of this
         // executable's C runtime handed to the OpenSSL DLL ends the process
         // (issue #93). This is the renewal half of that: with an existing key on
         // disk - every renewal, under the default AcmeReuseKey - the process
         // died here, before the order was even finalised.
         BIO *keyBio = BIO_new_file(keyPath.c_str(), "r");
         if (keyBio != nullptr)
         {
            domainKey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
            BIO_free(keyBio);

            if (domainKey != nullptr)
               LOG_APPLICATION("ACME: Reusing the existing certificate key (keeps published TLSA records valid).");
         }
      }

      if (domainKey == nullptr)
         domainKey = EVP_RSA_gen(2048);

      if (domainKey == nullptr)
      {
         Fail_("finalize", "A key for the certificate could not be read or made.");
         return false;
      }

      // Build the CSR with all domains as subject alternative names.
      AnsiString sanList;
      for (size_t i = 0; i < domains.size(); i++)
      {
         if (i > 0)
            sanList += ",";
         sanList += "DNS:" + domains[i];
      }

      X509_REQ *request = X509_REQ_new();

      bool csrOk = false;
      std::vector<unsigned char> csrDer;

      if (request != nullptr && X509_REQ_set_pubkey(request, domainKey) == 1)
      {
         X509_EXTENSION *sanExtension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, sanList.c_str());

         if (sanExtension != nullptr)
         {
            STACK_OF(X509_EXTENSION) *extensions = sk_X509_EXTENSION_new_null();
            sk_X509_EXTENSION_push(extensions, sanExtension);

            if (X509_REQ_add_extensions(request, extensions) == 1 &&
                X509_REQ_sign(request, domainKey, EVP_sha256()) > 0)
            {
               int derLength = i2d_X509_REQ(request, nullptr);
               if (derLength > 0)
               {
                  csrDer.resize(derLength);
                  unsigned char *writePointer = csrDer.data();
                  i2d_X509_REQ(request, &writePointer);
                  csrOk = true;
               }
            }

            sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
         }
      }

      if (request != nullptr)
         X509_REQ_free(request);

      if (!csrOk)
      {
         LOG_APPLICATION("ACME: Could not build the certificate signing request.");
         Fail_("finalize", "The certificate signing request could not be built.");
         EVP_PKEY_free(domainKey);
         return false;
      }

      AnsiString payload = "{\"csr\":\"" + Base64Url_(csrDer.data(), (int) csrDer.size()) + "\"}";

      HttpResponse finalizeResponse;
      if (!SignedPost_(finalizeUrl, payload, finalizeResponse) || finalizeResponse.status_code != 200)
      {
         // The CA's reply body carries the ACME problem document, which is the one
         // thing that says *why* - a rate limit, a CAA record, an unauthorized
         // identifier. It was discarded.
         LOG_APPLICATION(Formatter::Format("ACME: The CA rejected the certificate request (HTTP status {0}). Response: {1}",
            finalizeResponse.status_code, String(finalizeResponse.body.Mid(0, 500))));
         Fail_("finalize", "The CA rejected the certificate signing request.");

         EVP_PKEY_free(domainKey);
         return false;
      }

      // Poll the order until the certificate is issued.
      AnsiString certificateUrl;

      for (int attempt = 0; attempt < MaxPollAttempts; attempt++)
      {
         HttpResponse orderResponse;
         if (!SignedPost_(orderUrl, "", orderResponse) || orderResponse.status_code != 200)
         {
            LOG_APPLICATION(Formatter::Format("ACME: Could not read the order while waiting for the certificate to be issued (HTTP status {0}).",
               orderResponse.status_code));
            Fail_("finalize", "The order could not be read from the CA while the certificate was being issued.");

            EVP_PKEY_free(domainKey);
            return false;
         }

         AnsiString status = JsonStringValue_(orderResponse.body, "status");

         if (status == "valid")
         {
            certificateUrl = JsonStringValue_(orderResponse.body, "certificate");
            break;
         }

         if (status == "invalid")
         {
            LOG_APPLICATION("ACME: Order failed: " + String(orderResponse.body.Mid(0, 500)));
            Fail_("finalize", "The CA marked the order invalid.");
            EVP_PKEY_free(domainKey);
            return false;
         }

         Sleep(PollIntervalMilliseconds);
      }

      if (certificateUrl.IsEmpty())
      {
         // Either the order never left the "processing" state within the polling
         // window, or it went valid without naming a certificate. Both used to be
         // silent.
         LOG_APPLICATION("ACME: The order did not produce a certificate URL within the polling window.");
         Fail_("finalize", "The CA had not issued the certificate a minute after the order was finalized.");

         EVP_PKEY_free(domainKey);
         return false;
      }

      // Download the certificate chain.
      AcmeChecks::Step("certificate");

      HttpResponse certificateResponse;
      if (!SignedPost_(certificateUrl, "", certificateResponse) || certificateResponse.status_code != 200)
      {
         LOG_APPLICATION(Formatter::Format("ACME: Could not download the issued certificate (HTTP status {0}).",
            certificateResponse.status_code));
         Fail_("certificate", "The issued certificate could not be downloaded from the CA.");

         EVP_PKEY_free(domainKey);
         return false;
      }

      // What the CA sent is installed only if it is the certificate this
      // request was for. It used to be written as it came: a body that was not
      // a certificate, one for another key, or one without a name asked for
      // would have replaced a working pair with one no listener could load or
      // no client would accept, and the deployment would have restarted every
      // TLS port onto it. The pair in use stays as it is.
      String unusable;
      if (!CheckIssuedCertificate(certificateResponse.body, domainKey, domains, unusable))
      {
         LOG_APPLICATION("ACME: The certificate the CA issued was not installed: " + unusable + " The certificate and key in use are unchanged.");
         Fail_("certificate", "The certificate the CA issued was not installed: " + unusable);

         EVP_PKEY_free(domainKey);
         return false;
      }

      AcmeChecks::Step("install");

      // Both halves of the new pair are written under temporary names and moved
      // into place only once both writes have succeeded.
      //
      // The order used to be privkey.pem straight over the live key and then
      // fullchain.pem, and a failure on the second write - a full disk, a scanner
      // or a backup holding the file open - left the directory holding the *new*
      // private key beside the *previous* certificate. That pair does not match,
      // and a mismatched pair is not a degraded listener: OpenSSL refuses the key,
      // InitServer returns false, and every port using the ACME certificate stops
      // listening at the next restart. A renewal failure that takes SMTP, IMAP and
      // POP3 down is a worse outcome than the expiry it was trying to avoid.
      //
      // Two renames are not one atomic operation, but FileUtilities::Move is a
      // replacing rename - the destination always names either the old file or the
      // new one, never nothing - and a rename does not fail half-way the way a
      // write does. The key is moved first so that the certificate is never the
      // newer of the two: a certificate whose key has not arrived yet is the same
      // mismatch in the other direction.
      String directory = GetCertificateDirectory();
      String keyFile = directory + FileUtilities::PathSeparator + _T("privkey.pem");
      String certificateFile = directory + FileUtilities::PathSeparator + _T("fullchain.pem");
      String pendingKeyFile = keyFile + _T(".new");
      String pendingCertificateFile = certificateFile + _T(".new");

      AnsiString narrowPendingKeyFile = pendingKeyFile;

      bool keyWritten = false;

      BIO *bio = BIO_new_file(narrowPendingKeyFile.c_str(), "w");
      if (bio != nullptr)
      {
         keyWritten = PEM_write_bio_PrivateKey(bio, domainKey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
         BIO_free(bio);
      }

      EVP_PKEY_free(domainKey);

      if (!keyWritten)
      {
         LOG_APPLICATION(Formatter::Format("ACME: Could not write the new private key to {0}. The certificate and key in use are unchanged.", pendingKeyFile));
         Fail_("install", "The new private key could not be written to the certificate directory; the certificate and key in use are unchanged.");
         return false;
      }

      if (!FileUtilities::WriteToFile(pendingCertificateFile, certificateResponse.body))
      {
         LOG_APPLICATION(Formatter::Format("ACME: Could not write the issued certificate to {0}. The certificate and key in use are unchanged.", pendingCertificateFile));
         Fail_("install", "The issued certificate could not be written to the certificate directory; the certificate and key in use are unchanged.");

         // Leaving this behind would not break anything - nothing reads a .new
         // file - but it would be mistaken for a renewal in progress by the next
         // person to look in the directory.
         FileUtilities::DeleteFile(pendingKeyFile);
         return false;
      }

      if (!FileUtilities::Move(pendingKeyFile, keyFile))
      {
         LOG_APPLICATION(Formatter::Format("ACME: Could not replace {0}. The certificate and key in use are unchanged.", keyFile));
         Fail_("install", "privkey.pem could not be replaced; the certificate and key in use are unchanged.");

         FileUtilities::DeleteFile(pendingKeyFile);
         FileUtilities::DeleteFile(pendingCertificateFile);
         return false;
      }

      if (!FileUtilities::Move(pendingCertificateFile, certificateFile))
      {
         // The one genuinely bad outcome left, and the reason this is an error
         // rather than a log line: the key has been replaced and the certificate
         // has not, so the pair on disk does not match and every listener using it
         // will refuse to start. Say so explicitly, with the file to look at,
         // because the fix is manual.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5993, "AcmeClient::FinalizeOrder_",
            Formatter::Format("The new private key was installed as {0} but the certificate could not be moved from {1} to {2}. The key and certificate on disk no longer match, and any listener using them will refuse to start. Move {1} into place by hand, or restore the pair from a backup.",
               keyFile, pendingCertificateFile, certificateFile));
         Fail_("install", "The new key was installed and the certificate could not be moved into place beside it: the pair on disk no longer matches (error HM5993 says what to move).");

         return false;
      }

      return true;
   }

   bool
   AcmeClient::RequestCertificate()
   {
      AcmeChallengeStore::Clear();

      failure_ = Failure();

      const std::vector<AnsiString> domains = ParseDomainList(IniFileSettings::Instance()->GetAcmeDomains());

      if (domains.empty())
      {
         LOG_APPLICATION("ACME: No domains configured. Set AcmeDomains in hMailServer.ini.");
         Fail_("names", _T("AcmeDomains names no host, so there is nothing to ask the CA for."));
         return false;
      }

      AcmeChecks::Step("directory");

      if (!FetchDirectory_())
      {
         LOG_APPLICATION("ACME: Failed to fetch the ACME directory.");
         Fail_("directory", _T("The CA's ACME directory (AcmeDirectoryUrl) could not be read: the CA did not answer, or did not answer with a directory."));
         return false;
      }

      AcmeChecks::Step("account");

      if (!LoadOrCreateAccountKey_())
      {
         LOG_APPLICATION("ACME: Failed to load or create the account key.");
         Fail_("account", _T("The ACME account key could not be read or made in the certificate directory."));
         return false;
      }

      if (!RegisterAccount_())
      {
         LOG_APPLICATION("ACME: Account registration failed.");
         Fail_("account", _T("The CA did not register this server's ACME account."));
         return false;
      }

      AcmeChecks::Step("order");

      HttpResponse orderResponse;
      if (!CreateOrder_(domains, orderResponse))
      {
         LOG_APPLICATION("ACME: Order creation failed.");
         Fail_("order", _T("The CA did not create an order for the names AcmeDomains lists."));
         return false;
      }

      AnsiString orderUrl = orderResponse.location;
      AnsiString finalizeUrl = JsonStringValue_(orderResponse.body, "finalize");

      // The three checks below are the CA answering 201 with a body this client
      // cannot use. Each returned false without a word, which is the worst kind of
      // failure to diagnose remotely: the log said an order had been created and
      // then said nothing at all. The response is included because it is the only
      // evidence of what the CA actually sent.
      if (orderUrl.IsEmpty() || finalizeUrl.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("ACME: The new order is missing its location header or its finalize URL. Response: {0}",
            String(orderResponse.body.Mid(0, 500))));
         Fail_("order", _T("The CA's new order carried no location or no finalize URL."));
         return false;
      }

      // Complete every authorization in the order.
      int searchPosition = orderResponse.body.Find("\"authorizations\"");
      if (searchPosition < 0)
      {
         LOG_APPLICATION(Formatter::Format("ACME: The new order lists no authorizations. Response: {0}",
            String(orderResponse.body.Mid(0, 500))));
         Fail_("order", _T("The CA's new order listed no authorizations."));
         return false;
      }

      int arrayStart = orderResponse.body.Find("[", searchPosition);
      int arrayEnd = orderResponse.body.Find("]", arrayStart);

      if (arrayStart < 0 || arrayEnd < 0)
      {
         LOG_APPLICATION(Formatter::Format("ACME: The authorization list in the new order could not be parsed. Response: {0}",
            String(orderResponse.body.Mid(0, 500))));
         Fail_("order", _T("The authorization list of the CA's new order could not be read."));
         return false;
      }

      AcmeChecks::Step("authorization");

      AnsiString authorizationArray = orderResponse.body.Mid(arrayStart, arrayEnd - arrayStart + 1);

      int position = 0;
      for (;;)
      {
         int urlStart = authorizationArray.Find("\"", position);
         if (urlStart < 0)
            break;

         int urlEnd = authorizationArray.Find("\"", urlStart + 1);
         if (urlEnd < 0)
            break;

         AnsiString authorizationUrl = authorizationArray.Mid(urlStart + 1, urlEnd - urlStart - 1);
         position = urlEnd + 1;

         if (authorizationUrl.IsEmpty())
            continue;

         if (!CompleteAuthorization_(authorizationUrl))
         {
            LOG_APPLICATION("ACME: Authorization failed for one of the configured domains.");
            Fail_("authorization", _T("The CA did not validate one of the names AcmeDomains lists."));
            return false;
         }
      }

      AcmeChecks::Step("finalize");

      if (!FinalizeOrder_(finalizeUrl, orderUrl, domains))
      {
         LOG_APPLICATION("ACME: Order finalization failed.");
         Fail_("finalize", _T("The CA did not issue the certificate for the order."));
         return false;
      }

      LOG_APPLICATION("ACME: Certificate issued successfully: " + GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem"));

      // Deployment first: the certificate record, the port assignments and the
      // restart are what the renewal exists for. The TLSA line below is a
      // convenience for the administrators who publish DANE, and issue #93 was
      // that convenience taking the service down before the deployment had run.
      // Make the new certificate take effect without manual steps.
      AcmeChecks::Step("install");
      ApplyCertificate_();

      // Publish-ready DANE record for administrators running inbound DANE.
      AnsiString spkiHex;
      if (GetCertificateTlsa(GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem"), spkiHex))
      {
         LOG_APPLICATION("ACME: DANE TLSA record for this certificate: _25._tcp.<mx-host>. IN TLSA 3 1 1 " + String(spkiHex));
      }
      else
      {
         LOG_APPLICATION("ACME: The DANE TLSA record for the new certificate could not be computed from fullchain.pem. The certificate itself is installed; only this log line is missing.");
      }

      AcmeChallengeStore::Clear();

      return true;
   }

   // static
   bool
   AcmeClient::CheckIssuedCertificate(const AnsiString &chainPem, EVP_PKEY *key,
                                       const std::vector<AnsiString> &domains, String &reason)
   {
      reason = _T("");

      X509 *certificate = ReadPemCertificate(chainPem);
      if (certificate == nullptr)
      {
         reason = _T("what the CA sent is not a PEM certificate.");
         return false;
      }

      bool usable = true;

      if (key == nullptr || X509_check_private_key(certificate, key) != 1)
      {
         reason = _T("it is not for the key the request was made with.");
         usable = false;
      }

      if (usable)
      {
         std::vector<AnsiString> names;
         CertificateNames(certificate, names);

         const std::vector<AnsiString> missing = MissingNames(names, domains);
         if (!missing.empty())
         {
            reason = _T("it does not name ") + String(JoinNames(missing)) + _T(", which the order asked for.");
            usable = false;
         }
      }

      if (usable)
      {
         const __int64 notAfter = AsnTime(X509_get0_notAfter(certificate));
         if (notAfter <= (__int64) time(nullptr))
         {
            reason = _T("it has expired already, or its expiry cannot be read.");
            usable = false;
         }
      }

      X509_free(certificate);
      return usable;
   }

   bool
   AcmeClient::GetCertificateTlsa(const String &certificate_file, AnsiString &spki_sha256_hex)
   {
      spki_sha256_hex = "";

      // Through a BIO that OpenSSL opens itself, never a FILE* of ours. On
      // Windows a FILE* handed to the OpenSSL DLL is read through its "uplink"
      // table, which routes every stdio call back into the executable's own C
      // runtime by way of an OPENSSL_Applink export the executable has to provide
      // (openssl/applink.c). This executable provides none, and the DLL's answer
      // to that is not an error return: the first fread lands in a stub that
      // writes "OPENSSL_Uplink(...): no OPENSSL_Applink" to the Windows
      // Application log under the source "OpenSSL" and calls TerminateProcess.
      // That was issue #93 - the service gone the instant a certificate had been
      // issued, an OpenSSL event that looked empty beside a 7031 from the service
      // control manager in the same second, and no "ACME (automatic)" record
      // because this ran before the deployment. Every other file this client
      // reads or writes already goes through BIO_new_file; this function and
      // the key reuse in FinalizeOrder_ were the two that did not.
      AnsiString narrowPath = certificate_file;

      BIO *bio = BIO_new_file(narrowPath.c_str(), "r");
      if (bio == nullptr)
         return false;

      X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);

      if (certificate == nullptr)
         return false;

      bool success = false;

      int derLength = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), nullptr);
      if (derLength > 0)
      {
         std::vector<unsigned char> der(derLength);
         unsigned char *writePointer = der.data();
         i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), &writePointer);

         unsigned char digest[SHA256_DIGEST_LENGTH];
         SHA256(der.data(), derLength, digest);

         for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
         {
            AnsiString hexByte;
            hexByte.Format("%02x", digest[i]);
            spki_sha256_hex += hexByte;
         }

         success = true;
      }

      X509_free(certificate);

      return success;
   }

   bool
   AcmeClient::ApplyIssuedCertificateIfUnrecorded()
   {
      // Issue #93 left installations in exactly this state: 6.2.24 wrote a valid
      // fullchain.pem and privkey.pem and died before ApplyCertificate_ ran, so
      // the pair sat on disk with no "ACME (automatic)" record naming it and the
      // TLS ports pointed at nothing - and because the certificate was not due
      // for renewal, every later run of the task looked at it and left. A pair
      // on disk that no record names is deployed here, once, the way a fresh
      // issuance is; with the record in place this is one look-up and nothing
      // else. An administrator who deleted the record on purpose gets it back
      // at the next run, which is the lesser surprise: ACME is enabled, the
      // certificate is its, and a deleted record was the symptom of #93.
      String certificateFile = GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");
      String privateKeyFile = GetCertificateDirectory() + FileUtilities::PathSeparator + _T("privkey.pem");

      if (!FileUtilities::Exists(certificateFile) || !FileUtilities::Exists(privateKeyFile))
         return false;

      SSLCertificates certificates;
      certificates.Refresh();

      if (certificates.GetItemByName(_T("ACME (automatic)")))
         return false;

      LOG_APPLICATION("ACME: The certificate in " + GetCertificateDirectory() + " has no \"ACME (automatic)\" record - it was issued but never deployed. Deploying it now.");

      ApplyCertificate_();

      return true;
   }

   void
   AcmeClient::ApplyCertificate_()
   {
      const String certificateName = _T("ACME (automatic)");
      String certificateFile = GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");
      String privateKeyFile = GetCertificateDirectory() + FileUtilities::PathSeparator + _T("privkey.pem");

      // Create or update the SSL certificate record.
      SSLCertificates certificates;
      certificates.Refresh();

      std::shared_ptr<SSLCertificate> certificate = certificates.GetItemByName(certificateName);

      if (!certificate)
      {
         certificate = std::shared_ptr<SSLCertificate>(new SSLCertificate());
         certificate->SetName(certificateName);
      }

      certificate->SetCertificateFile(certificateFile);
      certificate->SetPrivateKeyFile(privateKeyFile);

      if (!PersistentSSLCertificate::SaveObject(certificate))
      {
         LOG_APPLICATION("ACME: Failed to save the SSL certificate record. Configure the certificate manually under Settings -> Advanced -> SSL certificates.");
         return;
      }

      // Assign the certificate to TLS-enabled ports that have none configured, and
      // to those that hold the self-signed certificate a new installation made
      // (Application/NewInstallDefaults.h): that one was only ever there so that
      // the listeners had TLS before anybody configured them, and a certificate
      // from a CA is what it was waiting for. A certificate an administrator chose
      // for a port is theirs, and is left.
      TCPIPPorts ports;
      ports.Refresh();

      for (int i = 0; i < ports.GetCount(); i++)
      {
         std::shared_ptr<TCPIPPort> port = ports.GetItem(i);
         if (!port)
            continue;

         if (port->GetConnectionSecurity() == CSNone)
            continue;

         bool replacesInstallation = false;
         if (port->GetSSLCertificateID() > 0)
         {
            std::shared_ptr<SSLCertificate> current = certificates.GetItemByDBID(port->GetSSLCertificateID());
            replacesInstallation = current && NewInstallDefaults::IsInstallationCertificate(current->GetName());
            if (!replacesInstallation)
               continue;
         }

         port->SetSSLCertificateID((int) certificate->GetID());

         String errorMessage;

         // The save was unchecked and the log line below ran either way, so a failure
         // here wrote "Assigned the certificate to port N" into the application log for
         // a port that still had no certificate - and the renewal that follows restarts
         // the TCP servers, which is exactly when someone reads this log to confirm the
         // new certificate went live. A port left without one accepts no TLS at all.
         if (!PersistentTCPIPPort::SaveObject(port, errorMessage, PersistenceModeNormal))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 6100, "AcmeClient::ApplyCertificate_",
               Formatter::Format("ACME: the certificate could NOT be assigned to port {0} - it has been left without one and will not accept TLS connections. {1}",
                  port->GetPortNumber(), errorMessage));

            continue;
         }

         String message;
         if (replacesInstallation)
            message.Format(_T("ACME: Assigned the certificate to port %d, in place of the self-signed certificate made at installation."), port->GetPortNumber());
         else
            message.Format(_T("ACME: Assigned the certificate to port %d (no certificate was configured)."), port->GetPortNumber());
         LOG_APPLICATION(message);
      }

      // Restart the TCP servers so the new certificate is loaded.
      LOG_APPLICATION("ACME: Restarting servers to load the new certificate.");
      Reinitializator::Instance()->ReInitialize();
   }

   // -------------------------------------------------------------------
   // AcmeChallengeStore
   // -------------------------------------------------------------------

   boost::recursive_mutex AcmeChallengeStore::mutex_;
   std::map<AnsiString, AnsiString> AcmeChallengeStore::challenges_;

   void
   AcmeChallengeStore::Set(const AnsiString &token, const AnsiString &key_authorization)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      challenges_[token] = key_authorization;
   }

   bool
   AcmeChallengeStore::Get(const AnsiString &token, AnsiString &key_authorization)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      auto iterator = challenges_.find(token);
      if (iterator == challenges_.end())
         return false;

      key_authorization = iterator->second;
      return true;
   }

   void
   AcmeChallengeStore::Clear()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      challenges_.clear();
   }

   // -------------------------------------------------------------------
   // AcmeChallengeServer
   // -------------------------------------------------------------------

   AcmeChallengeServer::AcmeChallengeServer() :
      listen_socket_(INVALID_SOCKET),
      running_(false),
      active_connections_(0)
   {

   }

   AcmeChallengeServer::~AcmeChallengeServer()
   {
      Stop();
   }

   bool
   AcmeChallengeServer::Start(int port)
   {
      sockaddr_in address = {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = INADDR_ANY;
      address.sin_port = htons(static_cast<unsigned short>(port));

      listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listen_socket_ == INVALID_SOCKET)
         return false;

      AcceptLoop::AllowRebindPastTimeWait(listen_socket_);

      // Not AcceptLoop::ClaimPortExclusively, alone of the server's listeners: this
      // one binds the wildcard of AcmeHttpPort for the seconds a validation takes,
      // on a port that another web server, or this server's REST listener, may
      // hold on one address of the machine (an intranet site on an internal
      // address, port 80). Without the option the wildcard bind takes every other
      // address, which is where the CA comes from; with it that bind is refused
      // (WSAEADDRINUSE, measured on Windows 11) and the certificate not renewed.

      if (bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
          listen(listen_socket_, 5) == SOCKET_ERROR)
      {
         closesocket(listen_socket_);
         listen_socket_ = INVALID_SOCKET;
         return false;
      }

      running_ = true;
      worker_ = std::thread(&AcmeChallengeServer::Run_, this);

      return true;
   }

   void
   AcmeChallengeServer::Stop()
   {
      if (!running_)
         return;

      running_ = false;

      // Wakes the worker out of accept(); the descriptor is released once the
      // worker has been joined. AcceptLoop.h says why the two are separate.
      AcceptLoop::Interrupt(listen_socket_);

      if (worker_.joinable())
         worker_.join();

      // The accept loop has stopped; wait for the connections it handed to their
      // own threads to finish before the socket is released and this object goes
      // away, so a detached worker never touches either afterwards. Bounded well
      // past a connection's five-second receive timeout, so a worker wedged in
      // send() to a client that has stopped reading cannot hold Stop for ever.
      {
         std::unique_lock<std::mutex> lock(active_mutex_);
         active_idle_.wait_for(lock, std::chrono::seconds(15), [this]() { return active_connections_ == 0; });
      }

      AcceptLoop::Release(listen_socket_);
   }

   void
   AcmeChallengeServer::SetChallenge(const AnsiString &token, const AnsiString &keyAuthorization)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      challenges_[token] = keyAuthorization;
   }

   namespace
   {
      // How many challenge connections may be served at once. Each lives only
      // for its receive timeout, so this is never reached in normal use - a CA
      // opens one connection; it is a ceiling against a flood of idle
      // connections opened to starve the responder, and the one case the loop
      // drops a connection rather than serve it concurrently.
      const int MaxConcurrentChallengeConnections = 64;
   }

   void
   AcmeChallengeServer::ServeConnection_(SOCKET clientSocket)
   {
      // Five seconds, in the form each platform takes: until 6.3.6 Linux was
      // handed the Windows form, refused it, and a client that connected and
      // sent nothing kept the CA's validation request from being answered.
      // Since W-11 each connection is served on its own thread, so this timeout
      // holds back only that thread and never the accept loop.
      AcceptLoop::SetTimeouts(clientSocket, 5000);

      char buffer[4096];
      int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

      AnsiString response = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

      if (bytesRead > 0)
      {
         buffer[bytesRead] = 0;
         AnsiString request = buffer;

         AnsiString prefix = "GET /.well-known/acme-challenge/";

         if (request.StartsWith(prefix))
         {
            int tokenEnd = request.Find(" ", prefix.GetLength());
            if (tokenEnd > 0)
            {
               AnsiString token = request.Mid(prefix.GetLength(), tokenEnd - prefix.GetLength());

               boost::lock_guard<boost::recursive_mutex> guard(mutex_);

               auto iter = challenges_.find(token);
               if (iter != challenges_.end())
               {
                  response.Format("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%hs",
                     iter->second.GetLength(), iter->second.c_str());
               }
            }
         }
      }

      send(clientSocket, response.c_str(), response.GetLength(), 0);
      shutdown(clientSocket, SD_SEND);
      closesocket(clientSocket);
   }

   void
   AcmeChallengeServer::Run_()
   {
      for (;;)
      {
         SOCKET clientSocket = accept(listen_socket_, nullptr, nullptr);

         if (clientSocket == INVALID_SOCKET)
         {
            if (!running_)
               return;

            continue;
         }

         // Each connection on its own short-lived thread, so that one that sends
         // nothing is held only by its own thread for its five-second receive
         // timeout and never by this loop - which goes straight back to accept()
         // and answers the CA's request behind it (W-11). Past the cap, or once
         // stopping, the connection is dropped rather than served here, which
         // would block the loop as the single-threaded responder did; the CA
         // retries, and idle connections free their slots as their timeouts
         // expire.
         bool accepted = false;
         {
            std::lock_guard<std::mutex> guard(active_mutex_);
            if (running_ && active_connections_ < MaxConcurrentChallengeConnections)
            {
               active_connections_++;
               accepted = true;
            }
         }

         if (!accepted)
         {
            closesocket(clientSocket);
            continue;
         }

         std::thread([this, clientSocket]()
         {
            ServeConnection_(clientSocket);

            {
               std::lock_guard<std::mutex> guard(active_mutex_);
               active_connections_--;
            }
            active_idle_.notify_all();
         }).detach();
      }
   }

   // -------------------------------------------------------------------
   // AcmeChecks
   // -------------------------------------------------------------------

   std::mutex AcmeChecks::mutex_;
   bool AcmeChecks::running_ = false;
   bool AcmeChecks::recheck_ = false;
   AcmeChecks::Attempt AcmeChecks::current_;
   AcmeChecks::Attempt AcmeChecks::last_;
   bool AcmeChecks::has_last_ = false;
   AcmeChecks::Attempt AcmeChecks::last_issuance_;
   bool AcmeChecks::has_last_issuance_ = false;
   __int64 AcmeChecks::next_id_ = 1;
   __int64 AcmeChecks::last_issued_at_ = 0;
   __int64 AcmeChecks::next_scheduled_at_ = 0;
   AcmeChecks::Spacing AcmeChecks::spacing_;

   namespace
   {
      // "2026-10-25 14:03:00 UTC", for a sentence.
      String TimeSentence(__int64 seconds)
      {
         time_t value = (time_t) seconds;
         tm utc = {};
         String text;
         if (gmtime_s(&utc, &value) != 0)
         {
            text.Format(_T("%I64d seconds after 1970-01-01 00:00:00 UTC"), seconds);
            return text;
         }

         text.Format(_T("%04d-%02d-%02d %02d:%02d:%02d UTC"), utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);
         return text;
      }

      String CertificateFile()
      {
         return AcmeClient::GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");
      }

      bool Serving()
      {
         const int state = ServerStatus::Instance()->GetState();
         return state == ServerStatus::StateRunning || state == ServerStatus::StateDraining;
      }

      // The sentence a check waiting for the spacing is answered with.
      String WaitSentence(const AnsiString &why, __int64 until)
      {
         if (why == "rate_limited")
            return _T("The CA answered this server's last order with a rate limit (urn:ietf:params:acme:error:rateLimited), so no check orders a certificate before ") +
                   TimeSentence(until) + _T(", when the CA said to come back.");

         if (why == "cooldown")
            return _T("The last attempt to issue a certificate failed, and none is made within twenty minutes of a failure, so that a setup that cannot validate does not use up the CA's limit of failed validations; the next may start at ") +
                   TimeSentence(until) + _T(".");

         return _T("A check was asked for less than a minute ago; ask again at ") + TimeSentence(until) +
                _T(", or read GET /api/v1/acme for how the last one went.");
      }
   }

   // static
   const char *
   AcmeChecks::TriggerWord(Trigger trigger)
   {
      switch (trigger)
      {
      case TriggerStart: return "start";
      case TriggerSettings: return "settings";
      case TriggerRequest: return "request";
      default: return "schedule";
      }
   }

   // static
   bool
   AcmeChecks::Allows(const Spacing &spacing, const AnsiString &directoryUrl, __int64 now, bool requested,
                      __int64 &until, AnsiString &why)
   {
      until = 0;
      why = "";

      // What was learnt of another CA holds nothing against this one.
      if (spacing.directory_url != directoryUrl)
         return true;

      // The latest of the times that hold, and the rule it is of.
      if (spacing.rate_limited_until > now && spacing.rate_limited_until > until)
      {
         until = spacing.rate_limited_until;
         why = "rate_limited";
      }

      const __int64 cooled = spacing.failed_at > 0 ? spacing.failed_at + FailureCooldownSeconds : 0;
      if (cooled > now && cooled > until)
      {
         until = cooled;
         why = "cooldown";
      }

      const __int64 spaced = requested && spacing.last_request_at > 0 ? spacing.last_request_at + RequestIntervalSeconds : 0;
      if (spaced > now && spaced > until)
      {
         until = spaced;
         why = "interval";
      }

      return until == 0;
   }

   // static
   int
   AcmeChecks::RateLimitSeconds(int retryAfterSeconds)
   {
      if (retryAfterSeconds < 0)
         return DefaultRateLimitSeconds;

      // Not a conditional expression: one of two lvalues would bind the
      // constant itself, which has no definition outside the class.
      int seconds = retryAfterSeconds;
      if (seconds > MaxRateLimitSeconds)
         seconds = MaxRateLimitSeconds;
      return seconds;
   }

   // static
   void
   AcmeChecks::NoteFinished(Spacing &spacing, const AnsiString &directoryUrl, __int64 now, bool orderedOrTried,
                            const AnsiString &outcome, const AcmeClient::Failure *failure)
   {
      if (orderedOrTried && spacing.directory_url != directoryUrl)
      {
         spacing = Spacing();
         spacing.directory_url = directoryUrl;
      }

      if (orderedOrTried && outcome == "failed")
      {
         spacing.failed_at = now;

         if (failure != nullptr && failure->rate_limited)
            spacing.rate_limited_until = now + RateLimitSeconds(failure->retry_after_seconds);
      }

      if (outcome == "issued")
         spacing.failed_at = 0;
   }

   // static
   bool
   AcmeChecks::Holds(bool running, const Attempt &current, __int64 now)
   {
      if (!running)
         return false;

      return !(current.step == "queued" && now - current.queued_at > AbandonedQueuedSeconds);
   }

   // static
   bool
   AcmeChecks::Queue_(Trigger trigger, Attempt &attempt)
   {
      // The reservation is made by the caller, under mutex_; this hands the
      // check to the maintenance work queue, outside it, and takes the
      // reservation back when there is no queue to hand it to.
      std::shared_ptr<WorkQueue> queue = Serving() ? Application::Instance()->GetMaintenanceWorkQueue() : std::shared_ptr<WorkQueue>();

      if (!queue)
      {
         std::lock_guard<std::mutex> guard(mutex_);

         if (running_ && current_.id == attempt.id && current_.step == "queued")
         {
            running_ = false;
            current_ = Attempt();
         }

         return false;
      }

      std::shared_ptr<AcmeRenewalTask> task = std::shared_ptr<AcmeRenewalTask>(new AcmeRenewalTask(trigger));
      task->SetName(_T("ACME check"));
      queue->AddTask(task);

      return true;
   }

   // static
   AcmeChecks::RequestResult
   AcmeChecks::Request(Attempt &attempt, __int64 &until, AnsiString &why)
   {
      until = 0;
      why = "";

      IniFileSettings *ini = IniFileSettings::Instance();

      if (!ini->GetAcmeEnabled())
         return RequestDisabled;

      const std::vector<AnsiString> domains = AcmeClient::ParseDomainList(ini->GetAcmeDomains());
      if (domains.empty())
         return RequestNoDomains;

      if (!Serving())
         return RequestNotServing;

      const __int64 now = (__int64) time(nullptr);
      const AnsiString directoryUrl = ini->GetAcmeDirectoryUrl();

      {
         std::lock_guard<std::mutex> guard(mutex_);

         if (running_ && !Holds(running_, current_, now))
         {
            running_ = false;
            current_ = Attempt();
         }

         if (running_)
         {
            attempt = current_;
            return RequestAlreadyRunning;
         }

         if (!Allows(spacing_, directoryUrl, now, true, until, why))
            return RequestWaiting;

         if (spacing_.directory_url != directoryUrl)
         {
            spacing_ = Spacing();
            spacing_.directory_url = directoryUrl;
         }

         spacing_.last_request_at = now;

         current_ = Attempt();
         current_.id = next_id_++;
         current_.trigger = TriggerRequest;
         current_.queued_at = now;
         current_.step = "queued";
         running_ = true;

         attempt = current_;
      }

      if (!Queue_(TriggerRequest, attempt))
         return RequestNotServing;

      // On the calling thread, which carries the actor REST or COM installed:
      // the check itself runs on the maintenance work queue, where there is
      // none, so the request is the half of it that says who asked. Under the
      // object type the trail gives hm_sslcertificates and the record's name,
      // so that the request and what a successful issuance writes to that
      // record read as one thing; the record's change itself is recorded by the
      // statement chokepoint, as every other change of hm_sslcertificates is.
      AuditTrail::Instance()->RecordEvent(_T("ssl certificate"), _T("ACME (automatic)"), _T("check requested"),
         _T("names: ") + String(JoinNames(domains)) + _T("; directory: ") + String(directoryUrl));

      LOG_APPLICATION("ACME: A check was requested over the administration interface; it is queued.");

      return RequestAccepted;
   }

   // static
   void
   AcmeChecks::DomainsChanged()
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      if (!ini->GetAcmeEnabled() || !Serving())
         return;

      const std::vector<AnsiString> domains = AcmeClient::ParseDomainList(ini->GetAcmeDomains());
      if (domains.empty())
         return;

      // Only a name the certificate in use lacks makes a save worth a check now.
      // A name taken off the list needs no new certificate - the one in use still
      // serves the rest - and is left for the renewal, as it always was.
      std::vector<AnsiString> names;
      if (AcmeClient::ReadCertificateNames(CertificateFile(), names) && AcmeClient::MissingNames(names, domains).empty())
         return;

      const __int64 now = (__int64) time(nullptr);
      const AnsiString directoryUrl = ini->GetAcmeDirectoryUrl();

      Attempt attempt;

      {
         std::lock_guard<std::mutex> guard(mutex_);

         if (running_ && !Holds(running_, current_, now))
         {
            running_ = false;
            current_ = Attempt();
         }

         // A check that is running read the list before this save: this one is
         // looked at again when it ends.
         if (running_)
         {
            recheck_ = true;
            return;
         }

         __int64 until = 0;
         AnsiString why;
         if (!Allows(spacing_, directoryUrl, now, false, until, why))
         {
            LOG_APPLICATION("ACME: AcmeDomains names a host the certificate in use does not, and the check it would start waits: " +
               WaitSentence(why, until) + " The hourly check issues it after that.");
            return;
         }

         current_ = Attempt();
         current_.id = next_id_++;
         current_.trigger = TriggerSettings;
         current_.queued_at = now;
         current_.step = "queued";
         running_ = true;

         attempt = current_;
      }

      if (Queue_(TriggerSettings, attempt))
         LOG_APPLICATION("ACME: AcmeDomains was saved with a host the certificate in use does not name; a check is queued now rather than at the next hour.");
   }

   // static
   bool
   AcmeChecks::Begin(Trigger trigger, Attempt &attempt)
   {
      const __int64 now = (__int64) time(nullptr);

      std::lock_guard<std::mutex> guard(mutex_);

      // The hourly registration ran, whether or not it finds a check running:
      // the next is an hour on, give or take the minute the scheduler wakes in.
      if (trigger == TriggerSchedule)
         next_scheduled_at_ = now + 60LL * 60;

      if (running_ && current_.step == "queued" && current_.trigger == trigger)
      {
         // The check a request or a save queued, starting now.
         current_.started_at = now;
         current_.step = "deciding";
         current_.directory_url_seen = IniFileSettings::Instance()->GetAcmeDirectoryUrl();
         attempt = current_;
         return true;
      }

      // A reservation that no longer holds is let go; and the check at start
      // takes over one queued and not yet started, whatever queued it: the
      // start is made after a stop of the servers, which can have dropped the
      // task that reservation was for (a reinitialise's ExitInstance takes the
      // whole queue away, Forget), and the start's check reads the list and the
      // certificate as they are now, which is all a queued one would have done.
      if (running_ && (!Holds(running_, current_, now) || (trigger == TriggerStart && current_.step == "queued")))
      {
         running_ = false;
         recheck_ = false;
         current_ = Attempt();
      }

      if (running_)
         return false;

      current_ = Attempt();
      current_.id = next_id_++;
      current_.trigger = trigger;
      current_.started_at = now;
      current_.step = "deciding";
      current_.directory_url_seen = IniFileSettings::Instance()->GetAcmeDirectoryUrl();
      running_ = true;

      attempt = current_;
      return true;
   }

   // static
   void
   AcmeChecks::Step(const AnsiString &step)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      if (running_)
         current_.step = step;
   }

   // static
   void
   AcmeChecks::Finish(Attempt attempt, bool orderedOrTried, const AcmeClient::Failure *failure)
   {
      const __int64 now = (__int64) time(nullptr);

      bool again = false;

      {
         std::lock_guard<std::mutex> guard(mutex_);

         attempt.finished_at = now;
         attempt.step = "";

         if (failure != nullptr)
         {
            attempt.failed_step = failure->step;
            attempt.error = failure->sentence;
            attempt.problem_type = failure->problem_type;
            attempt.problem_detail = failure->problem_detail;
         }

         NoteFinished(spacing_, attempt.directory_url_seen, now, orderedOrTried, attempt.outcome, failure);

         if (attempt.outcome == "issued")
            last_issued_at_ = now;

         last_ = attempt;
         has_last_ = true;

         if (orderedOrTried)
         {
            last_issuance_ = attempt;
            has_last_issuance_ = true;
         }

         if (running_ && current_.id == attempt.id)
         {
            running_ = false;
            current_ = Attempt();
         }

         again = recheck_;
         recheck_ = false;
      }

      // A save of AcmeDomains while this ran: judged again now, against the
      // certificate this check may just have installed.
      if (again)
         DomainsChanged();
   }

   // static
   bool
   AcmeChecks::MayOrder(__int64 now, __int64 &until, AnsiString &why)
   {
      const AnsiString directoryUrl = IniFileSettings::Instance()->GetAcmeDirectoryUrl();

      std::lock_guard<std::mutex> guard(mutex_);
      return Allows(spacing_, directoryUrl, now, false, until, why);
   }

   // static
   void
   AcmeChecks::SetNextScheduled(__int64 when)
   {
      std::lock_guard<std::mutex> guard(mutex_);
      next_scheduled_at_ = when;
   }

   // static
   void
   AcmeChecks::Forget()
   {
      std::lock_guard<std::mutex> guard(mutex_);

      if (running_ && current_.step == "queued")
      {
         running_ = false;
         recheck_ = false;
         current_ = Attempt();
      }
   }

   // static
   AnsiString
   AcmeChecks::AttemptJson(const Attempt &attempt)
   {
      const bool finished = attempt.finished_at > 0;
      const AnsiString state = finished ? AnsiString("finished") : (attempt.step == "queued" ? AnsiString("queued") : AnsiString("running"));

      AnsiString json = "{\"id\":" + JsonNumber(attempt.id);
      json += ",\"trigger\":" + JsonString(TriggerWord(attempt.trigger));
      json += ",\"state\":" + JsonString(state);
      json += ",\"step\":" + (finished || attempt.step.IsEmpty() ? AnsiString("null") : JsonString(attempt.step));
      json += ",\"queued_at\":" + JsonTime(attempt.queued_at);
      json += ",\"started_at\":" + JsonTime(attempt.started_at);
      json += ",\"finished_at\":" + JsonTime(attempt.finished_at);
      json += ",\"outcome\":" + (attempt.outcome.IsEmpty() ? AnsiString("null") : JsonString(attempt.outcome));
      json += ",\"reason\":" + (attempt.reason.IsEmpty() ? AnsiString("null") : JsonString(attempt.reason));
      json += ",\"failed_step\":" + (attempt.failed_step.IsEmpty() ? AnsiString("null") : JsonString(attempt.failed_step));
      json += ",\"error\":" + (attempt.error.IsEmpty() ? AnsiString("null") : JsonText(attempt.error));

      if (attempt.problem_type.IsEmpty())
         json += ",\"ca_problem\":null";
      else
         json += ",\"ca_problem\":{\"type\":" + JsonString(attempt.problem_type) + ",\"detail\":" + JsonText(attempt.problem_detail) + "}";

      json += "}";
      return json;
   }

   // static
   AnsiString
   AcmeChecks::StatusJson(__int64 now)
   {
      IniFileSettings *ini = IniFileSettings::Instance();

      const bool enabled = ini->GetAcmeEnabled();
      const AnsiString directoryUrl = ini->GetAcmeDirectoryUrl();
      const AnsiString configured = ini->GetAcmeDomains();
      const std::vector<AnsiString> domains = AcmeClient::ParseDomainList(configured);

      bool running = false;
      Attempt current;
      bool hasLast = false;
      Attempt lastIssuance;
      bool hasLastIssuance = false;
      Attempt last;
      __int64 lastIssuedAt = 0;
      __int64 nextScheduledAt = 0;
      Spacing spacing;

      {
         std::lock_guard<std::mutex> guard(mutex_);

         // A queued check that no longer holds - its task was dropped with the
         // queue - is not reported as running, as Request would not answer with it.
         running = Holds(running_, current_, now);
         current = current_;
         hasLast = has_last_;
         lastIssuance = last_issuance_;
         hasLastIssuance = has_last_issuance_;
         last = last_;
         lastIssuedAt = last_issued_at_;
         nextScheduledAt = next_scheduled_at_;
         spacing = spacing_;
      }

      AnsiString json = "{\"enabled\":";
      json += enabled ? "true" : "false";
      json += ",\"directory_url\":" + JsonString(directoryUrl);
      json += ",\"domains\":" + JsonNames(domains);
      json += ",\"unusable_domains\":" + JsonNames(AcmeClient::UnusableDomains(configured));
      json += ",\"certificate_directory\":" + JsonText(AcmeClient::GetCertificateDirectory());

      // The certificate in use: what the listeners load from the ACME
      // directory, read from the file now.
      const String certificateFile = CertificateFile();
      X509 *certificate = ReadPemCertificateFile(certificateFile);

      std::vector<AnsiString> missing;
      AnsiString dueReason;

      if (certificate == nullptr)
      {
         json += ",\"certificate\":null,\"covers_domains\":null,\"missing_domains\":" + JsonNames(domains);
         dueReason = "missing";
      }
      else
      {
         std::vector<AnsiString> names;
         CertificateNames(certificate, names);
         missing = AcmeClient::MissingNames(names, domains);

         const __int64 notBefore = AsnTime(X509_get0_notBefore(certificate));
         const __int64 notAfter = AsnTime(X509_get0_notAfter(certificate));
         const __int64 renewsAt = notAfter > 0 ? (__int64) AcmeClient::GetRenewalTime((time_t) notBefore, (time_t) notAfter) : 0;

         AnsiString serial;
         BIGNUM *serialNumber = ASN1_INTEGER_to_BN(X509_get0_serialNumber(certificate), nullptr);
         if (serialNumber != nullptr)
         {
            char *hex = BN_bn2hex(serialNumber);
            if (hex != nullptr)
            {
               serial = CleanText(AnsiString(hex), 100);
               serial.MakeLower();
               OPENSSL_free(hex);
            }
            BN_free(serialNumber);
         }

         // Deployed: the ACME (automatic) record names this file. A record of that
         // name that points elsewhere - AcmeCertificateDirectory changed since it
         // was made, or its paths edited by hand - is what the listeners load, and
         // this certificate is then not in use, whatever the record is called.
         // The path compared as the file system compares it: without regard to
         // case on Windows.
         SSLCertificates records;
         records.Refresh();
         std::shared_ptr<SSLCertificate> record = records.GetItemByName(_T("ACME (automatic)"));
#ifdef HM_PLATFORM_POSIX
         const bool deployed = record && record->GetCertificateFile() == certificateFile;
#else
         const bool deployed = record && record->GetCertificateFile().CompareNoCase(certificateFile) == 0;
#endif

         json += ",\"certificate\":{\"file\":" + JsonText(certificateFile);
         json += ",\"names\":" + JsonNames(names);
         json += ",\"subject\":" + JsonString(NameText(X509_get_subject_name(certificate)));
         json += ",\"issuer\":" + JsonString(NameText(X509_get_issuer_name(certificate)));
         json += ",\"serial\":" + JsonString(serial);
         json += ",\"not_before\":" + JsonTime(notBefore);
         json += ",\"not_after\":" + JsonTime(notAfter);
         json += ",\"expired\":";
         json += notAfter > 0 && notAfter <= now ? "true" : "false";
         json += ",\"renews_at\":" + JsonTime(renewsAt);
         json += ",\"deployed\":";
         json += deployed ? "true" : "false";
         json += "}";

         json += ",\"covers_domains\":";
         json += missing.empty() ? "true" : "false";
         json += ",\"missing_domains\":" + JsonNames(missing);

         if (notAfter <= 0)
            dueReason = "missing";
         else if (!missing.empty())
            dueReason = "names";
         else if (renewsAt > 0 && now >= renewsAt)
            dueReason = "lifetime";

         X509_free(certificate);
      }

      json += ",\"due\":";
      json += !dueReason.IsEmpty() ? "true" : "false";
      json += ",\"due_reason\":" + (dueReason.IsEmpty() ? AnsiString("null") : JsonString(dueReason));

      json += ",\"running\":" + (running ? AttemptJson(current) : AnsiString("null"));
      json += ",\"last_attempt\":" + (hasLast ? AttemptJson(last) : AnsiString("null"));
      json += ",\"last_issuance\":" + (hasLastIssuance ? AttemptJson(lastIssuance) : AnsiString("null"));
      json += ",\"last_issued_at\":" + JsonTime(lastIssuedAt);
      json += ",\"next_scheduled_check_at\":" + JsonTime(enabled ? nextScheduledAt : 0);

      __int64 until = 0;
      AnsiString why;
      const bool allowed = Allows(spacing, directoryUrl, now, true, until, why);

      json += ",\"requests\":{\"allowed_now\":";
      json += enabled && !running && allowed && !domains.empty() ? "true" : "false";
      json += ",\"waiting_for\":" + (allowed ? AnsiString("null") : JsonString(why));
      json += ",\"next_allowed_at\":" + JsonTime(allowed ? 0 : until);
      json += ",\"interval_seconds\":" + JsonNumber(RequestIntervalSeconds);
      json += ",\"cooldown_seconds\":" + JsonNumber(FailureCooldownSeconds);
      json += "}";

      json += ",\"rate_limited_until\":" +
         JsonTime(spacing.directory_url == directoryUrl && spacing.rate_limited_until > now ? spacing.rate_limited_until : 0);
      json += ",\"now\":" + JsonNumber(now);
      json += "}";

      return json;
   }

   // static
   int
   AcmeChecks::RequestAnswer(AnsiString &json, int &retryAfterSeconds, String &sentence)
   {
      retryAfterSeconds = 0;
      sentence = _T("");

      Attempt attempt;
      __int64 until = 0;
      AnsiString why;

      const RequestResult result = Request(attempt, until, why);
      const __int64 now = (__int64) time(nullptr);

      switch (result)
      {
      case RequestAccepted:
      case RequestAlreadyRunning:
         json = "{\"accepted\":";
         json += result == RequestAccepted ? "true" : "false";
         json += ",\"already_running\":";
         json += result == RequestAlreadyRunning ? "true" : "false";
         json += ",\"attempt\":" + AttemptJson(attempt);
         json += ",\"status\":\"/api/v1/acme\"}";
         return 202;

      case RequestWaiting:
         retryAfterSeconds = until > now ? (int) (until - now) : 1;
         sentence = WaitSentence(why, until);
         json = "{\"error\":" + JsonText(sentence);
         json += ",\"reason\":" + JsonString(why);
         json += ",\"retry_at\":" + JsonNumber(until);
         json += ",\"retry_after_seconds\":" + JsonNumber(retryAfterSeconds);
         json += ",\"status\":\"/api/v1/acme\"}";
         return 429;

      case RequestDisabled:
         sentence = _T("ACME is not enabled on this server (AcmeEnabled), so there is nothing to check.");
         json = "{\"error\":" + JsonText(sentence) + ",\"reason\":\"disabled\"}";
         return 409;

      case RequestNoDomains:
         sentence = _T("AcmeDomains names no host, so there is no certificate to issue.");
         json = "{\"error\":" + JsonText(sentence) + ",\"reason\":\"no_domains\"}";
         return 409;

      default:
         sentence = _T("The server is not running its services, so a check cannot be queued now.");
         json = "{\"error\":" + JsonText(sentence) + ",\"reason\":\"not_serving\"}";
         return 503;
      }
   }

   // -------------------------------------------------------------------
   // AcmeRenewalTask
   // -------------------------------------------------------------------

   AcmeRenewalTask::AcmeRenewalTask(AcmeChecks::Trigger trigger) :
      trigger_(trigger)
   {

   }

   AcmeRenewalTask::~AcmeRenewalTask()
   {

   }

   namespace
   {
      // Ends the check Begin started, however DoWork leaves: a throw - a
      // shutdown's interruption among them - is a failure, so that no check is
      // left running for ever and holding every one after it.
      class AttemptGuard
      {
      public:
         explicit AttemptGuard(AcmeChecks::Attempt &attempt) : attempt_(attempt), finished_(false) { }

         ~AttemptGuard()
         {
            if (finished_)
               return;

            attempt_.outcome = "failed";
            attempt_.error = _T("The check was ended before it finished (the service stopped, or an error was contained).");
            AcmeChecks::Finish(attempt_, false, nullptr);
         }

         void Finish(bool orderedOrTried, const AcmeClient::Failure *failure)
         {
            finished_ = true;
            AcmeChecks::Finish(attempt_, orderedOrTried, failure);
         }

      private:
         AcmeChecks::Attempt &attempt_;
         bool finished_;
      };
   }

   void
   AcmeRenewalTask::DoWork()
   {
      AcmeChecks::Attempt attempt;

      if (!AcmeChecks::Begin(trigger_, attempt))
      {
         LOG_DEBUG("ACME: A check is already running, so this one does nothing.");
         return;
      }

      Check_(attempt);
   }

   void
   AcmeRenewalTask::Check_(AcmeChecks::Attempt &attempt)
   {
      AttemptGuard guard(attempt);

      if (!IniFileSettings::Instance()->GetAcmeEnabled())
      {
         attempt.outcome = "disabled";
         guard.Finish(false, nullptr);
         return;
      }

      AcmeClient client;

      AnsiString reason;
      if (!client.ShouldRenewNow(reason))
      {
         // Not due - but a certificate that was issued and never deployed (issue
         // #93's aftermath) is deployed on the way out.
         client.ApplyIssuedCertificateIfUnrecorded();

         attempt.outcome = "not_due";
         guard.Finish(false, nullptr);
         return;
      }

      attempt.reason = reason;

      // Due, and held by the spacing (AcmeChecks): a failure a few minutes ago,
      // or a rate limit the CA set, which this check does not test again.
      const __int64 now = (__int64) time(nullptr);
      __int64 until = 0;
      AnsiString why;

      if (!AcmeChecks::MayOrder(now, until, why))
      {
         const String sentence = WaitSentence(why, until);
         LOG_APPLICATION("ACME: A certificate is due (" + String(reason) + "), and this check orders none: " + sentence);

         // A pair on disk that no record names is deployed whether or not a new
         // one is due: it is the certificate the listeners would otherwise lack
         // until one is issued. Due by its names, it is still the one that names
         // the rest.
         client.ApplyIssuedCertificateIfUnrecorded();

         attempt.outcome = "waiting";
         attempt.failed_step = why;
         attempt.error = sentence;
         guard.Finish(false, nullptr);
         return;
      }

      LOG_APPLICATION("ACME: Certificate is missing or due for renewal (" + String(reason) + "). Requesting a new certificate.");

      if (client.RequestCertificate())
      {
         attempt.outcome = "issued";
         guard.Finish(true, nullptr);
         return;
      }

      // As above: the pair in use, if it was never deployed, is deployed now.
      client.ApplyIssuedCertificateIfUnrecorded();

      attempt.outcome = "failed";
      guard.Finish(true, &client.GetFailure());

      // Why the return value is looked at at all now: it was discarded. A renewal
      // that failed left exactly one line in the log - the one above, announcing
      // that a renewal was about to be attempted - and nothing anywhere saying it
      // had not happened. An hour later the task ran again and said the same thing,
      // so the log read like a renewal permanently in progress right up to the
      // moment the certificate expired and TLS stopped working. Automatic renewal
      // whose failure is invisible is worse than manual renewal, because nobody is
      // watching the calendar either.
      LOG_APPLICATION("ACME: Certificate renewal FAILED. The failing step is in the lines above; the next attempt is in one hour. The certificate currently installed has not been changed.");

      String certificateFile = AcmeClient::GetCertificateDirectory() + FileUtilities::PathSeparator + _T("fullchain.pem");

      time_t notAfter = 0;

      if (!ReadCertificateNotAfter(certificateFile, notAfter))
      {
         // Nothing usable in the directory at all: no expiry to run out, but also
         // no certificate for the listeners that were pointed at this directory,
         // and ACME is switched on precisely so that there would be one. Reported
         // rather than logged because an operator who enabled ACME will not go
         // looking for the reason it never produced anything.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5992, "AcmeRenewalTask::DoWork",
            Formatter::Format("ACME is enabled but certificate issuance failed and there is no usable certificate in {0}. Any listener configured to use that pair has no certificate. The failing step is in the hMailServer application log.", certificateFile));

         return;
      }

      // An error, not just a log line, once the certificate this renewal was meant
      // to replace is close enough to expiry that continued failure means TLS
      // stops working. Deliberately not reported for every failure: renewal begins
      // 30 days out, and an ERROR entry for a failure with four weeks of slack left
      // would train an administrator to ignore the error log. Cannot fire on a
      // default installation - AcmeEnabled is 0 and DoWork returns above.
      time_t secondsRemaining = notAfter - time(nullptr);

      if (secondsRemaining > static_cast<time_t>(ImminentExpiryDays) * 86400)
         return;

      if (secondsRemaining <= 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5992, "AcmeRenewalTask::DoWork",
            Formatter::Format("ACME renewal failed and the certificate {0} has already expired. TLS clients are refusing this server's certificate now. The failing step is in the hMailServer application log.", certificateFile));

         return;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::High, 5992, "AcmeRenewalTask::DoWork",
         Formatter::Format("ACME renewal failed and the certificate {0} expires in less than {1} day(s). TLS will stop working when it does. The failing step is in the hMailServer application log.",
            certificateFile, static_cast<int>(secondsRemaining / 86400) + 1));
   }
   // -------------------------------------------------------------------
   // AcmeChecksTester
   // -------------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error("ACME checks: " + what);
      }

      std::string Narrow(const std::vector<AnsiString> &names)
      {
         return std::string(JoinNames(names).c_str());
      }

      // A key of its own for each case, never written anywhere.
      EVP_PKEY *MakeKey()
      {
         EVP_PKEY *key = EVP_EC_gen("P-256");
         if (key == nullptr)
            throw std::logic_error("ACME checks: an EC key could not be made");
         return key;
      }

      // A self-signed certificate for key: common name cn, the dNSName entries
      // names (no extension at all when there are none), valid from
      // fromSeconds to toSeconds relative to now. As PEM.
      AnsiString MakeCertificate(EVP_PKEY *key, const char *cn, const std::vector<AnsiString> &names,
                                 long fromSeconds, long toSeconds)
      {
         X509 *certificate = X509_new();
         if (certificate == nullptr)
            throw std::logic_error("ACME checks: X509_new failed");

         X509_set_version(certificate, 2);
         ASN1_INTEGER_set(X509_get_serialNumber(certificate), 4711);
         X509_gmtime_adj(X509_getm_notBefore(certificate), fromSeconds);
         X509_gmtime_adj(X509_getm_notAfter(certificate), toSeconds);
         X509_set_pubkey(certificate, key);

         X509_NAME *name = X509_NAME_new();
         X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *) cn, -1, -1, 0);
         X509_set_subject_name(certificate, name);
         X509_set_issuer_name(certificate, name);
         X509_NAME_free(name);

         if (!names.empty())
         {
            AnsiString list;
            for (size_t i = 0; i < names.size(); i++)
            {
               if (i > 0)
                  list += ",";
               list += "DNS:" + names[i];
            }

            X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, list.c_str());
            if (extension == nullptr)
            {
               X509_free(certificate);
               throw std::logic_error("ACME checks: the subject alternative names could not be made");
            }

            X509_add_ext(certificate, extension, -1);
            X509_EXTENSION_free(extension);
         }

         if (X509_sign(certificate, key, EVP_sha256()) <= 0)
         {
            X509_free(certificate);
            throw std::logic_error("ACME checks: the certificate could not be signed");
         }

         BIO *bio = BIO_new(BIO_s_mem());
         PEM_write_bio_X509(bio, certificate);

         char *data = nullptr;
         long length = BIO_get_mem_data(bio, &data);
         AnsiString pem;
         if (data != nullptr && length > 0)
            pem = AnsiString(data, (int) length);

         BIO_free(bio);
         X509_free(certificate);
         return pem;
      }

      std::vector<AnsiString> Names(const char *a, const char *b = nullptr, const char *c = nullptr)
      {
         std::vector<AnsiString> names;
         names.push_back(a);
         if (b != nullptr)
            names.push_back(b);
         if (c != nullptr)
            names.push_back(c);
         return names;
      }
   }

   void
   AcmeChecksTester::Test()
   {
      const long year = 365L * 86400L;

      // ---- the names a certificate covers
      {
         Expect(AcmeClient::MissingNames(Names("mail.example.test"), Names("mail.example.test")).empty(),
            "a certificate naming the one configured host was taken to lack it");

         Expect(AcmeClient::MissingNames(Names("MAIL.Example.TEST"), Names("mail.example.test")).empty(),
            "names were compared with their case");

         const std::vector<AnsiString> missing = AcmeClient::MissingNames(Names("mail.example.test"),
            Names("mail.example.test", "new.example.test", "other.example.test"));
         Expect(Narrow(missing) == "new.example.test, other.example.test",
            "the hosts a certificate lacks were not those, in the order configured: " + Narrow(missing));

         // A wildcard is one label, as a TLS client matches it.
         Expect(AcmeClient::MissingNames(Names("*.example.test"), Names("mail.example.test")).empty(),
            "*.example.test was taken not to cover mail.example.test");
         Expect(AcmeClient::MissingNames(Names("*.example.test"), Names("example.test")).size() == 1,
            "*.example.test was taken to cover example.test itself");
         Expect(AcmeClient::MissingNames(Names("*.example.test"), Names("a.b.example.test")).size() == 1,
            "*.example.test was taken to cover two labels");
         Expect(AcmeClient::MissingNames(Names("*."), Names("mail.example.test")).size() == 1,
            "a bare \"*.\" was taken to cover a host");

         Expect(AcmeClient::MissingNames(std::vector<AnsiString>(), Names("mail.example.test")).size() == 1,
            "a certificate with no names was taken to cover a host");
         Expect(AcmeClient::MissingNames(Names("mail.example.test"), std::vector<AnsiString>()).empty(),
            "an empty list was taken to need a host");
      }

      // ---- AcmeDomains as the client reads it
      {
         const std::vector<AnsiString> domains = AcmeClient::ParseDomainList(" Mail.Example.TEST, ,new.example.test,mail.example.test ,");
         Expect(Narrow(domains) == "mail.example.test, new.example.test",
            "AcmeDomains was not read trimmed, in lower case, each name once, the empty ones dropped: " + Narrow(domains));
         Expect(AcmeClient::ParseDomainList("").empty(), "an empty AcmeDomains named a host");

         // A name written fully qualified is the same host; one no certificate
         // can carry is left out, so that it neither makes every certificate due
         // nor fails every order for the rest, and is named.
         const AnsiString written = "mail.example.test., *.example.test, 192.0.2.1, [2001:db8::1], b\xC3\xBC" "cher.example.test, "
                                    "mail example.test, a;b.example.test, -bad.example.test, bad-.example.test, a..example.test, "
                                    "xn--bcher-kva.example.test, 4.example.test, MAIL.example.test";
         const std::vector<AnsiString> usable = AcmeClient::ParseDomainList(written);
         Expect(Narrow(usable) == "mail.example.test, xn--bcher-kva.example.test, 4.example.test",
            "AcmeDomains was not read with the final dot taken off and the names no certificate can carry left out: " + Narrow(usable));

         const std::vector<AnsiString> unusable = AcmeClient::UnusableDomains(written);
         Expect(unusable.size() == 9 && unusable[0] == "*.example.test" && unusable[1] == "192.0.2.1" &&
                unusable[5] == "a;b.example.test" && unusable[8] == "a..example.test",
            "the names left out were not the nine written so, in the order written: " + Narrow(unusable));
         Expect(AcmeClient::UnusableDomains(" mail.example.test, new.example.test. ").empty(),
            "a host name, or one written fully qualified, was named as one no certificate can carry");

         // A byte that is not printable ASCII is shown as \xHH, so that a name of
         // only such bytes - a Latin-1 character, kept as its byte when the
         // setting is narrowed - is never shown empty, as it was with each run of
         // them made a space and trimmed; cut never inside an escape.
         Expect(unusable[3] == "b\\xC3\\xBCcher.example.test",
            "a name past ASCII was not shown with its other bytes as \\xHH: " + Narrow(unusable));
         const std::vector<AnsiString> latin1 = AcmeClient::UnusableDomains("mail.example.test, \xE9, \xF1, \xE9");
         Expect(Narrow(latin1) == "\\xE9, \\xF1",
            "a name of only bytes past ASCII was not shown, each once, as \\xHH: " + Narrow(latin1));
         const std::vector<AnsiString> longName = AcmeClient::UnusableDomains(AnsiString(std::string(200, '\xE9').c_str()));
         Expect(longName.size() == 1 && longName[0].GetLength() == 300 && longName[0].Right(4) == "\\xE9",
            "a long name past ASCII was not cut to 300 characters between two escapes: " + Narrow(longName));

         std::string label63(63, 'a');
         std::string label64(64, 'a');
         Expect(AcmeClient::IsUsableHostName((label63 + ".example.test").c_str()), "a label of 63 characters was refused");
         Expect(!AcmeClient::IsUsableHostName((label64 + ".example.test").c_str()), "a label of 64 characters was taken");
         std::string long254;
         while (long254.size() < 254)
            long254 += "a.";
         long254 = long254.substr(0, 252) + "aa";
         Expect(long254.size() == 254 && !AcmeClient::IsUsableHostName(long254.c_str()), "a name of 254 characters was taken");
         Expect(AcmeClient::IsUsableHostName("localhost"), "a name of one label was refused");
         Expect(!AcmeClient::IsUsableHostName("example.123"), "a name whose last label is all digits was taken");
      }

      // ---- Retry-After, RFC 9110 section 10.2.3
      {
         const time_t now = 1792929600; // 2026-10-25 12:00:00 UTC

         Expect(AcmeClient::ParseRetryAfter("120", now) == 120, "delay-seconds 120 was not 120");
         Expect(AcmeClient::ParseRetryAfter(" 0 ", now) == 0, "delay-seconds 0 was not 0");
         Expect(AcmeClient::ParseRetryAfter("", now) == -1, "an empty Retry-After was read as a time");
         Expect(AcmeClient::ParseRetryAfter("-5", now) == -1, "a negative Retry-After was read");
         Expect(AcmeClient::ParseRetryAfter("soon", now) == -1, "a word was read as a Retry-After");
         Expect(AcmeClient::ParseRetryAfter("Sun, 25 Oct 2026 13:00:00 GMT", now) == 3600,
            "an HTTP-date an hour on was not 3600 seconds");
         Expect(AcmeClient::ParseRetryAfter("Sun, 25 Oct 2026 11:00:00 GMT", now) == 0,
            "an HTTP-date already past was not 0");
         Expect(AcmeClient::ParseRetryAfter("Sun, 25 Oct 2026 13:00:00 UTC", now) == -1,
            "an HTTP-date in another zone than GMT was read");
         Expect(AcmeClient::ParseRetryAfter("Sun, 25 Xyz 2026 13:00:00 GMT", now) == -1,
            "an HTTP-date with no month was read");
      }

      // ---- the CA's problem document
      {
         // What NoteProblem_ keeps of an answer: a rate limit, which holds every
         // order until its Retry-After, is the one type of that name and no other.
         AcmeClient::Problem problem;

         Expect(AcmeClient::ReadProblem("{\"type\":\"urn:ietf:params:acme:error:rateLimited\",\"detail\":\"too many\",\"status\":429}", problem) &&
                problem.rate_limited && problem.type == "urn:ietf:params:acme:error:rateLimited" && problem.detail == _T("too many"),
            "a rateLimited problem document was not read as a rate limit with its type and detail");
         Expect(AcmeClient::ReadProblem("{\n  \"type\": \"urn:ietf:params:acme:error:rateLimited\"\n}", problem) && problem.rate_limited,
            "a pretty-printed rateLimited problem document was not read as a rate limit");

         const char *others[] = { "rejectedIdentifier", "unauthorized", "malformed", "caa", "connection", "serverInternal",
                                  "rateLimitedX", "RateLimited" };
         for (const char *other : others)
         {
            const AnsiString type = AnsiString("urn:ietf:params:acme:error:") + other;
            Expect(AcmeClient::ReadProblem("{\"type\":\"" + type + "\",\"detail\":\"refused\",\"status\":400}", problem) &&
                   !problem.rate_limited && problem.type == type && problem.detail == _T("refused"),
               "the problem " + std::string(other) + " was not kept, or was taken for a rate limit");
         }

         Expect(!AcmeClient::ReadProblem("{\"type\":\"urn:ietf:params:acme:error:badNonce\",\"detail\":\"JWS has an invalid anti-replay nonce\"}", problem) &&
                !problem.rate_limited && problem.type.IsEmpty() && problem.detail.IsEmpty(),
            "a badNonce, which is asked again, was kept as what an attempt failed of");
         Expect(!AcmeClient::ReadProblem("{\"status\":\"valid\"}", problem) && !problem.rate_limited,
            "a body with no type was read as a problem");
         Expect(!AcmeClient::ReadProblem("{\"type\":\"about:blank\",\"detail\":\"rateLimited\"}", problem) && !problem.rate_limited,
            "a problem of a type outside ACME's was read as one");
         Expect(!AcmeClient::ReadProblem("{\"detail\":\"urn:ietf:params:acme:error:rateLimited\"}", problem) && !problem.rate_limited,
            "a detail naming the rate limit was taken for its type");

         // The detail is the CA's words, cleaned and cut: printable ASCII, a run of
         // anything else one space, 300 characters at most.
         const std::string longDetail(400, 'x');
         Expect(AcmeClient::ReadProblem("{\"type\":\"urn:ietf:params:acme:error:dns\",\"detail\":\"" + AnsiString(longDetail.c_str()) + "\"}", problem) &&
                problem.detail.GetLength() == 300,
            "a detail of 400 characters was not cut to 300");
         Expect(AcmeClient::ReadProblem("{\"type\":\"urn:ietf:params:acme:error:dns\",\"detail\":\"no\tA record\r\n\x01" "for it\"}", problem) &&
                problem.detail == _T("no A record for it"),
            "a detail's control characters were not each run made one space");

         // A CA quotes the name it refuses: the escaped quote is inside the detail,
         // not its end, and the escapes are read.
         Expect(AcmeClient::ReadProblem("{\"type\":\"urn:ietf:params:acme:error:rejectedIdentifier\","
                                        "\"detail\":\"Cannot issue for \\\"new.example.test\\\": no public suffix \\\\ \\/ \\u0041\\u00e9\\nend\","
                                        "\"status\":400}", problem) &&
                problem.detail == _T("Cannot issue for \"new.example.test\": no public suffix \\ / A end") && !problem.rate_limited,
            "a detail holding escapes was not read whole: " + std::string(AnsiString(problem.detail).c_str()));
      }

      // ---- the spacing of the checks
      {
         const AnsiString ca = "https://ca.example/directory";
         const __int64 now = 1792929600;
         __int64 until = 0;
         AnsiString why;

         AcmeChecks::Spacing spacing;
         Expect(AcmeChecks::Allows(spacing, ca, now, true, until, why) && until == 0 && why.IsEmpty(),
            "a CA with nothing recorded held a check back");

         spacing.directory_url = ca;
         spacing.last_request_at = now - 30;
         Expect(!AcmeChecks::Allows(spacing, ca, now, true, until, why) && why == "interval" && until == now + 30,
            "a request thirty seconds after the last was let through, or not held to the minute");
         Expect(AcmeChecks::Allows(spacing, ca, now, false, until, why),
            "the minute between requests held back a check nobody requested");
         Expect(AcmeChecks::Allows(spacing, ca, now + 30, true, until, why),
            "a request a minute after the last was held back");
         Expect(AcmeChecks::Allows(spacing, "https://other.example/directory", now, true, until, why),
            "the minute between requests at one CA held a request to another");

         spacing = AcmeChecks::Spacing();
         spacing.directory_url = ca;
         spacing.failed_at = now - 100;
         Expect(!AcmeChecks::Allows(spacing, ca, now, false, until, why) && why == "cooldown" &&
                until == now - 100 + AcmeChecks::FailureCooldownSeconds,
            "an order a hundred seconds after a failure was let through, or not held to twenty minutes");
         Expect(!AcmeChecks::Allows(spacing, ca, now, true, until, why) && why == "cooldown",
            "a request a hundred seconds after a failure was let through");
         Expect(AcmeChecks::Allows(spacing, ca, now - 100 + AcmeChecks::FailureCooldownSeconds, false, until, why),
            "an order twenty minutes after a failure was held back");
         Expect(AcmeChecks::Allows(spacing, "https://other.example/directory", now, false, until, why),
            "a failure at one CA held an order at another");

         // The CA's own time, when it is the later; the cooldown, when that is.
         spacing.rate_limited_until = now + 7200;
         Expect(!AcmeChecks::Allows(spacing, ca, now, false, until, why) && why == "rate_limited" && until == now + 7200,
            "a CA's rate limit two hours out was not what held the order");
         spacing.rate_limited_until = now + 60;
         Expect(!AcmeChecks::Allows(spacing, ca, now, false, until, why) && why == "cooldown",
            "a rate limit shorter than the cooldown was taken for the later of the two");
         spacing.failed_at = 0;
         Expect(!AcmeChecks::Allows(spacing, ca, now, false, until, why) && why == "rate_limited" && until == now + 60,
            "a rate limit alone held nothing");
         Expect(AcmeChecks::Allows(spacing, ca, now + 60, false, until, why),
            "a rate limit held an order after its time");
      }

      // ---- what a check that ended leaves in the spacing
      {
         const AnsiString ca = "https://ca.example/directory";
         const __int64 now = 1792929600;

         Expect(AcmeChecks::RateLimitSeconds(-1) == AcmeChecks::DefaultRateLimitSeconds && AcmeChecks::DefaultRateLimitSeconds == 3600,
            "a rate limit with no Retry-After did not hold for an hour");
         Expect(AcmeChecks::RateLimitSeconds(7200) == 7200, "a rate limit's Retry-After of two hours was not two hours");
         Expect(AcmeChecks::RateLimitSeconds(10000000) == AcmeChecks::MaxRateLimitSeconds && AcmeChecks::MaxRateLimitSeconds == 604800,
            "a Retry-After past seven days was not cut to seven days");

         AcmeClient::Failure failed;
         failed.step = "authorization";

         AcmeChecks::Spacing spacing;
         spacing.directory_url = ca;
         spacing.last_request_at = now - 10;

         AcmeChecks::NoteFinished(spacing, ca, now, true, "failed", &failed);
         Expect(spacing.failed_at == now && spacing.rate_limited_until == 0 && spacing.last_request_at == now - 10,
            "a failed attempt did not start the cooldown alone, or lost the last request's time");

         AcmeClient::Failure limited;
         limited.step = "order";
         limited.rate_limited = true;

         limited.retry_after_seconds = -1;
         AcmeChecks::NoteFinished(spacing, ca, now, true, "failed", &limited);
         Expect(spacing.rate_limited_until == now + 3600, "a rate limit with no Retry-After did not hold every order for an hour");

         limited.retry_after_seconds = 10000000;
         AcmeChecks::NoteFinished(spacing, ca, now, true, "failed", &limited);
         Expect(spacing.rate_limited_until == now + 604800, "a rate limit's Retry-After past seven days was not cut to seven days");

         __int64 until = 0;
         AnsiString why;
         Expect(!AcmeChecks::Allows(spacing, ca, now + 2LL * 86400, false, until, why) && why == "rate_limited" && until == now + 604800,
            "an order two days into a rate limit of seven days was let through");

         // A check that neither ordered nor tried leaves the spacing as it was,
         // whatever its outcome: one held by the spacing (waiting) above all.
         AcmeChecks::Spacing before = spacing;
         AcmeChecks::NoteFinished(spacing, ca, now + 100, false, "failed", &failed);
         AcmeChecks::NoteFinished(spacing, ca, now + 100, false, "waiting", nullptr);
         Expect(spacing.failed_at == before.failed_at && spacing.rate_limited_until == before.rate_limited_until,
            "a check that ordered nothing moved the cooldown or the rate limit");

         // An issued certificate ends the cooldown; the CA's rate limit stands.
         AcmeChecks::NoteFinished(spacing, ca, now + 200, true, "issued", nullptr);
         Expect(spacing.failed_at == 0 && spacing.rate_limited_until == now + 604800,
            "an issued certificate did not end the cooldown, or ended the CA's rate limit");

         // A check against another CA starts the spacing afresh for that one.
         AcmeChecks::NoteFinished(spacing, "https://other.example/directory", now + 300, true, "failed", &failed);
         Expect(spacing.directory_url == "https://other.example/directory" && spacing.failed_at == now + 300 &&
                spacing.rate_limited_until == 0 && spacing.last_request_at == 0,
            "a failure at another CA kept what was learnt of the first");
      }

      // ---- whether a reservation still holds a check
      {
         const __int64 now = 1792929600;

         AcmeChecks::Attempt queued;
         queued.step = "queued";
         queued.queued_at = now - 60;
         Expect(AcmeChecks::Holds(true, queued, now), "a check queued a minute ago was taken to be dropped");

         queued.queued_at = now - AcmeChecks::AbandonedQueuedSeconds - 1;
         Expect(!AcmeChecks::Holds(true, queued, now), "a check queued ten minutes ago and never started was taken to hold");

         AcmeChecks::Attempt running;
         running.step = "authorization";
         running.queued_at = now - 3LL * 3600;
         running.started_at = now - 3LL * 3600;
         Expect(AcmeChecks::Holds(true, running, now), "a check that is running was taken to be dropped for its age");

         Expect(!AcmeChecks::Holds(false, queued, now) && !AcmeChecks::Holds(false, running, now),
            "a reservation was taken to hold with no check running");
      }

      // ---- what a certificate names, and whether the CA's is one to install
      {
         EVP_PKEY *key = MakeKey();
         EVP_PKEY *otherKey = MakeKey();

         try
         {
            std::vector<AnsiString> names;

            const AnsiString withNames = MakeCertificate(key, "other.example.test", Names("mail.example.test", "*.wild.example.test"), -60, year);
            Expect(AcmeClient::ReadCertificateNamesFromPem(withNames, names) && Narrow(names) == "mail.example.test, *.wild.example.test",
               "the subject alternative names were not the names read, the common name left out: " + Narrow(names));

            const AnsiString commonNameOnly = MakeCertificate(key, "Legacy.Example.TEST", std::vector<AnsiString>(), -60, year);
            Expect(AcmeClient::ReadCertificateNamesFromPem(commonNameOnly, names) && Narrow(names) == "legacy.example.test",
               "a certificate with no subject alternative names was not read by its common name: " + Narrow(names));

            Expect(!AcmeClient::ReadCertificateNamesFromPem("not a certificate", names), "text that is no certificate was read as one");

            const std::vector<AnsiString> both = Names("mail.example.test", "new.example.test");
            const AnsiString issued = MakeCertificate(key, "mail.example.test", both, -60, year);
            String reason;

            Expect(AcmeClient::CheckIssuedCertificate(issued, key, both, reason),
               "a certificate for the key, naming both hosts, was refused");

            Expect(!AcmeClient::CheckIssuedCertificate(issued, key, Names("mail.example.test", "third.example.test"), reason) &&
                   reason.Find(_T("third.example.test")) >= 0,
               "a certificate that does not name a host the order asked for was installed, or the refusal did not name the host");

            Expect(!AcmeClient::CheckIssuedCertificate(issued, otherKey, both, reason),
               "a certificate for another key was installed");

            const AnsiString expired = MakeCertificate(key, "mail.example.test", both, -2 * year, -year);
            Expect(!AcmeClient::CheckIssuedCertificate(expired, key, both, reason),
               "an expired certificate was installed");

            Expect(!AcmeClient::CheckIssuedCertificate("<html>Service Unavailable</html>", key, both, reason),
               "a body that is no certificate was installed");
         }
         catch (...)
         {
            EVP_PKEY_free(key);
            EVP_PKEY_free(otherKey);
            throw;
         }

         EVP_PKEY_free(key);
         EVP_PKEY_free(otherKey);
      }
   }

}
