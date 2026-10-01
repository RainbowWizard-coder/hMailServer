// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Response compression for the HTTP listeners. The policy - which answers are
// compressed, which never are, and why the floor and the ceiling are where they
// are - is written out in HttpCompression.cpp.

#pragma once

#include <map>
#include <mutex>
#include <string>

namespace HM
{
   struct HttpRequest;
   struct HttpResponse;

   class HttpCompression
   {
   public:
      // The content codings this server can produce. "deflate" is the zlib data
      // format of RFC 1950, which is what RFC 9110 section 8.4.1.2 means by the
      // name - not the bare DEFLATE stream that IMAP's COMPRESS extension
      // carries (DeflateStreams.cpp), which is the same algorithm without the
      // two-byte header and the checksum.
      enum Encoding
      {
         EncodingNone = 0,
         EncodingGzip,
         EncodingDeflate
      };

      // The coding to answer a client that sent this Accept-Encoding with, or
      // EncodingNone when it asked for neither. Honours the weights, including
      // the "q=0" that means "not this one", and the "*" that stands for
      // whatever the client did not name.
      static Encoding Negotiate(const AnsiString &acceptEncoding);

      // The coding's name, as Content-Encoding spells it.
      static const char *Token(Encoding encoding);

      // Whether a body of this media type is worth compressing at all. The
      // media type may carry its parameters; they are ignored.
      static bool IsWorthCompressing(const AnsiString &contentType);

      // Whether these extra headers say the answer is a download - a
      // Content-Disposition of attachment - which is never compressed and
      // carries no Vary, whatever its media type says.
      static bool IsDownload(const AnsiString &extraHeaders);

      // Compresses plain into compressed. False when zlib refused, and false
      // when the result was not smaller than what went in - in both cases
      // compressed is untouched and the caller serves the plain body.
      static bool Compress(const AnsiString &plain, Encoding encoding, AnsiString &compressed);

      // Inflates what Compress produced. Nothing on the serving path needs
      // this; it is here so that HttpCompressionTester can prove that what
      // goes out decodes to exactly what went in, rather than that it is
      // merely smaller.
      static bool Decompress(const AnsiString &compressed, Encoding encoding, AnsiString &plain);

      // The whole policy applied to one answer on its way to the wire: the Vary
      // header on every answer whose type this server would compress, and a
      // compressed body with its Content-Encoding on the answers that earn one.
      static void Apply(const HttpRequest &request, HttpResponse &response);

      // An answer shorter than this is served as it is. Below about a kilobyte
      // the eighteen bytes of gzip framing and the poorer ratio on a short
      // string eat most of the saving, and the answer was going out in one
      // segment either way.
      static const int MinimumBytes = 1024;

      // And an answer longer than this is served as it is too. Four megabytes
      // at level 6 is already something like a tenth of a second of one of four
      // worker threads, and the answers above it are message files and
      // downloads whose bytes are mostly base64 of something already
      // compressed. A body that large is held back rather than allowed to turn
      // a request into a unit of CPU somebody else chose.
      static const int MaximumBytes = 4 * 1024 * 1024;
   };

   // A body the server serves unchanged on every request - the webmail's
   // script, its S/MIME module, its manifest, its service worker. Joined from
   // its literal pieces once and compressed once, because the portal is served
   // no-store and without this the server would deflate the same 341 KB of
   // script for every cold page load.
   class HttpStaticBody
   {
   public:
      // Joins pieces - a null-terminated array of string literals, as
      // build/generate-portal-page.py writes them - and compresses the result,
      // on the first call only. Then fills the response with the plain body and
      // both compressed forms, for HttpCompression::Apply to choose between.
      void Fill(HttpResponse &response, const char *const *pieces);

   private:
      AnsiString plain_;
      AnsiString gzip_;
      AnsiString deflate_;
      std::once_flag once_;
   };

   // A body the server builds per request from parts that seldom change - the
   // webmail's page, into which the branding and the reader's language
   // catalogue are substituted; the Deck's page and its catalogues, read from
   // disk; the OpenAPI description - held compressed by its exact bytes.
   // Compressing one of these is about two milliseconds of a worker thread
   // (Portal.html at level 6), and the pages are served to anyone, so without
   // this a stranger sending Accept-Encoding could ask for that as often as
   // they liked; a body that is the same bytes as one served before costs a
   // lookup instead. Keyed by the bytes themselves, so a file replaced on disk
   // or a branding saved is a new entry, never a stale one; bounded, the least
   // recently used entry going when the capacity is reached. Only bodies
   // between HttpCompression's floor and ceiling are held.
   class HttpCompressedCache
   {
   public:
      explicit HttpCompressedCache(size_t capacity = 32);

      // Fills gzip_body and deflate_body for response.body, compressing on the
      // first sight of these bytes and remembering the result.
      void Fill(HttpResponse &response);

      // How many bodies are held. For the tester.
      size_t Size();

   private:
      struct Entry
      {
         AnsiString gzip;
         AnsiString deflate;
         unsigned long long used = 0;
      };

      std::mutex mutex_;
      std::map<std::string, Entry> entries_;
      size_t capacity_;
      unsigned long long tick_;
   };

   // The negotiation, the media-type policy and the round trip, as functions of
   // values. Run by ClassTester::DoTests, which the regression suite reaches
   // over COM (Utilities.RunTestSuite, from Infrastructure.MainOperations
   // .TestInternals), so these run in Release on the bench.
   class HttpCompressionTester
   {
   public:
      void Test();

   private:
      void TestNegotiation_();
      void TestMediaTypes_();
      void TestRoundTrip_();
      void TestThresholds_();
      void TestApply_();
      void TestCache_();
   };
}
