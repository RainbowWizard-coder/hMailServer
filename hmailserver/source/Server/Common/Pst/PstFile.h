// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST's NDB layer ([MS-PST] 2.2): the header, the node B-tree (NBT) that
// maps a node's NID to its data and its subnodes, the block B-tree (BBT) that
// maps a block's BID to where it is, the blocks themselves - decoded as the
// header's bCryptMethod says, their trailers and CRCs checked - the data trees
// (XBLOCK and XXBLOCK) that join a node's blocks, and the subnode trees (SLBLOCK
// and SIBLOCK) that hold what a node keeps beside its data.
//
// What is read, and what is refused (PstRefusal, each with the sentence the job
// reports): a Unicode PST - wMagicClient "SM", wVer 23 or 37 (37 is what an
// Outlook that knows Windows Information Protection writes) - and an ANSI one
// (wVer 14 or 15, Outlook 97 to 2002: [MS-PST]'s 32-bit NDB layer, in
// PstFileAnsi.cpp), encoded with bCryptMethod 0 (none), 1 (permute) or 2
// (cyclic). An OST (wMagicClient "SO") of either version is read the same way,
// since [MS-PST] names no other difference at this layer; the store above
// decides whether its folders can be found (PstStore.h). The 4K-page layout
// (wVer 36, which Outlook 2013 and later write for an OST and which [MS-PST]
// does not describe), a file protected with Windows Information Protection
// (bCryptMethod 0x10) and any other version or encoding are refused, and so is
// a header whose CRC fails or that says the file is longer than it is. A block
// whose CRC, size or BID does not match what points at it is that block's
// failure (PstError), not the file's.
//
// Every page and block read here is charged to the file's work budget
// (PstFormat.h, PstBudget), with the bytes it decodes; the budget asks the job's
// stop check at each one. A data tree that names one block twice, and a data
// block of no bytes inside an XBLOCK, are refused: nothing Outlook writes does
// either, and both would let a small tree make the reader read a million blocks.

#pragma once

#include "PstFormat.h"
#include "PstSource.h"

#include <map>
#include <memory>
#include <set>

namespace HM
{
   class PstFile
   {
   public:
      explicit PstFile(std::shared_ptr<PstByteSource> source);

      // The header, read and checked. Refused with a PstRefusal.
      void Open();

      int GetVersion() const { return version_; }
      int GetCryptMethod() const { return crypt_method_; }
      uint64_t GetEndOfFile() const { return end_of_file_; }

      // An ANSI file (wVer 14 or 15), and a file whose header calls it an OST
      // ("SO").
      bool IsAnsi() const { return ansi_; }
      bool IsOffline() const { return offline_; }

      // What one data block of this file may hold: 8,192 bytes less the
      // trailer - 16 in a Unicode file, 12 in an ANSI one ([MS-PST] 2.2.2.8).
      // A table's rows are packed into blocks of this size (2.3.4.4).
      size_t GetMaximumBlockData() const { return ansi_ ? MaximumAnsiBlockData : MaximumBlockData; }

      struct Node
      {
         Node() : nid(0), data(0), sub(0), parent(0) { }

         uint32_t nid;
         uint64_t data;       // bidData
         uint64_t sub;        // bidSub, 0 for none
         uint32_t parent;     // nidParent
      };

      // The node with this NID; false when the NBT has none. A damaged NBT page
      // throws.
      bool FindNode(uint32_t nid, Node &node);

      // What reading this file may cost, and the stop check (PstFormat.h).
      PstBudget &Budget() { return budget_; }

      // The blocks of a data tree in order, decoded, at most limit bytes in all.
      // A tree that says it holds more than limit throws before anything of it
      // is read.
      void ReadDataBlocks(uint64_t bid, size_t limit, std::vector<std::string> &blocks);

      // The same, joined.
      std::string ReadData(uint64_t bid, size_t limit);

      struct Subnode
      {
         Subnode() : nid(0), data(0), sub(0) { }

         uint32_t nid;
         uint64_t data;
         uint64_t sub;
      };

      // A subnode tree's entries: an SLBLOCK, or an SIBLOCK of SLBLOCKs.
      void ReadSubnodes(uint64_t bid, std::map<uint32_t, Subnode> &subnodes);

      // The most levels a B-tree may have. A real file's have four or five.
      static const int MaximumTreeDepth = 12;

      // The most subnodes one node may have.
      static const size_t MaximumSubnodes = 1000000;

      // What one block may hold ([MS-PST] 2.2.2.8: 8192 bytes less the trailer).
      static const size_t MaximumBlockData = 8176;
      static const size_t MaximumAnsiBlockData = 8180;

      // The sentences of two refusals, one for either layout: a file protected
      // with Windows Information Protection, and the 4K-page layout.
      static std::string ProtectedSentence();
      static std::string FourKSentence();

   private:
      // The ANSI file's header and NDB layer (PstFileAnsi.cpp): the same
      // walks as the Unicode ones below, over [MS-PST]'s 32-bit structures.
      // Each Unicode function hands over to its ANSI twin when ansi_ is set.
      void OpenAnsi_(const std::string &header);
      std::string PageAnsi_(uint64_t ib, uint64_t bid, uint8_t pageType);
      bool SearchAnsi_(uint64_t rootBid, uint64_t rootIb, uint8_t pageType, uint64_t key, size_t leafEntrySize, std::string &entry);
      bool FindNodeAnsi_(uint32_t nid, Node &node);
      bool FindBlockAnsi_(uint64_t bid, uint64_t &ib, uint16_t &cb);
      std::string BlockAnsi_(uint64_t bid);
      void DataBlocksAnsi_(uint64_t bid, int level, size_t limit, size_t &total, std::set<uint64_t> &visited, std::vector<std::string> &blocks);
      void SubnodesAnsi_(uint64_t bid, bool leafOnly, std::set<uint64_t> &visited, std::map<uint32_t, Subnode> &subnodes);

      bool ansi_ = false;
      bool offline_ = false;

      bool Search_(uint64_t rootBid, uint64_t rootIb, uint8_t pageType, uint64_t key, size_t leafEntrySize, std::string &entry);
      std::string Page_(uint64_t ib, uint64_t bid, uint8_t pageType);
      bool FindBlock_(uint64_t bid, uint64_t &ib, uint16_t &cb);
      std::string Block_(uint64_t bid);
      void DataBlocks_(uint64_t bid, int level, size_t limit, size_t &total, std::set<uint64_t> &visited, std::vector<std::string> &blocks);
      void Subnodes_(uint64_t bid, bool leafOnly, std::set<uint64_t> &visited, std::map<uint32_t, Subnode> &subnodes);

      std::shared_ptr<PstByteSource> source_;
      int version_;
      int crypt_method_;
      uint64_t end_of_file_;
      uint64_t nbt_bid_;
      uint64_t nbt_ib_;
      uint64_t bbt_bid_;
      uint64_t bbt_ib_;
      bool open_;
      PstBudget budget_;
   };
}
