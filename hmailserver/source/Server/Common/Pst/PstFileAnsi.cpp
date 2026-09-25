// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The ANSI file's NDB layer (see PstFile.h): the file Outlook 97, 98, 2000 and
// 2002 wrote, wVer 14 or 15, which [MS-PST] specifies beside the Unicode one
// field for field. What differs, and nothing else does - the LTP layer of heaps,
// property contexts and tables above it is the same:
//
//   - The header ([MS-PST] 2.2.2.6) is 512 bytes: bidNextB, bidNextP and
//     dwUnique of four bytes each from offset 24, rgnid at 36, the ROOT at 164
//     (dwReserved, ibFileEof, ibAMapLast, cbAMapFree, cbPMapFree, BREFNBT and
//     BREFBBT, every one four bytes wide, a BREF being a BID and an IB),
//     bSentinel at 460 and bCryptMethod at 461. It has one CRC, dwCRCPartial,
//     over the 471 bytes from wMagicClient, as a Unicode header does; there is
//     no dwCRCFull.
//   - BIDs, IBs and NIDs are four bytes (2.2.2.2, 2.2.2.3, 2.2.2.4).
//   - A B-tree page (2.2.2.7) holds 496 bytes of entries, then cEnt, cEntMax,
//     cbEnt and cLevel, then a 12-byte trailer: ptype, ptypeRepeat, wSig, bid,
//     dwCRC - the BID before the CRC, the other way round from Unicode. The
//     CRC covers the 500 bytes before the trailer. A BTENTRY is 12 bytes
//     (btkey, BREF), a BBTENTRY 12 (BREF, cb, cRef) and an NBTENTRY 16 (nid,
//     bidData, bidSub, nidParent).
//   - A block's trailer (2.2.2.8.1) is 12 bytes: cb, wSig, bid, dwCRC; a block
//     holds at most 8,180 bytes, and is laid out to a multiple of 64 as in a
//     Unicode file.
//   - An XBLOCK or XXBLOCK (2.2.2.8.3.2) lists BIDs of four bytes after the same
//     eight-byte head; an SLBLOCK or SIBLOCK (2.2.2.8.3.3) has no dwPadding
//     after cEnt, and its SLENTRY is 12 bytes (nid, bidData, bidSub) and its
//     SIENTRY 8 (nid, bid).
//   - Data blocks are encoded as in a Unicode file - NDB_CRYPT_PERMUTE, or
//     NDB_CRYPT_CYCLIC keyed by the BID (which in an ANSI file is all of it).
//
// Every rule the Unicode walks keep is kept here the same way: every read
// bounded by the file and by the block it is in, a depth cap and a visited set
// on every tree, a size checked against the caller's limit before anything is
// read, a damaged block that block's failure, and every page, block and
// subnode entry charged to the file's work budget (PstFormat.h) before it is
// read, so that the budget's limits and the job's stop reach an ANSI file as
// they reach a Unicode one.

#include "StdAfx.h"

#include "PstFile.h"

#include <cstdio>

namespace HM
{
   const size_t PstFile::MaximumAnsiBlockData;

   namespace
   {
      const size_t AnsiHeaderSize = 512;
      const size_t AnsiOffsetEndOfFile = 168;
      const size_t AnsiOffsetNbt = 184;
      const size_t AnsiOffsetBbt = 192;
      const size_t AnsiOffsetCryptMethod = 461;

      const size_t AnsiPageSize = 512;
      const size_t AnsiPageEntryBytes = 496;
      const size_t AnsiPageTrailer = 500;
      const uint8_t AnsiPageTypeBbt = 0x80;
      const uint8_t AnsiPageTypeNbt = 0x81;

      const size_t AnsiNbtLeafEntrySize = 16;
      const size_t AnsiBbtLeafEntrySize = 12;
      const size_t AnsiIntermediateEntrySize = 12;
      const size_t AnsiBlockTrailerSize = 12;

