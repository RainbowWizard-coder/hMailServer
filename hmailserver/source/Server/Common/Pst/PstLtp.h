// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST's LTP layer ([MS-PST] 2.3): the heap-on-node a node's data is laid
// out as - allocations named by HIDs, over as many blocks as the node has - the
// B-tree-on-heap (BTH) that sorts records in it, and the two things built from
// those: a property context (PC), a node's properties by their ids, and a table
// context (TC), rows of columns with a row index by row id.
//
// A value that does not fit a heap allocation (3,580 bytes) is in a subnode of
// the node instead; both are read here the same way, by HNID. Values are read
// when they are asked for, with a limit each time, so a property nobody asks
// for - a large RTF body beside an HTML one - is never read at all.
//
// Every heap allocation and every table cell read is charged to the file's work
// budget (PstFormat.h, PstBudget), and a table cell is read with a limit as a
// property is, so that a table whose every row names one large value costs what
// the budget allows and no more.

#pragma once

#include "PstFile.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace HM
{
   class PstHeap
   {
   public:
      PstHeap();

      // blocks: a node's data blocks, decoded; taken over, and each one's
      // header and page map checked.
      void Load(std::vector<std::string> &blocks);

      // Where each allocation read is charged; none, for a heap read on its
      // own (PstTester).
      void SetBudget(PstBudget *budget) { budget_ = budget; }

      uint8_t GetClientSignature() const { return client_; }
      uint32_t GetUserRoot() const { return user_root_; }

      // One allocation's bytes. HID 0 is the empty value; a HID that names no
      // allocation throws.
      std::string Allocation(uint32_t hid) const;

      // The records of the BTH whose header is the allocation hid: each a key
      // of keySize bytes and data of dataSize, in key order, at most limit of
      // them. A BTH of other sizes, too deep, or that points back into itself
      // throws.
      void BthRecords(uint32_t hid, size_t keySize, size_t dataSize, size_t limit,
                      std::vector<std::pair<std::string, std::string> > &records) const;

      // How deep a BTH may go. [MS-PST] gives no ceiling; a real one is two or
      // three levels.
      static const int MaximumBthLevels = 8;

   private:
      void Walk_(uint32_t hid, int level, size_t keySize, size_t dataSize, size_t limit, std::set<uint32_t> &visited,
                 std::vector<std::pair<std::string, std::string> > &records) const;

      std::vector<std::string> blocks_;
      std::vector<std::vector<uint16_t> > maps_;
      uint8_t client_;
      uint32_t user_root_;
      PstBudget *budget_;
   };

   class PstPropertyContext
   {
   public:
      PstPropertyContext();

      // The node's heap and its BTH of properties, and its subnodes. The values
      // are read when asked for.
      void Read(PstFile &file, uint64_t bidData, uint64_t bidSub);

      bool Has(uint16_t id) const;
      bool TypeOf(uint16_t id, uint16_t &type) const;

      bool Int32(uint16_t id, int32_t &value) const;
      bool Boolean(uint16_t id, bool &value) const;
      bool Time(uint16_t id, uint64_t &fileTime) const;

      // The bytes of a value that is not held in the record itself: a string,
      // binary, a multi-valued property, or one of the eight-byte types. At most
      // limit bytes; a larger value throws.
      bool Bytes(uint16_t id, size_t limit, std::string &value) const;

      // A PT_UNICODE value, or a PT_STRING8 one in codePage, as UTF-8, without a
      // terminating NUL. decoded is false when a PT_STRING8's code page is not
      // one this reader converts: the text then keeps its ASCII and has U+FFFD
      // for every other byte.
      bool Text(uint16_t id, int codePage, size_t limit, std::string &utf8, bool &decoded) const;
      bool Text(uint16_t id, size_t limit, std::string &utf8) const;

      // A PT_MV_UNICODE (or PT_MV_STRING8) value's strings, and a PT_MV_BINARY
      // value's items.
      bool MultiText(uint16_t id, int codePage, size_t limit, std::vector<std::string> &values) const;
      bool MultiBinary(uint16_t id, size_t limit, std::vector<std::string> &values) const;

      // A PtypObject value ([MS-PST] 2.3.3.6): the subnode that holds the object.
      bool ObjectNid(uint16_t id, uint32_t &nid) const;

      const std::map<uint32_t, PstFile::Subnode> &GetSubnodes() const { return subnodes_; }

      // The most a property context's heap may be.
      static const size_t MaximumHeapBytes = 64 * 1024 * 1024;

   private:
      std::string Value_(uint32_t hnid, size_t limit) const;

      struct Entry
      {
         uint16_t type;
         uint32_t value;
      };

      PstFile *file_;
      PstHeap heap_;
      std::map<uint16_t, Entry> entries_;
      std::map<uint32_t, PstFile::Subnode> subnodes_;
   };

   class PstTable
   {
   public:
      PstTable();

      // The table's column descriptions and its row index, at most rowLimit
      // rows. The rows themselves are read when a cell is asked for.
      void Read(PstFile &file, uint64_t bidData, uint64_t bidSub, size_t rowLimit);

      size_t RowCount() const { return rows_.size(); }

      // The row's id - for a folder's tables the NID of the child folder,
      // message or attachment the row stands for.
      uint32_t RowId(size_t row) const;

      // A cell's value, read with at most limit bytes: a larger one throws
      // PstTooLarge. Every cell read is charged to the file's work budget.
      bool CellInt32(size_t row, uint16_t id, int32_t &value);
      bool CellText(size_t row, uint16_t id, int codePage, std::string &utf8, size_t limit = MaximumCellBytes);

      // The most bytes a table's rows may take.
      static const size_t MaximumMatrixBytes = 256 * 1024 * 1024;

      // The most bytes one cell's value may take unless the caller says less:
      // a recipient's name or address, an attachment's, a folder's.
      static const size_t MaximumCellBytes = 64 * 1024;

   private:
      bool Cell_(size_t row, uint16_t id, size_t limit, uint16_t &type, std::string &value);
      std::string Row_(size_t row);

      struct Column
      {
         uint16_t id;
         uint16_t type;
         uint16_t offset;
         uint8_t size;
         uint8_t bit;
      };

      PstFile *file_;
      PstHeap heap_;
      std::map<uint32_t, PstFile::Subnode> subnodes_;
      std::vector<Column> columns_;
      std::vector<std::pair<uint32_t, uint32_t> > rows_;
      size_t end_of_values_;
      size_t row_size_;
      uint32_t rows_hnid_;
      bool matrix_loaded_;
      std::vector<std::string> matrix_;
   };
}
