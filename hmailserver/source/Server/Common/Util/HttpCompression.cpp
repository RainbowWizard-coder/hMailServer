// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the HTTP listeners compress, and what they leave alone.
//
// Until this existed the server sent every byte of the webmail uncompressed:
// the page, 100 KB; its script, 341 KB; its S/MIME module, 60 KB - half a
// megabyte on a cold load, over a link the reader may be paying for by the
// megabyte, when four fifths of it is the same few thousand JavaScript keywords
// written over and over. zlib was already in the tree and already linked, for
// IMAP's COMPRESS extension (DeflateStreams.cpp); nothing was using it for
// HTTP.
//
// THE POLICY, in four rules.
//
// 1. Only when the client asked. Accept-Encoding is the whole of the
//    negotiation: a client that does not send one, or that weights gzip and
//    deflate at zero, gets exactly the bytes it always got. There is no
//    guessing from a User-Agent and no default-on for a client that stayed
//    silent.
//
// 2. Only what is worth compressing. The media types are a list, not a rule
//    with exceptions: text, JSON, XML, JavaScript, a manifest, a message file.
//    Anything not on the list - an image, a font, a PDF, a zip, a 7-Zip backup,
//    application/octet-stream - is served as it is, because its bytes are
//    already compressed and running them through DEFLATE spends CPU to make
//    them very slightly larger. A media type this list has never heard of
//    therefore defaults to "leave it alone", which is the safe direction.
//
// 3. Only between the floor and the ceiling. MinimumBytes and MaximumBytes,
//    both argued for where they are declared.
//
// 4. Never a download, and never what a handler said not to. An answer with
//    Content-Disposition: attachment is bytes to keep, not a document to
//    render, and its media type says what the file is called rather than what
//    is under it - a .eml is mostly base64 of whatever was attached, a .csv is
//    whatever the report held, and a part calling itself text/plain over
//    thirty megabytes of anything is not a reason to believe it. Apply leaves
//    every such answer alone, Vary and all, whatever the type says; and a
//    handler that knows better for any other reason sets
//    HttpResponse::no_compress, which this does not second-guess.
//
// ON BREACH, which is the reason a great many APIs refuse to compress
// anything, and the reason the REST listener compresses only a short list of
// its own documents. The attack needs three things: text the attacker chose
// and a secret in the same compressed answer; the victim's client fetching
// that answer again and again; and an observer of the size of the TLS records.
// It needs no CSRF - the request is the reader's own - so the session cookie's
// SameSite=Strict and the X-Requested-With header the mutations demand, which
// keep another origin from making a credentialed request, do not touch it.
// The mailbox API supplies all three: the attacker puts the text in a message
// they send (its subject, its sender, its first line), the folder listing
// lays that beside every other message's subject and sender in one JSON
// answer, and the webmail re-reads the open listing on its own whenever its
// change probe sees a new message - one message per guess, and the reader
// need do nothing. The same shape holds, at a slower pace, for anything built
// from the database: a message file, a report, an administrator's trace of a
// message, a calendar or address book synchronised by a client that polls.
//
// So the line is drawn at where the bytes come from, and it is drawn at one
// place: RestApiServer's CompressibleRoute_ names the answers that are the
// same for every reader - the webmail's page (branding and catalogue are
// admin-set and public), its script, its S/MIME module, its manifest, its
// service worker and its language catalogues, the Deck's page and catalogues,
// and the OpenAPI description - and marks everything else no_compress before
// it reaches Apply. A route added later is plain until it is named there. The
// web-services listener, whose answers are built from the database and small,
// has compression off altogether (HttpLimits::compression). What compresses
// therefore never holds a secret, and what holds a secret never compresses.
//
// ON CONTENT-LENGTH AND FRAMING. HttpServer::Serialize writes Content-Length
// from the body it is given, so replacing the body before it runs is all that
// is needed for the framing to be right; there is no chunking here and nothing
// to keep in step. Nothing in either listener serves a byte range or streams an
// answer in pieces - there is no Accept-Ranges, no Content-Range and no 206 -
// so there is no partial answer for a coding to be applied to half of.

#include "StdAfx.h"

#include "HttpCompression.h"
#include "HttpServer.h"

// zlib: on Windows the copy vendored under Server/zlib, which hMailServer.exe
// compiles in; on POSIX the distribution's, whose library CMakeLists.txt links,
// so the header and the library are one version (and the source tarball can
// leave Server/zlib out: build/source-exclusions.txt).
#ifdef HM_PLATFORM_POSIX
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif

