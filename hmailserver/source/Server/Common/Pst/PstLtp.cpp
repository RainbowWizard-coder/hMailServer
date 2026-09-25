// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstLtp.h.

#include "StdAfx.h"

#include "PstLtp.h"
#include "PstCodePages.h"

namespace HM
{
   const int PstHeap::MaximumBthLevels;
   const size_t PstPropertyContext::MaximumHeapBytes;
   const size_t PstTable::MaximumMatrixBytes;
   const size_t PstTable::MaximumCellBytes;

   namespace
   {
      // [MS-PST] 2.3.1.2: a heap's signature and its clients.
      const uint8_t HeapSignature = 0xEC;
      const uint8_t ClientTable = 0x7C;
      const uint8_t ClientPropertyContext = 0xBC;
      const uint8_t BthSignature = 0xB5;

      bool IsHid(uint32_t hnid)
      {
         return (hnid & 0x1F) == 0;
      }

      // Whether a type's value is in the property record itself ([MS-PST]
      // 2.3.3.3): the fixed types of four bytes or fewer.
      bool Inline(uint16_t type)
      {
         return type == PstTypeInteger16 || type == PstTypeInteger32 || type == PstTypeFloating32 ||
                type == PstTypeErrorCode || type == PstTypeBoolean;
      }

      // A table's fixed-size cells are in the row: everything of eight bytes or
      // fewer that is not a variable-size type.
      bool FixedInRow(uint16_t type)
      {
         return Inline(type) || type == PstTypeFloating64 || type == PstTypeCurrency || type == PstTypeFloatingTime ||
                type == PstTypeInteger64 || type == PstTypeTime;
      }

      std::string StripNul(const std::string &utf8)
      {
         size_t end = utf8.size();
         while (end > 0 && utf8[end - 1] == '\0')
            end--;
         return utf8.substr(0, end);
      }

      // Bytes in a code page the table does not have: the ASCII kept, U+FFFD for
      // the rest.
      std::string AsciiOnly(const std::string &bytes)
      {
         std::string out;
         for (size_t i = 0; i < bytes.size(); i++)
         {
            const unsigned char c = (unsigned char) bytes[i];
            if (c < 0x80)
               out += (char) c;
            else
               PstFormat::AppendUtf8(out, 0xFFFD);
         }
         return out;
      }

      std::string String8(const std::string &bytes, int codePage, bool &decoded)
      {
         std::string utf8;
         decoded = PstCodePages::Decode(bytes, codePage, utf8);
         if (!decoded)
            utf8 = AsciiOnly(bytes);
         return StripNul(utf8);
      }

      // [MS-PST] 2.3.3.4.2: a multi-valued variable-size value is a count, an
      // offset per item from the start of the value, and the items.
      void SplitMultiple(const std::string &value, std::vector<std::string> &items)
      {
         items.clear();
         if (value.empty())
            return;

         const uint32_t count = PstFormat::U32(value, 0, "a multi-valued property");
         if (!PstFormat::Fits(4, (uint64_t) count * 4, value.size()))
            throw PstError("A multi-valued property of the file claims more items than it holds.");

         for (uint32_t i = 0; i < count; i++)
         {
            const uint32_t start = PstFormat::U32(value, 4 + (size_t) i * 4, "a multi-valued property");
            const uint32_t end = i + 1 < count ? PstFormat::U32(value, 4 + (size_t) (i + 1) * 4, "a multi-valued property")
                                               : (uint32_t) value.size();
            if (start > end || end > value.size() || start < 4 + (uint64_t) count * 4)
               throw PstError("A multi-valued property of the file has an item outside it.");
            items.push_back(value.substr(start, end - start));
         }
      }
   }

   // ------------------------------------------------------------------ the heap

   PstHeap::PstHeap() :
      client_(0),
      user_root_(0),
      budget_(nullptr)
   {
   }

