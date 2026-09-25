// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstFile.h.

#include "StdAfx.h"

#include "PstFile.h"

#include <cstdio>

namespace HM
{
   const int PstFile::MaximumTreeDepth;
   const size_t PstFile::MaximumSubnodes;
   const size_t PstFile::MaximumBlockData;

   namespace
   {
      // [MS-PST] 2.2.2.6: the Unicode header.
      const size_t HeaderSize = 564;
      const size_t OffsetMagicClient = 8;
      const size_t OffsetVersion = 10;
      const size_t OffsetCrcPartial = 4;
      const size_t OffsetEndOfFile = 184;
      const size_t OffsetNbt = 216;
      const size_t OffsetBbt = 232;
      const size_t OffsetCryptMethod = 513;
      const size_t OffsetCrcFull = 524;

      // [MS-PST] 2.2.2.7: a page, its entries and its trailer.
      const size_t PageSize = 512;
      const size_t PageEntryBytes = 488;
      const uint8_t PageTypeBbt = 0x80;
      const uint8_t PageTypeNbt = 0x81;

      const size_t NbtLeafEntrySize = 32;
      const size_t BbtLeafEntrySize = 24;
      const size_t IntermediateEntrySize = 24;

      std::string Number(uint64_t value)
      {
         char text[32];
         snprintf(text, sizeof(text), "%llu", (unsigned long long) value);
         return text;
      }

      std::string HexNumber(uint64_t value)
      {
         char text[32];
         snprintf(text, sizeof(text), "0x%llx", (unsigned long long) value);
         return text;
      }
   }

   PstFile::PstFile(std::shared_ptr<PstByteSource> source) :
      source_(source),
      version_(0),
      crypt_method_(0),
      end_of_file_(0),
      nbt_bid_(0),
      nbt_ib_(0),
      bbt_bid_(0),
      bbt_ib_(0),
      open_(false)
   {
   }

   void
   PstFile::Open()
   {
      open_ = false;
      ansi_ = false;
      offline_ = false;

      std::string header;
      if (!source_ || source_->Size() < HeaderSize || !source_->Read(0, HeaderSize, header))
         throw PstRefusal(PstRefusal::ReasonNotPst, "This is not an Outlook data file (PST): it is shorter than a PST's header.");

      if (header.compare(0, 4, "!BDN") != 0)
         throw PstRefusal(PstRefusal::ReasonNotPst, "This is not an Outlook data file (PST): it does not begin with a PST's header.");

      const std::string client = header.substr(OffsetMagicClient, 2);
      const int version = PstFormat::U16(header, OffsetVersion, "the header");
      const int crypt = PstFormat::U8(header, OffsetCryptMethod, "the header");

      // What kind of file it is, before whether it is whole: the 4K-page
      // layout is refused for what it is, whatever else is wrong with it.
      if (version == 36)
         throw PstRefusal(client == "SO" ? PstRefusal::ReasonOffline : PstRefusal::ReasonFourK, FourKSentence());

      // "SM" is a PST and "SO" an OST, whose pages, blocks and B-trees are the
      // same ([MS-PST] names only the one magic; the store decides the rest).
      if (client != "SM" && client != "SO")
         throw PstRefusal(PstRefusal::ReasonNotPst, "This is not an Outlook data file (PST): its header names another kind of file.");

      offline_ = client == "SO";

      // [MS-PST]'s ANSI file: a header, pages and blocks of its own.
      if (version == 14 || version == 15)
      {
         OpenAnsi_(header);
         return;
      }

      if (crypt == 0x10)
         throw PstRefusal(PstRefusal::ReasonProtected, ProtectedSentence());

      if ((version != 23 && version != 37) || (crypt != 0 && crypt != 1 && crypt != 2))
      {
         throw PstRefusal(PstRefusal::ReasonUnknownFormat,
            "This PST's format (version " + Number((uint64_t) version) + ", encoding " + Number((uint64_t) crypt) +
            ") is not one this reader knows; open it in a current Outlook and export a new PST.");
      }

      // [MS-PST] 2.2.2.6: dwCRCPartial covers the 471 bytes from wMagicClient,
      // dwCRCFull the 516.
      const uint32_t partial = PstFormat::U32(header, OffsetCrcPartial, "the header");
      const uint32_t full = PstFormat::U32(header, OffsetCrcFull, "the header");
      if (PstFormat::Crc((const unsigned char *) header.data() + OffsetMagicClient, 471) != partial ||
          PstFormat::Crc((const unsigned char *) header.data() + OffsetMagicClient, 516) != full)
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file's header is damaged (its checksum does not match), so nothing in it can be trusted; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and try again.");
      }