#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Level 6 is zlib's own default and the level DeflateStreams uses: the
      // fair trade of CPU for bytes on text that is read once. Memory level 8
      // is the default too.
      const int CompressionLevel = 6;
      const int MemoryLevel = 8;

      // deflateInit2's window size carries the wrapper as well as the window:
      // 15 is the zlib header and its adler32, which is what Content-Encoding
      // calls "deflate"; 15 + 16 is the gzip header and its CRC-32, which is
      // what it calls "gzip".
      const int ZlibWindowBits = 15;
      const int GzipWindowBits = 15 + 16;

      // inflateInit2 takes 32 + 15 to mean "zlib or gzip, tell them apart from
      // the header"; the tester wants to be told which it got, so it asks for
      // one or the other.
      const int InflateZlibWindowBits = 15;
      const int InflateGzipWindowBits = 15 + 16;

      AnsiString ToLower(const AnsiString &value)
      {
         AnsiString result = value;
         result.MakeLower();
         return result;
      }

      AnsiString Trim(const AnsiString &value)
      {
         const char *text = value.c_str();
         size_t start = 0;
         size_t end = value.size();

         while (start < end && (text[start] == ' ' || text[start] == '\t'))
            start++;
         while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
            end--;

         return AnsiString(value.substr(start, end - start));
      }

      // An RFC 9110 qvalue - "1", "1.0", "0.85", "0.001" - as thousandths, so
      // that the comparison between two of them is integer arithmetic. A weight
      // this cannot read is taken as 1000, the same as no weight at all: a
      // client that wrote nonsense after the semicolon still asked for the
      // coding it named.
      int Quality(const AnsiString &text)
      {
         const AnsiString value = Trim(text);
         if (value.IsEmpty())
            return 1000;

         int index = 0;
         int whole = 0;
         while (index < value.GetLength() && value[index] >= '0' && value[index] <= '9')
         {
            whole = whole * 10 + (value[index] - '0');
            index++;
            if (whole > 1)
               return 1000;
         }

         if (index == 0)
            return 1000;

         int thousandths = whole * 1000;

         if (index < value.GetLength())
         {
            if (value[index] != '.')
               return 1000;
            index++;

            int place = 100;
            while (index < value.GetLength() && place > 0)
            {
               if (value[index] < '0' || value[index] > '9')
                  return 1000;
               thousandths += (value[index] - '0') * place;
               place /= 10;
               index++;
            }

            // More than three digits after the point is not a qvalue; the
            // client meant something by it, but not something this can read.
            if (index < value.GetLength())
               return 1000;
         }

         return thousandths > 1000 ? 1000 : thousandths;
      }

      // The media types worth compressing that are not caught by the "text/"
      // prefix or by a "+json" or "+xml" suffix.
      const char *const WorthCompressing[] =
      {
         "application/json",
         "application/javascript",
         "application/x-javascript",
         "application/ecmascript",
         "application/xml",
         "application/xml-dtd",
         "application/ld+json",
         "application/x-apple-aspen-config",
         "message/rfc822",
         "message/global",
         "message/delivery-status",
         "image/x-icon",
         "image/vnd.microsoft.icon",
         0
      };

      // Written the first time zlib refuses, and never again: a refusal here is
      // out of memory or a build whose zlib was configured differently, neither
      // of which improves for being logged a thousand times. Deliberately the
      // application log rather than the error log - the server has just served
      // the answer uncompressed, which is correct, so nothing has failed for
      // the client, and an ERROR line here would fail every regression fixture
      // that checks for a clean log.
      std::once_flag refusalLogged;

      void ReportZlibRefusal(const char *what)
      {
         std::call_once(refusalLogged, [what]()
         {
            LOG_APPLICATION(String(_T("HttpCompression: zlib refused to ")) + String(what) +
               _T(". Answers are being served uncompressed. This is logged once."));
         });
      }
   }

   HttpCompression::Encoding
   HttpCompression::Negotiate(const AnsiString &acceptEncoding)
   {
      if (acceptEncoding.IsEmpty())
         return EncodingNone;

      // -1 is "the client did not name this one"; anything from 0 up is the
      // weight it gave, and 0 is a refusal.
      int gzip = -1;
      int deflate = -1;
      int anything = -1;

      size_t position = 0;
      while (position <= acceptEncoding.size())
      {
         size_t comma = acceptEncoding.find(',', position);
         if (comma == std::string::npos)
            comma = acceptEncoding.size();

         const AnsiString element = AnsiString(acceptEncoding.substr(position, comma - position));
         position = comma + 1;

         // The coding is what stands before the first semicolon; the
         // parameters after it are weights, and a weight is all this reads.
         int semicolon = element.Find(";");
         const AnsiString coding = ToLower(Trim(semicolon >= 0 ? element.Mid(0, semicolon) : element));
         if (coding.IsEmpty())
            continue;

         int quality = 1000;
         if (semicolon >= 0)
         {
            AnsiString parameters = element.Mid(semicolon + 1);
            size_t at = 0;
            while (at <= parameters.size())
            {
               size_t next = parameters.find(';', at);
               if (next == std::string::npos)
                  next = parameters.size();

               const AnsiString parameter = ToLower(Trim(AnsiString(parameters.substr(at, next - at))));
               if (parameter.StartsWith("q="))
                  quality = Quality(parameter.Mid(2));

               at = next + 1;
            }
         }

         // x-gzip is gzip under the name Netscape gave it; RFC 9110 lists it as
         // a synonym and some clients still send it.
         if (coding == "gzip" || coding == "x-gzip")
            gzip = quality;
         else if (coding == "deflate")
            deflate = quality;
         else if (coding == "*")
            anything = quality;
      }

      const int forGzip = gzip >= 0 ? gzip : anything;
      const int forDeflate = deflate >= 0 ? deflate : anything;

      // gzip wins a tie. Every client that speaks either speaks gzip, and
      // "deflate" has a history of being sent as a bare DEFLATE stream by
      // servers that read RFC 2616 the other way, so the less-travelled of the
      // two codings is used only when the client asked for it by preference.
      if (forGzip > 0 && forGzip >= forDeflate)
         return EncodingGzip;
      if (forDeflate > 0)
         return EncodingDeflate;

      return EncodingNone;
   }

   const char *
   HttpCompression::Token(Encoding encoding)
   {
      switch (encoding)
      {
      case EncodingGzip: return "gzip";
      case EncodingDeflate: return "deflate";
      default: return "identity";
      }
   }

   bool
   HttpCompression::IsWorthCompressing(const AnsiString &contentType)
   {
      AnsiString type = contentType;

      const int semicolon = type.Find(";");
      if (semicolon >= 0)
         type = type.Mid(0, semicolon);

      type = ToLower(Trim(type));
      if (type.IsEmpty())
         return false;

      // Everything textual, whatever the subtype: HTML, plain, CSS, calendar,
      // vCard, the ACME challenge's one line.
      if (type.StartsWith("text/"))
         return true;

      // A structured syntax suffix says what the bytes are made of, whatever
      // the type is called: image/svg+xml, application/manifest+json,
      // application/problem+json, application/xhtml+xml.
      if (type.EndsWith("+json") || type.EndsWith("+xml"))
         return true;

      for (const char *const *named = WorthCompressing; *named; named++)
      {
         if (type == *named)
            return true;
      }

      return false;
   }

   bool
   HttpCompression::Compress(const AnsiString &plain, Encoding encoding, AnsiString &compressed)
   {
      if (encoding == EncodingNone || plain.IsEmpty())
         return false;

      z_stream stream;
      memset(&stream, 0, sizeof(stream));

      const int windowBits = encoding == EncodingGzip ? GzipWindowBits : ZlibWindowBits;
      if (deflateInit2(&stream, CompressionLevel, Z_DEFLATED, windowBits, MemoryLevel, Z_DEFAULT_STRATEGY) != Z_OK)
      {
         ReportZlibRefusal("start a compressor");
         return false;
      }

      // deflateBound is zlib's own answer to "how large can this get", for this
      // stream's wrapper and this input's length, so one Z_FINISH always has
      // room and there is no loop to get wrong.
      const uLong bound = deflateBound(&stream, (uLong) plain.GetLength());
      std::vector<char> buffer((size_t) bound + 1);

      stream.next_in = (Bytef *) plain.data();
      stream.avail_in = (uInt) plain.GetLength();
      stream.next_out = (Bytef *) &buffer[0];
      stream.avail_out = (uInt) buffer.size();

      const int code = deflate(&stream, Z_FINISH);
      const uLong produced = stream.total_out;
      deflateEnd(&stream);

      if (code != Z_STREAM_END)
      {
         ReportZlibRefusal("finish a compressed answer");
         return false;
      }

      // An answer that DEFLATE cannot shrink - a short one, or one that is
      // already compressed under a media type this list did not know about -
      // goes out as it was. Sending it larger and with a Content-Encoding on it
      // would cost the client a decompression for the privilege.
      if (produced >= (uLong) plain.GetLength())
         return false;

      compressed.assign(&buffer[0], (size_t) produced);
      return true;
   }

   bool
   HttpCompression::Decompress(const AnsiString &compressed, Encoding encoding, AnsiString &plain)
   {
      if (encoding == EncodingNone || compressed.IsEmpty())
         return false;

      z_stream stream;
      memset(&stream, 0, sizeof(stream));

      const int windowBits = encoding == EncodingGzip ? InflateGzipWindowBits : InflateZlibWindowBits;
      if (inflateInit2(&stream, windowBits) != Z_OK)
         return false;

      stream.next_in = (Bytef *) compressed.data();
      stream.avail_in = (uInt) compressed.GetLength();

      AnsiString result;
      char buffer[16384];
      int code = Z_OK;

      while (code != Z_STREAM_END)
      {
         stream.next_out = (Bytef *) buffer;
         stream.avail_out = sizeof(buffer);

         code = inflate(&stream, Z_NO_FLUSH);
         if (code != Z_OK && code != Z_STREAM_END)
         {
            inflateEnd(&stream);
            return false;
         }

         const size_t produced = sizeof(buffer) - stream.avail_out;
         if (produced > 0)
            result.append(buffer, produced);
         else if (code != Z_STREAM_END)
         {
            // Nothing produced and not finished: the stream ended early.
            inflateEnd(&stream);
            return false;
         }
      }

      inflateEnd(&stream);
      plain = result;
      return true;
   }

   bool
   HttpCompression::IsDownload(const AnsiString &extraHeaders)
   {
      const int at = extraHeaders.FindNoCase("Content-Disposition:");
      if (at < 0)
         return false;

      // The value up to the end of its line, lower-cased and trimmed: a
      // download says "attachment" first, and may say a filename after it.
      const int valueAt = at + (int) strlen("Content-Disposition:");
      int lineEnd = extraHeaders.Find("\r\n", valueAt);
      if (lineEnd < 0)
         lineEnd = extraHeaders.GetLength();

      const AnsiString value = ToLower(Trim(extraHeaders.Mid(valueAt, lineEnd - valueAt)));
      return value.StartsWith("attachment");
   }

   void
   HttpCompression::Apply(const HttpRequest &request, HttpResponse &response)
   {
      if (response.no_compress)
         return;

      // Rule 4: a download goes out as it is, without a Vary, since nothing
      // about it varies on what the client can decode. The routes that serve
      // one say so themselves as well (no_compress); this is the rule that
      // holds for a route written later that forgets to.
      if (IsDownload(response.extra_headers))
         return;

      if (!IsWorthCompressing(response.content_type))
         return;

      // The Vary goes on every answer whose type this server would compress,
      // not only on the ones it did: a cache between here and the reader that
      // kept the plain form of an answer must not hand it to the next client
      // under a key that ignores what that client can decode, and the reverse
      // mistake - handing a gzip body to a client that asked for none - is the
      // one that shows as a page of mojibake.
      if (response.extra_headers.FindNoCase("Vary:") < 0)
         response.extra_headers += "Vary: Accept-Encoding\r\n";

      // A handler that encoded its own body has already said what the coding
      // is, and nothing here is going to encode it twice.
      if (response.extra_headers.FindNoCase("Content-Encoding:") >= 0)
         return;

      const Encoding encoding = Negotiate(request.Header("accept-encoding"));
      if (encoding == EncodingNone)
         return;

      const AnsiString &cached = encoding == EncodingGzip ? response.gzip_body : response.deflate_body;
      if (!cached.IsEmpty())
      {
         response.body = cached;
      }
      else
      {
         if (response.body.GetLength() < MinimumBytes || response.body.GetLength() > MaximumBytes)
            return;

         AnsiString squeezed;
         if (!Compress(response.body, encoding, squeezed))
            return;

         response.body = squeezed;
      }

      response.extra_headers += AnsiString("Content-Encoding: ") + Token(encoding) + "\r\n";
   }

   // ------------------------------------------------------------------------

   void
   HttpStaticBody::Fill(HttpResponse &response, const char *const *pieces)
   {
      std::call_once(once_, [this, pieces]()
      {
         for (const char *const *piece = pieces; *piece; piece++)
            plain_ += *piece;

         if (plain_.GetLength() >= HttpCompression::MinimumBytes &&
             plain_.GetLength() <= HttpCompression::MaximumBytes)
         {
            if (!HttpCompression::Compress(plain_, HttpCompression::EncodingGzip, gzip_))
               gzip_.Empty();
            if (!HttpCompression::Compress(plain_, HttpCompression::EncodingDeflate, deflate_))
               deflate_.Empty();
         }
      });

      response.body = plain_;
      response.gzip_body = gzip_;
      response.deflate_body = deflate_;
   }

   // ------------------------------------------------------------------------

   HttpCompressedCache::HttpCompressedCache(size_t capacity) :
      capacity_(capacity == 0 ? 1 : capacity),
      tick_(0)
   {
   }

   void
   HttpCompressedCache::Fill(HttpResponse &response)
   {
      if (response.body.GetLength() < HttpCompression::MinimumBytes ||
          response.body.GetLength() > HttpCompression::MaximumBytes)
         return;

      const std::string key(response.body.c_str(), response.body.size());

      {
         std::lock_guard<std::mutex> guard(mutex_);
         std::map<std::string, Entry>::iterator found = entries_.find(key);
         if (found != entries_.end())
         {
            found->second.used = ++tick_;
            response.gzip_body = found->second.gzip;
            response.deflate_body = found->second.deflate;
            return;
         }
      }

      // Compressed outside the lock: two threads meeting the same new body at
      // once each compress it, which is the cost of one cold request twice,
      // and the second to arrive merely overwrites the first's entry with the
      // same bytes.
      Entry entry;
      if (!HttpCompression::Compress(response.body, HttpCompression::EncodingGzip, entry.gzip))
         entry.gzip.Empty();
      if (!HttpCompression::Compress(response.body, HttpCompression::EncodingDeflate, entry.deflate))
         entry.deflate.Empty();

      response.gzip_body = entry.gzip;
      response.deflate_body = entry.deflate;

      std::lock_guard<std::mutex> guard(mutex_);
      entry.used = ++tick_;

      if (entries_.size() >= capacity_ && entries_.find(key) == entries_.end())
      {
         std::map<std::string, Entry>::iterator oldest = entries_.begin();
         for (std::map<std::string, Entry>::iterator it = entries_.begin(); it != entries_.end(); ++it)
         {
            if (it->second.used < oldest->second.used)
               oldest = it;
         }
         entries_.erase(oldest);
      }

      entries_[key] = entry;
   }

   size_t
   HttpCompressedCache::Size()
   {
      std::lock_guard<std::mutex> guard(mutex_);
      return entries_.size();
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   void
   HttpCompressionTester::Test()
   {
      TestNegotiation_();
      TestMediaTypes_();
      TestRoundTrip_();
      TestThresholds_();
      TestApply_();
      TestCache_();
   }

   void
   HttpCompressionTester::TestNegotiation_()
   {
      struct Case
      {
         const char *header;
         HttpCompression::Encoding expected;
      };

      const Case cases[] =
      {
         { "", HttpCompression::EncodingNone },
         { "identity", HttpCompression::EncodingNone },
         { "br", HttpCompression::EncodingNone },
         { "gzip", HttpCompression::EncodingGzip },
         { "GZIP", HttpCompression::EncodingGzip },
         { "x-gzip", HttpCompression::EncodingGzip },
         { "deflate", HttpCompression::EncodingDeflate },
         { "gzip, deflate", HttpCompression::EncodingGzip },
         { "deflate, gzip", HttpCompression::EncodingGzip },
         { "gzip, deflate, br", HttpCompression::EncodingGzip },
         { " gzip ;q=1.0 , deflate ", HttpCompression::EncodingGzip },
         // A weight of zero is a refusal, which is the whole point of having
         // weights: "anything but gzip".
         { "gzip;q=0, deflate", HttpCompression::EncodingDeflate },
         { "gzip;q=0.0, deflate;q=0.001", HttpCompression::EncodingDeflate },
         { "gzip;q=0, deflate;q=0", HttpCompression::EncodingNone },
         { "gzip;q=0.5, deflate;q=0.9", HttpCompression::EncodingDeflate },
         { "gzip;q=0.9, deflate;q=0.5", HttpCompression::EncodingGzip },
         // The star stands for what was not named, weight and all.
         { "*", HttpCompression::EncodingGzip },
         { "*;q=0", HttpCompression::EncodingNone },
         { "gzip;q=0, *", HttpCompression::EncodingDeflate },
         { "identity;q=0, *;q=0", HttpCompression::EncodingNone },
         // A malformed weight is not a refusal: the coding was still named.
         { "gzip;q=wrong", HttpCompression::EncodingGzip },
         { 0, HttpCompression::EncodingNone }
      };

      for (const Case *test = cases; test->header; test++)
      {
         if (HttpCompression::Negotiate(test->header) != test->expected)
         {
            throw std::logic_error(std::string("HttpCompression::Negotiate read \"") + test->header +
               "\" as the wrong content coding.");
         }
      }
   }

   void
   HttpCompressionTester::TestMediaTypes_()
   {
      const char *const worth[] =
      {
         "text/html; charset=utf-8",
         "text/javascript; charset=utf-8",
         "text/plain",
         "text/calendar",
         "TEXT/HTML",
         "application/json",
         "application/json; charset=utf-8",
         "application/manifest+json",
         "application/xhtml+xml",
         "image/svg+xml",
         "text/xml",
         "message/rfc822",
         0
      };

      const char *const notWorth[] =
      {
         "",
         "application/octet-stream",
         "image/png",
         "image/jpeg",
         "application/pdf",
         "application/zip",
         "application/gzip",
         "application/x-7z-compressed",
         "application/epub+zip",
         "video/mp4",
         "audio/mpeg",
         "font/woff2",
         0
      };

      for (const char *const *type = worth; *type; type++)
      {
         if (!HttpCompression::IsWorthCompressing(*type))
            throw std::logic_error(std::string("HttpCompression::IsWorthCompressing refused ") + *type + ", which is text.");
      }

      for (const char *const *type = notWorth; *type; type++)
      {
         if (HttpCompression::IsWorthCompressing(*type))
         {
            throw std::logic_error(std::string("HttpCompression::IsWorthCompressing accepted ") + *type +
               ", whose bytes are already compressed.");
         }
      }
   }

   void
   HttpCompressionTester::TestRoundTrip_()
   {
      // Something long enough to be worth compressing and repetitive enough to
      // compress well, with every byte value in it: a body is bytes, not text,
      // and a coding that lost a NUL or a high byte would be found here rather
      // than by a reader whose page came out truncated.
      AnsiString plain;
      for (int repeat = 0; repeat < 64; repeat++)
      {
         plain += "function draw(node) { return node.parentNode ? draw(node.parentNode) : node; }\r\n";
         for (int byte = 0; byte < 256; byte++)
            plain += (char) byte;
      }

      const HttpCompression::Encoding codings[] = { HttpCompression::EncodingGzip, HttpCompression::EncodingDeflate };

      for (int index = 0; index < 2; index++)
      {
         AnsiString compressed;
         if (!HttpCompression::Compress(plain, codings[index], compressed))
         {
            throw std::logic_error(std::string("HttpCompression::Compress refused a compressible body as ") +
               HttpCompression::Token(codings[index]) + ".");
         }

         if (compressed.GetLength() >= plain.GetLength())
         {
            throw std::logic_error(std::string("HttpCompression::Compress made a body no smaller as ") +
               HttpCompression::Token(codings[index]) + ".");
         }

         // gzip starts 1f 8b 08; a zlib stream starts 78 and a header whose two
         // bytes are a multiple of 31. Reading the first bytes is how a client
         // that trusted the wrong Content-Encoding would fail, so they are
         // checked rather than assumed.
         if (codings[index] == HttpCompression::EncodingGzip)
         {
            if (compressed.GetLength() < 3 ||
                (unsigned char) compressed[0] != 0x1f || (unsigned char) compressed[1] != 0x8b ||
                (unsigned char) compressed[2] != 0x08)
            {
               throw std::logic_error("HttpCompression::Compress did not put a gzip header on a gzip body.");
            }
         }
         else
         {
            const unsigned first = (unsigned char) compressed[0];
            const unsigned second = (unsigned char) compressed[1];
            if ((first & 0x0f) != 8 || ((first << 8) + second) % 31 != 0)
               throw std::logic_error("HttpCompression::Compress did not put a zlib header on a deflate body.");
         }

         AnsiString back;
         if (!HttpCompression::Decompress(compressed, codings[index], back))
         {
            throw std::logic_error(std::string("A body compressed as ") + HttpCompression::Token(codings[index]) +
               " could not be decompressed again.");
         }

         if (back.GetLength() != plain.GetLength() || memcmp(back.data(), plain.data(), plain.GetLength()) != 0)
         {
            throw std::logic_error(std::string("A body compressed as ") + HttpCompression::Token(codings[index]) +
               " did not decompress to exactly what went in.");
         }

         // And the two codings are not interchangeable: a client told "gzip"
         // must be reading a gzip stream.
         AnsiString wrong;
         const HttpCompression::Encoding other = codings[index] == HttpCompression::EncodingGzip
            ? HttpCompression::EncodingDeflate : HttpCompression::EncodingGzip;
         if (HttpCompression::Decompress(compressed, other, wrong))
         {
            throw std::logic_error(std::string("A body compressed as ") + HttpCompression::Token(codings[index]) +
               " was also readable as the other coding, so the wrapper is not what it says.");
         }
      }
   }

   void
   HttpCompressionTester::TestThresholds_()
   {
      // Random-looking bytes DEFLATE cannot shrink: Compress must say so rather
      // than hand back something longer than it was given.
      AnsiString incompressible;
      unsigned state = 2463534242u;
      for (int i = 0; i < 4096; i++)
      {
         state ^= state << 13;
         state ^= state >> 17;
         state ^= state << 5;
         incompressible += (char) (state & 0xff);
      }

      AnsiString compressed;
      if (HttpCompression::Compress(incompressible, HttpCompression::EncodingGzip, compressed))
         throw std::logic_error("HttpCompression::Compress claimed to shrink bytes that DEFLATE cannot shrink.");

      if (!compressed.IsEmpty())
         throw std::logic_error("HttpCompression::Compress wrote to its output after refusing.");

      if (HttpCompression::Compress("", HttpCompression::EncodingGzip, compressed))
         throw std::logic_error("HttpCompression::Compress accepted an empty body.");

      if (HttpCompression::Compress("anything", HttpCompression::EncodingNone, compressed))
         throw std::logic_error("HttpCompression::Compress accepted EncodingNone.");
   }

   namespace
   {
      // A request that offers gzip, as HttpServer would have parsed it.
      HttpRequest RequestOffering(const char *acceptEncoding)
      {
         HttpRequest request;
         request.method = "GET";
         request.target = "/";
         if (acceptEncoding)
            request.headers.push_back(std::make_pair(AnsiString("accept-encoding"), AnsiString(acceptEncoding)));
         return request;
      }

      // Four kilobytes of compressible text.
      AnsiString Prose()
      {
         AnsiString text;
         while (text.GetLength() < 4096)
            text += "The quick brown fox jumps over the lazy dog. ";
         return text;
      }

      HttpResponse TextAnswer(const AnsiString &extraHeaders)
      {
         HttpResponse response;
         response.status = 200;
         response.content_type = "text/plain; charset=utf-8";
         response.body = Prose();
         response.extra_headers = extraHeaders;
         return response;
      }
   }

   void
   HttpCompressionTester::TestApply_()
   {
      const AnsiString plain = Prose();

      // The ordinary case: a text answer to a client offering gzip goes out
      // as gzip, with the Vary, and decodes to what went in.
      {
         HttpResponse response = TextAnswer("X-Content-Type-Options: nosniff\r\n");
         HttpCompression::Apply(RequestOffering("gzip, deflate"), response);
         if (response.extra_headers.Find("Content-Encoding: gzip\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply did not encode a text answer for a client offering gzip.");
         if (response.extra_headers.Find("Vary: Accept-Encoding\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply did not add Vary to a compressed answer.");
         if (response.extra_headers.Find("X-Content-Type-Options: nosniff\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply lost a header the handler set.");
         AnsiString decoded;
         if (!HttpCompression::Decompress(response.body, HttpCompression::EncodingGzip, decoded) || decoded != plain)
            throw std::logic_error("HttpCompression::Apply's gzip body does not decode to the plain answer.");
      }

      // No offer: the same answer, untouched but for the Vary.
      {
         HttpResponse response = TextAnswer("");
         HttpCompression::Apply(RequestOffering(0), response);
         if (response.body != plain || response.extra_headers.Find("Content-Encoding:") >= 0)
            throw std::logic_error("HttpCompression::Apply encoded an answer for a client that offered nothing.");
         if (response.extra_headers.Find("Vary: Accept-Encoding\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply did not add Vary to a plain answer of a compressible type.");
      }

      // Rule 4: a download is left alone, Vary and all, whatever its type.
      {
         HttpResponse response = TextAnswer("Content-Disposition: attachment; filename=\"prose.txt\"\r\nCache-Control: no-store\r\n");
         HttpCompression::Apply(RequestOffering("gzip, deflate"), response);
         if (response.body != plain || response.extra_headers.Find("Content-Encoding:") >= 0)
            throw std::logic_error("HttpCompression::Apply compressed a download.");
         if (response.extra_headers.Find("Vary:") >= 0)
            throw std::logic_error("HttpCompression::Apply put a Vary on a download.");
      }

      // ... but an inline disposition is a document to render, and is not a
      // download.
      {
         HttpResponse response = TextAnswer("content-disposition: inline\r\n");
         HttpCompression::Apply(RequestOffering("gzip"), response);
         if (response.extra_headers.Find("Content-Encoding: gzip\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply took an inline disposition for a download.");
      }

      // And a handler's no_compress is final.
      {
         HttpResponse response = TextAnswer("");
         response.no_compress = true;
         HttpCompression::Apply(RequestOffering("gzip"), response);
         if (response.body != plain || response.extra_headers.Find("Content-Encoding:") >= 0 ||
             response.extra_headers.Find("Vary:") >= 0)
            throw std::logic_error("HttpCompression::Apply touched an answer marked no_compress.");
      }

      // A type not worth compressing is left alone without a Vary.
      {
         HttpResponse response = TextAnswer("");
         response.content_type = "image/png";
         HttpCompression::Apply(RequestOffering("gzip"), response);
         if (response.body != plain || response.extra_headers.Find("Vary:") >= 0 ||
             response.extra_headers.Find("Content-Encoding:") >= 0)
            throw std::logic_error("HttpCompression::Apply touched an answer whose type is not worth compressing.");
      }

      // Below the floor: the Vary, and nothing else.
      {
         HttpResponse response = TextAnswer("");
         response.body = "short";
         HttpCompression::Apply(RequestOffering("gzip"), response);
         if (response.body != "short" || response.extra_headers.Find("Content-Encoding:") >= 0)
            throw std::logic_error("HttpCompression::Apply compressed an answer under the floor.");
         if (response.extra_headers.Find("Vary: Accept-Encoding\r\n") < 0)
            throw std::logic_error("HttpCompression::Apply left the Vary off an answer under the floor.");
      }
   }

   void
   HttpCompressionTester::TestCache_()
   {
      HttpCompressedCache cache(2);
      const AnsiString plain = Prose();

      // First sight compresses and remembers; Apply then serves the held form,
      // which decodes to the body.
      HttpResponse first = TextAnswer("");
      cache.Fill(first);
      if (first.gzip_body.IsEmpty() || first.deflate_body.IsEmpty())
         throw std::logic_error("HttpCompressedCache::Fill did not compress a new body.");
      if (cache.Size() != 1)
         throw std::logic_error("HttpCompressedCache did not remember a new body.");

      HttpCompression::Apply(RequestOffering("gzip"), first);
      AnsiString decoded;
      if (!HttpCompression::Decompress(first.body, HttpCompression::EncodingGzip, decoded) || decoded != plain)
         throw std::logic_error("HttpCompressedCache's gzip form does not decode to the body it was filled from.");

      HttpResponse again = TextAnswer("");
      cache.Fill(again);
      if (again.gzip_body != first.gzip_body || cache.Size() != 1)
         throw std::logic_error("HttpCompressedCache did not recognise the same bytes.");

      HttpResponse deflated = TextAnswer("");
      cache.Fill(deflated);
      HttpCompression::Apply(RequestOffering("deflate"), deflated);
      if (!HttpCompression::Decompress(deflated.body, HttpCompression::EncodingDeflate, decoded) || decoded != plain)
         throw std::logic_error("HttpCompressedCache's deflate form does not decode to the body it was filled from.");

      // The capacity holds: a third distinct body evicts the least recently
      // used of the two before it.
      HttpResponse second = TextAnswer("");
      second.body = plain + " Second.";
      cache.Fill(second);
      HttpResponse third = TextAnswer("");
      third.body = plain + " Third.";
      cache.Fill(third);
      if (cache.Size() != 2)
         throw std::logic_error("HttpCompressedCache grew past its capacity.");

      // A body under the floor is neither compressed nor remembered.
      HttpResponse tiny = TextAnswer("");
      tiny.body = "short";
      cache.Fill(tiny);
      if (!tiny.gzip_body.IsEmpty() || cache.Size() != 2)
         throw std::logic_error("HttpCompressedCache held a body under the floor.");
   }
}