   void
   PstHeap::Load(std::vector<std::string> &blocks)
   {
      blocks_.clear();
      maps_.clear();
      client_ = 0;
      user_root_ = 0;

      if (blocks.empty())
         throw PstError("A node of the file that should hold a heap holds nothing.");

      blocks_.swap(blocks);

      // [MS-PST] 2.3.1.2: HNHDR in the first block (ibHnpm, bSig, bClientSig,
      // hidUserRoot, rgbFillLevel); HNPAGEHDR or HNBITMAPHDR (ibHnpm first, as
      // well) in the others.
      const std::string &first = blocks_[0];
      if (PstFormat::U8(first, 2, "a heap header") != HeapSignature)
         throw PstError("A node of the file that should hold a heap does not.");

      client_ = PstFormat::U8(first, 3, "a heap header");
      user_root_ = PstFormat::U32(first, 4, "a heap header");

      for (size_t index = 0; index < blocks_.size(); index++)
      {
         const std::string &block = blocks_[index];
         const size_t pageMap = PstFormat::U16(block, 0, "a heap page");

         // [MS-PST] 2.3.1.5: HNPAGEMAP - cAlloc, cFree, then cAlloc + 1 offsets,
         // each allocation running from one to the next.
         const size_t count = PstFormat::U16(block, pageMap, "a heap page map");
         if (!PstFormat::Fits(pageMap + 4, (uint64_t) (count + 1) * 2, block.size()))
            throw PstError("A heap page of the file has a page map that runs past it.");

         std::vector<uint16_t> offsets;
         offsets.reserve(count + 1);
         for (size_t i = 0; i <= count; i++)
         {
            const uint16_t offset = PstFormat::U16(block, pageMap + 4 + i * 2, "a heap page map");
            if ((!offsets.empty() && offset < offsets.back()) || offset > pageMap)
               throw PstError("A heap page of the file has allocations out of order or over its page map.");
            offsets.push_back(offset);
         }

         maps_.push_back(offsets);
      }
   }

   std::string
   PstHeap::Allocation(uint32_t hid) const
   {
      if (hid == 0)
         return std::string();

      if (!IsHid(hid))
         throw PstError("A heap reference in the file is not one.");

      // [MS-PST] 2.3.1.1: hidType (5 bits), hidIndex (11 bits, from 1),
      // hidBlockIndex (16 bits).
      const size_t index = (hid >> 5) & 0x7FF;
      const size_t block = hid >> 16;

      if (block >= maps_.size() || index == 0 || index >= maps_[block].size())
         throw PstError("A heap reference in the file names no allocation.");

      const size_t start = maps_[block][index - 1];
      const size_t end = maps_[block][index];
      if (end > blocks_[block].size())
         throw PstError("A heap allocation of the file runs past its page.");

      if (budget_)
      {
         budget_->Item();
         budget_->Bytes(end - start);
      }

      return blocks_[block].substr(start, end - start);
   }

   void
   PstHeap::Walk_(uint32_t hid, int level, size_t keySize, size_t dataSize, size_t limit, std::set<uint32_t> &visited,
                  std::vector<std::pair<std::string, std::string> > &records) const
   {
      if (hid == 0)
         return;

      if (!visited.insert(hid).second)
         throw PstError("A B-tree in a heap of the file points back into itself.");

      const std::string data = Allocation(hid);

      if (level == 0)
      {
         const size_t size = keySize + dataSize;
         if (data.size() % size != 0)
            throw PstError("A B-tree leaf in a heap of the file is not a whole number of records.");

         const size_t count = data.size() / size;
         if (count > limit - records.size())
            throw PstError("A table or property set of the file has more entries than this reader takes.");

         for (size_t i = 0; i < count; i++)
            records.push_back(std::make_pair(data.substr(i * size, keySize), data.substr(i * size + keySize, dataSize)));
         return;
      }

      // [MS-PST] 2.3.2.2: an intermediate record is a key and the HID of the
      // level below.
      const size_t size = keySize + 4;
      if (data.size() % size != 0)
         throw PstError("A B-tree page in a heap of the file is not a whole number of records.");

      for (size_t i = 0; i < data.size() / size; i++)
         Walk_(PstFormat::U32(data, i * size + keySize, "a heap B-tree record"), level - 1, keySize, dataSize, limit, visited, records);
   }

