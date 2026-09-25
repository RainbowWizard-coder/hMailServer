// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ObjectStoreSignature.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      std::string LowerAscii(const std::string &text)
      {
         std::string lower = text;
         for (size_t i = 0; i < lower.size(); i++)
         {
            if (lower[i] >= 'A' && lower[i] <= 'Z')
               lower[i] = (char) (lower[i] - 'A' + 'a');
         }
         return lower;
      }

      // Leading and trailing spaces off, and every run of spaces inside made
      // one: SigV4's Trimall, which is also what Shared Key asks of a header's
      // value.
      std::string Trimall(const std::string &value)
      {
         std::string out;
         bool space = false;
         for (size_t i = 0; i < value.size(); i++)
         {
            const char c = value[i];
            if (c == ' ' || c == '\t')
            {
               space = !out.empty();
               continue;
            }

            if (space)
               out += ' ';
            space = false;
            out += c;
         }
         return out;
      }

      std::string DigestOf(const EVP_MD *digest, const char *bytes, size_t length)
      {
         unsigned char out[EVP_MAX_MD_SIZE];
         unsigned int outLength = 0;
         if (EVP_Digest(bytes, length, out, &outLength, digest, nullptr) != 1)
            return std::string();

         return std::string((const char *) out, outLength);
      }

      bool IsUnreserved(unsigned char c)
      {
         return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
      }

      const char *WeekDays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
      const char *Months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

      bool UtcOf(time_t when, struct tm &parts)
      {
#ifdef HM_PLATFORM_POSIX
         return gmtime_r(&when, &parts) != nullptr;
#else
         return gmtime_s(&parts, &when) == 0;
#endif
      }
   }

   std::string
   ObjectStoreSignature::Sha256Of(const char *bytes, size_t length)
   {
      return DigestOf(EVP_sha256(), bytes, length);
   }

   std::string
   ObjectStoreSignature::Sha256HexOf(const std::string &bytes)
   {
      return HexOf(Sha256Of(bytes.data(), bytes.size()));
   }

   std::string
   ObjectStoreSignature::Md5Of(const char *bytes, size_t length)
   {
      return DigestOf(EVP_md5(), bytes, length);
   }

   std::string
   ObjectStoreSignature::HmacSha256Of(const std::string &key, const std::string &data)
   {
      unsigned char out[EVP_MAX_MD_SIZE];
      unsigned int outLength = 0;
      if (HMAC(EVP_sha256(), key.data(), (int) key.size(), (const unsigned char *) data.data(), data.size(), out, &outLength) == nullptr)
         return std::string();

      return std::string((const char *) out, outLength);
   }

   std::string
   ObjectStoreSignature::HexOf(const std::string &raw)
   {
      static const char *digits = "0123456789abcdef";
      std::string hex;
      hex.reserve(raw.size() * 2);
      for (size_t i = 0; i < raw.size(); i++)
      {
         const unsigned char byte = (unsigned char) raw[i];
         hex += digits[byte >> 4];
         hex += digits[byte & 0x0F];
      }
      return hex;
   }

   std::string
   ObjectStoreSignature::Base64Of(const std::string &raw)
   {
      static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve((raw.size() + 2) / 3 * 4);

      size_t i = 0;
      for (; i + 2 < raw.size(); i += 3)
      {
         const unsigned int n = ((unsigned char) raw[i] << 16) | ((unsigned char) raw[i + 1] << 8) | (unsigned char) raw[i + 2];
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
         out += alphabet[n & 63];
      }

      if (i + 1 == raw.size())
      {
         const unsigned int n = (unsigned char) raw[i] << 16;
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += "==";
      }
      else if (i + 2 == raw.size())
      {
         const unsigned int n = ((unsigned char) raw[i] << 16) | ((unsigned char) raw[i + 1] << 8);
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
         out += '=';
      }

      return out;
   }

   bool
   ObjectStoreSignature::FromBase64(const std::string &text, std::string &raw)
   {
      raw.clear();

      unsigned int accumulated = 0;
      int bits = 0;
      size_t padding = 0;

      for (size_t i = 0; i < text.size(); i++)
      {
         const char c = text[i];
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
         else if (c == '=')
         {
            padding++;
            continue;
         }
         else if (c == '\r' || c == '\n' || c == ' ')
            continue;
         else
            return false;

         // Nothing after the padding.
         if (padding > 0)
            return false;

         accumulated = (accumulated << 6) | (unsigned int) value;
         bits += 6;
         if (bits >= 8)
         {
            bits -= 8;
            raw += (char) ((accumulated >> bits) & 0xFF);
         }
      }

      return padding <= 2 && !raw.empty();
   }

   std::string
   ObjectStoreSignature::UriEncode(const std::string &text, bool encodeSlash)
   {
      static const char *digits = "0123456789ABCDEF";
      std::string out;
      out.reserve(text.size() * 3);
      for (size_t i = 0; i < text.size(); i++)
      {
         const unsigned char c = (unsigned char) text[i];
         if (IsUnreserved(c) || (c == '/' && !encodeSlash))
            out += (char) c;
         else
         {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 0x0F];
         }
      }
      return out;
   }

   std::string
   ObjectStoreSignature::AmzDate(time_t when)
   {
      struct tm parts;
      memset(&parts, 0, sizeof(parts));
      if (!UtcOf(when, parts))
         return std::string();

      char text[32];
      snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02d%02dZ", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
               parts.tm_hour, parts.tm_min, parts.tm_sec);
      return std::string(text);
   }

   std::string
   ObjectStoreSignature::Rfc1123Date(time_t when)
   {
      struct tm parts;
      memset(&parts, 0, sizeof(parts));
      if (!UtcOf(when, parts) || parts.tm_wday < 0 || parts.tm_wday > 6 || parts.tm_mon < 0 || parts.tm_mon > 11)
         return std::string();

      // Written out, never through strftime: %a and %b follow the locale, and
      // the store wants the English names.
      char text[40];
      snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT", WeekDays[parts.tm_wday], parts.tm_mday,
               Months[parts.tm_mon], parts.tm_year + 1900, parts.tm_hour, parts.tm_min, parts.tm_sec);
      return std::string(text);
   }

   std::string
   ObjectStoreSignature::CanonicalQuery(const Pairs &query)
   {
      // Each name and value encoded, then sorted by name and by value.
      std::vector<std::pair<std::string, std::string> > encoded;
      for (size_t i = 0; i < query.size(); i++)
         encoded.push_back(std::make_pair(UriEncode(query[i].first, true), UriEncode(query[i].second, true)));

      std::sort(encoded.begin(), encoded.end());

      std::string canonical;
      for (size_t i = 0; i < encoded.size(); i++)
      {
         if (i > 0)
            canonical += '&';
         canonical += encoded[i].first;
         canonical += '=';
         canonical += encoded[i].second;
      }
      return canonical;
   }

   ObjectStoreSignature::V4Signed
   ObjectStoreSignature::SignV4(const V4Request &request, const std::string &amzDate, const std::string &region,
                                const std::string &service, const std::string &accessKeyId, const std::string &secretKey)
   {
      V4Signed result;

      // The headers by their lower-case names, sorted; two of one name are one
      // line with the values comma-joined in the order given.
      std::map<std::string, std::string> headers;
      for (size_t i = 0; i < request.headers.size(); i++)
      {
         const std::string name = LowerAscii(Trimall(request.headers[i].first));
         const std::string value = Trimall(request.headers[i].second);
         std::map<std::string, std::string>::iterator found = headers.find(name);
         if (found == headers.end())
            headers[name] = value;
         else
            found->second += "," + value;
      }

      std::string canonicalHeaders;
      for (std::map<std::string, std::string>::const_iterator header = headers.begin(); header != headers.end(); ++header)
      {
         canonicalHeaders += header->first + ":" + header->second + "\n";
         if (!result.signed_headers.empty())
            result.signed_headers += ';';
         result.signed_headers += header->first;
      }

      std::string canonicalPath = request.path.empty() ? std::string("/") : UriEncode(request.path, false);
      if (request.double_encode_path)
         canonicalPath = UriEncode(canonicalPath, false);

      result.canonical_request = request.method + "\n" + canonicalPath + "\n" + CanonicalQuery(request.query) + "\n" +
                                 canonicalHeaders + "\n" + result.signed_headers + "\n" + request.payload_hash;

      const std::string date = amzDate.substr(0, 8);
      const std::string scope = date + "/" + region + "/" + service + "/aws4_request";

      result.string_to_sign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + Sha256HexOf(result.canonical_request);

      std::string key = HmacSha256Of("AWS4" + secretKey, date);
      key = HmacSha256Of(key, region);
      key = HmacSha256Of(key, service);
      key = HmacSha256Of(key, "aws4_request");

      result.signature = HexOf(HmacSha256Of(key, result.string_to_sign));
      result.authorization = "AWS4-HMAC-SHA256 Credential=" + accessKeyId + "/" + scope + ", SignedHeaders=" + result.signed_headers +
                             ", Signature=" + result.signature;
      return result;
   }

   std::string
   ObjectStoreSignature::SharedKeyStringToSign(const SharedKeyRequest &request, const std::string &account)
   {
      // The standard headers by name; a Range given as x-ms-range is among the
      // x-ms- headers below instead.
      std::string range;
      std::string ifMatch;
      std::map<std::string, std::string> msHeaders;
      for (size_t i = 0; i < request.headers.size(); i++)
      {
         const std::string name = LowerAscii(Trimall(request.headers[i].first));
         const std::string value = Trimall(request.headers[i].second);
         if (name == "range")
            range = value;
         else if (name == "if-match")
            ifMatch = value;
         else if (name.compare(0, 5, "x-ms-") == 0)
            msHeaders[name] = value;
      }

      // Content-Length is empty for a request with no body (version 2015-02-21
      // and later): the header the client sends then says 0.
      std::string length;
      if (request.content_length > 0)
      {
         char text[32];
         snprintf(text, sizeof(text), "%llu", request.content_length);
         length = text;
      }

      std::string toSign = request.method + "\n" +
                           "\n" +                          // Content-Encoding
                           "\n" +                          // Content-Language
                           length + "\n" +                 // Content-Length
                           "\n" +                          // Content-MD5
                           request.content_type + "\n" +   // Content-Type
                           "\n" +                          // Date (x-ms-date is sent instead)
                           "\n" +                          // If-Modified-Since
                           ifMatch + "\n" +                // If-Match
                           "\n" +                          // If-None-Match
                           "\n" +                          // If-Unmodified-Since
                           range + "\n";

      for (std::map<std::string, std::string>::const_iterator header = msHeaders.begin(); header != msHeaders.end(); ++header)
         toSign += header->first + ":" + header->second + "\n";

      // The resource: the account, then the path as the URL carries it -
      // which, for the emulator, begins with the account a second time - then
      // each query parameter by its lower-case name, sorted, its values sorted
      // and comma-joined.
      toSign += "/" + account + UriEncode(request.path, false);

      std::map<std::string, std::vector<std::string> > parameters;
      for (size_t i = 0; i < request.query.size(); i++)
         parameters[LowerAscii(request.query[i].first)].push_back(request.query[i].second);

      for (std::map<std::string, std::vector<std::string> >::iterator parameter = parameters.begin(); parameter != parameters.end(); ++parameter)
      {
         std::sort(parameter->second.begin(), parameter->second.end());
         toSign += "\n" + parameter->first + ":";
         for (size_t v = 0; v < parameter->second.size(); v++)
         {
            if (v > 0)
               toSign += ",";
            toSign += parameter->second[v];
         }
      }

      return toSign;
   }

   bool
   ObjectStoreSignature::SignSharedKey(const SharedKeyRequest &request, const std::string &account, const std::string &base64Key,
                                       std::string &authorization)
   {
      authorization.clear();

      std::string key;
      if (!FromBase64(base64Key, key))
         return false;

      const std::string signature = Base64Of(HmacSha256Of(key, SharedKeyStringToSign(request, account)));
      authorization = "SharedKey " + account + ":" + signature;
      return true;
   }

   // ---- ObjectStoreSignatureTester ---------------------------------------------

   namespace
   {
      struct SuiteCase
      {
         const char *name;
         const char *path;
         const char *query[3][2];
         const char *signature;
      };

      void Expect(std::vector<AnsiString> &failures, bool condition, const std::string &what)
      {
         if (!condition)
            failures.push_back(AnsiString(what.c_str()));
      }
   }

   std::vector<AnsiString>
   ObjectStoreSignatureTester::Run()
   {
      std::vector<AnsiString> failures;
      typedef ObjectStoreSignature S;

      const std::string emptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
      Expect(failures, S::Sha256HexOf(std::string()) == emptyHash, "the SHA-256 of nothing");

      // The encoding: unreserved characters kept, everything else %XX in upper case.
      Expect(failures, S::UriEncode("a b/c~$", false) == "a%20b/c~%24", "UriEncode of a path");
      Expect(failures, S::UriEncode("a b/c", true) == "a%20b%2Fc", "UriEncode of a query value");
      Expect(failures, S::UriEncode("\xE1\x88\xB4", true) == "%E1%88%B4", "UriEncode of UTF-8");

      // Base64 both ways, with the refusal of what is not base64.
      std::string raw;
      Expect(failures, S::Base64Of("hMailServer") == "aE1haWxTZXJ2ZXI=", "Base64Of");
      Expect(failures, S::FromBase64("aE1haWxTZXJ2ZXI=", raw) && raw == "hMailServer", "FromBase64");
      Expect(failures, !S::FromBase64("not base64!", raw), "FromBase64 refuses what is not base64");

      Expect(failures, S::AmzDate((time_t) 1440938160) == "20150830T123600Z", "AmzDate");
      Expect(failures, S::Rfc1123Date((time_t) 1440938160) == "Sun, 30 Aug 2015 12:36:00 GMT", "Rfc1123Date");

      // AWS's Signature Version 4 test suite: the service "service" in
      // us-east-1 at 20150830T123600Z with the suite's own key, a GET to
      // example.amazonaws.com signing host and x-amz-date - get-vanilla, the
      // get-vanilla-query family and get-unreserved, as the suite publishes
      // their signatures.
      const SuiteCase suite[] =
      {
         { "get-vanilla", "/", { { nullptr, nullptr } }, "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31" },
         { "get-vanilla-query", "/", { { nullptr, nullptr } }, "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31" },
         { "get-vanilla-empty-query-key", "/", { { "Param1", "value1" }, { nullptr, nullptr } }, "a67d582fa61cc504c4bae71f336f98b97f1ea3c7a6bfe1b6e45aec72011b9aeb" },
         { "get-vanilla-query-order-key", "/", { { "Param1", "value2" }, { "Param1", "Value1" }, { nullptr, nullptr } }, "eedbc4e291e521cf13422ffca22be7d2eb8146eecf653089df300a15b2382bd1" },
         { "get-vanilla-query-order-key-case", "/", { { "Param2", "value2" }, { "Param1", "value1" }, { nullptr, nullptr } }, "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500" },
         { "get-vanilla-query-order-value", "/", { { "Param1", "value2" }, { "Param1", "value1" }, { nullptr, nullptr } }, "5772eed61e12b33fae39ee5e7012498b51d56abc0abb7c60486157bd471c4694" },
         { "get-vanilla-query-unreserved", "/", { { "-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", "-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" }, { nullptr, nullptr } }, "9c3e54bfcdf0b19771a7f523ee5669cdf59bc7cc0884027167c21bb143a40197" },
         { "get-vanilla-utf8-query", "/", { { "\xE1\x88\xB4", "bar" }, { nullptr, nullptr } }, "2cdec8eed098649ff3a119c94853b13c643bcf08f8b0a1d91e12c9027818dd04" },
         { "get-unreserved", "/-._~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", { { nullptr, nullptr } }, "07ef7494c76fa4850883e2b006601f940f8a34d404d0cfa977f52a65bbf5f24f" }
      };

      for (size_t c = 0; c < sizeof(suite) / sizeof(suite[0]); c++)
      {
         S::V4Request request;
         request.method = "GET";
         request.path = suite[c].path;
         for (int q = 0; q < 3 && suite[c].query[q][0] != nullptr; q++)
            request.query.push_back(std::make_pair(std::string(suite[c].query[q][0]), std::string(suite[c].query[q][1])));
         request.headers.push_back(std::make_pair(std::string("Host"), std::string("example.amazonaws.com")));
         request.headers.push_back(std::make_pair(std::string("X-Amz-Date"), std::string("20150830T123600Z")));
         request.payload_hash = emptyHash;
         request.double_encode_path = true;

         const S::V4Signed signedRequest = S::SignV4(request, "20150830T123600Z", "us-east-1", "service", "AKIDEXAMPLE",
                                                     "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
         Expect(failures, signedRequest.signature == suite[c].signature, std::string("SigV4 test suite: ") + suite[c].name + " signs " + signedRequest.signature);

         if (c == 0)
         {
            Expect(failures, signedRequest.canonical_request ==
               "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n" + emptyHash,
               "SigV4 test suite: get-vanilla's canonical request");
            Expect(failures, signedRequest.string_to_sign ==
               "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
               "SigV4 test suite: get-vanilla's string to sign");
            Expect(failures, signedRequest.authorization ==
               "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date, "
               "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31",
               "SigV4 test suite: get-vanilla's Authorization header");

            // The negative control: one byte of the secret, and the signature is
            // another.
            const S::V4Signed otherKey = S::SignV4(request, "20150830T123600Z", "us-east-1", "service", "AKIDEXAMPLE",
                                                   "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEz");
            Expect(failures, otherKey.signature != signedRequest.signature, "SigV4: a changed secret gives the same signature");
         }
      }

      // The four S3 examples of the SigV4 documentation: examplebucket at
      // 20130524T000000Z, the path encoded once, as S3 wants it.
      const std::string s3Access = "AKIAIOSFODNN7EXAMPLE";
      const std::string s3Secret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
      const std::string s3Date = "20130524T000000Z";
      const std::string s3Host = "examplebucket.s3.amazonaws.com";

      {
         S::V4Request request;
         request.method = "GET";
         request.path = "/test.txt";
         request.headers.push_back(std::make_pair(std::string("Host"), s3Host));
         request.headers.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-9")));
         request.headers.push_back(std::make_pair(std::string("x-amz-content-sha256"), emptyHash));
         request.headers.push_back(std::make_pair(std::string("x-amz-date"), s3Date));
         request.payload_hash = emptyHash;
         Expect(failures, S::SignV4(request, s3Date, "us-east-1", "s3", s3Access, s3Secret).signature ==
            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41", "SigV4 S3 example: GET Object with a Range");
      }

      {
         const std::string body = "Welcome to Amazon S3.";
         S::V4Request request;
         request.method = "PUT";
         request.path = "/test$file.text";
         request.headers.push_back(std::make_pair(std::string("Date"), std::string("Fri, 24 May 2013 00:00:00 GMT")));
         request.headers.push_back(std::make_pair(std::string("Host"), s3Host));
         request.headers.push_back(std::make_pair(std::string("x-amz-content-sha256"), S::Sha256HexOf(body)));
         request.headers.push_back(std::make_pair(std::string("x-amz-date"), s3Date));
         request.headers.push_back(std::make_pair(std::string("x-amz-storage-class"), std::string("REDUCED_REDUNDANCY")));
         request.payload_hash = S::Sha256HexOf(body);
         Expect(failures, request.payload_hash == "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072", "SigV4 S3 example: the PUT's payload hash");
         Expect(failures, S::SignV4(request, s3Date, "us-east-1", "s3", s3Access, s3Secret).signature ==
            "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd", "SigV4 S3 example: PUT Object ($ encoded once)");
      }

      {
         S::V4Request request;
         request.method = "GET";
         request.path = "/";
         request.query.push_back(std::make_pair(std::string("lifecycle"), std::string()));
         request.headers.push_back(std::make_pair(std::string("Host"), s3Host));
         request.headers.push_back(std::make_pair(std::string("x-amz-content-sha256"), emptyHash));
         request.headers.push_back(std::make_pair(std::string("x-amz-date"), s3Date));
         request.payload_hash = emptyHash;
         Expect(failures, S::SignV4(request, s3Date, "us-east-1", "s3", s3Access, s3Secret).signature ==
            "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543", "SigV4 S3 example: GET Bucket lifecycle (a parameter without a value)");
      }

      {
         S::V4Request request;
         request.method = "GET";
         request.path = "/";
         request.query.push_back(std::make_pair(std::string("prefix"), std::string("J")));
         request.query.push_back(std::make_pair(std::string("max-keys"), std::string("2")));
         request.headers.push_back(std::make_pair(std::string("Host"), s3Host));
         request.headers.push_back(std::make_pair(std::string("x-amz-content-sha256"), emptyHash));
         request.headers.push_back(std::make_pair(std::string("x-amz-date"), s3Date));
         request.payload_hash = emptyHash;
         Expect(failures, S::SignV4(request, s3Date, "us-east-1", "s3", s3Access, s3Secret).signature ==
            "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7", "SigV4 S3 example: GET Bucket, the query sorted");
      }

      // Shared Key: the string to sign as Microsoft's specification lays it out
      // for a Put Blob to the emulator's path-style URL (the account twice in
      // the resource), and one with query parameters, each signed with the
      // emulator's published key. The signatures were computed apart from this
      // code, with Python's hmac, and Azurite accepted requests signed this way
      // (docs/ObjectStorage.md, "How it was proved").
      const std::string emulatorKey = "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==";
      {
         S::SharedKeyRequest request;
         request.method = "PUT";
         request.path = "/devstoreaccount1/hmslab/example.com/user/ab/{AB12CD34-0000-0000-0000-000000000011}.eml";
         request.headers.push_back(std::make_pair(std::string("x-ms-version"), std::string("2021-08-06")));
         request.headers.push_back(std::make_pair(std::string("x-ms-date"), std::string("Fri, 25 Sep 2026 05:00:00 GMT")));
         request.headers.push_back(std::make_pair(std::string("x-ms-blob-type"), std::string("BlockBlob")));
         request.content_length = 57;
         request.content_type = "application/octet-stream";

         Expect(failures, S::SharedKeyStringToSign(request, "devstoreaccount1") ==
            "PUT\n\n\n57\n\napplication/octet-stream\n\n\n\n\n\n\n"
            "x-ms-blob-type:BlockBlob\nx-ms-date:Fri, 25 Sep 2026 05:00:00 GMT\nx-ms-version:2021-08-06\n"
            "/devstoreaccount1/devstoreaccount1/hmslab/example.com/user/ab/%7BAB12CD34-0000-0000-0000-000000000011%7D.eml",
            "Shared Key: the string to sign of a Put Blob");

         std::string authorization;
         Expect(failures, S::SignSharedKey(request, "devstoreaccount1", emulatorKey, authorization) &&
            authorization == "SharedKey devstoreaccount1:008ka12sLVNZ1FQvPb+1XQfW071TTa9Yu9gQOEyBvhY=", "Shared Key: a Put Blob's signature");

         request.content_length = 58;
         std::string changed;
         Expect(failures, S::SignSharedKey(request, "devstoreaccount1", emulatorKey, changed) && changed != authorization,
            "Shared Key: a changed length gives the same signature");
         Expect(failures, !S::SignSharedKey(request, "devstoreaccount1", "not a key!", changed), "Shared Key: a key that is not base64 is refused");
      }

      {
         S::SharedKeyRequest request;
         request.method = "GET";
         request.path = "/devstoreaccount1/hmslab";
         request.query.push_back(std::make_pair(std::string("restype"), std::string("container")));
         request.query.push_back(std::make_pair(std::string("comp"), std::string("list")));
         request.query.push_back(std::make_pair(std::string("prefix"), std::string("example.com/")));
         request.headers.push_back(std::make_pair(std::string("x-ms-date"), std::string("Fri, 25 Sep 2026 05:00:00 GMT")));
         request.headers.push_back(std::make_pair(std::string("x-ms-version"), std::string("2021-08-06")));

         std::string authorization;
         Expect(failures, S::SignSharedKey(request, "devstoreaccount1", emulatorKey, authorization) &&
            authorization == "SharedKey devstoreaccount1:VQgUmMTYuUE3XC1WtpyFav2OJGdQdOJK20ixtaOCA3U=", "Shared Key: a List Blobs signature, its parameters sorted");
      }

      return failures;
   }

   void
   ObjectStoreSignatureTester::Test()
   {
      const std::vector<AnsiString> failures = Run();
      if (failures.empty())
         return;

      for (size_t i = 0; i < failures.size(); i++)
         ErrorManager::Instance()->ReportError(ErrorManager::High, 7477, "ObjectStoreSignatureTester::Test", String(failures[i]));

      throw 0;
   }
}
