// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// How a request to an object store says who is asking: AWS Signature Version 4
// for the S3 API (AWS S3, MinIO, Wasabi, Backblaze B2's S3 API, Ceph RGW), and
// Shared Key for Azure Blob Storage. Functions of values only - no clock, no
// network, no configuration - so that ObjectStoreSignatureTester can hold them
// to AWS's published vectors on both platforms (docs/ObjectStorage.md), and the
// object store client (Persistence/MessageStoreObjectClient.h) builds its
// requests on them.
//
// Every string here is bytes: UTF-8 text, or a digest. A key with a character
// outside ASCII is UTF-8 on the wire and in the signature alike.

#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace HM
{
   class ObjectStoreSignature
   {
   public:

      // Name and value, in the order given.
      typedef std::vector<std::pair<std::string, std::string> > Pairs;

      // ---- The pieces --------------------------------------------------------

      static std::string Sha256Of(const char *bytes, size_t length);
      static std::string Sha256HexOf(const std::string &bytes);
      static std::string HmacSha256Of(const std::string &key, const std::string &data);
      static std::string Md5Of(const char *bytes, size_t length);
      static std::string HexOf(const std::string &raw);
      static std::string Base64Of(const std::string &raw);
      static bool FromBase64(const std::string &text, std::string &raw);

      // RFC 3986's unreserved characters as they are, every other byte %XX in
      // upper-case hex; a '/' kept when encodeSlash is false (a path).
      static std::string UriEncode(const std::string &text, bool encodeSlash);

      // 20150830T123600Z, and Mon, 30 Aug 2015 12:36:00 GMT, of a time in UTC.
      static std::string AmzDate(time_t when);
      static std::string Rfc1123Date(time_t when);

      // ---- AWS Signature Version 4 -------------------------------------------

      struct V4Request
      {
         V4Request() : double_encode_path(false) {}

         std::string method;
         std::string path;            // the absolute path, not encoded: /bucket/key
         Pairs query;                 // not encoded
         Pairs headers;               // every header signed; names in any case
         std::string payload_hash;    // the hex SHA-256 of the body
         bool double_encode_path;     // S3 encodes the path once; every other service twice
      };

      struct V4Signed
      {
         std::string canonical_request;
         std::string string_to_sign;
         std::string signed_headers;
         std::string signature;
         std::string authorization;   // the Authorization header's value
      };

      static std::string CanonicalQuery(const Pairs &query);
      static V4Signed SignV4(const V4Request &request, const std::string &amzDate, const std::string &region,
                             const std::string &service, const std::string &accessKeyId, const std::string &secretKey);

      // ---- Azure Blob Storage, Shared Key ------------------------------------

      struct SharedKeyRequest
      {
         SharedKeyRequest() : content_length(0) {}

         std::string method;
         std::string path;            // the URL's path, not encoded - for the emulator it begins with the account
         Pairs query;                 // not encoded
         Pairs headers;               // the x-ms- headers, and Range and If-Match when there are
         unsigned long long content_length;
         std::string content_type;
      };

      static std::string SharedKeyStringToSign(const SharedKeyRequest &request, const std::string &account);

      // False when the account key is not base64.
      static bool SignSharedKey(const SharedKeyRequest &request, const std::string &account, const std::string &base64Key,
                                std::string &authorization);
   };

   // AWS's Signature Version 4 test suite (the get-vanilla family and
   // get-unreserved, for the service "service" in us-east-1), the four S3
   // examples of the S3 SigV4 documentation, and Shared Key against a string to
   // sign and a signature written out by hand from Microsoft's specification -
   // each with its negative control. Run() answers what failed; Test() reports
   // each failure (HM7477) and throws, which is how ClassTester's testers fail.
   class ObjectStoreSignatureTester
   {
   public:
      std::vector<AnsiString> Run();
      void Test();
   };
}