   void
   PstHeap::BthRecords(uint32_t hid, size_t keySize, size_t dataSize, size_t limit,
                       std::vector<std::pair<std::string, std::string> > &records) const
   {
      records.clear();

      // [MS-PST] 2.3.2.1: BTHHEADER - bType, cbKey, cbEnt, bIdxLevels, hidRoot.
      const std::string header = Allocation(hid);
      if (header.size() < 8 || PstFormat::U8(header, 0, "a heap B-tree") != BthSignature)
         throw PstError("A B-tree in a heap of the file is not one.");

      if (PstFormat::U8(header, 1, "a heap B-tree") != keySize || PstFormat::U8(header, 2, "a heap B-tree") != dataSize)
         throw PstError("A B-tree in a heap of the file has records of the wrong size.");

      const int levels = PstFormat::U8(header, 3, "a heap B-tree");
      if (levels > MaximumBthLevels)
         throw PstError("A B-tree in a heap of the file is deeper than this reader takes.");

      std::set<uint32_t> visited;
      Walk_(PstFormat::U32(header, 4, "a heap B-tree"), levels, keySize, dataSize, limit, visited, records);
   }

   // ------------------------------------------------------- the property context

   PstPropertyContext::PstPropertyContext() :
      file_(nullptr)
   {
   }

   void
   PstPropertyContext::Read(PstFile &file, uint64_t bidData, uint64_t bidSub)
   {
      file_ = &file;
      entries_.clear();

      std::vector<std::string> blocks;
      file.ReadDataBlocks(bidData, MaximumHeapBytes, blocks);
      heap_.SetBudget(&file.Budget());
      heap_.Load(blocks);

      if (heap_.GetClientSignature() != ClientPropertyContext)
         throw PstError("A node of the file that should hold properties holds something else.");

      file.ReadSubnodes(bidSub, subnodes_);

      // [MS-PST] 2.3.3.3: PC BTH records - wPropId as the key, then wPropType
      // and dwValueHnid.
      std::vector<std::pair<std::string, std::string> > records;
      heap_.BthRecords(heap_.GetUserRoot(), 2, 6, 65536, records);

      for (const std::pair<std::string, std::string> &record : records)
      {
         Entry entry;
         entry.type = PstFormat::U16(record.second, 0, "a property record");
         entry.value = PstFormat::U32(record.second, 2, "a property record");
         entries_[PstFormat::U16(record.first, 0, "a property record")] = entry;
      }
   }

   bool
   PstPropertyContext::Has(uint16_t id) const
   {
      return entries_.find(id) != entries_.end();
   }