      std::string AnsiNumber(uint64_t value)
      {
         char text[32];
         snprintf(text, sizeof(text), "%llu", (unsigned long long) value);
         return text;
      }

      std::string AnsiHex(uint64_t value)
      {
         char text[32];
         snprintf(text, sizeof(text), "0x%llx", (unsigned long long) value);
         return text;
      }
   }

   std::string
   PstFile::ProtectedSentence()
   {
      return "This PST is encrypted with Windows Information Protection, under a key that only the Windows account and device that "
             "protected it can use, so no server can decrypt it; open it in Outlook on that computer, as that account, and export "
             "its folders to a new PST saved as a personal file, with the protection removed.";
   }

   std::string
   PstFile::FourKSentence()
   {
      return "This file is in the 4K-page layout (version 36) that Outlook 2013 and later write for an OST - Outlook's offline copy "
             "of a mailbox that lives on a server - with 4 KB pages and compressed blocks that Microsoft's published PST format "
             "([MS-PST]) does not describe, so this reader, written from that specification alone, cannot read it; migrate the "
             "mailbox from its server with a migration job, or export its folders to a PST in Outlook.";
   }

   void
   PstFile::OpenAnsi_(const std::string &header)
   {
      const int version = PstFormat::U16(header, 10, "the header");
      const int crypt = PstFormat::U8(header, AnsiOffsetCryptMethod, "the header");

      if (crypt == 0x10)
         throw PstRefusal(PstRefusal::ReasonProtected, ProtectedSentence());

      if (crypt != 0 && crypt != 1 && crypt != 2)
      {
         throw PstRefusal(PstRefusal::ReasonUnknownFormat,
            "This PST's format (version " + AnsiNumber((uint64_t) version) + ", encoding " + AnsiNumber((uint64_t) crypt) +
            ") is not one this reader knows; open it in a current Outlook and export a new PST.");
      }

      // dwCRCPartial covers the 471 bytes from wMagicClient, as in a Unicode
      // header; an ANSI header has no dwCRCFull.
      const uint32_t partial = PstFormat::U32(header, 4, "the header");
      if (!PstFormat::Fits(8, 471, header.size()) || PstFormat::Crc((const unsigned char *) header.data() + 8, 471) != partial)
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file's header is damaged (its checksum does not match), so nothing in it can be trusted; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and try again.");
      }

