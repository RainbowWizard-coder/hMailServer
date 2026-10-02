// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "DeflateStreams.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

// zlib: on Windows the copy vendored under Server/zlib, which hMailServer.exe
// compiles in; on POSIX the distribution's, whose library CMakeLists.txt links,
// so the header and the library are one version (and the source tarball can
// leave Server/zlib out: build/source-exclusions.txt).
#ifdef HM_PLATFORM_POSIX
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   struct DeflateStreams::State
   {
      z_stream inflate;
      z_stream deflate;
      bool inflate_ready;
      bool deflate_ready;

      State() : inflate_ready(false), deflate_ready(false)
      {
         memset(&inflate, 0, sizeof(inflate));
         memset(&deflate, 0, sizeof(deflate));
      }
   };

   namespace
   {
      // A negative window size asks zlib for raw DEFLATE - no zlib header, no
      // adler32 trailer - which is what the wire carries.
      const int RAW_DEFLATE_WINDOW_BITS = -15;
      // Level 6 is zlib's own default: a fair trade of CPU for bytes for text that
      // is read once and never stored. Memory level 8 is the default too.
      const int COMPRESSION_LEVEL = 6;
      const int MEMORY_LEVEL = 8;

      String ZlibMessage_(const z_stream &stream, int code)
      {
         if (stream.msg)
            return String(stream.msg);
         String text;
         text.Format(_T("zlib error %d"), code);
         return text;
      }
   }

   DeflateStreams::DeflateStreams() :
      state_(new State()),
      started_(false)
   {
   }

   DeflateStreams::~DeflateStreams()
   {
      if (state_->inflate_ready)
         inflateEnd(&state_->inflate);
      if (state_->deflate_ready)
         deflateEnd(&state_->deflate);
      delete state_;
   }

   bool
   DeflateStreams::Start()
   {
      if (started_)
         return true;

      if (inflateInit2(&state_->inflate, RAW_DEFLATE_WINDOW_BITS) != Z_OK)
         return false;
      state_->inflate_ready = true;

      if (deflateInit2(&state_->deflate, COMPRESSION_LEVEL, Z_DEFLATED, RAW_DEFLATE_WINDOW_BITS, MEMORY_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
      {
         inflateEnd(&state_->inflate);
         state_->inflate_ready = false;
         return false;
      }
      state_->deflate_ready = true;

      started_ = true;
      return true;
   }

   bool
   DeflateStreams::Inflate(const char *compressed, size_t length, size_t output_limit, std::string &output,
                           size_t &consumed, bool &limit_reached, std::string &error)
   {
      consumed = 0;
      limit_reached = false;

      if (!started_)
      {
         error = "compression is not active";
         return false;
      }

      // zlib counts in uInt; what is offered beyond that is simply not consumed,
      // and the caller gives it again.
      const uInt offered = length > std::numeric_limits<uInt>::max() ? std::numeric_limits<uInt>::max() : (uInt) length;

      z_stream &stream = state_->inflate;
      stream.next_in = (Bytef *) compressed;
      stream.avail_in = offered;

      size_t produced_in_all = 0;
      char buffer[16384];
      for (;;)
      {
         // Until 6.3.6 there was no limit: one read of 64 KB of zeros inflated to
         // some 45 MB, all of it held for the connection at once.
         if (produced_in_all >= output_limit)
         {
            limit_reached = true;
            break;
         }

         const size_t room = std::min(sizeof(buffer), output_limit - produced_in_all);
         stream.next_out = (Bytef *) buffer;
         stream.avail_out = (uInt) room;

         int code = inflate(&stream, Z_SYNC_FLUSH);
         if (code != Z_OK && code != Z_BUF_ERROR && code != Z_STREAM_END)
         {
            error = std::string(AnsiString(ZlibMessage_(stream, code)).c_str());
            return false;
         }

         const size_t produced = room - stream.avail_out;
         if (produced > 0)
         {
            output.append(buffer, produced);
            produced_in_all += produced;
         }

         // Z_STREAM_END: the peer finished its stream, which RFC 4978 does not
         // provide for. Whatever follows is not DEFLATE; stop here rather than
         // misread it.
         if (code == Z_STREAM_END)
         {
            if (stream.avail_in > 0)
            {
               error = "the peer ended its DEFLATE stream and sent more";
               return false;
            }
            break;
         }

         // Room left over: zlib has used all the input it can and has nothing more
         // to give until more arrives, which is the normal state between segments.
         // A buffer filled to the last octet may have more behind it - a match
         // that runs on past it - even when the input is all consumed, so that is
         // asked again rather than left in zlib until the peer sends something.
         if (stream.avail_out != 0)
            break;
      }

      consumed = offered - stream.avail_in;
      return true;
   }

   bool
   DeflateStreams::Deflate(const char *plain, size_t length, std::string &output, std::string &error)
   {
      if (!started_)
      {
         error = "compression is not active";
         return false;
      }

      z_stream &stream = state_->deflate;
      stream.next_in = (Bytef *) plain;
      stream.avail_in = (uInt) length;

      char buffer[16384];
      // Z_SYNC_FLUSH until zlib reports it has nothing more to say: the flush
      // itself can need several output buffers for a large write.
      for (;;)
      {
         stream.next_out = (Bytef *) buffer;
         stream.avail_out = sizeof(buffer);

         int code = deflate(&stream, Z_SYNC_FLUSH);
         if (code != Z_OK && code != Z_BUF_ERROR)
         {
            error = std::string(AnsiString(ZlibMessage_(stream, code)).c_str());
            return false;
         }

         size_t produced = sizeof(buffer) - stream.avail_out;
         if (produced > 0)
            output.append(buffer, produced);

         if (stream.avail_out != 0)
            break;
      }

      return true;
   }

   // ---- the tester --------------------------------------------------------

   namespace
   {
      void ExpectInflation_(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("DeflateStreamsTester: " + message);
      }

      std::string Deflated_(const std::string &plain)
      {
         DeflateStreams writer;
         ExpectInflation_(writer.Start(), "zlib refused to start a deflater");

         std::string compressed;
         std::string error;
         ExpectInflation_(writer.Deflate(plain.data(), plain.size(), compressed, error), "Deflate failed: " + error);
         return compressed;
      }

      // Everything of compressed, inflated in calls of no more than output_limit plain
      // octets each, as TCPConnection inflates: what a call did not take is given
      // to the next, and a call the limit stopped is followed by another even
      // when no input is left, for what zlib still holds.
      std::string InflatedInPieces_(const std::string &compressed, size_t output_limit, size_t &calls)
      {
         DeflateStreams reader;
         ExpectInflation_(reader.Start(), "zlib refused to start an inflater");

         std::string whole;
         size_t offset = 0;
         calls = 0;

         for (;;)
         {
            std::string piece;
            size_t consumed = 0;
            bool limit_reached = false;
            std::string error;

            ExpectInflation_(reader.Inflate(compressed.data() + offset, compressed.size() - offset, output_limit, piece, consumed, limit_reached, error),
               "a piece did not inflate: " + error);
            ExpectInflation_(piece.size() <= output_limit,
               "a call made " + std::to_string(piece.size()) + " octets with a limit of " + std::to_string(output_limit));
            ExpectInflation_(consumed <= compressed.size() - offset, "a call took more octets than it was given");
            ExpectInflation_(limit_reached || consumed == compressed.size() - offset,
               "a call stopped short of its input with the limit not reached");

            whole += piece;
            offset += consumed;
            calls++;

            if (!limit_reached)
               break;

            ExpectInflation_(calls <= compressed.size() * 1100 / (output_limit > 0 ? output_limit : 1) + 16, "the pieces never came to an end");
         }

         return whole;
      }
   }

   void
   DeflateStreamsTester::Test()
   {
      // A round trip of two responses, with room to spare.
      {
         const std::string text = "* CAPABILITY IMAP4rev1 COMPRESS=DEFLATE\r\nA1 OK CAPABILITY completed\r\n";
         const std::string compressed = Deflated_(text);

         DeflateStreams reader;
         ExpectInflation_(reader.Start(), "zlib refused to start an inflater");

         std::string plain;
         size_t consumed = 0;
         bool limitReached = true;
         std::string error;
         ExpectInflation_(reader.Inflate(compressed.data(), compressed.size(), 1 << 20, plain, consumed, limitReached, error), "the round trip did not inflate: " + error);
         ExpectInflation_(plain == text, "the round trip came back as something else");
         ExpectInflation_(consumed == compressed.size() && !limitReached, "the round trip did not take all of its input");
      }

      // Four megabytes of one command line, which DEFLATE makes some hundreds of
      // times smaller: what one read of a mailbox user's COMPRESS session can
      // carry. Inflated with the limit TCPConnection uses - twice its receive
      // buffer's 250,000 octets - it comes out in pieces no larger than that, and
      // whole. Until 6.3.6 one call made all of it.
      {
         const size_t fourMegabytes = static_cast<size_t>(4) * 1024 * 1024;
         std::string lines;
         lines.reserve(fourMegabytes);
         while (lines.size() < fourMegabytes)
            lines += "a NOOP\r\n";

         const std::string compressed = Deflated_(lines);
         ExpectInflation_(compressed.size() * 100 < lines.size(),
            "four megabytes of one line deflated to " + std::to_string(compressed.size()) + " octets, which is not the ratio this test is about");

         size_t calls = 0;
         const std::string whole = InflatedInPieces_(compressed, 500000, calls);
         ExpectInflation_(whole == lines, "the pieces put together are not the lines that were deflated");
         ExpectInflation_(calls > lines.size() / 500000, "the lines came out in " + std::to_string(calls) + " calls, fewer than the limit allows");
      }

      // One octet at a time: the limit stops zlib inside a match, with every
      // input octet taken and the rest of the match still in zlib, which the
      // next call - given no input at all - has to bring out.
      {
         const std::string run = std::string(100000, 'A') + "\r\n";
         size_t calls = 0;
         const std::string whole = InflatedInPieces_(Deflated_(run), 1, calls);
         ExpectInflation_(whole == run, "a run inflated one octet at a time came back as something else");
      }

      // A limit of nothing makes nothing and takes nothing.
      {
         const std::string compressed = Deflated_("A1 NOOP\r\n");

         DeflateStreams reader;
         ExpectInflation_(reader.Start(), "zlib refused to start an inflater");

         std::string plain;
         size_t consumed = 1;
         bool limitReached = false;
         std::string error;
         ExpectInflation_(reader.Inflate(compressed.data(), compressed.size(), 0, plain, consumed, limitReached, error), "a limit of nothing failed: " + error);
         ExpectInflation_(plain.empty() && consumed == 0 && limitReached, "a limit of nothing made or took something");
      }

      // A block type DEFLATE does not have (11) is refused, not read on.
      {
         DeflateStreams reader;
         ExpectInflation_(reader.Start(), "zlib refused to start an inflater");

         const std::string corrupt("\xff\xff\xff\xff", 4);
         std::string plain;
         size_t consumed = 0;
         bool limitReached = false;
         std::string error;
         ExpectInflation_(!reader.Inflate(corrupt.data(), corrupt.size(), 1 << 20, plain, consumed, limitReached, error), "a corrupt stream was inflated");
         ExpectInflation_(!error.empty(), "a corrupt stream was refused without saying why");
      }
   }
}
