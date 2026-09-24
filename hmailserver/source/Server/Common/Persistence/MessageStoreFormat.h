// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The stored form of a message: plain RFC 822, or a container.
//
// A stored file is one or the other, per file, forever: nothing is migrated, and
// the reader tells them apart by the first eight bytes. A container is a header
// that says how to get the message back, then the message's bytes with its large
// parts cut out and put through the stages the header names, in order.
//
// THE HEADER, byte by byte (every number little-endian):
//
//    0   8   the magic, 89 48 4D 53 0D 0A 1A 0A ("\x89HMS\r\n\x1a\n"). The
//            first byte is above 0x7F, which no RFC 5322 header field begins
//            with, so no message is mistaken for a container; a message that
//            begins with the magic anyway is stored as a container with no
//            stages, so the test stays exact. The store only ever finds a
//            container it wrote itself: bytes from outside - a client's, a
//            remote server's, an importer's - that begin with the magic are
//            wrapped so, never taken for one (MessageStore.h).
//    8   1   the format's version, 1. A reader refuses a later one.
//    9   1   flags: bit 0 set when references follow.
//   10   2   reserved, 0.
//   12   4   the header's length: where the body begins.
//   16   8   the original message's size.
//   24  32   the SHA-256 of the original message.
//   56   1   the number of stages, then for each: its id (1 byte), the length of
//            its parameters (1 byte) and the parameters.
//    .   4   the number of references, then for each: the offset of the part's
//            bytes in the original (8), their length (8), and the SHA-256 of
//            those bytes (32), which is the blob's name. In offset order, never
//            overlapping.
//    .   .   zeros to the header's length.
//
// THE STAGES, by id. The body is the original with the referenced ranges cut
// out, put through each stage in the order listed; the reader undoes them in the
// reverse order. A stage this reader does not know refuses the file.
//
//    1   deflate (zlib 1.3.1's, zlib-wrapped so the adler32 is checked);
//        parameter: the level, one byte.
//    2   reserved for zstd.
//    3   reserved for the account-key encryption the "Mailbox encryption at
//        rest" row adds, run after compression.
//
// What the reader checks: the magic, the version, that the header's own
// lengths agree with each other and with the file, the stage ids, that the
// references lie in order inside the original, that each blob is there with
// the length and the hash its reference names, that the stages give back
// exactly the bytes the size says, and last that the SHA-256 of the whole is
// the one the header carries. A container that fails any of it is refused with
// the reason - never read as something else. A read of the header alone
// (DecodeStart) checks all of it but the blobs and the SHA-256 of the whole,
// which need every part: the deflate stream is read to its end, so damage to
// anything the container holds itself is refused there too.

#pragma once

#include <functional>
#include <string>

namespace HM
{
   // One part of a message held once, as a blob, outside the message's file.
   struct MessageStoreReference
   {
      MessageStoreReference() : offset(0), length(0) {}

      unsigned __int64 offset;   // where the part's bytes begin in the original
      unsigned __int64 length;   // how many
      AnsiString sha256;         // 64 lower-case hex digits: the blob's name
   };

   struct MessageStoreHeader
   {
      MessageStoreHeader() : version(0), flags(0), header_length(0), original_size(0) {}

      unsigned char version;
      unsigned char flags;
      unsigned int header_length;
      unsigned __int64 original_size;
      std::string sha256;                                         // 32 raw bytes
      std::vector<std::pair<unsigned char, std::string> > stages; // id, parameters
      std::vector<MessageStoreReference> references;
   };

   class MessageStoreFormat
   {
   public:

      enum StageId
      {
         StageDeflate = 1,
         StageZstd = 2,          // reserved
         StageAccountKey = 3,    // reserved: "Mailbox encryption at rest"
      };

      enum Constants
      {
         MagicLength = 8,
         FixedHeaderLength = 57,
         ReferenceLength = 48,
         Version = 1,
         FlagReferences = 1,
         MaximumStages = 8,
         MaximumReferences = 1024,
         DeflateLevel = 6,

         // 1024 references and eight stages come to well under 64 KB; a header
         // longer than this is damage, and is refused before it is read.
         MaximumHeaderLength = 1048576
      };

      // A container is decoded whole in memory; a larger message is stored as
      // it came.
      static const unsigned __int64 MaximumOriginalBytes = 64ULL * 1024 * 1024;

      // Does a file that begins with these bytes begin with the magic?
      static bool HasMagic(const char *bytes, size_t length);

      // How long the header of a container is, from its first 16 bytes; 0 when
      // they are not a container's.
      static unsigned int HeaderLengthOf(const char *bytes, size_t length);

      // The header, from the header's bytes. False, with the reason, for
      // anything this reader cannot read.
      static bool ParseHeader(const std::string &bytes, MessageStoreHeader &header, String &problem);

      // A container for the original bytes, with the given ranges cut out and
      // named, and the rest deflated when compress is set. An empty list of
      // references and no compression makes the container with no stages that
      // a message beginning with the magic is stored as.
      static bool Encode(const std::string &original, const std::vector<MessageStoreReference> &references, bool compress,
                         std::string &container, String &problem);

      // The original bytes from a whole container. blob reads a referenced
      // part by its reference, checking its length and hash itself.
      typedef std::function<bool(const MessageStoreReference &reference, std::string &bytes, String &problem)> BlobReader;
      static bool Decode(const std::string &container, const BlobReader &blob, std::string &original,
                         MessageStoreHeader &header, String &problem);

      // The first bytes of the original, up to maximum, from a whole container,
      // without reading a blob: the top-level header is always before the first
      // referenced part, so what the stages give back before it is the
      // original's. Every stage is undone to its end and checked - the deflate
      // stream must give exactly the bytes left once the parts are taken away,
      // end there with its Adler-32 right and leave nothing over - but only the
      // first bytes are kept. The blobs and the SHA-256 of the whole, which need
      // every part, are checked when the whole is read (Decode).
      static bool DecodeStart(const std::string &container, size_t maximum, std::string &start, String &problem);

      // The parts of a message worth holding once: every leaf of its MIME tree
      // whose body is at least minimumBytes decoded (base64 counted as the bytes
      // it decodes to), with the ranges as they are on the wire, so that putting
      // them back gives the message byte for byte. An empty list for 0.
      static void FindLargeParts(const std::string &message, unsigned __int64 minimumBytes, std::vector<MessageStoreReference> &parts);

      // SHA-256: 32 raw bytes, and as 64 lower-case hex digits.
      static std::string Sha256(const char *bytes, size_t length);
      static AnsiString Hex(const std::string &raw);

      // One stage by its id, both ways; false with the reason.
      static bool ApplyStage(unsigned char id, const std::string &parameters, const std::string &in, std::string &out, String &problem);
      static bool UndoStage(unsigned char id, const std::string &parameters, const std::string &in, size_t expected, std::string &out, String &problem);

      static void Test();
   };
}