      const uint64_t endOfFile = PstFormat::U32(header, AnsiOffsetEndOfFile, "the header");
      if (endOfFile > source_->Size() || endOfFile < AnsiHeaderSize)
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file is " + AnsiNumber(source_->Size()) + " bytes and its header says " + AnsiNumber(endOfFile) +
            ": it was cut short, most likely while it was copied; copy it again.");
      }

      version_ = version;
      crypt_method_ = crypt;
      end_of_file_ = endOfFile;
      nbt_bid_ = PstFormat::U32(header, AnsiOffsetNbt, "the header");
      nbt_ib_ = PstFormat::U32(header, AnsiOffsetNbt + 4, "the header");
      bbt_bid_ = PstFormat::U32(header, AnsiOffsetBbt, "the header");
      bbt_ib_ = PstFormat::U32(header, AnsiOffsetBbt + 4, "the header");

      if (!PstFormat::Fits(nbt_ib_, AnsiPageSize, end_of_file_) || !PstFormat::Fits(bbt_ib_, AnsiPageSize, end_of_file_))
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file's header points past its end, so nothing in it can be found; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and try again.");
      }

      ansi_ = true;
      open_ = true;
   }

   std::string
   PstFile::PageAnsi_(uint64_t ib, uint64_t bid, uint8_t pageType)
   {
      budget_.Item();
      budget_.Bytes(AnsiPageSize);

      std::string page;
      if (!PstFormat::Fits(ib, AnsiPageSize, end_of_file_) || !source_->Read(ib, AnsiPageSize, page))
         throw PstError("A B-tree page at " + AnsiHex(ib) + " lies past the end of the file.");

      // [MS-PST] 2.2.2.7.1, ANSI: ptype, ptypeRepeat, wSig, bid, dwCRC.
      const uint8_t type = PstFormat::U8(page, AnsiPageTrailer, "a page trailer");
      const uint8_t repeat = PstFormat::U8(page, AnsiPageTrailer + 1, "a page trailer");
      const uint32_t trailerBid = PstFormat::U32(page, AnsiPageTrailer + 4, "a page trailer");
      const uint32_t crc = PstFormat::U32(page, AnsiPageTrailer + 8, "a page trailer");

      if (type != pageType || repeat != pageType)
         throw PstError("The B-tree page at " + AnsiHex(ib) + " is not the kind of page that should be there.");

      if (PstFormat::Crc((const unsigned char *) page.data(), AnsiPageTrailer) != crc)
         throw PstError("The B-tree page at " + AnsiHex(ib) + " is damaged (its checksum does not match).");

      if ((uint64_t) trailerBid != bid)
         throw PstError("The B-tree page at " + AnsiHex(ib) + " is not the page its parent names.");

      return page;
   }

   bool
   PstFile::SearchAnsi_(uint64_t rootBid, uint64_t rootIb, uint8_t pageType, uint64_t key, size_t leafEntrySize, std::string &entry)
   {
      if (!open_)
         throw PstError("The file is not open.");

      const bool nodes = pageType == AnsiPageTypeNbt;
      uint64_t bid = rootBid;
      uint64_t ib = rootIb;
      int expectedLevel = -1;
      std::set<uint64_t> visited;

      for (int depth = 0; depth < MaximumTreeDepth; depth++)
      {
         if (!visited.insert(ib).second)
            throw PstError("A B-tree of the file points back at one of its own pages.");

         const std::string page = PageAnsi_(ib, bid, pageType);
         const size_t count = PstFormat::U8(page, AnsiPageEntryBytes, "a B-tree page");
         const size_t entrySize = PstFormat::U8(page, AnsiPageEntryBytes + 2, "a B-tree page");
         const int level = PstFormat::U8(page, AnsiPageEntryBytes + 3, "a B-tree page");

         if (expectedLevel >= 0 && level != expectedLevel)
            throw PstError("A B-tree page of the file is at the wrong level.");

         if (level == 0)
         {
            if (entrySize != leafEntrySize || count * entrySize > AnsiPageEntryBytes)
               throw PstError("A B-tree leaf of the file has entries of the wrong shape.");

            for (size_t i = 0; i < count; i++)
            {
               uint64_t entryKey = PstFormat::U32(page, i * entrySize, "a B-tree leaf");
               if (!nodes)
                  entryKey &= ~1ULL;
               if (entryKey == key)
               {
                  entry = page.substr(i * entrySize, entrySize);
                  return true;
               }
            }

            return false;
         }

         if (entrySize != AnsiIntermediateEntrySize || count == 0 || count * entrySize > AnsiPageEntryBytes)
            throw PstError("A B-tree page of the file has entries of the wrong shape.");

         // [MS-PST] 2.2.2.7.7.2, ANSI: btkey, then the BREF (bid, ib).
         bool found = false;
         uint64_t childBid = 0;
         uint64_t childIb = 0;
         for (size_t i = 0; i < count; i++)
         {
            uint64_t entryKey = PstFormat::U32(page, i * entrySize, "a B-tree page");
            if (!nodes)
               entryKey &= ~1ULL;
            if (entryKey > key)
               break;

            childBid = PstFormat::U32(page, i * entrySize + 4, "a B-tree page");
            childIb = PstFormat::U32(page, i * entrySize + 8, "a B-tree page");
            found = true;
         }

         if (!found)
            return false;

         bid = childBid;
         ib = childIb;
         expectedLevel = level - 1;
      }

      throw PstError("A B-tree of the file is deeper than any file Outlook writes.");
   }

   bool
   PstFile::FindNodeAnsi_(uint32_t nid, Node &node)
   {
      std::string entry;
      if (!SearchAnsi_(nbt_bid_, nbt_ib_, AnsiPageTypeNbt, nid, AnsiNbtLeafEntrySize, entry))
         return false;

      // [MS-PST] 2.2.2.7.7.4, ANSI: nid, bidData, bidSub, nidParent.
      node.nid = PstFormat::U32(entry, 0, "a node entry");
      node.data = PstFormat::U32(entry, 4, "a node entry");
      node.sub = PstFormat::U32(entry, 8, "a node entry");
      node.parent = PstFormat::U32(entry, 12, "a node entry");
      return true;
   }

   bool
   PstFile::FindBlockAnsi_(uint64_t bid, uint64_t &ib, uint16_t &cb)
   {
      // An ANSI BID is four bytes: anything wider names no block.
      if (bid > 0xFFFFFFFFULL)
         return false;

      std::string entry;
      if (!SearchAnsi_(bbt_bid_, bbt_ib_, AnsiPageTypeBbt, bid & ~1ULL, AnsiBbtLeafEntrySize, entry))
         return false;

      // [MS-PST] 2.2.2.7.7.3, ANSI: BREF (bid, ib), cb, cRef.
      ib = PstFormat::U32(entry, 4, "a block entry");
      cb = PstFormat::U16(entry, 8, "a block entry");
      return true;
   }

   std::string
   PstFile::BlockAnsi_(uint64_t bid)
   {
      uint64_t ib = 0;
      uint16_t cb = 0;
      if (!FindBlockAnsi_(bid, ib, cb))
         throw PstError("The block " + AnsiHex(bid) + " is not in the file's block index.");

      if (cb > MaximumAnsiBlockData)
         throw PstError("The block " + AnsiHex(bid) + " says it holds more than a block can.");

      // The data, padding to a multiple of 64 bytes, and the 12-byte trailer:
      // cb, wSig, bid, dwCRC.
      const uint64_t size = ((uint64_t) cb + AnsiBlockTrailerSize + 63) / 64 * 64;

      budget_.Item();
      budget_.Bytes(size);
      std::string block;
      if (!PstFormat::Fits(ib, size, end_of_file_) || !source_->Read(ib, (size_t) size, block))
         throw PstError("The block " + AnsiHex(bid) + " lies past the end of the file.");

      const size_t trailer = (size_t) size - AnsiBlockTrailerSize;
      const uint16_t trailerCb = PstFormat::U16(block, trailer, "a block trailer");
      const uint32_t trailerBid = PstFormat::U32(block, trailer + 4, "a block trailer");
      const uint32_t trailerCrc = PstFormat::U32(block, trailer + 8, "a block trailer");

      if (trailerCb != cb || ((uint64_t) trailerBid & ~1ULL) != (bid & ~1ULL))
         throw PstError("The block " + AnsiHex(bid) + " is not the block the index names.");

      block.resize(cb);
      if (PstFormat::Crc(block) != trailerCrc)
         throw PstError("The block " + AnsiHex(bid) + " is damaged (its checksum does not match).");

      const bool internal = (bid & 2) != 0;
      if (!internal)
      {
         if (crypt_method_ == 1)
            PstFormat::DecodePermute(block);
         else if (crypt_method_ == 2)
            PstFormat::CryptCyclic(block, (uint32_t) (bid & 0xFFFFFFFFULL));
      }

      return block;
   }

   // level: -2 for a node's one data block, -1 for a data block under an
   // XBLOCK, 0 for the top of a tree, 1 for an XBLOCK under an XXBLOCK - as
   // DataBlocks_; visited is every block the tree has named so far, and a
   // block named twice, or a block of nothing under an XBLOCK, is refused as
   // it is there.
   void
   PstFile::DataBlocksAnsi_(uint64_t bid, int level, size_t limit, size_t &total, std::set<uint64_t> &visited,
                            std::vector<std::string> &blocks)
   {
      if (!visited.insert(bid & ~1ULL).second)
         throw PstError("A data tree of the file names one of its blocks twice.");

      if ((bid & 2) == 0)
      {
         std::string block = BlockAnsi_(bid);
         if (block.empty() && level == -1)
            throw PstError("A data tree of the file lists a block that holds nothing.");
         if (block.size() > limit - total)
            throw PstTooLarge("A value in the file is larger than this reader takes.");
         total += block.size();
         blocks.push_back(block);
         return;
      }

      // btype 0x01, cLevel, cEnt, lcbTotal, then BIDs of four bytes.
      const std::string raw = BlockAnsi_(bid);
      const uint8_t type = PstFormat::U8(raw, 0, "a data tree block");
      const int blockLevel = PstFormat::U8(raw, 1, "a data tree block");
      const size_t count = PstFormat::U16(raw, 2, "a data tree block");
      const uint32_t declared = PstFormat::U32(raw, 4, "a data tree block");

      if (type != 0x01 || blockLevel < 1 || blockLevel > 2 || (level != 0 && blockLevel != level))
         throw PstError("A data tree of the file is not shaped as one.");

      if (!PstFormat::Fits(8, (uint64_t) count * 4, raw.size()))
         throw PstError("A data tree block of the file lists more blocks than it holds.");

      if (declared > limit - total)
         throw PstTooLarge("A value in the file is larger than this reader takes.");

      const size_t before = total;
      for (size_t i = 0; i < count; i++)
      {
         const uint64_t child = PstFormat::U32(raw, 8 + i * 4, "a data tree block");
         if (blockLevel == 1)
         {
            if ((child & 2) != 0)
               throw PstError("A data tree of the file is not shaped as one.");
            DataBlocksAnsi_(child, -1, limit, total, visited, blocks);
         }
         else
         {
            if ((child & 2) == 0)
               throw PstError("A data tree of the file is not shaped as one.");
            DataBlocksAnsi_(child, 1, limit, total, visited, blocks);
         }
      }

      if (total - before != declared)
         throw PstError("A data tree of the file does not hold what it says it does.");
   }

   void
   PstFile::SubnodesAnsi_(uint64_t bid, bool leafOnly, std::set<uint64_t> &visited, std::map<uint32_t, Subnode> &subnodes)
   {
      if ((bid & 2) == 0)
         throw PstError("A subnode tree of the file is not shaped as one.");

      if (!visited.insert(bid & ~1ULL).second)
         throw PstError("A subnode tree of the file points back at one of its own blocks.");

      // btype 0x02, cLevel 0 (SLBLOCK: nid, bidData, bidSub) or 1 (SIBLOCK:
      // nid, bid), cEnt, and the entries straight after - no padding.
      const std::string raw = BlockAnsi_(bid);
      const uint8_t type = PstFormat::U8(raw, 0, "a subnode block");
      const int level = PstFormat::U8(raw, 1, "a subnode block");
      const size_t count = PstFormat::U16(raw, 2, "a subnode block");

      if (type != 0x02 || level > 1 || (leafOnly && level != 0))
         throw PstError("A subnode tree of the file is not shaped as one.");

      const size_t entrySize = level == 0 ? 12 : 8;
      if (!PstFormat::Fits(4, (uint64_t) count * entrySize, raw.size()))
         throw PstError("A subnode block of the file lists more entries than it holds.");

      for (size_t i = 0; i < count; i++)
      {
         budget_.Item();

         const size_t at = 4 + i * entrySize;
         if (level == 0)
         {
            Subnode subnode;
            subnode.nid = PstFormat::U32(raw, at, "a subnode entry");
            subnode.data = PstFormat::U32(raw, at + 4, "a subnode entry");
            subnode.sub = PstFormat::U32(raw, at + 8, "a subnode entry");

            if (subnodes.size() >= MaximumSubnodes)
               throw PstError("A node of the file has more subnodes than this reader takes.");

            subnodes.insert(std::make_pair(subnode.nid, subnode));
         }
         else
         {
            SubnodesAnsi_(PstFormat::U32(raw, at + 4, "a subnode entry"), true, visited, subnodes);
         }
      }
   }
}
