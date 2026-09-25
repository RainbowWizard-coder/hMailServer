// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// One object store, spoken to over HTTPS: the S3 API - AWS S3, MinIO, Wasabi,
// Backblaze B2's S3 API, Ceph RGW - signed with AWS Signature Version 4, path-
// style or virtual-hosted, or Azure Blob Storage with Shared Key or a SAS, in
// block blobs. What the message store's object backend (MessageStoreObjects.h)
// asks of a store, and nothing more: the head, a range, a put, a copy, a
// remove, a listing, and whether the bucket answers at all.
//
// THE TRANSPORT
//
// HttpsClient::RequestTrusting: HTTP/1.0, one connection per request, TLS
// verified always - against the machine's roots, or against the CA file the
// administrator named for a store behind a CA of the organisation's own - and
// plain http accepted only to a loopback address, as everywhere else in this
// server. HttpProxy carries it as it carries every other client here.
//
// WHAT A FAILURE IS
//
// Every request answers one of four outcomes, and the difference between the
// last two is the one the rest of the server acts on:
//
//   Done         the store did it (a remove of what was not there included)
//   Absent       the store answered, and there is no such object
//   Failed       the store answered and refused this request for a reason
//                that another try would not change - a 4xx other than 404, 401
//                and 403, or an answer that could not be read
//   Unavailable  the store could not be reached, or answered 408, 429 or 5xx
//                through every retry, or refused this server's credentials
//                (401, 403), or has no such bucket (a 404 to a put or to the
//                bucket's HEAD): a condition outside the message, which the
//                committer waits out with the message on the local disk, IMAP
//                answers NO [UNAVAILABLE] and POP3 -ERR [SYS/TEMP]
//                (docs/ObjectStorage.md)
//
// A request is tried three times at most, 200 ms and then 800 ms apart, and
// each try is bounded as a whole - the lookup, the connection, the handshake,
// the transfer - by 15 seconds and 2 more per MB it carries, 120 at most; an
// answer whose body ends short of the Content-Length it gave is a try that
// failed, never the object. After a request ends Unavailable the store is not
// asked again for ten seconds: every request in that time answers Unavailable
// at once, so that a store that has gone away costs a delivery thread one
// bounded wait and not one a message.
// HM7470 (not reached) and HM7471 (credentials refused) are said once a minute
// at most, HM7474 (a refusal the store explained) once a minute too.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace HM
{
   class MessageStoreObjectClient
   {
   public:

      enum Outcome
      {
         OutcomeDone = 0,
         OutcomeAbsent = 1,
         OutcomeFailed = 2,
         OutcomeUnavailable = 3
      };

      enum Service
      {
         ServiceNone = 0,
         ServiceS3 = 1,
         ServiceAzure = 2
      };

      struct Settings
      {
         Settings() : service(ServiceNone) {}

         Service service;
         std::string endpoint;      // https://host[:port][/path]; empty for AWS's, or the account's own at Azure
         std::string region;        // S3's signing region
         std::string bucket;        // the bucket, or the Azure container
         std::string addressing;    // S3: auto, path or virtual
         std::string access_key;    // S3's access key id; Azure's storage account name
         std::string secret;        // S3's secret access key; Azure's account key (base64)
         std::string sas;           // Azure: a SAS token, used instead of the account key when set
         String ca_file;            // a PEM file of the CA the store's certificate is issued by, or empty

         bool operator==(const Settings &other) const;
         bool operator!=(const Settings &other) const { return !(*this == other); }
      };

      struct Properties
      {
         Properties() : size(0) {}

         unsigned __int64 size;
         std::string etag;          // without its quotes
         std::string content_md5;   // the MD5 of the object, raw, when the store keeps one (Azure's Content-MD5)
      };

      struct Listed
      {
         Listed() : size(0), modified(0) {}

         std::string key;
         unsigned __int64 size;
         __int64 modified;          // seconds since 1970, 0 when the store did not say
      };

      // length bytes from offset of what is being put, into bytes. False when
      // they cannot be had (the local file went away), which ends the put.
      typedef std::function<bool(unsigned __int64 offset, size_t length, std::string &bytes)> Source;

      // What a put above this size is split into: S3's multipart upload and
      // Azure's blocks, each part this long but the last.
      static const size_t PartBytes = 16 * 1024 * 1024;

      virtual ~MessageStoreObjectClient() {}

      // A client for the settings, or null with the reason when they do not
      // describe a store this client can speak to.
      static std::shared_ptr<MessageStoreObjectClient> Create(const Settings &settings, String &problem);

      // The object's size, ETag and MD5.
      virtual Outcome Head(const std::string &key, Properties &properties) = 0;

      // length bytes from offset, or to the end when length is 0; total is
      // the whole object's size, and etag, when asked for, the ETag the store
      // gave. An offset at or past the end answers Done, no bytes, and the
      // total. To the end is read in windows - the first megabyte, then 8 MB at
      // a time - so that each request's deadline is its window's and never a
      // guess at the object's size; every window after the first is asked for
      // If-Match the first one's ETag, so that an object rewritten in the
      // middle of a read fails the read (Failed) instead of splicing two
      // versions. With ifMatch given, the first window is held to it too.
      Outcome Read(const std::string &key, unsigned __int64 offset, unsigned __int64 length,
                   std::string &bytes, unsigned __int64 &total, const std::string &ifMatch = std::string(), std::string *etag = nullptr);

      // The object written whole - one request up to PartBytes, parts above -
      // and nothing under the key until it is: a single PUT is atomic, and a
      // multipart upload or a block list is committed by its last request. A
      // multipart upload that fails is aborted; Azure's uncommitted blocks are
      // the store's to expire. md5 is the MD5 of what was put, raw.
      virtual Outcome Put(const std::string &key, unsigned __int64 size, const Source &source, std::string &md5) = 0;

      // A copy made by the store itself where it can (S3's CopyObject, which
      // replaces the destination whole or not at all; Azure's Copy Blob with
      // Shared Key onto a name that holds nothing); read and put again through
      // this client where it cannot - Azure with a SAS, or onto a name that
      // holds an object, since a Copy Blob that ends pending or failed leaves
      // its destination empty.
      virtual Outcome Copy(const std::string &from, const std::string &to) = 0;

      // Gone, including when it was never there.
      virtual Outcome Remove(const std::string &key) = 0;

      // Every object whose key begins with prefix.
      virtual Outcome List(const std::string &prefix, std::vector<Listed> &objects) = 0;

      // Whether the bucket or container answers this server's credentials.
      virtual Outcome Probe() = 0;

      // Whether the client is leaving the store alone after a request that
      // ended Unavailable: every request answers Unavailable at once until the
      // pause is over (the object store's committer stops there).
      bool Paused() const;

      // What the last request on this thread that did not end Done or Absent
      // failed of, for a report.
      static String LastProblem();

      // For the copy verb: the ETag S3 gives an object put through Put with
      // these parts' MD5s - the MD5 of the whole for one part, the MD5 of the
      // parts' MD5s and their number for more.
      static std::string ExpectedS3Etag(const std::vector<std::string> &partMd5s, const std::string &wholeMd5);

   protected:

      MessageStoreObjectClient();

      // The retry, the backoff, the pause after a failure and the reports:
      // one request, built afresh for each try by build, which signs it.
      struct Answer
      {
         Answer() : status(0) {}
         int status;
         std::string headers;
         std::string body;
      };

      struct Request
      {
         Request() : max_response(1024 * 1024), transfer_bytes(0) {}
         AnsiString method;
         AnsiString url;
         std::vector<AnsiString> headers;
         AnsiString content_type;
         AnsiString body;
         size_t max_response;
         size_t transfer_bytes;     // what the answer is expected to carry, for the deadline
      };

      typedef std::function<bool(Request &request)> Builder;

      Outcome Send_(const Builder &build, const std::string &what, Answer &answer);

      // One window of an object: length bytes (never 0) from offset, held to
      // ifMatch when it is given; the store's own request, built by the
      // service, whose answer RangeAnswer_ reads.
      virtual Outcome ReadRange_(const std::string &key, unsigned __int64 offset, unsigned __int64 length, const std::string &ifMatch,
                                 std::string &bytes, unsigned __int64 &total, std::string &etag) = 0;

      // A ranged GET's answer: 206 and its Content-Range; 416, an offset at or
      // past the end; and a 200 - the range ignored by the store or a proxy on
      // the way - cut to the window it was asked for, so that bytes from the
      // start are never taken for bytes from the offset.
      Outcome RangeAnswer_(const Builder &build, const std::string &what, unsigned __int64 offset, unsigned __int64 length,
                           std::string &bytes, unsigned __int64 &total, std::string &etag);

      // A 404 is an absent object only while the bucket is there - S3 answers
      // a HEAD in a bucket that is not with the same 404 as a HEAD of a key
      // that is not - so it is taken for one after Probe has said so, within
      // the last minute; otherwise the store is unavailable.
      bool ConfirmBucket_();
      Outcome Absent_();

      // A 404 to a put, or to the bucket's own HEAD: the bucket or container
      // is not there, which every message waits out (HM7474, once a minute).
      Outcome NoBucket_(const std::string &bucket);

      static std::string HeaderOf_(const std::string &headers, const char *name);
      static void XmlValues_(const std::string &xml, const char *element, std::vector<std::string> &values);
      static std::string XmlUnescape_(const std::string &text);
      static void SetProblem_(const String &problem);

      String ca_file_;

   private:

      // Until when (milliseconds of the steady clock) every request answers
      // Unavailable without asking, and until when the bucket is taken to be
      // there.
      std::atomic<long long> paused_until_;
      std::atomic<long long> confirmed_until_;
   };
}