      const uint64_t endOfFile = PstFormat::U64(header, OffsetEndOfFile, "the header");
      if (endOfFile > source_->Size() || endOfFile < HeaderSize)
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file is " + Number(source_->Size()) + " bytes and its header says " + Number(endOfFile) +
            ": it was cut short, most likely while it was copied; copy it again.");
      }

      version_ = version;
      crypt_method_ = crypt;
      end_of_file_ = endOfFile;
      nbt_bid_ = PstFormat::U64(header, OffsetNbt, "the header");
      nbt_ib_ = PstFormat::U64(header, OffsetNbt + 8, "the header");
      bbt_bid_ = PstFormat::U64(header, OffsetBbt, "the header");
      bbt_ib_ = PstFormat::U64(header, OffsetBbt + 8, "the header");

      if (!PstFormat::Fits(nbt_ib_, PageSize, end_of_file_) || !PstFormat::Fits(bbt_ib_, PageSize, end_of_file_))
      {
         throw PstRefusal(PstRefusal::ReasonDamaged,
            "The file's header points past its end, so nothing in it can be found; repair the file with Outlook's Inbox Repair Tool (scanpst.exe) and try again.");
      }

      open_ = true;
   }

   std::string
   PstFile::Page_(uint64_t ib, uint64_t bid, uint8_t pageType)
   {
      budget_.Item();
      budget_.Bytes(PageSize);

      std::string page;
      if (!PstFormat::Fits(ib, PageSize, end_of_file_) || !source_->Read(ib, PageSize, page))
         throw PstError("A B-tree page at " + HexNumber(ib) + " lies past the end of the file.");

      // [MS-PST] 2.2.2.7.1: ptype, ptypeRepeat, wSig, dwCRC, bid.
      const uint8_t type = PstFormat::U8(page, 496, "a page trailer");
      const uint8_t repeat = PstFormat::U8(page, 497, "a page trailer");
      const uint32_t crc = PstFormat::U32(page, 500, "a page trailer");
      const uint64_t trailerBid = PstFormat::U64(page, 504, "a page trailer");

      if (type != pageType || repeat != pageType)
         throw PstError("The B-tree page at " + HexNumber(ib) + " is not the kind of page that should be there.");

      if (PstFormat::Crc((const unsigned char *) page.data(), 496) != crc)
         throw PstError("The B-tree page at " + HexNumber(ib) + " is damaged (its checksum does not match).");

      if (trailerBid != bid)
         throw PstError("The B-tree page at " + HexNumber(ib) + " is not the page its parent names.");

      return page;
   }

   bool
   PstFile::Search_(uint64_t rootBid, uint64_t rootIb, uint8_t pageType, uint64_t key, size_t leafEntrySize, std::string &entry)
   {
      if (!open_)
         throw PstError("The file is not open.");

      const bool nodes = pageType == PageTypeNbt;
      uint64_t bid = rootBid;
      uint64_t ib = rootIb;
      int expectedLevel = -1;
      std::set<uint64_t> visited;

      for (int depth = 0; depth < MaximumTreeDepth; depth++)
      {
         // A page seen before on this walk is a tree that points back into
         // itself: refused, rather than walked for ever.
         if (!visited.insert(ib).second)
            throw PstError("A B-tree of the file points back at one of its own pages.");

         const std::string page = Page_(ib, bid, pageType);
         const size_t count = PstFormat::U8(page, 488, "a B-tree page");
         const size_t entrySize = PstFormat::U8(page, 490, "a B-tree page");
         const int level = PstFormat::U8(page, 491, "a B-tree page");

         // Each step down is one level lower, ending at 0: a page that is not
         // is out of place.
         if (expectedLevel >= 0 && level != expectedLevel)
            throw PstError("A B-tree page of the file is at the wrong level.");

         if (level == 0)
         {
            if (entrySize != leafEntrySize || count * entrySize > PageEntryBytes)
               throw PstError("A B-tree leaf of the file has entries of the wrong shape.");

            for (size_t i = 0; i < count; i++)
            {
               uint64_t entryKey = PstFormat::U64(page, i * entrySize, "a B-tree leaf");
               entryKey = nodes ? (entryKey & 0xFFFFFFFFULL) : (entryKey & ~1ULL);
               if (entryKey == key)
               {
                  entry = page.substr(i * entrySize, entrySize);
                  return true;
               }
            }

            return false;
         }

         if (entrySize != IntermediateEntrySize || count == 0 || count * entrySize > PageEntryBytes)
            throw PstError("A B-tree page of the file has entries of the wrong shape.");

         // The last entry whose key is at most the one sought.
         bool found = false;
         uint64_t childBid = 0;
         uint64_t childIb = 0;
         for (size_t i = 0; i < count; i++)
         {
            uint64_t entryKey = PstFormat::U64(page, i * entrySize, "a B-tree page");
            entryKey = nodes ? (entryKey & 0xFFFFFFFFULL) : (entryKey & ~1ULL);
            if (entryKey > key)
               break;

            childBid = PstFormat::U64(page, i * entrySize + 8, "a B-tree page");
            childIb = PstFormat::U64(page, i * entrySize + 16, "a B-tree page");
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
   PstFile::FindNode(uint32_t nid, Node &node)
   {
      if (ansi_)
         return FindNodeAnsi_(nid, node);

      std::string entry;
      if (!Search_(nbt_bid_, nbt_ib_, PageTypeNbt, nid, NbtLeafEntrySize, entry))
         return false;

      // [MS-PST] 2.2.2.7.7.4: nid (8, the low 32 bits used), bidData, bidSub,
      // nidParent, padding.
      node.nid = (uint32_t) (PstFormat::U64(entry, 0, "a node entry") & 0xFFFFFFFFULL);
      node.data = PstFormat::U64(entry, 8, "a node entry");
      node.sub = PstFormat::U64(entry, 16, "a node entry");
      node.parent = PstFormat::U32(entry, 24, "a node entry");
      return true;
   }

   bool
   PstFile::FindBlock_(uint64_t bid, uint64_t &ib, uint16_t &cb)
   {
      if (ansi_)
         return FindBlockAnsi_(bid, ib, cb);

      std::string entry;
      if (!Search_(bbt_bid_, bbt_ib_, PageTypeBbt, bid & ~1ULL, BbtLeafEntrySize, entry))
         return false;

      // [MS-PST] 2.2.2.7.7.3: BREF (bid, ib), cb, cRef, padding.
      ib = PstFormat::U64(entry, 8, "a block entry");
      cb = PstFormat::U16(entry, 16, "a block entry");
      return true;
   }

   std::string
   PstFile::Block_(uint64_t bid)
   {
      if (ansi_)
         return BlockAnsi_(bid);

      uint64_t ib = 0;
      uint16_t cb = 0;
      if (!FindBlock_(bid, ib, cb))
         throw PstError("The block " + HexNumber(bid) + " is not in the file's block index.");

      if (cb > MaximumBlockData)
         throw PstError("The block " + HexNumber(bid) + " says it holds more than a block can.");

      // [MS-PST] 2.2.2.8: the data, padding to a multiple of 64 bytes, and the
      // 16-byte trailer at the end.
      const uint64_t size = ((uint64_t) cb + 16 + 63) / 64 * 64;

      budget_.Item();
      budget_.Bytes(size);
      std::string block;
      if (!PstFormat::Fits(ib, size, end_of_file_) || !source_->Read(ib, (size_t) size, block))
         throw PstError("The block " + HexNumber(bid) + " lies past the end of the file.");

      const size_t trailer = (size_t) size - 16;
      const uint16_t trailerCb = PstFormat::U16(block, trailer, "a block trailer");
      const uint32_t trailerCrc = PstFormat::U32(block, trailer + 4, "a block trailer");
      const uint64_t trailerBid = PstFormat::U64(block, trailer + 8, "a block trailer");

      if (trailerCb != cb || (trailerBid & ~1ULL) != (bid & ~1ULL))
         throw PstError("The block " + HexNumber(bid) + " is not the block the index names.");

      block.resize(cb);
      if (PstFormat::Crc(block) != trailerCrc)
         throw PstError("The block " + HexNumber(bid) + " is damaged (its checksum does not match).");

      // [MS-PST] 5: only external blocks - data - are encoded; the internal
      // ones (XBLOCK, XXBLOCK, SLBLOCK, SIBLOCK) are not.
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
   // XBLOCK, 0 for the top of a tree (an XBLOCK or an XXBLOCK), 1 for an XBLOCK
   // under an XXBLOCK. visited is every block the tree has named so far.
   void
   PstFile::DataBlocks_(uint64_t bid, int level, size_t limit, size_t &total, std::set<uint64_t> &visited,
                        std::vector<std::string> &blocks)
   {
      // An ANSI file's tree is walked by its twin (PstFileAnsi.cpp), which
      // keeps the same rules: a block named twice, or a block of nothing
      // under an XBLOCK, is refused.
      if (ansi_)
      {
         DataBlocksAnsi_(bid, level, limit, total, visited, blocks);
         return;
      }

      // A block named twice in one tree: an XXBLOCK that lists one XBLOCK a
      // thousand times, each listing one data block a thousand times, would be a
      // million reads from 17 KB of file. Outlook never repeats a block in a
      // tree, so a repeat is damage.
      if (!visited.insert(bid & ~1ULL).second)
         throw PstError("A data tree of the file names one of its blocks twice.");

      if ((bid & 2) == 0)
      {
         std::string block = Block_(bid);
         if (block.empty() && level == -1)
            throw PstError("A data tree of the file lists a block that holds nothing.");
         if (block.size() > limit - total)
            throw PstTooLarge("A value in the file is larger than this reader takes.");
         total += block.size();
         blocks.push_back(block);
         return;
      }

      // [MS-PST] 2.2.2.8.3.2: btype 0x01, cLevel 1 (XBLOCK: data blocks) or 2
      // (XXBLOCK: XBLOCKs), cEnt, lcbTotal, then the BIDs. An XXBLOCK is the
      // most a tree may be, and an XBLOCK under it has level 1.
      const std::string raw = Block_(bid);
      const uint8_t type = PstFormat::U8(raw, 0, "a data tree block");
      const int blockLevel = PstFormat::U8(raw, 1, "a data tree block");
      const size_t count = PstFormat::U16(raw, 2, "a data tree block");
      const uint32_t declared = PstFormat::U32(raw, 4, "a data tree block");

      if (type != 0x01 || blockLevel < 1 || blockLevel > 2 || (level != 0 && blockLevel != level))
         throw PstError("A data tree of the file is not shaped as one.");

      if (!PstFormat::Fits(8, (uint64_t) count * 8, raw.size()))
         throw PstError("A data tree block of the file lists more blocks than it holds.");

      // What the tree says it holds is checked against the limit before any of
      // it is read: a lcbTotal of 4 GB is refused here, not allocated.
      if (declared > limit - total)
         throw PstTooLarge("A value in the file is larger than this reader takes.");

      const size_t before = total;
      for (size_t i = 0; i < count; i++)
      {
         const uint64_t child = PstFormat::U64(raw, 8 + i * 8, "a data tree block");
         if (blockLevel == 1)
         {
            if ((child & 2) != 0)
               throw PstError("A data tree of the file is not shaped as one.");
            DataBlocks_(child, -1, limit, total, visited, blocks);
         }
         else
         {
            if ((child & 2) == 0)
               throw PstError("A data tree of the file is not shaped as one.");
            DataBlocks_(child, 1, limit, total, visited, blocks);
         }
      }

      if (total - before != declared)
         throw PstError("A data tree of the file does not hold what it says it does.");
   }

   void
   PstFile::ReadDataBlocks(uint64_t bid, size_t limit, std::vector<std::string> &blocks)
   {
      blocks.clear();
      if (bid == 0)
         return;

      size_t total = 0;
      std::set<uint64_t> visited;
      if ((bid & 2) == 0)
      {
         DataBlocks_(bid, -2, limit, total, visited, blocks);
         return;
      }

      DataBlocks_(bid, 0, limit, total, visited, blocks);
   }

   std::string
   PstFile::ReadData(uint64_t bid, size_t limit)
   {
      std::vector<std::string> blocks;
      ReadDataBlocks(bid, limit, blocks);

      if (blocks.size() == 1)
         return blocks[0];

      size_t size = 0;
      for (const std::string &block : blocks)
         size += block.size();

      std::string data;
      data.reserve(size);
      for (const std::string &block : blocks)
         data += block;
      return data;
   }

   void
   PstFile::Subnodes_(uint64_t bid, bool leafOnly, std::set<uint64_t> &visited, std::map<uint32_t, Subnode> &subnodes)
   {
      if (ansi_)
      {
         SubnodesAnsi_(bid, leafOnly, visited, subnodes);
         return;
      }

      if ((bid & 2) == 0)
         throw PstError("A subnode tree of the file is not shaped as one.");

      if (!visited.insert(bid & ~1ULL).second)
         throw PstError("A subnode tree of the file points back at one of its own blocks.");

      // [MS-PST] 2.2.2.8.3.3: btype 0x02, cLevel 0 (SLBLOCK: SLENTRY of nid,
      // bidData, bidSub) or 1 (SIBLOCK: SIENTRY of nid, bid of an SLBLOCK),
      // cEnt, padding, then the entries.
      const std::string raw = Block_(bid);
      const uint8_t type = PstFormat::U8(raw, 0, "a subnode block");
      const int level = PstFormat::U8(raw, 1, "a subnode block");
      const size_t count = PstFormat::U16(raw, 2, "a subnode block");

      if (type != 0x02 || level > 1 || (leafOnly && level != 0))
         throw PstError("A subnode tree of the file is not shaped as one.");

      const size_t entrySize = level == 0 ? 24 : 16;
      if (!PstFormat::Fits(8, (uint64_t) count * entrySize, raw.size()))
         throw PstError("A subnode block of the file lists more entries than it holds.");

      for (size_t i = 0; i < count; i++)
      {
         budget_.Item();

         const size_t at = 8 + i * entrySize;
         if (level == 0)
         {
            Subnode subnode;
            subnode.nid = (uint32_t) (PstFormat::U64(raw, at, "a subnode entry") & 0xFFFFFFFFULL);
            subnode.data = PstFormat::U64(raw, at + 8, "a subnode entry");
            subnode.sub = PstFormat::U64(raw, at + 16, "a subnode entry");

            if (subnodes.size() >= MaximumSubnodes)
               throw PstError("A node of the file has more subnodes than this reader takes.");

            subnodes.insert(std::make_pair(subnode.nid, subnode));
         }
         else
         {
            Subnodes_(PstFormat::U64(raw, at + 8, "a subnode entry"), true, visited, subnodes);
         }
      }
   }

   void
   PstFile::ReadSubnodes(uint64_t bid, std::map<uint32_t, Subnode> &subnodes)
   {
      subnodes.clear();
      if (bid == 0)
         return;

      std::set<uint64_t> visited;
      Subnodes_(bid, false, visited, subnodes);
   }
}