   bool
   PstPropertyContext::TypeOf(uint16_t id, uint16_t &type) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end())
         return false;
      type = found->second.type;
      return true;
   }

   std::string
   PstPropertyContext::Value_(uint32_t hnid, size_t limit) const
   {
      if (IsHid(hnid))
      {
         std::string value = heap_.Allocation(hnid);
         if (value.size() > limit)
            throw PstTooLarge("A value in the file is larger than this reader takes.");
         return value;
      }

      std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = subnodes_.find(hnid);
      if (subnode == subnodes_.end())
         throw PstError("A value of the file is in a subnode that is not there.");

      return file_->ReadData(subnode->second.data, limit);
   }

   bool
   PstPropertyContext::Int32(uint16_t id, int32_t &value) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end())
         return false;

      if (found->second.type == PstTypeInteger32 || found->second.type == PstTypeErrorCode)
         value = (int32_t) found->second.value;
      else if (found->second.type == PstTypeInteger16)
         value = (int16_t) (found->second.value & 0xFFFF);
      else if (found->second.type == PstTypeBoolean)
         value = (found->second.value & 0xFF) != 0 ? 1 : 0;
      else
         return false;

      return true;
   }

   bool
   PstPropertyContext::Boolean(uint16_t id, bool &value) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || found->second.type != PstTypeBoolean)
         return false;

      value = (found->second.value & 0xFF) != 0;
      return true;
   }

   bool
   PstPropertyContext::Time(uint16_t id, uint64_t &fileTime) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || (found->second.type != PstTypeTime && found->second.type != PstTypeInteger64))
         return false;

      const std::string value = Value_(found->second.value, 8);
      if (value.size() != 8)
         return false;

      fileTime = PstFormat::U64(value, 0, "a time property");
      return true;
   }

   bool
   PstPropertyContext::Bytes(uint16_t id, size_t limit, std::string &value) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || Inline(found->second.type))
         return false;

      value = Value_(found->second.value, limit);
      return true;
   }

   bool
   PstPropertyContext::Text(uint16_t id, int codePage, size_t limit, std::string &utf8, bool &decoded) const
   {
      decoded = true;

      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end())
         return false;

      if (found->second.type == PstTypeString)
      {
         utf8 = StripNul(PstFormat::Utf16ToUtf8(Value_(found->second.value, limit)));
         return true;
      }

      if (found->second.type == PstTypeString8)
      {
         utf8 = String8(Value_(found->second.value, limit), codePage, decoded);
         return true;
      }

      return false;
   }

   bool
   PstPropertyContext::Text(uint16_t id, size_t limit, std::string &utf8) const
   {
      bool decoded = true;
      return Text(id, 1252, limit, utf8, decoded);
   }

   bool
   PstPropertyContext::MultiText(uint16_t id, int codePage, size_t limit, std::vector<std::string> &values) const
   {
      values.clear();

      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || (found->second.type != PstTypeMultipleString && found->second.type != PstTypeMultipleString8))
         return false;

      std::vector<std::string> items;
      SplitMultiple(Value_(found->second.value, limit), items);

      for (const std::string &item : items)
      {
         bool decoded = true;
         values.push_back(found->second.type == PstTypeMultipleString ? StripNul(PstFormat::Utf16ToUtf8(item))
                                                                      : String8(item, codePage, decoded));
      }
      return true;
   }

   bool
   PstPropertyContext::MultiBinary(uint16_t id, size_t limit, std::vector<std::string> &values) const
   {
      values.clear();

      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || found->second.type != PstTypeMultipleBinary)
         return false;

      SplitMultiple(Value_(found->second.value, limit), values);
      return true;
   }

   bool
   PstPropertyContext::ObjectNid(uint16_t id, uint32_t &nid) const
   {
      std::map<uint16_t, Entry>::const_iterator found = entries_.find(id);
      if (found == entries_.end() || found->second.type != PstTypeObject)
         return false;

      // [MS-PST] 2.3.3.6: the value's HNID names eight bytes on the heap - the
      // subnode's NID and the object's size.
      const std::string reference = Value_(found->second.value, 64);
      if (reference.size() < 4)
         throw PstError("An embedded object's reference in the file is cut short.");

      nid = PstFormat::U32(reference, 0, "an embedded object's reference");
      return true;
   }

   // ------------------------------------------------------------------ the table

   PstTable::PstTable() :
      file_(nullptr),
      end_of_values_(0),
      row_size_(0),
      rows_hnid_(0),
      matrix_loaded_(false)
   {
   }

   void
   PstTable::Read(PstFile &file, uint64_t bidData, uint64_t bidSub, size_t rowLimit)
   {
      file_ = &file;
      columns_.clear();
      rows_.clear();
      matrix_.clear();
      matrix_loaded_ = false;

      std::vector<std::string> blocks;
      file.ReadDataBlocks(bidData, PstPropertyContext::MaximumHeapBytes, blocks);
      heap_.SetBudget(&file.Budget());
      heap_.Load(blocks);

      if (heap_.GetClientSignature() != ClientTable)
         throw PstError("A node of the file that should hold a table holds something else.");

      file.ReadSubnodes(bidSub, subnodes_);

      // [MS-PST] 2.3.4.1: TCINFO - bType, cCols, rgib (the ends of the
      // eight-and-four, two and one byte values and of the cell existence
      // bitmap), hidRowIndex, hnidRows, hidIndex, then the columns.
      const std::string info = heap_.Allocation(heap_.GetUserRoot());
      if (PstFormat::U8(info, 0, "a table's description") != ClientTable)
         throw PstError("A table of the file has no description.");

      const size_t count = PstFormat::U8(info, 1, "a table's description");
      const size_t end4 = PstFormat::U16(info, 2, "a table's description");
      const size_t end2 = PstFormat::U16(info, 4, "a table's description");
      const size_t end1 = PstFormat::U16(info, 6, "a table's description");
      const size_t endBitmap = PstFormat::U16(info, 8, "a table's description");
      const uint32_t rowIndex = PstFormat::U32(info, 10, "a table's description");
      rows_hnid_ = PstFormat::U32(info, 14, "a table's description");

      if (end4 > end2 || end2 > end1 || end1 > endBitmap || endBitmap - end1 < (count + 7) / 8)
         throw PstError("A table of the file describes rows that do not add up.");

      end_of_values_ = end1;
      row_size_ = endBitmap;

      for (size_t i = 0; i < count; i++)
      {
         // [MS-PST] 2.3.4.2: TCOLDESC - tag, ibData, cbData, iBit.
         const size_t at = 22 + i * 8;
         Column column;
         const uint32_t tag = PstFormat::U32(info, at, "a table's column");
         column.type = (uint16_t) (tag & 0xFFFF);
         column.id = (uint16_t) (tag >> 16);
         column.offset = PstFormat::U16(info, at + 4, "a table's column");
         column.size = PstFormat::U8(info, at + 6, "a table's column");
         column.bit = PstFormat::U8(info, at + 7, "a table's column");

         if ((size_t) column.offset + column.size > end1 || (size_t) column.bit >= (endBitmap - end1) * 8)
            throw PstError("A table of the file has a column outside its rows.");

         columns_.push_back(column);
      }

      // [MS-PST] 2.3.4.3: the row index, a BTH of row id to row number.
      std::vector<std::pair<std::string, std::string> > records;
      heap_.BthRecords(rowIndex, 4, 4, rowLimit, records);
      rows_.reserve(records.size());
      for (const std::pair<std::string, std::string> &record : records)
      {
         rows_.push_back(std::make_pair(PstFormat::U32(record.first, 0, "a table's row index"),
                                        PstFormat::U32(record.second, 0, "a table's row index")));
      }
   }

   uint32_t
   PstTable::RowId(size_t row) const
   {
      if (row >= rows_.size())
         throw PstError("A row that is not in the table was asked for.");
      return rows_[row].first;
   }

   std::string
   PstTable::Row_(size_t row)
   {
      if (row >= rows_.size())
         throw PstError("A row that is not in the table was asked for.");

      if (row_size_ == 0)
         throw PstError("A table of the file has rows of no size.");

      if (!matrix_loaded_)
      {
         matrix_.clear();
         if (rows_hnid_ == 0)
         {
            // No rows at all.
         }
         else if (IsHid(rows_hnid_))
         {
            matrix_.push_back(heap_.Allocation(rows_hnid_));
         }
         else
         {
            std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = subnodes_.find(rows_hnid_);
            if (subnode == subnodes_.end())
               throw PstError("A table of the file keeps its rows in a subnode that is not there.");
            file_->ReadDataBlocks(subnode->second.data, MaximumMatrixBytes, matrix_);
         }
         matrix_loaded_ = true;
      }

      const uint64_t index = rows_[row].second;

      if (IsHid(rows_hnid_))
      {
         if (matrix_.empty() || !PstFormat::Fits(index * row_size_, row_size_, matrix_[0].size()))
            throw PstError("A table of the file names a row it does not hold.");
         return matrix_[0].substr((size_t) (index * row_size_), row_size_);
      }

      // [MS-PST] 2.3.4.4: in a subnode, each block holds as many whole rows as
      // fit in a block - 8,176 bytes, or 8,180 in an ANSI file.
      const size_t perBlock = file_->GetMaximumBlockData() / row_size_;
      if (perBlock == 0)
         throw PstError("A table of the file has rows wider than a block.");

      const uint64_t block = index / perBlock;
      const uint64_t offset = (index % perBlock) * row_size_;
      if (block >= matrix_.size() || !PstFormat::Fits(offset, row_size_, matrix_[(size_t) block].size()))
         throw PstError("A table of the file names a row it does not hold.");

      return matrix_[(size_t) block].substr((size_t) offset, row_size_);
   }

   bool
   PstTable::Cell_(size_t row, uint16_t id, size_t limit, uint16_t &type, std::string &value)
   {
      const Column *column = nullptr;
      for (const Column &candidate : columns_)
      {
         if (candidate.id == id)
         {
            column = &candidate;
            break;
         }
      }

      if (!column)
         return false;

      // Every cell read is one item of the work budget, and its row's bytes
      // are counted: a table of a million rows that all name one row costs what
      // a million rows do.
      file_->Budget().Item();
      file_->Budget().Bytes(row_size_);

      const std::string data = Row_(row);

      // [MS-PST] 2.3.4.4.1: the cell existence bitmap, most significant bit
      // first.
      const unsigned char bits = (unsigned char) data[end_of_values_ + column->bit / 8];
      if ((bits & (0x80 >> (column->bit % 8))) == 0)
         return false;

      type = column->type;
      const std::string cell = data.substr(column->offset, column->size);

      if (FixedInRow(type))
      {
         value = cell;
         return true;
      }

      if (cell.size() != 4)
         throw PstError("A table of the file has a variable-size cell that is not a reference.");

      const uint32_t hnid = PstFormat::U32(cell, 0, "a table cell");
      if (IsHid(hnid))
      {
         value = heap_.Allocation(hnid);
         if (value.size() > limit)
            throw PstTooLarge("A table cell in the file is larger than this reader takes.");
         return true;
      }

      std::map<uint32_t, PstFile::Subnode>::const_iterator subnode = subnodes_.find(hnid);
      if (subnode == subnodes_.end())
         throw PstError("A table cell of the file is in a subnode that is not there.");

      value = file_->ReadData(subnode->second.data, limit);
      return true;
   }

   bool
   PstTable::CellInt32(size_t row, uint16_t id, int32_t &value)
   {
      uint16_t type = 0;
      std::string cell;
      if (!Cell_(row, id, MaximumCellBytes, type, cell))
         return false;

      if ((type == PstTypeInteger32 || type == PstTypeErrorCode) && cell.size() >= 4)
         value = (int32_t) PstFormat::U32(cell, 0, "a table cell");
      else if (type == PstTypeInteger16 && cell.size() >= 2)
         value = (int16_t) PstFormat::U16(cell, 0, "a table cell");
      else if (type == PstTypeBoolean && cell.size() >= 1)
         value = cell[0] != 0 ? 1 : 0;
      else
         return false;

      return true;
   }

   bool
   PstTable::CellText(size_t row, uint16_t id, int codePage, std::string &utf8, size_t limit)
   {
      uint16_t type = 0;
      std::string cell;
      if (!Cell_(row, id, limit, type, cell))
         return false;

      if (type == PstTypeString)
      {
         utf8 = StripNul(PstFormat::Utf16ToUtf8(cell));
         return true;
      }

      if (type == PstTypeString8)
      {
         bool decoded = true;
         utf8 = String8(cell, codePage, decoded);
         return true;
      }

      return false;
   }
}
