// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstItemsTester.h.

#include "StdAfx.h"

#include "PstItemsTester.h"
#include "PstFile.h"
#include "PstItems.h"
#include "PstLtp.h"
#include "PstRtf.h"
#include "PstStore.h"
#include "../Util/ContactCard.h"
#include "../Util/ICalendar.h"
#include "../Util/Unicode.h"
#include "../Util/VCard.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace HM
{
   namespace
   {
      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      bool Throws(const std::function<void()> &call)
      {
         try
         {
            call();
         }
         catch (PstError &)
         {
            return true;
         }
         return false;
      }

      // Whether the call throws PstStopped - which is not a PstError.
      bool ThrowsStopped(const std::function<void()> &call)
      {
         try
         {
            call();
         }
         catch (PstStopped &)
         {
            return true;
         }
         catch (PstError &)
         {
            return false;
         }
         return false;
      }

      // Whether the call throws PstBudgetExhausted, for the whole file or not.
      bool ThrowsExhausted(const std::function<void()> &call, bool wholeFile)
      {
         try
         {
            call();
         }
         catch (PstBudgetExhausted &exhausted)
         {
            return exhausted.WholeFile() == wholeFile;
         }
         catch (PstError &)
         {
            return false;
         }
         return false;
      }

      bool ThrowsTooLarge(const std::function<void()> &call)
      {
         try
         {
            call();
         }
         catch (PstTooLarge &)
         {
            return true;
         }
         catch (PstError &)
         {
            return false;
         }
         return false;
      }

      std::string Le16(uint32_t value)
      {
         std::string out;
         out += (char) (value & 0xFF);
         out += (char) ((value >> 8) & 0xFF);
         return out;
      }

      std::string Le32(uint32_t value)
      {
         return Le16(value & 0xFFFF) + Le16(value >> 16);
      }

      std::string Utf16(const char *ascii)
      {
         std::string out;
         for (const char *p = ascii; *p; p++)
         {
            out += *p;
            out += '\0';
         }
         return out;
      }

      void Put(std::string &buffer, size_t at, const std::string &bytes)
      {
         buffer.replace(at, bytes.size(), bytes);
      }

      uint32_t Hid(int index)
      {
         return (uint32_t) index << 5;
      }

      int64_t Days(int year, int month, int day)
      {
         year -= month <= 2 ? 1 : 0;
         const int64_t era = (year >= 0 ? year : year - 399) / 400;
         const int64_t yearOfEra = year - era * 400;
         const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
         return era * 146097 + yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear - 719468;
      }

      int64_t Utc(int year, int month, int day, int hour = 0, int minute = 0)
      {
         return Days(year, month, day) * 86400 + hour * 3600 + minute * 60;
      }

      uint32_t Minutes(int year, int month, int day, int hour = 0, int minute = 0)
      {
         return (uint32_t) (Utc(year, month, day, hour, minute) / 60 + 11644473600LL / 60);
      }

      uint64_t FileTime(int year, int month, int day, int hour = 0, int minute = 0)
      {
         return (uint64_t) (Utc(year, month, day, hour, minute) + 11644473600LL) * 10000000ULL;
      }

      // Whether the text, its folded lines joined again (RFC 5545 3.1, RFC 6350
      // 3.2), holds the part.
      bool Contains(const std::string &text, const char *part)
      {
         std::string unfolded;
         for (size_t i = 0; i < text.size(); i++)
         {
            if (text[i] == '\r' && i + 2 < text.size() && text[i + 1] == '\n' && (text[i + 2] == ' ' || text[i + 2] == '\t'))
            {
               i += 2;
               continue;
            }
            unfolded += text[i];
         }
         return unfolded.find(part) != std::string::npos;
      }

      // A heap-on-node of one block ([MS-PST] 2.3.1), as PstTester builds one.
      std::string HeapBlock(uint8_t client, uint32_t userRoot, const std::vector<std::string> &allocations)
      {
         std::string block(12, '\0');
         std::vector<uint16_t> offsets;
         for (const std::string &allocation : allocations)
         {
            offsets.push_back((uint16_t) block.size());
            block += allocation;
         }
         offsets.push_back((uint16_t) block.size());
         if (block.size() % 2)
            block += '\0';

         const uint16_t pageMap = (uint16_t) block.size();
         block += Le16((uint16_t) allocations.size()) + Le16(0);
         for (uint16_t offset : offsets)
            block += Le16(offset);

         Put(block, 0, Le16(pageMap));
         block[2] = (char) 0xEC;
         block[3] = (char) client;
         Put(block, 4, Le32(userRoot));
         return block;
      }

      // An ANSI PST built in memory from [MS-PST] 2.2: blocks with their
      // 12-byte trailers (cb, wSig, bid, dwCRC), a one-leaf block B-tree of
      // 12-byte entries, a node B-tree of one leaf of 16-byte entries - or of
      // two under a root whose second entry leads back to the root - and the
      // 512-byte ANSI header with its one CRC.
      class MemoryAnsiPst
      {
      public:
         explicit MemoryAnsiPst(int crypt) : crypt_(crypt), bytes_(1024, '\0') { }

         void Block(uint32_t bid, const std::string &data, bool breakCrc = false)
         {
            std::string stored = data;
            if ((bid & 2) == 0)
            {
               if (crypt_ == 1)
                  PstFormat::EncodePermute(stored);
               else if (crypt_ == 2)
                  PstFormat::CryptCyclic(stored, bid);
            }

            const uint32_t ib = (uint32_t) bytes_.size();
            const uint32_t crc = PstFormat::Crc(stored);
            if (breakCrc && !stored.empty())
               stored[0] = (char) (stored[0] ^ 0xFF);

            const size_t size = (stored.size() + 12 + 63) / 64 * 64;
            std::string block = stored;
            block.resize(size - 12, '\0');
            block += Le16((uint32_t) stored.size()) + Le16(Signature(ib, bid)) + Le32(bid) + Le32(crc);
            bytes_ += block;

            blocks_.push_back(Entry(bid, Le32(bid) + Le32(ib) + Le16((uint32_t) stored.size()) + Le16(2)));
         }

         void Node(uint32_t nid, uint32_t data, uint32_t sub, uint32_t parent)
         {
            nodes_.push_back(Entry(nid, Le32(nid) + Le32(data) + Le32(sub) + Le32(parent)));
         }

         std::string Finish(int version = 14, const char *client = "SM", int cryptByte = -1, bool loop = false, uint32_t endExtra = 0,
                            bool breakHeader = false)
         {
            while (bytes_.size() % 512)
               bytes_ += '\0';

            Sort(blocks_);
            Sort(nodes_);

            std::vector<std::string> blockEntries;
            for (const Entry &entry : blocks_)
               blockEntries.push_back(entry.second);
            const uint32_t bbtIb = (uint32_t) bytes_.size();
            const uint32_t bbtBid = 0x10000u << 2;
            bytes_ += Page(blockEntries, 12, 0, 0x80, bbtBid, bbtIb);

            uint32_t nbtIb = 0;
            uint32_t nbtBid = 0;
            if (!loop)
            {
               std::vector<std::string> nodeEntries;
               for (const Entry &entry : nodes_)
                  nodeEntries.push_back(entry.second);
               nbtIb = (uint32_t) bytes_.size();
               nbtBid = 0x10001u << 2;
               bytes_ += Page(nodeEntries, 16, 0, 0x81, nbtBid, nbtIb);
            }
            else
            {
               std::vector<std::string> low, high;
               uint32_t highKey = 0;
               for (const Entry &entry : nodes_)
               {
                  if (entry.first < 0x100)
                     low.push_back(entry.second);
                  else
                  {
                     if (high.empty())
                        highKey = entry.first;
                     high.push_back(entry.second);
                  }
               }

               const uint32_t lowIb = (uint32_t) bytes_.size();
               const uint32_t lowBid = 0x10002u << 2;
               bytes_ += Page(low, 16, 0, 0x81, lowBid, lowIb);

               nbtIb = (uint32_t) bytes_.size();
               nbtBid = 0x10003u << 2;
               std::vector<std::string> root;
               root.push_back(Le32(0) + Le32(lowBid) + Le32(lowIb));
               root.push_back(Le32(highKey) + Le32(nbtBid) + Le32(nbtIb));
               bytes_ += Page(root, 12, 1, 0x81, nbtBid, nbtIb);
            }

            std::string header(512, '\0');
            Put(header, 0, "!BDN");
            Put(header, 8, std::string(client, 2));
            Put(header, 10, Le16((uint32_t) version) + Le16(19));
            header[14] = 1;
            header[15] = 1;
            Put(header, 168, Le32((uint32_t) bytes_.size() + endExtra));
            Put(header, 184, Le32(nbtBid) + Le32(nbtIb) + Le32(bbtBid) + Le32(bbtIb));
            header[460] = (char) 0x80;
            header[461] = (char) (cryptByte >= 0 ? cryptByte : crypt_);
            Put(header, 4, Le32(PstFormat::Crc((const unsigned char *) header.data() + 8, 471)));
            if (breakHeader)
               header[4] = (char) (header[4] ^ 0xFF);

            std::string file = bytes_;
            file.replace(0, header.size(), header);
            return file;
         }

      private:
         typedef std::pair<uint32_t, std::string> Entry;

         static void Sort(std::vector<Entry> &entries)
         {
            std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.first < b.first; });
         }

         static uint32_t Signature(uint32_t ib, uint32_t bid)
         {
            const uint32_t x = ib ^ bid;
            return (x >> 16) ^ (x & 0xFFFF);
         }

         static std::string Page(const std::vector<std::string> &entries, size_t entrySize, int level, uint8_t type, uint32_t bid, uint32_t ib)
         {
            std::string page(496, '\0');
            for (size_t i = 0; i < entries.size(); i++)
               Put(page, i * entrySize, entries[i]);
            page += (char) entries.size();
            page += (char) (496 / entrySize);
            page += (char) entrySize;
            page += (char) level;
            const uint32_t crc = PstFormat::Crc(page);
            page += (char) type;
            page += (char) type;
            page += Le16(Signature(ib, bid));
            page += Le32(bid);
            page += Le32(crc);
            return page;
         }

         int crypt_;
         std::string bytes_;
         std::vector<Entry> blocks_;
         std::vector<Entry> nodes_;
      };

      int RefusalOf(const std::string &file)
      {
         try
         {
            PstFile pst(std::make_shared<PstMemorySource>(file));
            pst.Open();
         }
         catch (PstRefusal &refusal)
         {
            return (int) refusal.GetReason();
         }
         return 0;
      }

      void TestAnsiNdb()
      {
         for (int crypt = 0; crypt <= 2; crypt++)
         {
            MemoryAnsiPst pst(crypt);
            pst.Block(4, "hello, ansi");
            pst.Block(8, "abc");
            pst.Block(12, "defg");
            pst.Block(18, std::string("\x01\x01", 2) + Le16(2) + Le32(7) + Le32(8) + Le32(12));                  // an XBLOCK, 4-byte BIDs
            pst.Block(22, std::string("\x01\x02", 2) + Le16(1) + Le32(7) + Le32(18));                            // an XXBLOCK of it
            pst.Block(26, std::string("\x01\x01", 2) + Le16(1) + Le32(0xFFFFFFFF) + Le32(8));                    // claims 4 GB
            pst.Block(30, std::string("\x01\x01", 2) + Le16(1) + Le32(99) + Le32(8));                            // claims what it does not hold
            pst.Block(34, std::string("\x01\x02", 2) + Le16(1) + Le32(3) + Le32(34));                            // an XXBLOCK of itself
            pst.Block(38, std::string("\x02\x00", 2) + Le16(2) +
                          Le32(0x692) + Le32(4) + Le32(0) + Le32(0x671) + Le32(8) + Le32(0));                     // an SLBLOCK: no padding, 12-byte entries
            pst.Block(42, std::string("\x02\x01", 2) + Le16(1) + Le32(0x692) + Le32(38));                        // an SIBLOCK of it: 8-byte entries
            pst.Block(46, std::string("\x02\x01", 2) + Le16(1) + Le32(0x692) + Le32(46));                        // an SIBLOCK of itself
            pst.Block(50, std::string("\x02\x00", 2) + Le16(700) + Le32(0x692));                                 // an SLBLOCK that lists more than it holds
            pst.Block(52, "a block whose CRC fails", true);
            pst.Node(0x21, 4, 38, 0);
            pst.Node(0x61, 8, 0, 0);

            PstFile file(std::make_shared<PstMemorySource>(pst.Finish()));
            file.Open();
            Expect(file.IsAnsi() && !file.IsOffline() && file.GetVersion() == 14 && file.GetCryptMethod() == crypt,
                   "An ANSI header: the version, the encoding at offset 461.");
            Expect(file.GetMaximumBlockData() == 8180, "An ANSI block holds 8,180 bytes: 8,192 less a 12-byte trailer.");

            PstFile::Node node;
            Expect(file.FindNode(0x21, node) && node.data == 4 && node.sub == 38 && node.parent == 0, "A node is found in the ANSI NBT: 16-byte entries.");
            Expect(!file.FindNode(0x999, node), "A node that is not there is not found.");
            Expect(file.ReadData(4, 1024) == "hello, ansi", "An ANSI data block is decoded as the header says, its trailer's BID before its CRC.");
            Expect(file.ReadData(18, 1024) == "abcdefg", "An ANSI XBLOCK joins its blocks by four-byte BIDs.");
            Expect(file.ReadData(22, 1024) == "abcdefg", "An ANSI XXBLOCK joins its XBLOCKs.");
            Expect(ThrowsTooLarge([&]() { file.ReadData(26, 1024 * 1024); }), "An ANSI tree that claims 4 GB is refused before anything is allocated.");
            Expect(Throws([&]() { file.ReadData(30, 1024); }), "An ANSI tree that holds less than it claims is refused.");
            Expect(Throws([&]() { file.ReadData(34, 1024); }), "An ANSI XXBLOCK that lists itself is refused.");
            Expect(Throws([&]() { file.ReadData(52, 1024); }), "An ANSI block whose CRC fails is refused.");
            Expect(Throws([&]() { file.ReadData(0x400, 1024); }), "A block not in the ANSI BBT is refused.");
            Expect(Throws([&]() { file.ReadData(0x100000000ULL, 1024); }), "A BID wider than four bytes names no ANSI block.");

            std::map<uint32_t, PstFile::Subnode> subnodes;
            file.ReadSubnodes(38, subnodes);
            Expect(subnodes.size() == 2 && subnodes[0x692].data == 4 && subnodes[0x671].data == 8, "An ANSI SLBLOCK lists its subnodes.");
            file.ReadSubnodes(42, subnodes);
            Expect(subnodes.size() == 2, "An ANSI SIBLOCK lists its SLBLOCKs' subnodes.");
            Expect(Throws([&]() { file.ReadSubnodes(46, subnodes); }), "An ANSI SIBLOCK that lists itself is refused.");
            Expect(Throws([&]() { file.ReadSubnodes(50, subnodes); }), "An ANSI SLBLOCK that lists more than it holds is refused.");
            Expect(Throws([&]() { file.ReadSubnodes(4, subnodes); }), "A data block is not a subnode tree.");
         }

         // The header's refusals, and what it takes.
         {
            MemoryAnsiPst pst(1);
            pst.Block(4, "x");
            pst.Node(0x21, 4, 0, 0);
            Expect(RefusalOf(pst.Finish(14)) == 0 && RefusalOf(pst.Finish(15)) == 0, "Versions 14 and 15 are ANSI files, and are read.");
            Expect(RefusalOf(pst.Finish(14, "SO")) == 0, "An ANSI OST opens as an ANSI PST does.");
            Expect(RefusalOf(pst.Finish(14, "SM", 7)) == PstRefusal::ReasonUnknownFormat, "An unknown encoding in an ANSI header is refused.");
            Expect(RefusalOf(pst.Finish(14, "SM", 0x10)) == PstRefusal::ReasonProtected, "Windows Information Protection in an ANSI header is refused.");
            Expect(RefusalOf(pst.Finish(14, "SM", -1, false, 0, true)) == PstRefusal::ReasonDamaged, "An ANSI header whose CRC fails is refused.");
            Expect(RefusalOf(pst.Finish(14, "SM", -1, false, 4096)) == PstRefusal::ReasonDamaged, "An ANSI file shorter than its header says is refused.");

            PstFile offline(std::make_shared<PstMemorySource>(pst.Finish(15, "SO")));
            offline.Open();
            Expect(offline.IsAnsi() && offline.IsOffline(), "An ANSI OST says it is both.");
         }

         // A node B-tree whose root leads back to itself.
         {
            MemoryAnsiPst pst(1);
            pst.Block(4, "x");
            pst.Node(0x21, 4, 0, 0);
            pst.Node(0x61, 4, 0, 0);
            pst.Node(0x122, 4, 0, 0);
            pst.Node(0x8022, 4, 0, 0);
            PstFile file(std::make_shared<PstMemorySource>(pst.Finish(14, "SM", -1, true)));
            file.Open();
            PstFile::Node node;
            Expect(file.FindNode(0x21, node), "The half of a looping ANSI tree that does not loop is read.");
            Expect(Throws([&]() { PstFile::Node other; file.FindNode(0x8022, other); }), "An ANSI B-tree that points back at itself is refused.");
         }

         // A table whose rows are 20 bytes: an ANSI block holds 409 of them
         // (8,180 / 20), a Unicode one 408 - the 410th row is the first of the
         // second block only when the reader counts as the ANSI file does.
         {
            std::vector<std::string> allocations;
            std::string info = std::string("\x7C\x06", 2) + Le16(16) + Le16(18) + Le16(19) + Le16(20) + Le32(Hid(2)) + Le32(0x3F) + Le32(0);
            const struct { uint32_t tag; int offset; int size; int bit; } columns[] =
            {
               { 0x67F20003, 0, 4, 0 }, { 0x67F30003, 4, 4, 1 }, { 0x66000003, 8, 4, 2 }, { 0x66010003, 12, 4, 3 },
               { 0x66020002, 16, 2, 4 }, { 0x6603000B, 18, 1, 5 }
            };
            for (const auto &column : columns)
               info += Le32(column.tag) + Le16((uint32_t) column.offset) + std::string(1, (char) column.size) + std::string(1, (char) column.bit);
            allocations.push_back(info);
            allocations.push_back(std::string("\xB5\x04\x04\x00", 4) + Le32(Hid(3)));
            std::string index;
            std::string rows;
            for (uint32_t row = 0; row < 410; row++)
            {
               index += Le32(0x1000 + row) + Le32(row);
               rows += Le32(0x1000 + row) + Le32(1) + Le32(row * 3) + Le32(0) + Le16(0) + std::string(1, '\0') + std::string(1, (char) 0xFC);
            }
            allocations.push_back(index);

            MemoryAnsiPst pst(2);
            pst.Block(8, HeapBlock(0x7C, Hid(1), allocations));
            pst.Block(12, rows.substr(0, 409 * 20));
            pst.Block(16, rows.substr(409 * 20));
            pst.Block(22, std::string("\x01\x01", 2) + Le16(2) + Le32(410 * 20) + Le32(12) + Le32(16));
            pst.Block(26, std::string("\x02\x00", 2) + Le16(1) + Le32(0x3F) + Le32(22) + Le32(0));
            pst.Node(0x21, 8, 26, 0);

            PstFile file(std::make_shared<PstMemorySource>(pst.Finish()));
            file.Open();
            PstTable table;
            table.Read(file, 8, 26, 1000);
            int32_t value = -1;
            Expect(table.RowCount() == 410 && table.RowId(409) == 0x1000 + 409, "An ANSI table's row index.");
            Expect(table.CellInt32(408, 0x6600, value) && value == 408 * 3, "The last row of an ANSI table's first block.");
            Expect(table.CellInt32(409, 0x6600, value) && value == 409 * 3,
                   "An ANSI table's rows are packed 8,180 bytes to a block: the 410th of 20-byte rows opens the second block.");
         }

         // Every ANSI page, block and subnode entry is charged to the file's
         // work budget before it is read, and asks the job's stop - as a
         // Unicode file's are.
         {
            MemoryAnsiPst pst(1);
            pst.Block(4, "hello, ansi");
            pst.Block(38, std::string("\x02\x00", 2) + Le16(2) +
                          Le32(0x692) + Le32(4) + Le32(0) + Le32(0x671) + Le32(4) + Le32(0));
            pst.Node(0x21, 4, 38, 0);
            const std::string bytes = pst.Finish();

            PstFile stopped(std::make_shared<PstMemorySource>(bytes));
            stopped.Open();
            stopped.Budget().SetStop([]() { return true; });
            Expect(ThrowsStopped([&]() { PstFile::Node node; stopped.FindNode(0x21, node); }), "A stop ends an ANSI file's reading at its next page.");

            PstFile pages(std::make_shared<PstMemorySource>(bytes));
            pages.Open();
            pages.Budget().SetFileLimits(511, 0);
            Expect(ThrowsExhausted([&]() { PstFile::Node node; pages.FindNode(0x21, node); }, true),
                   "An ANSI page's 512 bytes are charged before it is read.");

            PstFile blocks(std::make_shared<PstMemorySource>(bytes));
            blocks.Open();
            blocks.Budget().SetFileLimits(0, 1);
            Expect(ThrowsExhausted([&]() { blocks.ReadData(4, 1024); }, true),
                   "An ANSI block is a read of its own: the block index's page and the block are two.");

            PstFile entries(std::make_shared<PstMemorySource>(bytes));
            entries.Open();
            entries.Budget().SetFileLimits(0, 3);
            std::map<uint32_t, PstFile::Subnode> subnodes;
            Expect(ThrowsExhausted([&]() { entries.ReadSubnodes(38, subnodes); }, true),
                   "Each entry of an ANSI SLBLOCK is a read: a page, a block and two entries are four.");

            PstFile enough(std::make_shared<PstMemorySource>(bytes));
            enough.Open();
            enough.Budget().SetFileLimits(0, 4);
            enough.ReadSubnodes(38, subnodes);
            Expect(subnodes.size() == 2, "... and four reads are enough for them.");
         }
      }

      // ---------------------------------------------------------------- the recurrence blob

      struct TestException
      {
         uint32_t start, end, original;
         const char *subject;
         const char *location;
      };

      // [MS-OXOCAL] 2.2.1.44.5, written as generate-pst-fixtures.py writes it.
      // fixedEnd is where ReservedBlock1 ends: what the reader must have.
      std::string Recurrence(uint16_t reader, uint16_t frequency, uint16_t pattern, uint32_t period, const std::string &specific,
                             uint32_t endType, uint32_t count, uint32_t startDate, uint32_t endDate, uint32_t startOffset, uint32_t endOffset,
                             const std::vector<uint32_t> &deleted, const std::vector<uint32_t> &modified,
                             const std::vector<TestException> &exceptions, size_t &fixedEnd, uint32_t extendedShift = 0)
      {
         std::string out = Le16(reader) + Le16(0x3004) + Le16(frequency) + Le16(pattern) + Le16(0);
         out += Le32(0) + Le32(period) + Le32(0) + specific;
         out += Le32(endType) + Le32(count) + Le32(1) + Le32((uint32_t) deleted.size());
         for (uint32_t date : deleted)
            out += Le32(date);
         out += Le32((uint32_t) modified.size());
         for (uint32_t date : modified)
            out += Le32(date);
         out += Le32(startDate) + Le32(endDate);
         out += Le32(0x3006) + Le32(0x3009) + Le32(startOffset) + Le32(endOffset) + Le16((uint32_t) exceptions.size());
         for (const TestException &exception : exceptions)
         {
            const int flags = (exception.subject ? 0x0001 : 0) | (exception.location ? 0x0010 : 0);
            out += Le32(exception.start) + Le32(exception.end) + Le32(exception.original) + Le16((uint32_t) flags);
            if (exception.subject)
               out += Le16((uint32_t) strlen(exception.subject) + 1) + Le16((uint32_t) strlen(exception.subject)) + exception.subject;
            if (exception.location)
               out += Le16((uint32_t) strlen(exception.location) + 1) + Le16((uint32_t) strlen(exception.location)) + exception.location;
         }
         out += Le32(0);
         fixedEnd = out.size();
         for (const TestException &exception : exceptions)
         {
            out += Le32(4) + Le32(0) + Le32(0);
            if (exception.subject || exception.location)
            {
               out += Le32(exception.start + extendedShift) + Le32(exception.end) + Le32(exception.original);
               if (exception.subject)
                  out += Le16((uint32_t) strlen(exception.subject) + 7) + Utf16(exception.subject) + Utf16(" (wide)");
               if (exception.location)
                  out += Le16((uint32_t) strlen(exception.location)) + Utf16(exception.location);
               out += Le32(0);
            }
         }
         return out + Le32(0);
      }

      std::string Weekly(size_t &fixedEnd, uint32_t extendedShift = 0)
      {
         std::vector<uint32_t> deleted;
         deleted.push_back(Minutes(2024, 3, 18));
         deleted.push_back(Minutes(2024, 3, 25));
         std::vector<uint32_t> modified(1, Minutes(2024, 3, 25));
         std::vector<TestException> exceptions;
         TestException moved = { Minutes(2024, 3, 25, 10), Minutes(2024, 3, 25, 10, 30), Minutes(2024, 3, 25, 9), "Moved", "Room 2" };
         exceptions.push_back(moved);
         return Recurrence(0x3004, 0x200B, 0x0001, 1, Le32(0x02), 0x2022, 6, Minutes(2024, 3, 4), Minutes(2024, 4, 8), 540, 570,
                           deleted, modified, exceptions, fixedEnd, extendedShift);
      }

      PstTimeZone WesternEurope()
      {
         // TZSTRUCT: UTC+1, +2 from the last Sunday of March at 02:00 to the
         // last Sunday of October at 03:00.
         std::string blob = Le32((uint32_t) -60) + Le32(0) + Le32((uint32_t) -60) + Le16(0) +
                            Le16(0) + Le16(10) + Le16(0) + Le16(5) + Le16(3) + Le16(0) + Le16(0) + Le16(0) + Le16(0) +
                            Le16(0) + Le16(3) + Le16(0) + Le16(5) + Le16(2) + Le16(0) + Le16(0) + Le16(0);
         PstTimeZone zone;
         Expect(PstItems::ParseTimeZoneStruct(blob, zone) && zone.HasDaylight(), "A TZSTRUCT is read.");
         zone.name = "W. Europe Standard Time";
         return zone;
      }

      void TestRecurrence()
      {
         size_t fixedEnd = 0;
         const std::string weekly = Weekly(fixedEnd);

         PstRecurrence recurrence;
         std::string problem;
         Expect(PstItems::ParseRecurrence(weekly, 1252, recurrence, problem), "[MS-OXOCAL]'s recurrence blob is read.");
         Expect(recurrence.frequency == 0x200B && recurrence.patternType == 1 && recurrence.dayMask == 0x02 && recurrence.occurrenceCount == 6 &&
                recurrence.startTimeOffset == 540 && recurrence.endTimeOffset == 570 && recurrence.deleted.size() == 2,
                "Its pattern, its count, its times of day and its removed occurrences.");
         Expect(recurrence.exceptions.size() == 1 && recurrence.exceptions[0].hasSubject && recurrence.exceptions[0].subject == "Moved (wide)" &&
                recurrence.exceptions[0].location == "Room 2", "A changed occurrence's subject and place: the Unicode ones of its ExtendedException.");

         // An ExtendedException whose times are not the ExceptionInfo's is not
         // believed: the eight-bit subject stands.
         Expect(PstItems::ParseRecurrence(Weekly(fixedEnd, 1), 1252, recurrence, problem) && recurrence.exceptions.size() == 1 &&
                recurrence.exceptions[0].subject == "Moved", "An ExtendedException that does not match is left, and the eight-bit subject kept.");

         // Cut short at every length: never a throw, never a read past the
         // end, and refused wherever what [MS-OXOCAL] requires is not all there.
         for (size_t length = 0; length < weekly.size(); length++)
         {
            bool read = false;
            try
            {
               read = PstItems::ParseRecurrence(weekly.substr(0, length), 1252, recurrence, problem);
            }
            catch (...)
            {
               Expect(false, "A recurrence blob cut short throws rather than being refused.");
            }
            if (length < fixedEnd)
               Expect(!read, "A recurrence blob cut short before its exceptions end is refused.");
         }

         std::vector<uint32_t> none;
         std::vector<TestException> noExceptions;
         auto refused = [&](const std::string &blob) { return !PstItems::ParseRecurrence(blob, 1252, recurrence, problem); };
         Expect(refused(Recurrence(0x3005, 0x200B, 1, 1, Le32(2), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A reader version that is not 0x3004 is refused.");
         Expect(refused(Recurrence(0x3004, 0x2009, 1, 1, Le32(2), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A frequency [MS-OXOCAL] does not name is refused.");
         Expect(refused(Recurrence(0x3004, 0x200C, 0x000A, 1, Le32(5), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A Hijri pattern is refused: an RRULE cannot say it.");
         Expect(refused(Recurrence(0x3004, 0x200A, 0, 1000, "", 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A daily period that is not a whole number of days is refused.");
         Expect(refused(Recurrence(0x3004, 0x200B, 1, 1, Le32(0), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A weekly pattern with no day is refused.");
         Expect(refused(Recurrence(0x3004, 0x200C, 3, 1, Le32(4) + Le32(6), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A sixth week of the month is refused.");
         Expect(refused(Recurrence(0x3004, 0x200C, 2, 1, Le32(32), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "A 32nd day of the month is refused.");
         Expect(refused(Recurrence(0x3004, 0x200B, 1, 1, Le32(2), 0x2024, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd)),
                "An end type [MS-OXOCAL] does not name is refused.");
         Expect(refused(Recurrence(0x3004, 0x200B, 1, 1, Le32(2), 0x2021, 6, Minutes(2024, 3, 4), Minutes(2024, 3, 1), 540, 570, none, none, noExceptions, fixedEnd)),
                "A recurrence that ends before it starts is refused.");
         Expect(refused(Recurrence(0x3004, 0x200B, 1, 1, Le32(2), 0x2022, 6, Minutes(2024, 3, 4), 0, 1500, 1530, none, none, noExceptions, fixedEnd)),
                "A start later in the day than a day is refused.");

         // Counts the blob cannot hold.
         std::string counted = Recurrence(0x3004, 0x200B, 1, 1, Le32(2), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, none, none, noExceptions, fixedEnd);
         std::string many = counted;
         Put(many, 38, Le32(0x40000000));
         Expect(refused(many), "A removed-occurrence count past what the blob holds is refused, not allocated.");
         many = counted;
         Put(many, 42, Le32(0x40000000));
         Expect(refused(many), "A changed-occurrence count past what the blob holds is refused.");
         many = weekly;
         Put(many, fixedEnd - 4 - (4 + 4 + 4 + 2 + 2 + 2 + 5 + 2 + 2 + 6) - 2, Le16(0xFFFF));
         Expect(refused(many), "An exception count past what the blob holds is refused.");
         many = weekly;
         Put(many, fixedEnd - 4 - 6 - 2 - 2 - 5 - 2, Le16(9));
         Expect(refused(many), "A subject longer than its own length says is refused.");

         // [MS-OXOCAL] 2.2.1.44.5: as many exceptions as changed occurrences.
         std::vector<uint32_t> twoDates;
         twoDates.push_back(Minutes(2024, 3, 18));
         twoDates.push_back(Minutes(2024, 3, 25));
         std::vector<TestException> oneChanged;
         TestException changedOne = { Minutes(2024, 3, 25, 10), Minutes(2024, 3, 25, 10, 30), Minutes(2024, 3, 25, 9), "Moved", nullptr };
         oneChanged.push_back(changedOne);
         Expect(refused(Recurrence(0x3004, 0x200B, 1, 1, Le32(2), 0x2022, 6, Minutes(2024, 3, 4), 0, 540, 570, twoDates, twoDates, oneChanged, fixedEnd)) &&
                problem.find("requires to be the same") != std::string::npos,
                "Two occurrences changed and one described is refused, never half a series.");

         // Given the file's budget, the blob is charged and asks the job's
         // stop; what the budget and the stop throw is thrown, never taken for
         // a blob cut short.
         PstBudget stop;
         stop.SetStop([]() { return true; });
         Expect(ThrowsStopped([&]() { PstItems::ParseRecurrence(weekly, 1252, recurrence, problem, &stop); }),
                "A stop reaches the recurrence blob's reading.");
         PstBudget bytes;
         bytes.SetFileLimits(weekly.size() - 1, 0);
         Expect(ThrowsExhausted([&]() { PstItems::ParseRecurrence(weekly, 1252, recurrence, problem, &bytes); }, true),
                "The blob's bytes are charged.");
         PstBudget reads;
         reads.SetFileLimits(0, 1);
         Expect(ThrowsExhausted([&]() { PstItems::ParseRecurrence(weekly, 1252, recurrence, problem, &reads); }, true),
                "Each changed occurrence is a read of its own, charged inside the blob's own catch and still thrown.");
         PstBudget enough;
         enough.SetFileLimits(0, 2);
         Expect(PstItems::ParseRecurrence(weekly, 1252, recurrence, problem, &enough) && recurrence.exceptions.size() == 1,
                "... and the blob and its one changed occurrence are two.");
      }

      void TestTimeZones()
      {
         const PstTimeZone zone = WesternEurope();
         Expect(zone.StandardOffset() == 60 && zone.DaylightOffset() == 120, "UTC+1, and +2 in summer.");
         Expect(PstItems::NthWeekday(2024, 3, 0, 5) == 31 && PstItems::NthWeekday(2024, 10, 0, 5) == 27 && PstItems::NthWeekday(2024, 3, 2, 2) == 12,
                "The last Sunday of March and October 2024, and the second Tuesday of March.");
         Expect(PstItems::LocalToUtc(zone, Utc(2024, 3, 25, 9)) == Utc(2024, 3, 25, 8), "09:00 on 25 March 2024 in Berlin is 08:00 UTC.");
         Expect(PstItems::LocalToUtc(zone, Utc(2024, 4, 1, 9)) == Utc(2024, 4, 1, 7), "09:00 on 1 April 2024, after the change, is 07:00 UTC.");
         Expect(PstItems::UtcToLocal(zone, Utc(2024, 3, 28, 23)) == Utc(2024, 3, 29), "23:00 UTC on 28 March is midnight on the 29th.");
         Expect(PstItems::UtcToLocal(zone, Utc(2024, 7, 1, 22)) == Utc(2024, 7, 2), "22:00 UTC on 1 July is midnight on the 2nd.");

         // The south: AUS Eastern, UTC+10, +11 from the first Sunday of October
         // to the first Sunday of April.
         std::string blob = Le32((uint32_t) -600) + Le32(0) + Le32((uint32_t) -60) + Le16(0) +
                            Le16(0) + Le16(4) + Le16(0) + Le16(1) + Le16(3) + Le16(0) + Le16(0) + Le16(0) + Le16(0) +
                            Le16(0) + Le16(10) + Le16(0) + Le16(1) + Le16(2) + Le16(0) + Le16(0) + Le16(0);
         PstTimeZone south;
         Expect(PstItems::ParseTimeZoneStruct(blob, south), "A southern zone is read.");
         Expect(PstItems::LocalToUtc(south, Utc(2024, 1, 15, 9)) == Utc(2024, 1, 14, 22), "January is summer in Sydney: UTC+11.");
         Expect(PstItems::LocalToUtc(south, Utc(2024, 7, 15, 9)) == Utc(2024, 7, 14, 23), "July is winter: UTC+10.");

         // What is not a zone.
         std::string bad = blob;
         Put(bad, 16, Le16(13));
         Expect(!PstItems::ParseTimeZoneStruct(bad, south), "A thirteenth month is not a zone.");
         Expect(!PstItems::ParseTimeZoneStruct(blob.substr(0, 40), south), "A TZSTRUCT cut short is not a zone.");
         bad = blob;
         Put(bad, 0, Le32(5000));
         Expect(!PstItems::ParseTimeZoneStruct(bad, south), "A bias of more than a day is not a zone.");

         // TZDEFINITION: the key name, and the rule for the year.
         const std::string rule2006 = std::string("\x02\x01", 2) + Le16(0x3E) + Le16(0) + Le16(2000) + std::string(14, '\0') +
                                      Le32((uint32_t) -60) + Le32(0) + Le32(0) + std::string(32, '\0');
         const std::string rule2007 = std::string("\x02\x01", 2) + Le16(0x3E) + Le16(2) + Le16(2007) + std::string(14, '\0') +
                                      blob.substr(0, 12) + blob.substr(14, 16) + blob.substr(32, 16);
         const std::string key = "Test Zone";
         const std::string header = Le16(2) + Le16((uint32_t) key.size()) + Utf16(key.c_str()) + Le16(2);
         const std::string definition = std::string("\x02\x01", 2) + Le16((uint32_t) header.size()) + header + rule2006 + rule2007;
         PstTimeZone defined;
         Expect(PstItems::ParseTimeZoneDefinition(definition, 2024, defined) && defined.name == "Test Zone" && defined.HasDaylight() &&
                defined.StandardOffset() == 600, "A TZDEFINITION: its key name, and the latest rule that starts by the year.");
         Expect(PstItems::ParseTimeZoneDefinition(definition, 2003, defined) && !defined.HasDaylight() && defined.StandardOffset() == 60,
                "An earlier year takes the earlier rule.");
         Expect(!PstItems::ParseTimeZoneDefinition(definition.substr(0, definition.size() - 1), 2024, defined), "A TZDEFINITION cut short is refused.");
         std::string wrongCount = definition;
         Put(wrongCount, 4 + header.size() - 2, Le16(900));
         Expect(!PstItems::ParseTimeZoneDefinition(wrongCount, 2024, defined), "More rules than the definition holds are refused.");

         // Given the file's budget, each zone is charged and asks the stop.
         PstBudget stop;
         stop.SetStop([]() { return true; });
         Expect(ThrowsStopped([&]() { PstItems::ParseTimeZoneStruct(blob, south, &stop); }), "A stop reaches a TZSTRUCT's reading.");
         PstBudget rules;
         rules.SetFileLimits(0, 2);
         Expect(ThrowsExhausted([&]() { PstItems::ParseTimeZoneDefinition(definition, 2024, defined, &rules); }, true),
                "Each rule of a TZDEFINITION is a read, and the budget running out is thrown, never taken for a zone that is not one.");
      }

      void TestIdentifiers()
      {
         const std::string arrayId("\x04\x00\x00\x00\x82\x00\xE0\x00\x74\xC5\xB7\x10\x1A\x82\xE0\x08", 16);
         const std::string data = std::string("vCal-Uid\x01\x00\x00\x00", 12) + "kickoff@example.org" + std::string(1, '\0');
         const std::string goid = arrayId + std::string("\x07\xE8\x03\x0E", 4) + std::string(16, '\0') + Le32((uint32_t) data.size()) + data;
         Expect(PstItems::UidOfGlobalObjectId(goid) == "kickoff@example.org", "A global object id that carries a UID: that UID ([MS-OXCICAL]).");

         const std::string plain = arrayId + std::string("\x07\xE8\x03\x0E", 4) + std::string(16, '\x11') + Le32(2) + std::string("\xAB\xCD", 2);
         const std::string hex = PstItems::UidOfGlobalObjectId(plain);
         Expect(hex.size() == plain.size() * 2 && hex.compare(32, 8, "00000000") == 0 && hex.compare(hex.size() - 4, 4, "ABCD") == 0,
                "Any other: its bytes in upper-case hex, the instance's date zeroed.");
         Expect(PstItems::UidOfGlobalObjectId(std::string(40, '\0')).empty(), "Bytes that are not a global object id name no UID.");
         Expect(PstItems::UidOfGlobalObjectId(arrayId + std::string(20, '\0') + Le32(99)).empty(), "A global object id whose data runs past it names none.");

         Expect(PstItems::IsNoDate(FileTime(4501, 1, 1)) && !PstItems::IsNoDate(FileTime(2024, 3, 1)), "Outlook's 4501-01-01 is no date.");
         Expect(PstItems::ImageType(std::string("\xFF\xD8\xFF\xE0", 4)) == "image/jpeg" && PstItems::ImageType("GIF89a...") == "image/gif" &&
                PstItems::ImageType(std::string("\x89PNG\r\n\x1a\n", 8)) == "image/png" && PstItems::ImageType("BM....").empty(),
                "A picture's type from its bytes, and nothing else.");
         Expect(PstStore::IsMessageLikeFolderClass("IPF.StickyNote") && PstStore::IsMessageLikeFolderClass("IPF.Journal") &&
                !PstStore::IsMessageLikeFolderClass("IPF.Contact") && !PstStore::IsMessageLikeFolderClass(""), "Notes and journals are taken as messages.");
      }

      void TestAppointments()
      {
         size_t fixedEnd = 0;
         PstAppointment weekly;
         weekly.messageClass = "IPM.Appointment";
         weekly.subject = "Weekly sync";
         weekly.location = "Room 1";
         weekly.body = "Agenda; items, and more\r\n";
         weekly.start = FileTime(2024, 3, 4, 8);
         weekly.end = FileTime(2024, 3, 4, 8, 30);
         weekly.recurring = true;
         weekly.recurrenceBlob = Weekly(fixedEnd);
         weekly.zone = WesternEurope();
         weekly.reminderSet = true;
         weekly.reminderMinutes = 10;
         weekly.organizerName = "Alice \"the\" Example";
         weekly.organizerAddress = "alice@example.org";
         PstAttendee bob;
         bob.name = "Bob";
         bob.address = "bob@example.org";
         bob.trackStatus = 3;
         weekly.attendees.push_back(bob);

         std::string text, word, problem;
         Expect(PstItems::AppointmentText(weekly, "pst-fallback", text, word, problem), "A recurring appointment becomes iCalendar.");
         Expect(Contains(text, "RRULE:FREQ=WEEKLY;INTERVAL=1;BYDAY=MO;WKST=MO;COUNT=6\r\n") &&
                Contains(text, "EXDATE;TZID=W. Europe Standard Time:20240318T090000\r\n") &&
                Contains(text, "RECURRENCE-ID;TZID=W. Europe Standard Time:20240325T090000\r\n") &&
                Contains(text, "DTSTART;TZID=W. Europe Standard Time:20240325T100000\r\n") &&
                Contains(text, "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\n") && Contains(text, "TZOFFSETTO:+0200\r\n") &&
                Contains(text, "PARTSTAT=ACCEPTED") && Contains(text, "TRIGGER:-PT10M\r\n") && Contains(text, "Agenda\\; items\\, and more"),
                "Its rule, its removed and changed occurrences, its zone, its people, its reminder and its escaped text.");
         Expect(!Contains(text, "\"the\""), "A quote cannot close a parameter from inside a name.");

         // What CalDAV makes of it: parsed, summarised, and expanded across
         // the clock change at 09:00 Berlin time, the moved one where it went.
         ICalComponent tree;
         AnsiString parseProblem;
         ICalendar::Summary summary;
         Expect(ICalendar::Parse(AnsiString(text), tree, parseProblem) && ICalendar::Summarize(tree, summary, parseProblem) &&
                summary.component == "VEVENT" && summary.recurring, "This server's own CalDAV check takes it.");
         std::vector<ICalInstance> instances;
         bool truncated = false;
         Expect(ICalendar::Expand(tree, Utc(2024, 3, 1), Utc(2024, 4, 15), 100, instances, truncated, parseProblem) && instances.size() == 5,
                "Six weekly occurrences, one removed.");
         Expect(instances[0].start == Utc(2024, 3, 4, 8) && instances[1].start == Utc(2024, 3, 11, 8) && instances[2].start == Utc(2024, 3, 25, 9) &&
                instances[2].overridden && instances[3].start == Utc(2024, 4, 1, 7) && instances[4].start == Utc(2024, 4, 8, 7),
                "09:00 in Berlin before and after the clock changes, and the moved one at 10:00.");

         // A monthly "day 30": the last day of a shorter month, as Outlook has it.
         std::vector<uint32_t> none;
         std::vector<TestException> noExceptions;
         PstAppointment monthly = weekly;
         monthly.attendees.clear();
         monthly.organizerAddress.clear();
         monthly.recurrenceBlob = Recurrence(0x3004, 0x200C, 2, 1, Le32(30), 0x2021, 0, Minutes(2024, 1, 30), Minutes(2024, 4, 30), 600, 660,
                                             none, none, noExceptions, fixedEnd);
         Expect(PstItems::AppointmentText(monthly, "pst-fallback", text, word, problem) &&
                Contains(text, "BYMONTHDAY=28,29,30;BYSETPOS=-1;UNTIL=20240430T080000Z"), "Day 30 of every month, until the last one's start in UTC.");
         Expect(ICalendar::Parse(AnsiString(text), tree, parseProblem) &&
                ICalendar::Expand(tree, Utc(2024, 1, 1), Utc(2024, 6, 1), 100, instances, truncated, parseProblem) && instances.size() == 4 &&
                instances[1].start == Utc(2024, 2, 29, 9), "Four occurrences, February's on its 29th.");

         // Every second Tuesday, and yearly on the last Friday of November.
         PstAppointment nth = monthly;
         nth.recurrenceBlob = Recurrence(0x3004, 0x200D, 3, 12, Le32(0x20) + Le32(5), 0x2023, 0, Minutes(2024, 11, 29), 0x5AE980DF, 600, 660,
                                         none, none, noExceptions, fixedEnd);
         Expect(PstItems::AppointmentText(nth, "pst-fallback", text, word, problem) &&
                Contains(text, "RRULE:FREQ=YEARLY;INTERVAL=1;BYMONTH=11;BYDAY=FR;BYSETPOS=-1\r\n"), "The last Friday of November, every year.");

         // An all-day day in its zone, and an appointment with a time alone.
         PstAppointment holiday;
         holiday.subject = "Holiday";
         holiday.start = FileTime(2024, 3, 28, 23);
         holiday.end = FileTime(2024, 3, 29, 23);
         holiday.allDay = true;
         holiday.startZone = WesternEurope();
         holiday.busyStatus = 0;
         Expect(PstItems::AppointmentText(holiday, "pst-holiday", text, word, problem) && Contains(text, "DTSTART;VALUE=DATE:20240329\r\n") &&
                Contains(text, "DTEND;VALUE=DATE:20240330\r\n") && Contains(text, "TRANSP:TRANSPARENT") && Contains(text, "UID:pst-holiday"),
                "An all-day appointment is its own day where it was made.");

         PstAppointment plain;
         plain.subject = "Dentist";
         plain.start = FileTime(2024, 3, 7, 15);
         plain.end = FileTime(2024, 3, 7, 16);
         plain.sensitivity = 2;
         Expect(PstItems::AppointmentText(plain, "pst-x", text, word, problem) && Contains(text, "DTSTART:20240307T150000Z\r\n") &&
                Contains(text, "CLASS:PRIVATE") && !Contains(text, "VTIMEZONE"), "A single appointment in UTC.");

         // What is refused, with its word.
         PstAppointment timeless;
         timeless.subject = "No time";
         Expect(!PstItems::AppointmentText(timeless, "pst-x", text, word, problem) && word == "no_start", "An appointment with no time: no_start.");
         PstAppointment broken = weekly;
         broken.recurrenceBlob = weekly.recurrenceBlob.substr(0, 20);
         Expect(!PstItems::AppointmentText(broken, "pst-x", text, word, problem) && word == "recurrence_unreadable", "A recurrence cut short: recurrence_unreadable.");
         broken.recurrenceBlob.clear();
         Expect(!PstItems::AppointmentText(broken, "pst-x", text, word, problem) && word == "recurrence_unreadable", "Recurring, with no pattern: the same.");

         // With no zone at all, the offset between the pattern and the start.
         PstAppointment zoneless = monthly;
         zoneless.zone = PstTimeZone();
         zoneless.start = FileTime(2024, 1, 30, 8);
         Expect(PstItems::AppointmentText(zoneless, "pst-x", text, word, problem) && Contains(text, "TZID:UTC+0200") &&
                Contains(text, "UNTIL=20240430T080000Z"), "A series with no zone is placed at the offset its start shows.");

         // Each changed occurrence repeats the series' description: 200 KB of
         // it and ten changed occurrences would be 2 MB, and are refused as
         // too_large before the copies are made.
         std::vector<uint32_t> tenDates;
         std::vector<TestException> tenChanged;
         for (uint32_t i = 0; i < 10; i++)
         {
            const uint32_t original = Minutes(2024, 3, 4, 9) + i * 7 * 1440;
            tenDates.push_back(original - original % 1440);
            TestException changed = { original + 60, original + 90, original, "Moved", nullptr };
            tenChanged.push_back(changed);
         }
         PstAppointment heavy = weekly;
         heavy.recurrenceBlob = Recurrence(0x3004, 0x200B, 0x0001, 1, Le32(0x02), 0x2022, 20, Minutes(2024, 3, 4), 0, 540, 570,
                                           tenDates, tenDates, tenChanged, fixedEnd);
         heavy.body = std::string(200 * 1024, 'x');
         Expect(!PstItems::AppointmentText(heavy, "pst-x", text, word, problem) && word == "too_large",
                "Ten changed occurrences of a 200 KB description: too_large, counted before they are built.");
         heavy.body = "Agenda";
         Expect(PstItems::AppointmentText(heavy, "pst-x", text, word, problem) && text.size() < PstItems::MaximumItemTextBytes &&
                Contains(text, "RECURRENCE-ID;TZID=W. Europe Standard Time:20240506T090000\r\n"),
                "The same series with a short description is written, its ten changed occurrences with it.");

         // Given the file's budget, the stop reaches the building of it.
         PstBudget stop;
         stop.SetStop([]() { return true; });
         Expect(ThrowsStopped([&]() { PstItems::AppointmentText(heavy, "pst-x", text, word, problem, &stop); }),
                "A stop reaches an appointment's recurrence as it is written.");

         // A zone's name is cut to 100 bytes at a character, never inside one.
         PstAppointment longZone = monthly;
         longZone.zone.name = std::string(99, 'a') + "\xC3\xA9";
         Expect(PstItems::AppointmentText(longZone, "pst-x", text, word, problem) &&
                Contains(text, ("TZID:" + std::string(99, 'a') + "\r\n").c_str()),
                "A two-byte character that would end past 100 bytes is left out whole.");

         // A zone whose transitions are dates, a year given: read as those
         // dates every year, by the offsets and by the VTIMEZONE alike.
         const std::string fixedBlob = Le32((uint32_t) -60) + Le32(0) + Le32((uint32_t) -60) + Le16(0) +
                                       Le16(2010) + Le16(10) + Le16(0) + Le16(31) + Le16(3) + Le16(0) + Le16(0) + Le16(0) + Le16(0) +
                                       Le16(2010) + Le16(3) + Le16(0) + Le16(28) + Le16(2) + Le16(0) + Le16(0) + Le16(0);
         PstTimeZone fixedZone;
         Expect(PstItems::ParseTimeZoneStruct(fixedBlob, fixedZone) && fixedZone.HasDaylight(), "A zone of fixed dates is read.");
         fixedZone.name = "Fixed Zone";
         Expect(PstItems::LocalToUtc(fixedZone, Utc(2024, 3, 29, 9)) == Utc(2024, 3, 29, 7) &&
                PstItems::LocalToUtc(fixedZone, Utc(2024, 3, 22, 9)) == Utc(2024, 3, 22, 8),
                "Its offsets: daylight time from 28 March, in 2024 as in 2010.");
         PstAppointment fixedSeries = monthly;
         fixedSeries.zone = fixedZone;
         fixedSeries.recurrenceBlob = Recurrence(0x3004, 0x200B, 1, 1, Le32(0x20), 0x2022, 2, Minutes(2024, 3, 22), 0, 540, 570,
                                                 none, none, noExceptions, fixedEnd);
         Expect(PstItems::AppointmentText(fixedSeries, "pst-x", text, word, problem) &&
                Contains(text, "RRULE:FREQ=YEARLY;BYMONTH=3;BYMONTHDAY=28\r\n") && Contains(text, "RRULE:FREQ=YEARLY;BYMONTH=10;BYMONTHDAY=-1\r\n"),
                "Its VTIMEZONE says the same dates every year, the 31st of October as the month's last day.");
         Expect(ICalendar::Parse(AnsiString(text), tree, parseProblem) &&
                ICalendar::Expand(tree, Utc(2024, 3, 1), Utc(2024, 4, 15), 100, instances, truncated, parseProblem) && instances.size() == 2 &&
                instances[0].start == Utc(2024, 3, 22, 8) && instances[1].start == Utc(2024, 3, 29, 7),
                "And a calendar reads it as the offsets do: 09:00 is 08:00 UTC before 28 March and 07:00 after.");

         // The 28th of February given in a common year is the 28th in a leap
         // year too, not the month's last day.
         std::string february = fixedBlob;
         Put(february, 32, Le16(2010) + Le16(2) + Le16(0) + Le16(28));
         PstTimeZone februaryZone;
         Expect(PstItems::ParseTimeZoneStruct(february, februaryZone), "A zone whose daylight time starts on the 28th of February is read.");
         februaryZone.name = "February Zone";
         fixedSeries.zone = februaryZone;
         Expect(PstItems::AppointmentText(fixedSeries, "pst-x", text, word, problem) &&
                Contains(text, "RRULE:FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=28\r\n") && !Contains(text, "BYMONTH=2;BYMONTHDAY=-1"),
                "The 28th of February is the 28th, as TransitionIn reads it in 2024.");
      }

      void TestNamesAndBodies()
      {
         // A name for a column of 255: cut at a character, one past U+FFFF
         // counting two UTF-16 units.
         const std::string wide = "\xF0\x9F\x98\x80";
         Expect(PstItems::CutToUnits(std::string(300, 'a'), 255) == std::string(255, 'a'), "300 characters are cut to 255.");
         Expect(PstItems::CutToUnits(std::string(254, 'a') + wide, 255) == std::string(254, 'a'),
                "A character that would take the 256th unit is left out whole.");
         Expect(PstItems::CutToUnits(std::string(253, 'a') + wide + "b", 255) == std::string(253, 'a') + wide,
                "... and kept where both its units fit.");
         Expect(PstItems::CutToUnits(std::string(254, 'a') + "\xC3\xA9" + "b", 255) == std::string(254, 'a') + "\xC3\xA9",
                "A two-byte character is one unit.");
         Expect(PstItems::CutToUnits("ab", 255) == "ab" && PstItems::CutToUnits("", 255).empty(), "What fits is kept.");
         Expect(PstItems::CutToUnits(std::string("\xC3", 1) + "x", 1) == std::string("\xC3", 1),
                "A byte that leads no whole sequence is a character of its own.");

         // The contacts folders Exchange and Outlook keep for themselves.
         Expect(PstStore::IsSystemContactsClass("IPF.Contact.RecipientCache") && PstStore::IsSystemContactsClass("IPF.Contact.MOC.QuickContacts") &&
                PstStore::IsSystemContactsClass("IPF.Contact.GalContacts") && !PstStore::IsSystemContactsClass("IPF.Contact") &&
                !PstStore::IsSystemContactsClass("IPF.Contact.") && !PstStore::IsSystemContactsClass("IPF.Appointment.Birthday"),
                "IPF.Contact's sub-classes are the system's; IPF.Contact is a person's.");

         // A body kept only as RTF: its text, through the mail phase's reader.
         std::string text;
         std::string problem;
         Expect(PstItems::RtfBodyText(PstRtf::CompressLiterals("{\\rtf1\\ansi\\ansicpg1252 Bring the samples.\\par}"), text, problem) &&
                text.find("Bring the samples.") != std::string::npos, "A body written in RTF comes in as its text.");
         Expect(!PstItems::RtfBodyText(PstRtf::CompressLiterals("{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 {\\*\\htmltag19 <html>}Hi{\\*\\htmltag27 </html>}}"),
                                       text, problem) && problem.find("HTML") != std::string::npos,
                "HTML carried in RTF is not taken for plain text.");
         Expect(!PstItems::RtfBodyText("not compressed RTF", text, problem) && !problem.empty(), "What is not compressed RTF is refused, with the reason.");

         // Charged after its header is checked, and before it is decompressed.
         PstBudget tight;
         tight.SetFileLimits(1, 0);
         Expect(!PstItems::RtfBodyText("short", text, problem, &tight), "A value shorter than a header costs nothing, and is refused.");
         const std::string written = "{\\rtf1\\ansi\\ansicpg1252 " + std::string(200, 'x') + "\\par}";
         PstBudget limited;
         limited.SetFileLimits(written.size() - 1, 0);
         Expect(ThrowsExhausted([&]() { PstItems::RtfBodyText(PstRtf::CompressLiterals(written), text, problem, &limited); }, true),
                "What the RTF says it holds is charged before it is decompressed.");

         // A header that claims 4 GB is charged nine times its value's bytes,
         // not the 4 MB ceiling: 180 for 20 bytes.
         const std::string claiming = Le32(16) + Le32(0xFFFFFFFF) + std::string("LZFu", 4) + Le32(0) + std::string(4, 'x');
         PstBudget nine;
         nine.SetFileLimits(180, 0);
         bool withinNine = true;
         try
         {
            PstItems::RtfBodyText(claiming, text, problem, &nine);
         }
         catch (PstBudgetExhausted &)
         {
            withinNine = false;
         }
         Expect(withinNine, "An RTF that claims 4 GB costs at most nine times its own bytes.");

         // A value of a header's length whose type is neither LZFu nor MELA
         // costs nothing.
         PstBudget one;
         one.SetFileLimits(1, 0);
         Expect(!PstItems::RtfBodyText(Le32(16) + Le32(16) + std::string("ABCD", 4) + Le32(0) + std::string(4, 'x'), text, problem, &one),
                "A header of another type is refused before anything is charged.");
      }

      void TestTasksAndContacts()
      {
         PstTask task;
         task.subject = "File the report";
         task.start = FileTime(2024, 3, 10);
         task.due = FileTime(2024, 3, 15);
         task.status = 1;
         task.percent = 50;
         task.importance = 2;
         std::string text = PstItems::TaskText(task, "pst-task");
         Expect(Contains(text, "BEGIN:VTODO") && Contains(text, "DTSTART;VALUE=DATE:20240310") && Contains(text, "DUE;VALUE=DATE:20240315") &&
                Contains(text, "STATUS:IN-PROCESS") && Contains(text, "PERCENT-COMPLETE:50") && Contains(text, "PRIORITY:1"), "A task as a VTODO.");
         ICalComponent tree;
         AnsiString problem;
         ICalendar::Summary summary;
         Expect(ICalendar::Parse(AnsiString(text), tree, problem) && ICalendar::Summarize(tree, summary, problem) && summary.component == "VTODO",
                "This server's own CalDAV check takes it.");

         PstTask done;
         done.subject = "Renew";
         done.due = FileTime(4501, 1, 1);
         done.start = FileTime(2024, 3, 1);
         done.complete = true;
         done.completedAt = FileTime(2024, 3, 2);
         text = PstItems::TaskText(done, "pst-done");
         Expect(!Contains(text, "DUE") && Contains(text, "STATUS:COMPLETED") && Contains(text, "COMPLETED:20240302T000000Z") &&
                Contains(text, "PERCENT-COMPLETE:100"), "A task done, with Outlook's no-date for its due date.");

         PstTask sameDay;
         sameDay.subject = "Same day";
         sameDay.start = FileTime(2024, 3, 1);
         sameDay.due = FileTime(2024, 3, 1);
         text = PstItems::TaskText(sameDay, "pst-same");
         Expect(!Contains(text, "DTSTART") && Contains(text, "DUE;VALUE=DATE:20240301"), "A start on the due day is left out: DUE must follow DTSTART.");

         PstContact contact;
         contact.displayName = "Ann Example";
         contact.givenName = "Ann";
         contact.surname = "Example";
         contact.company = "Example; Ltd";
         contact.emails.push_back("Ann@Example.org");
         contact.emails.push_back("ann.private@example.net");
         PstContact::Phone phone;
         phone.number = "+44 20 7946 0000";
         phone.types = "work,voice";
         contact.phones.push_back(phone);
         PstContact::Address home;
         home.type = "home";
         home.street = "1 High Street";
         home.city = "London";
         contact.addresses.push_back(home);
         contact.birthday = "19800412";
         contact.note = "Line one\r\nline two";
         contact.photo = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(100, 'x');
         contact.photoType = "image/jpeg";

         const std::string card = PstItems::ContactText(contact, "pst-ann", true);
         std::vector<VCardProperty> properties;
         AnsiString name, address;
         Expect(VCard::Parse(AnsiString(card), properties, problem) && VCard::ExtractNameAndAddress(properties, name, address) &&
                name == "Ann Example" && address == "ann@example.org", "A contact as a vCard 4.0: the address book's name and first address.");
         Expect(Contains(card, "VERSION:4.0\r\n") && Contains(card, "N:Example;Ann;;;\r\n") && Contains(card, "ORG:Example\\; Ltd\r\n") &&
                Contains(card, "TEL;TYPE=work,voice:+44 20 7946 0000\r\n") && Contains(card, "ADR;TYPE=home:;;1 High Street;London;;;\r\n") &&
                Contains(card, "BDAY:19800412\r\n") && Contains(card, "NOTE:Line one\\nline two\r\n") && Contains(card, "EMAIL;PREF=1:Ann@Example.org"),
                "Its names, company, telephone, address, birthday and note, escaped as RFC 6350 asks.");

         String wide;
         Unicode::MultiByteToWide(AnsiString(card), wide);
         AnsiString photo, type;
         Expect(ContactCard::Photo(wide, photo, type) && type == "image/jpeg" && std::string(photo.c_str(), photo.size()) == contact.photo,
                "The picture comes back as the contacts route serves it.");
         Expect(!Contains(PstItems::ContactText(contact, "pst-ann", false), "PHOTO"), "And is left out when asked.");

         PstContact nameless;
         nameless.givenName = "Bert";
         nameless.surname = "Example";
         Expect(PstItems::ContactName(nameless) == "Bert Example", "A contact without a display name is its given and family names.");

         PstJournal journal;
         journal.type = "Phone call";
         journal.start = FileTime(2024, 3, 5, 10);
         journal.end = FileTime(2024, 3, 5, 10, 15);
         journal.duration = 15;
         const std::string entry = PstItems::JournalText(journal);
         Expect(Contains(entry, "Journal entry: Phone call\r\nStart: 2024-03-05 10:00 UTC\r\nEnd: 2024-03-05 10:15 UTC\r\nDuration: 15 minutes\r\n\r\n"),
                "A journal entry's type, times and duration above its text.");
      }
   }

   void
   PstItemsTester::Test()
   {
      TestAnsiNdb();
      TestRecurrence();
      TestTimeZones();
      TestIdentifiers();
      TestAppointments();
      TestNamesAndBodies();
      TestTasksAndContacts();
   }
}
