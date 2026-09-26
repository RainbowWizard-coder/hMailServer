// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstTester.h.

#include "StdAfx.h"

#include "PstTester.h"
#include "PstCodePages.h"
#include "PstFile.h"
#include "PstLtp.h"
#include "PstMime.h"
#include "PstRtf.h"
#include "PstStore.h"
#include "../Application/MigrationJob.h"
#include "../Util/Encoding/ModifiedUTF7.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"

#include <algorithm>
#include <cstdlib>
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

      // Whether the call throws a PstError (or one derived from it); anything
      // else it throws goes on up, a defect of its own.
      bool ThrowsPstError(const std::function<void()> &call)
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

      // Whether the call throws PstStopped - which is not a PstError, so that
      // no catch of the reader's own takes it for damage.
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

      // The sentence the call throws a PstError with, or empty.
      std::string PstErrorOf(const std::function<void()> &call)
      {
         try
         {
            call();
         }
         catch (PstError &error)
         {
            return error.what();
         }
         return std::string();
      }

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

      std::string Le16(uint16_t value)
      {
         std::string out;
         out += (char) (value & 0xFF);
         out += (char) (value >> 8);
         return out;
      }

      std::string Le32(uint32_t value)
      {
         return Le16((uint16_t) (value & 0xFFFF)) + Le16((uint16_t) (value >> 16));
      }

      std::string Le64(uint64_t value)
      {
         return Le32((uint32_t) (value & 0xFFFFFFFFULL)) + Le32((uint32_t) (value >> 32));
      }

      void Put(std::string &buffer, size_t at, const std::string &bytes)
      {
         buffer.replace(at, bytes.size(), bytes);
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

      std::string Hex(const char *hex)
      {
         std::string out;
         for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
         {
            const char pair[3] = { hex[i], hex[i + 1], 0 };
            out += (char) strtol(pair, nullptr, 16);
         }
         return out;
      }

      // A PST built in memory from [MS-PST] 2.2: blocks, a one-leaf block
      // B-tree, a node B-tree of one leaf - or of two under a root whose second
      // entry leads back to the root - and the header, CRCs and all.
      class MemoryPst
      {
      public:
         explicit MemoryPst(int crypt) : crypt_(crypt), bytes_(1024, '\0'), next_page_(0x10000) { }

         void Block(uint64_t bid, const std::string &data, bool breakCrc = false)
         {
            std::string stored = data;
            if ((bid & 2) == 0)
            {
               if (crypt_ == 1)
                  PstFormat::EncodePermute(stored);
               else if (crypt_ == 2)
                  PstFormat::CryptCyclic(stored, (uint32_t) bid);
            }

            const uint64_t ib = bytes_.size();
            const uint32_t crc = PstFormat::Crc(stored);
            if (breakCrc && !stored.empty())
               stored[0] = (char) (stored[0] ^ 0xFF);

            const size_t size = (stored.size() + 16 + 63) / 64 * 64;
            std::string block = stored;
            block.resize(size - 16, '\0');
            block += Le16((uint16_t) stored.size()) + Le16(PstSignature(ib, bid)) + Le32(crc) + Le64(bid);
            bytes_ += block;

            blocks_.push_back(Entry(bid, Le64(bid) + Le64(ib) + Le16((uint16_t) stored.size()) + Le16(2) + Le32(0)));
         }

         void Node(uint32_t nid, uint64_t data, uint64_t sub, uint32_t parent)
         {
            nodes_.push_back(Entry(nid, Le64(nid) + Le64(data) + Le64(sub) + Le32(parent) + Le32(0)));
         }

         std::string Finish(int version = 23, const char *client = "SM", int cryptByte = -1, bool loop = false,
                            uint64_t endExtra = 0, bool breakHeader = false)
         {
            while (bytes_.size() % 512)
               bytes_ += '\0';

            Sort(blocks_);
            Sort(nodes_);

            next_page_ = 0x10000;
            uint64_t bbtIb = 0;
            uint64_t bbtBid = 0;
            Tree(blocks_, 24, 0x80, bbtBid, bbtIb);

            uint64_t nbtIb = 0;
            uint64_t nbtBid = 0;
            if (!loop)
            {
               Tree(nodes_, 32, 0x81, nbtBid, nbtIb);
            }
            else
            {
               // Two leaves - NIDs below 0x100, and the rest - and a root whose
               // entry for the rest is the root itself.
               std::vector<std::string> low, high;
               uint64_t highKey = 0;
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

               const uint64_t lowIb = bytes_.size();
               const uint64_t lowBid = 0x10002ULL << 2;
               bytes_ += Page(low, 32, 0, 0x81, lowBid, lowIb);

               nbtIb = bytes_.size();
               nbtBid = 0x10003ULL << 2;
               std::vector<std::string> root;
               root.push_back(Le64(0) + Le64(lowBid) + Le64(lowIb));
               root.push_back(Le64(highKey) + Le64(nbtBid) + Le64(nbtIb));
               bytes_ += Page(root, 24, 1, 0x81, nbtBid, nbtIb);
            }

            std::string header(564, '\0');
            Put(header, 0, "!BDN");
            Put(header, 8, std::string(client, 2));
            Put(header, 10, Le16((uint16_t) version) + Le16(19));
            header[14] = 1;
            header[15] = 1;
            Put(header, 184, Le64(bytes_.size() + endExtra));
            Put(header, 216, Le64(nbtBid) + Le64(nbtIb) + Le64(bbtBid) + Le64(bbtIb));
            header[512] = (char) 0x80;
            header[513] = (char) (cryptByte >= 0 ? cryptByte : crypt_);
            Put(header, 4, Le32(PstFormat::Crc((const unsigned char *) header.data() + 8, 471)));
            Put(header, 524, Le32(PstFormat::Crc((const unsigned char *) header.data() + 8, 516)));
            if (breakHeader)
               header[524] = (char) (header[524] ^ 0xFF);

            std::string file = bytes_;
            file.replace(0, header.size(), header);
            return file;
         }

      private:
         typedef std::pair<uint64_t, std::string> Entry;

         // A B-tree of the entries, sorted: as many leaves as they need, and
         // pages above them, twenty to a page, until one is the root.
         void Tree(const std::vector<Entry> &entries, size_t entrySize, uint8_t type, uint64_t &rootBid, uint64_t &rootIb)
         {
            struct Child
            {
               uint64_t key;
               uint64_t bid;
               uint64_t ib;
            };

            std::vector<Child> level;
            const size_t perLeaf = 488 / entrySize;
            for (size_t start = 0; start == 0 || start < entries.size(); start += perLeaf)
            {
               std::vector<std::string> chunk;
               for (size_t i = start; i < entries.size() && i < start + perLeaf; i++)
                  chunk.push_back(entries[i].second);
               Child child;
               child.key = start < entries.size() ? entries[start].first : 0;
               child.bid = (next_page_++) << 2;
               child.ib = bytes_.size();
               bytes_ += Page(chunk, entrySize, 0, type, child.bid, child.ib);
               level.push_back(child);
            }

            int height = 0;
            while (level.size() > 1)
            {
               height++;
               std::vector<Child> up;
               for (size_t start = 0; start < level.size(); start += 20)
               {
                  std::vector<std::string> chunk;
                  for (size_t i = start; i < level.size() && i < start + 20; i++)
                     chunk.push_back(Le64(level[i].key) + Le64(level[i].bid) + Le64(level[i].ib));
                  Child child;
                  child.key = level[start].key;
                  child.bid = (next_page_++) << 2;
                  child.ib = bytes_.size();
                  bytes_ += Page(chunk, 24, height, type, child.bid, child.ib);
                  up.push_back(child);
               }
               level.swap(up);
            }

            rootBid = level[0].bid;
            rootIb = level[0].ib;
         }

         static void Sort(std::vector<Entry> &entries)
         {
            std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.first < b.first; });
         }

         static uint16_t PstSignature(uint64_t ib, uint64_t bid)
         {
            const uint64_t x = ib ^ bid;
            return (uint16_t) ((x >> 16) ^ x);
         }

         static std::string Page(const std::vector<std::string> &entries, size_t entrySize, int level, uint8_t type, uint64_t bid, uint64_t ib)
         {
            std::string page(488, '\0');
            for (size_t i = 0; i < entries.size(); i++)
               Put(page, i * entrySize, entries[i]);
            page += (char) entries.size();
            page += (char) (488 / entrySize);
            page += (char) entrySize;
            page += (char) level;
            page += Le32(0);
            const uint32_t crc = PstFormat::Crc(page);
            page += (char) type;
            page += (char) type;
            page += Le16(PstSignature(ib, bid));
            page += Le32(crc);
            page += Le64(bid);
            return page;
         }

         int crypt_;
         std::string bytes_;
         std::vector<Entry> blocks_;
         std::vector<Entry> nodes_;
         uint64_t next_page_;
      };

      // A heap-on-node of one block ([MS-PST] 2.3.1): the header, the
      // allocations end to end, and the page map.
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

      uint32_t Hid(int index)
      {
         return (uint32_t) index << 5;
      }

      // A HID in another block of a heap than the first.
      uint32_t HidIn(int block, int index)
      {
         return ((uint32_t) block << 16) | ((uint32_t) index << 5);
      }

      // A heap-on-node over several blocks: the first with HNHDR, the others
      // with HNPAGEHDR ([MS-PST] 2.3.1.3), each with its page map.
      std::vector<std::string> HeapBlocks(uint8_t client, uint32_t userRoot, const std::vector<std::vector<std::string> > &allocations)
      {
         std::vector<std::string> blocks;
         for (size_t b = 0; b < allocations.size(); b++)
         {
            std::string block(b == 0 ? 12 : 2, '\0');
            std::vector<uint16_t> offsets;
            for (const std::string &allocation : allocations[b])
            {
               offsets.push_back((uint16_t) block.size());
               block += allocation;
            }
            offsets.push_back((uint16_t) block.size());
            if (block.size() % 2)
               block += '\0';

            const uint16_t pageMap = (uint16_t) block.size();
            block += Le16((uint16_t) allocations[b].size()) + Le16(0);
            for (uint16_t offset : offsets)
               block += Le16(offset);

            Put(block, 0, Le16(pageMap));
            if (b == 0)
            {
               block[2] = (char) 0xEC;
               block[3] = (char) client;
               Put(block, 4, Le32(userRoot));
            }
            blocks.push_back(block);
         }
         return blocks;
      }

      // An XBLOCK of data blocks already given BIDs.
      std::string XBlock(const std::vector<uint64_t> &bids, const std::vector<std::string> &blocks)
      {
         size_t total = 0;
         for (const std::string &block : blocks)
            total += block.size();
         std::string x = std::string("\x01\x01", 2) + Le16((uint16_t) bids.size()) + Le32((uint32_t) total);
         for (uint64_t bid : bids)
            x += Le64(bid);
         return x;
      }

      // A property of a property context: its value in the record itself (an
      // Int32, a Boolean, or the NID of a subnode that holds it), or bytes on
      // the heap.
      struct Property
      {
         Property(uint16_t i, uint16_t t, uint32_t v) : id(i), type(t), inRecord(true), value(v) { }
         Property(uint16_t i, uint16_t t, const std::string &b) : id(i), type(t), inRecord(false), value(0), bytes(b) { }

         uint16_t id;
         uint16_t type;
         bool inRecord;
         uint32_t value;
         std::string bytes;
      };

      std::string PropertyBlock(std::vector<Property> properties)
      {
         std::sort(properties.begin(), properties.end(), [](const Property &a, const Property &b) { return a.id < b.id; });

         std::vector<std::string> allocations;
         allocations.push_back(std::string("\xB5\x02\x06\x00", 4) + Le32(properties.empty() ? 0 : Hid(2)));
         std::string records;
         std::vector<std::string> values;
         for (const Property &property : properties)
         {
            if (property.inRecord)
               records += Le16(property.id) + Le16(property.type) + Le32(property.value);
            else
            {
               records += Le16(property.id) + Le16(property.type) + Le32(Hid(3 + (int) values.size()));
               values.push_back(property.bytes);
            }
         }
         allocations.push_back(records);
         allocations.insert(allocations.end(), values.begin(), values.end());
         return HeapBlock(0xBC, Hid(1), allocations);
      }

      // A table context's heap: its columns, rows of rowSize bytes (the cell
      // existence bitmap last), the row index - each entry a row id and the
      // row it names, as many leaves as it needs, each in a block of its own
      // under one intermediate record per leaf - and extra allocations in the
      // first block after the rows, whose HIDs are Hid(5), Hid(6)...
      struct Column
      {
         uint32_t tag;
         uint16_t offset;
         uint8_t size;
         uint8_t bit;
      };

      std::vector<std::string> TableBlocks(const std::vector<Column> &columns, size_t endOfValues, size_t rowSize,
                                           const std::vector<std::string> &rows, const std::vector<std::pair<uint32_t, uint32_t> > &index,
                                           const std::vector<std::string> &extras)
      {
         const size_t perLeaf = 800;
         std::vector<std::vector<std::string> > allocations(1);

         std::string info = std::string("\x7C", 1) + (char) columns.size() + Le16((uint16_t) endOfValues) + Le16((uint16_t) endOfValues) +
                            Le16((uint16_t) endOfValues) + Le16((uint16_t) rowSize) + Le32(Hid(2)) + Le32(rows.empty() ? 0 : Hid(4)) + Le32(0);
         for (const Column &column : columns)
            info += Le32(column.tag) + Le16(column.offset) + (char) column.size + (char) column.bit;
         allocations[0].push_back(info);

         std::vector<std::string> leaves;
         for (size_t start = 0; start < index.size(); start += perLeaf)
         {
            std::string leaf;
            for (size_t i = start; i < index.size() && i < start + perLeaf; i++)
               leaf += Le32(index[i].first) + Le32(index[i].second);
            leaves.push_back(leaf);
         }

         if (leaves.size() <= 1)
         {
            allocations[0].push_back(std::string("\xB5\x04\x04\x00", 4) + Le32(Hid(3)));
            allocations[0].push_back(leaves.empty() ? std::string() : leaves[0]);
         }
         else
         {
            allocations[0].push_back(std::string("\xB5\x04\x04\x01", 4) + Le32(Hid(3)));
            std::string intermediate;
            for (size_t l = 0; l < leaves.size(); l++)
            {
               intermediate += Le32(index[l * perLeaf].first) + Le32(HidIn((int) l + 1, 1));
               allocations.push_back(std::vector<std::string>(1, leaves[l]));
            }
            allocations[0].push_back(intermediate);
         }

         std::string matrix;
         for (const std::string &row : rows)
            matrix += row;
         allocations[0].push_back(matrix);
         allocations[0].insert(allocations[0].end(), extras.begin(), extras.end());

         return HeapBlocks(0x7C, Hid(1), allocations);
      }

      // A table of row ids alone: one Int32 column, each row its id.
      std::vector<std::string> IdTableBlocks(const std::vector<uint32_t> &ids)
      {
         std::vector<Column> columns(1);
         columns[0].tag = 0x67F20003;
         columns[0].offset = 0;
         columns[0].size = 4;
         columns[0].bit = 0;

         std::vector<std::string> rows;
         std::vector<std::pair<uint32_t, uint32_t> > index;
         for (size_t i = 0; i < ids.size(); i++)
         {
            rows.push_back(Le32(ids[i]) + std::string("\x80", 1));
            index.push_back(std::make_pair(ids[i], (uint32_t) i));
         }
         std::sort(index.begin(), index.end());
         return TableBlocks(columns, 4, 5, rows, index, std::vector<std::string>());
      }

      const char *const FixtureRecordKey = "5053545f666978747572655f72656331";

      // The message store's properties: its record key, and the IPM subtree
      // by an EntryID of this store.
      std::string StoreBlock(uint32_t ipmSubtree)
      {
         std::vector<Property> store;
         store.push_back(Property(0x0FF9, 0x0102, Hex(FixtureRecordKey)));
         store.push_back(Property(0x35E0, 0x0102, Le32(0) + Hex(FixtureRecordKey) + Le32(ipmSubtree)));
         return PropertyBlock(store);
      }

      // Blocks put into a MemoryPst one after another, each given the next
      // data BID (a multiple of four) or internal BID (two more than one).
      class BlockMaker
      {
      public:
         explicit BlockMaker(MemoryPst &pst) : pst_(pst), next_(0x100) { }

         uint64_t Data(const std::string &data)
         {
            const uint64_t bid = (next_++) << 2;
            pst_.Block(bid, data);
            return bid;
         }

         uint64_t Internal(const std::string &data)
         {
            const uint64_t bid = ((next_++) << 2) | 2;
            pst_.Block(bid, data);
            return bid;
         }

         // A node's data of one block or many: an XBLOCK over them.
         uint64_t Heap(const std::vector<std::string> &blocks)
         {
            if (blocks.size() == 1)
               return Data(blocks[0]);
            std::vector<uint64_t> bids;
            for (const std::string &block : blocks)
               bids.push_back(Data(block));
            return Internal(XBlock(bids, blocks));
         }

         // A BID for an internal block made later, with Subnodes(entries, bid).
         uint64_t ReserveInternal()
         {
            return ((next_++) << 2) | 2;
         }

         // An SLBLOCK: each subnode's NID, data and subnodes; at the BID given,
         // when one was reserved for it.
         uint64_t Subnodes(std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t> > > entries, uint64_t bid = 0)
         {
            std::sort(entries.begin(), entries.end());
            std::string block = std::string("\x02\x00", 2) + Le16((uint16_t) entries.size()) + Le32(0);
            for (const std::pair<uint32_t, std::pair<uint64_t, uint64_t> > &entry : entries)
               block += Le64(entry.first) + Le64(entry.second.first) + Le64(entry.second.second);
            if (bid == 0)
               return Internal(block);
            pst_.Block(bid, block);
            return bid;
         }

      private:
         MemoryPst &pst_;
         uint64_t next_;
      };


      // ------------------------------------------------ the review's files

      typedef std::pair<uint32_t, std::pair<uint64_t, uint64_t> > SubnodeEntry;

      SubnodeEntry Sub(uint32_t nid, uint64_t data, uint64_t sub)
      {
         return std::make_pair(nid, std::make_pair(data, sub));
      }

      const uint32_t TestIpmSubtree = (0x400u << 5) | 0x02;
      const uint32_t TestMessage = (0x10000u << 5) | 0x04;

      // A store whose one message has an attachment table listing distinct
      // attachments, each listed times over, and each an embedded message that
      // is the message itself again: its properties and its own subnodes. Read
      // without a count of its parts, it is distinct to the tenth power of
      // reads.
      std::string EmbeddedLoopFile(int distinct, int listed)
      {
         MemoryPst pst(1);
         BlockMaker make(pst);
         const uint64_t store = make.Data(StoreBlock(TestIpmSubtree));

         std::vector<Property> messageProperties;
         messageProperties.push_back(Property(0x001A, 0x001F, Utf16("IPM.Note")));
         messageProperties.push_back(Property(0x0037, 0x001F, Utf16("Loop")));
         const uint64_t messagePc = make.Data(PropertyBlock(messageProperties));

         std::vector<uint32_t> attachmentIds;
         for (int d = 0; d < distinct; d++)
         {
            for (int l = 0; l < listed; l++)
               attachmentIds.push_back(((0x100u + (uint32_t) d) << 5) | 0x05);
         }
         const uint64_t attachmentTable = make.Heap(IdTableBlocks(attachmentIds));

         // The message's subnodes are named by its attachments' own, so the
         // BID is taken before the block is made.
         const uint64_t messageSub = make.ReserveInternal();

         const uint32_t objectNid = (0x200u << 5) | 0x04;
         std::vector<SubnodeEntry> attachmentSubnodes;
         attachmentSubnodes.push_back(Sub(objectNid, messagePc, messageSub));
         const uint64_t attachmentSub = make.Subnodes(attachmentSubnodes);

         std::vector<Property> attachmentProperties;
         attachmentProperties.push_back(Property(0x3705, 0x0003, 5));
         attachmentProperties.push_back(Property(0x3701, 0x000D, Le32(objectNid) + Le32(0)));
         attachmentProperties.push_back(Property(0x3707, 0x001F, Utf16("loop.msg")));
         const uint64_t attachmentPc = make.Data(PropertyBlock(attachmentProperties));

         std::vector<SubnodeEntry> messageSubnodes;
         messageSubnodes.push_back(Sub(PstNidAttachmentTable, attachmentTable, 0));
         for (int d = 0; d < distinct; d++)
            messageSubnodes.push_back(Sub(((0x100u + (uint32_t) d) << 5) | 0x05, attachmentPc, attachmentSub));
         make.Subnodes(messageSubnodes, messageSub);

         pst.Node(PstNidMessageStore, store, 0, 0);
         pst.Node(TestMessage, messagePc, messageSub, TestIpmSubtree);
         return pst.Finish();
      }

      // A store whose one message has a recipient table of one row that
      // every entry of its row index names, the row's name cell naming one
      // value: nameLength characters on the heap, or - inSubnode - in a
      // subnode of the table, in blocks of 8,000 bytes.
      std::string RecipientsFile(size_t entries, size_t nameLength, bool inSubnode)
      {
         MemoryPst pst(1);
         BlockMaker make(pst);
         const uint64_t store = make.Data(StoreBlock(TestIpmSubtree));

         const uint32_t nameNid = (0x300u << 5) | 0x1F;
         std::vector<Column> columns(3);
         columns[0].tag = 0x67F20003; columns[0].offset = 0; columns[0].size = 4; columns[0].bit = 0;
         columns[1].tag = 0x3001001F; columns[1].offset = 4; columns[1].size = 4; columns[1].bit = 1;
         columns[2].tag = 0x39FE001F; columns[2].offset = 8; columns[2].size = 4; columns[2].bit = 2;

         const std::string name = Utf16(std::string(nameLength, 'n').c_str());
         std::vector<std::string> rows(1, Le32(1) + Le32(inSubnode ? nameNid : Hid(5)) + Le32(Hid(6)) + std::string("\xE0", 1));
         std::vector<std::pair<uint32_t, uint32_t> > index;
         for (size_t i = 0; i < entries; i++)
            index.push_back(std::make_pair((uint32_t) (i + 1), 0u));
         std::vector<std::string> extras;
         extras.push_back(inSubnode ? std::string() : name);
         extras.push_back(Utf16("ann@example.test"));
         const uint64_t table = make.Heap(TableBlocks(columns, 12, 13, rows, index, extras));

         uint64_t tableSub = 0;
         if (inSubnode)
         {
            std::vector<std::string> pieces;
            for (size_t at = 0; at < name.size(); at += 8000)
               pieces.push_back(name.substr(at, 8000));
            std::vector<SubnodeEntry> subnodes;
            subnodes.push_back(Sub(nameNid, make.Heap(pieces), 0));
            tableSub = make.Subnodes(subnodes);
         }

         std::vector<Property> messageProperties;
         messageProperties.push_back(Property(0x001A, 0x001F, Utf16("IPM.Note")));
         const uint64_t messagePc = make.Data(PropertyBlock(messageProperties));

         std::vector<SubnodeEntry> messageSubnodes;
         messageSubnodes.push_back(Sub(PstNidRecipientTable, table, tableSub));
         const uint64_t messageSub = make.Subnodes(messageSubnodes);

         pst.Node(PstNidMessageStore, store, 0, 0);
         pst.Node(TestMessage, messagePc, messageSub, TestIpmSubtree);
         return pst.Finish();
      }

      // A folder tree as the review built it: a chain of depth folders under
      // the IPM subtree, all of one property context - one name of 3,000
      // characters, in a subnode - and below the last, children folders of the
      // same, and the NIDs of missing folders that are not there at all.
      std::string FolderChainFile(int depth, int children, int missing)
      {
         MemoryPst pst(1);
         BlockMaker make(pst);
         const uint64_t store = make.Data(StoreBlock(TestIpmSubtree));

         const uint32_t nameNid = (0x300u << 5) | 0x1F;
         std::vector<SubnodeEntry> folderSubnodes;
         folderSubnodes.push_back(Sub(nameNid, make.Data(Utf16(std::string(3000, 'f').c_str())), 0));
         const uint64_t folderSub = make.Subnodes(folderSubnodes);

         std::vector<Property> folderProperties;
         folderProperties.push_back(Property(0x3001, 0x001F, nameNid));
         folderProperties.push_back(Property(0x3602, 0x0003, 1));
         const uint64_t folderPc = make.Data(PropertyBlock(folderProperties));

         uint32_t parent = TestIpmSubtree;
         for (int level = 0; level < depth; level++)
         {
            const uint32_t nid = ((0x1000u + (uint32_t) level) << 5) | 0x02;
            pst.Node(nid, folderPc, folderSub, parent);
            pst.Node((parent & ~0x1Fu) | 0x0D, make.Heap(IdTableBlocks(std::vector<uint32_t>(1, nid))), 0, parent);
            parent = nid;
         }

         std::vector<uint32_t> below;
         for (int c = 0; c < children; c++)
         {
            const uint32_t nid = ((0x20000u + (uint32_t) c) << 5) | 0x02;
            pst.Node(nid, folderPc, folderSub, parent);
            below.push_back(nid);
         }
         for (int m = 0; m < missing; m++)
            below.push_back(((0x40000u + (uint32_t) m) << 5) | 0x02);
         if (!below.empty())
            pst.Node((parent & ~0x1Fu) | 0x0D, make.Heap(IdTableBlocks(below)), 0, parent);

         pst.Node(PstNidMessageStore, store, 0, 0);
         return pst.Finish();
      }

      bool HasWarning(const PstMessage &message, const char *word)
      {
         for (const PstWarning &warning : message.warnings)
         {
            if (warning.word == word)
               return true;
         }
         return false;
      }

      // Each case is the construction a finding of the review of 25 September
      // 2026 described, at a size a unit test can hold, and fails on the reader
      // as it was before the fix.
      void TestStoreLimits()
      {
         // Three attachments, each the message itself embedded again: 3 + 9 +
         // ... + 3^9 parts, some 30,000 before the depth rule would end it, and
         // with six 60 million. The message's parts are counted instead, and
         // at 10,001 it is refused.
         {
            PstStore store(std::make_shared<PstMemorySource>(EmbeddedLoopFile(3, 1)));
            store.Open("");
            PstMessage message;
            Expect(ThrowsTooLarge([&]() { store.ReadMessage(TestMessage, message); }) &&
                   PstErrorOf([&]() { store.ReadMessage(TestMessage, message); }).find("attachments and embedded messages") != std::string::npos,
                   "An embedded message that holds itself three times over is refused at 10,000 parts.");
         }

         // One attachment listed five times is one attachment: the chain goes
         // ten deep and stops there with a warning, rather than 5^10 reads.
         {
            PstStore store(std::make_shared<PstMemorySource>(EmbeddedLoopFile(1, 5)));
            store.Open("");
            PstMessage message;
            store.ReadMessage(TestMessage, message);
            Expect(message.attachments.size() == 1 && message.attachments[0].embedded && HasWarning(message, "embedded_too_deep") &&
                   message.warnings.size() <= PstStore::MaximumWarnings,
                   "An attachment listed five times is read once, and the chain stops ten deep.");
         }

         // A stop is seen inside a message, through the catches that turn a
         // part's damage into a warning: PstStopped is not a PstError.
         {
            PstStore store(std::make_shared<PstMemorySource>(EmbeddedLoopFile(3, 1)));
            store.Open("");
            int reads = 0;
            store.Budget().SetStop([&]() { return ++reads > 300; });
            PstMessage message;
            Expect(ThrowsStopped([&]() { store.ReadMessage(TestMessage, message); }), "A stop ends a message's reading between two reads.");
         }

         // 5,100 entries of a recipient table's row index, every one naming
         // row 0, whose name cell names one value of 1,000 characters: 5,000
         // recipients kept and the rest counted.
         {
            PstStore store(std::make_shared<PstMemorySource>(RecipientsFile(5100, 1000, false)));
            store.Open("");
            PstMessage message;
            store.ReadMessage(TestMessage, message);
            Expect(message.recipients.size() == PstStore::MaximumRecipients && HasWarning(message, "recipients_left_out") &&
                   message.recipients[0].name.size() == 1000 && message.recipients[0].address == "ann@example.test",
                   "A recipient table of 5,100 rows keeps 5,000.");
         }

         // The same, with a work budget of 8,000 reads for the file: each cell
         // is charged, so the file's budget runs out inside the recipients -
         // and the per-part catch lets it through.
         {
            PstStore store(std::make_shared<PstMemorySource>(RecipientsFile(5100, 1000, false)));
            store.Budget().SetFileLimits(0, 8000);
            store.Open("");
            PstMessage message;
            Expect(ThrowsExhausted([&]() { store.ReadMessage(TestMessage, message); }, true) && store.Budget().FileExhausted(),
                   "Every table cell a message reads is charged to the work budget.");
         }

         // A recipient's name of 70,000 bytes, in the table's subnode: a cell
         // is read with a limit of 64 KB, so the recipients cannot be read and
         // the message says so - rather than keeping 16 MB a cell.
         {
            PstStore store(std::make_shared<PstMemorySource>(RecipientsFile(3, 35000, true)));
            store.Open("");
            PstMessage message;
            store.ReadMessage(TestMessage, message);
            Expect(message.recipients.empty() && HasWarning(message, "recipients_unreadable"), "A table cell past 64 KB is refused.");
         }

         // A chain of 63 folders of one name, 300 more at the bottom: each of
         // those would hold a path of 64 names. The names are cut to 1,024
         // bytes, and all the paths together to 16 MB, past which the file is
         // refused.
         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(63, 300, 0)));
            store.Open("");
            std::vector<PstFolderInfo> folders;
            std::vector<std::string> problems;
            Expect(PstErrorOf([&]() { store.ListFolders(folders, problems); }).find("add up to more than") != std::string::npos,
                   "The names of a file's folders are limited all together.");
         }

         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(63, 300, 0)));
            store.Budget().SetFileLimits(0, 2000);
            store.Open("");
            std::vector<PstFolderInfo> folders;
            std::vector<std::string> problems;
            Expect(ThrowsExhausted([&]() { store.ListFolders(folders, problems); }, true),
                   "Listing the folders is charged to the work budget, and its end for the file is not one folder's problem.");
         }

         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(2, 0, 0)));
            store.Open("");
            std::vector<PstFolderInfo> folders;
            std::vector<std::string> problems;
            store.ListFolders(folders, problems);
            Expect(folders.size() == 2 && folders[1].path.size() == 2 && folders[1].path[0].size() <= PstStore::MaximumFolderNameBytes &&
                   folders[1].path[0].size() > 1000 && problems.empty(),
                   "A folder's name of 3,000 characters is cut to 1,024 bytes.");
         }

         // 1,200 folders named in a hierarchy table and not in the file: 1,000
         // sentences, and the rest counted in one.
         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(1, 0, 1200)));
            store.Open("");
            std::vector<PstFolderInfo> folders;
            std::vector<std::string> problems;
            store.ListFolders(folders, problems);
            Expect(folders.size() == 1 && problems.size() == PstStore::MaximumProblems + 1 &&
                   problems.back().find("And 200 more") == 0,
                   "What could not be read is at most 1,000 sentences.");
         }

         // Seventy deep: the 64th is listed, and what is below it is not read.
         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(70, 0, 0)));
            store.Open("");
            std::vector<PstFolderInfo> folders;
            std::vector<std::string> problems;
            store.ListFolders(folders, problems);
            Expect(folders.size() == (size_t) PstStore::MaximumFolderDepth && problems.size() == 1 &&
                   problems[0].find("nested deeper than") != std::string::npos,
                   "Folders deeper than 64 levels are not read.");
         }

         // A stop is seen while the store is opened.
         {
            PstStore store(std::make_shared<PstMemorySource>(FolderChainFile(1, 0, 0)));
            store.Budget().SetStop([]() { return true; });
            Expect(ThrowsStopped([&]() { store.Open(""); }), "A stop ends the store's opening.");
         }
      }

      // An OST is refused as one before anything else of its store is asked:
      // an Exchange mailbox's cache whose store has no record key a PST's
      // EntryID could hold is an OST to refuse, not a damaged PST (the review
      // of the extension, 25 September 2026).
      void TestOfflineStoreOrder()
      {
         MemoryPst pst(1);
         BlockMaker make(pst);
         std::vector<Property> storeProperties;
         storeProperties.push_back(Property(0x3001, 0x001F, Utf16("Exchange cache")));
         pst.Node(PstNidMessageStore, make.Data(PropertyBlock(storeProperties)), 0, 0);

         int reason = 0;
         try
         {
            PstStore offline(std::make_shared<PstMemorySource>(pst.Finish(23, "SO")));
            offline.Open("");
         }
         catch (PstRefusal &refusal)
         {
            reason = (int) refusal.GetReason();
         }
         catch (PstError &)
         {
            reason = -1;
         }
         Expect(reason == PstRefusal::ReasonOffline, "An OST whose store has no record key is refused as an OST, not as damaged.");

         PstStore plain(std::make_shared<PstMemorySource>(pst.Finish(23, "SM")));
         Expect(PstErrorOf([&]() { plain.Open(""); }).find("no record key") != std::string::npos, "A PST's store with none is damage.");
      }

      void TestBudget()
      {
         PstBudget unit;
         for (uint64_t i = 0; i < PstBudget::UnitItems; i++)
            unit.Item();
         Expect(ThrowsExhausted([&]() { unit.Item(); }, false) && !unit.FileExhausted(), "One read past a unit's 4,000,000 ends the unit.");
         unit.BeginUnit();
         unit.Item();
         Expect(ThrowsExhausted([&]() { unit.Bytes(PstBudget::UnitBytes + 1); }, false), "More than 512 MB decoded ends the unit.");

         PstBudget file;
         file.SetFileLimits(1000, 10);
         for (int i = 0; i < 10; i++)
            file.Item();
         Expect(ThrowsExhausted([&]() { file.Item(); }, true) && file.FileExhausted(), "The eleventh read of a file allowed ten ends the file.");
         file.BeginUnit();
         Expect(ThrowsExhausted([&]() { file.Item(); }, true), "A new unit does not give the file its budget back.");

         PstBudget bytes;
         bytes.SetFileLimits(1000, 0);
         bytes.Bytes(1000);
         Expect(ThrowsExhausted([&]() { bytes.Bytes(1); }, true), "Bytes past the file's limit end the file.");

         PstBudget stopped;
         bool stop = false;
         stopped.SetStop([&]() { return stop; });
         stopped.Item();
         stop = true;
         Expect(ThrowsStopped([&]() { stopped.Item(); }), "A stop is seen at the next read, as PstStopped.");
      }

      void TestFormat()
      {
         Expect(PstFormat::Crc(std::string("123456789")) == 0x2DFD2D88, "[MS-PST] 5.3's CRC: the CRC-32 table, starting at 0, not inverted.");

         const unsigned char *table = PstFormat::CryptTable();
         for (int x = 0; x < 256; x++)
         {
            Expect(table[512 + table[x]] == x, "mpbbI is the inverse of mpbbR.");
            Expect(table[256 + table[256 + x]] == x, "mpbbS is its own inverse.");
         }

         std::string data("The quick brown fox", 19);
         std::string permuted = data;
         PstFormat::EncodePermute(permuted);
         Expect(permuted != data && (unsigned char) permuted[0] == table[(unsigned char) 'T'], "NDB_CRYPT_PERMUTE encodes with mpbbR.");
         PstFormat::DecodePermute(permuted);
         Expect(permuted == data, "NDB_CRYPT_PERMUTE decodes with mpbbI.");

         std::string cycled = data;
         PstFormat::CryptCyclic(cycled, 0x12345678);
         Expect(cycled != data, "NDB_CRYPT_CYCLIC changes the bytes.");
         PstFormat::CryptCyclic(cycled, 0x12345678);
         Expect(cycled == data, "NDB_CRYPT_CYCLIC is its own inverse.");

         int64_t seconds = -1;
         Expect(PstFormat::FileTimeToUnix(116444736000000000ULL, seconds) && seconds == 0, "A FILETIME of 1970-01-01 is 0.");
         Expect(PstFormat::FileTimeToUnix(133537572000000000ULL, seconds) && seconds == 1709283600, "A FILETIME of 2024-03-01 09:00 UTC.");
         Expect(!PstFormat::FileTimeToUnix(0, seconds), "A FILETIME of 0 is no time.");

         const std::string recordKey = Hex("5053545f666978747572655f72656331");
         uint32_t nid = 0;
         Expect(PstFormat::NidOfEntryId(Le32(0) + recordKey + Le32(0x8062), recordKey, nid) && nid == 0x8062,
                "An EntryID names a node by its last four bytes.");
         Expect(!PstFormat::NidOfEntryId(Le32(0) + Hex("00112233445566778899aabbccddeeff") + Le32(0x8062), recordKey, nid),
                "An EntryID of another store names nothing here.");
         Expect(!PstFormat::NidOfEntryId(Le32(0x8062), recordKey, nid), "An EntryID that is not 24 bytes names nothing.");

         Expect(PstFormat::Utf16ToUtf8(Hex("3dd800de")) == Hex("f09f9880"), "A surrogate pair is one character.");
         Expect(PstFormat::Utf16ToUtf8(Hex("3dd84100")) == Hex("efbfbd") + "A", "A lone surrogate is U+FFFD.");
         Expect(PstFormat::Utf8ToUtf16(Hex("f09f9880")) == Hex("3dd800de"), "And back.");

         Expect(ThrowsPstError([]() { PstFormat::U32(std::string("abc"), 0, "a test"); }), "A read past a buffer throws.");
         Expect(!PstFormat::Fits(0xFFFFFFFFFFFFFFF0ULL, 0x20, 0xFFFFFFFFFFFFFFFFULL), "An offset and a length that overflow do not fit.");
      }

      void TestNdb()
      {
         for (int crypt = 0; crypt <= 2; crypt++)
         {
            MemoryPst pst(crypt);
            pst.Block(4, "hello, world");
            pst.Block(8, "abc");
            pst.Block(12, "defg");
            pst.Block(18, std::string("\x01\x01", 2) + Le16(2) + Le32(7) + Le64(8) + Le64(12));                  // an XBLOCK
            pst.Block(22, std::string("\x01\x02", 2) + Le16(1) + Le32(7) + Le64(18));                            // an XXBLOCK of it
            pst.Block(26, std::string("\x01\x01", 2) + Le16(1) + Le32(0xFFFFFFFF) + Le64(8));                    // claims 4 GB
            pst.Block(30, std::string("\x01\x01", 2) + Le16(1) + Le32(99) + Le64(8));                            // claims what it does not hold
            pst.Block(34, std::string("\x01\x02", 2) + Le16(1) + Le32(3) + Le64(34));                            // an XXBLOCK of itself
            pst.Block(38, std::string("\x02\x00", 2) + Le16(2) + Le32(0) +
                          Le64(0x692) + Le64(4) + Le64(0) + Le64(0x671) + Le64(8) + Le64(0));                     // an SLBLOCK
            pst.Block(42, std::string("\x02\x01", 2) + Le16(1) + Le32(0) + Le64(0x692) + Le64(38));             // an SIBLOCK of it
            pst.Block(46, std::string("\x02\x01", 2) + Le16(1) + Le32(0) + Le64(0x692) + Le64(46));             // an SIBLOCK of itself
            pst.Block(52, "a block whose CRC fails", true);
            pst.Block(56, std::string());                                                                          // a block of nothing
            pst.Block(58, std::string("\x01\x01", 2) + Le16(2) + Le32(6) + Le64(8) + Le64(8));                      // names one block twice
            pst.Block(62, std::string("\x01\x02", 2) + Le16(2) + Le32(14) + Le64(18) + Le64(18));                   // names one XBLOCK twice
            pst.Block(66, std::string("\x01\x01", 2) + Le16(2) + Le32(3) + Le64(8) + Le64(56));                     // lists a block of nothing
            pst.Node(0x21, 4, 38, 0);
            pst.Node(0x61, 8, 0, 0);

            PstFile file(std::make_shared<PstMemorySource>(pst.Finish()));
            file.Open();
            Expect(file.GetCryptMethod() == crypt && file.GetVersion() == 23, "The header's version and encoding.");

            PstFile::Node node;
            Expect(file.FindNode(0x21, node) && node.data == 4 && node.sub == 38, "A node is found in the NBT.");
            Expect(!file.FindNode(0x999, node), "A node that is not there is not found.");
            Expect(file.ReadData(4, 1024) == "hello, world", "A data block is decoded as the header says.");
            Expect(file.ReadData(18, 1024) == "abcdefg", "An XBLOCK joins its blocks.");
            Expect(file.ReadData(22, 1024) == "abcdefg", "An XXBLOCK joins its XBLOCKs.");
            Expect(ThrowsTooLarge([&]() { file.ReadData(26, 1024 * 1024); }), "A tree that claims 4 GB is refused before anything is allocated.");
            Expect(ThrowsTooLarge([&]() { file.ReadData(18, 5); }), "A tree larger than the caller's limit is refused.");
            Expect(ThrowsPstError([&]() { file.ReadData(30, 1024); }), "A tree that holds less than it claims is refused.");
            Expect(ThrowsPstError([&]() { file.ReadData(34, 1024); }), "An XXBLOCK that lists itself is refused.");
            Expect(ThrowsPstError([&]() { file.ReadData(52, 1024); }), "A block whose CRC fails is refused.");
            Expect(ThrowsPstError([&]() { file.ReadData(0x400, 1024); }), "A block not in the BBT is refused.");

            // A data tree that names one block twice: an XXBLOCK listing one
            // XBLOCK a thousand times, each listing one block a thousand times,
            // would be a million reads. And a block of nothing inside an
            // XBLOCK, which lets a tree of a million pass every size check.
            Expect(PstErrorOf([&]() { file.ReadData(58, 1024); }).find("twice") != std::string::npos,
                   "An XBLOCK that names one block twice is refused.");
            Expect(PstErrorOf([&]() { file.ReadData(62, 1024); }).find("twice") != std::string::npos,
                   "An XXBLOCK that names one XBLOCK twice is refused.");
            Expect(PstErrorOf([&]() { file.ReadData(66, 1024); }).find("holds nothing") != std::string::npos,
                   "An XBLOCK that lists a block of nothing is refused.");
            Expect(file.ReadData(56, 1024).empty(), "A node's one data block of nothing is an empty value.");

            // Every page and block read is charged: a file allowed five reads
            // cannot read a data tree of three blocks, and says it is the file.
            PstFile tight(std::make_shared<PstMemorySource>(pst.Finish()));
            tight.Budget().SetFileLimits(0, 5);
            tight.Open();
            Expect(ThrowsExhausted([&]() { tight.ReadData(22, 1024); }, true), "Pages and blocks are charged to the work budget.");

            std::map<uint32_t, PstFile::Subnode> subnodes;
            file.ReadSubnodes(38, subnodes);
            Expect(subnodes.size() == 2 && subnodes[0x692].data == 4 && subnodes[0x671].data == 8, "An SLBLOCK lists its subnodes.");
            file.ReadSubnodes(42, subnodes);
            Expect(subnodes.size() == 2, "An SIBLOCK lists its SLBLOCKs' subnodes.");
            Expect(ThrowsPstError([&]() { file.ReadSubnodes(46, subnodes); }), "An SIBLOCK that lists itself is refused.");
            Expect(ThrowsPstError([&]() { file.ReadSubnodes(4, subnodes); }), "A data block is not a subnode tree.");
         }

         // The header's refusals, each for what it is.
         {
            MemoryPst pst(1);
            pst.Block(4, "x");
            pst.Node(0x21, 4, 0, 0);
            Expect(RefusalOf(pst.Finish(23, "SO")) == 0, "An OST of version 23 opens as a PST does: its pages and blocks are the same.");
            Expect(RefusalOf(pst.Finish(36, "SO")) == PstRefusal::ReasonOffline, "An OST in the 4K-page layout is refused as one.");
            Expect(RefusalOf(pst.Finish(36)) == PstRefusal::ReasonFourK, "The 4K-page layout is refused.");
            Expect(RefusalOf(pst.Finish(14)) == PstRefusal::ReasonDamaged,
                   "A Unicode header that says version 14 is read as an ANSI one, whose end of file it does not have: refused as damaged, not misread.");
            Expect(RefusalOf(pst.Finish(23, "SM", 0x10)) == PstRefusal::ReasonProtected, "Windows Information Protection is refused.");
            Expect(RefusalOf(pst.Finish(37, "SM", 0x10)) == PstRefusal::ReasonProtected, "... under version 37 as well.");
            Expect(RefusalOf(pst.Finish(40)) == PstRefusal::ReasonUnknownFormat, "An unknown version is refused.");
            Expect(RefusalOf(pst.Finish(23, "SM", 3)) == PstRefusal::ReasonUnknownFormat, "An unknown encoding is refused.");
            Expect(RefusalOf(pst.Finish(23, "SM", -1, false, 0, true)) == PstRefusal::ReasonDamaged, "A header whose CRC fails is refused.");
            Expect(RefusalOf(pst.Finish(23, "SM", -1, false, 4096)) == PstRefusal::ReasonDamaged, "A file shorter than its header says is refused.");
            Expect(RefusalOf(pst.Finish().substr(0, 500)) == PstRefusal::ReasonNotPst, "A file shorter than a header is refused.");
            Expect(RefusalOf(std::string(600, 'x')) == PstRefusal::ReasonNotPst, "A file that is not a PST is refused.");
            Expect(RefusalOf(pst.Finish(37)) == 0, "Version 37, not protected, is read.");
            Expect(RefusalOf(pst.Finish(23)) == 0, "Version 23 is read.");
         }

         // A node B-tree whose root leads back to itself: refused, not walked
         // for ever.
         {
            MemoryPst pst(1);
            pst.Block(4, "x");
            pst.Node(0x21, 4, 0, 0);
            pst.Node(0x61, 4, 0, 0);
            pst.Node(0x122, 4, 0, 0);
            pst.Node(0x8022, 4, 0, 0);
            PstFile file(std::make_shared<PstMemorySource>(pst.Finish(23, "SM", -1, true)));
            file.Open();
            PstFile::Node node;
            Expect(file.FindNode(0x21, node), "The half of a looping tree that does not loop is read.");
            Expect(ThrowsPstError([&]() { PstFile::Node other; file.FindNode(0x8022, other); }), "A B-tree that points back at itself is refused.");
         }
      }

      void TestLtp()
      {
         // A property context: an Int32 and a string on the heap.
         std::vector<std::string> allocations;
         allocations.push_back(std::string("\xB5\x02\x06\x00", 4) + Le32(Hid(2)));
         allocations.push_back(Le16(0x0037) + Le16(0x001F) + Le32(Hid(3)) + Le16(0x0E07) + Le16(0x0003) + Le32(9));
         allocations.push_back(Utf16("Hi"));
         const std::string pcBlock = HeapBlock(0xBC, Hid(1), allocations);

         {
            std::vector<std::string> blocks(1, pcBlock);
            PstHeap heap;
            heap.Load(blocks);
            Expect(heap.GetClientSignature() == 0xBC && heap.Allocation(Hid(3)) == Utf16("Hi"), "A heap's allocations are found by HID.");
            std::vector<std::pair<std::string, std::string> > records;
            heap.BthRecords(Hid(1), 2, 6, 100, records);
            Expect(records.size() == 2, "A BTH's records are read.");
            Expect(ThrowsPstError([&]() { heap.BthRecords(Hid(1), 4, 4, 100, records); }), "A BTH of other sizes is refused.");
            Expect(ThrowsPstError([&]() { heap.BthRecords(Hid(1), 2, 6, 1, records); }), "More records than the limit are refused.");
            Expect(ThrowsPstError([&]() { heap.Allocation(Hid(4)); }), "A HID past the page map is refused.");
            Expect(ThrowsPstError([&]() { heap.Allocation(0x10000 | Hid(1)); }), "A HID in a block the heap does not have is refused.");
            Expect(ThrowsPstError([&]() { heap.Allocation(Hid(1) | 1); }), "A NID is not a HID.");
         }

         {
            // Two levels, and a BTH that points back into itself.
            std::vector<std::string> deep;
            deep.push_back(std::string("\xB5\x02\x06\x01", 4) + Le32(Hid(2)));
            deep.push_back(Le16(0x0037) + Le32(Hid(3)) + Le16(0x0E07) + Le32(Hid(4)));
            deep.push_back(Le16(0x0037) + Le16(0x0003) + Le32(1));
            deep.push_back(Le16(0x0E07) + Le16(0x0003) + Le32(2));
            deep.push_back(std::string("\xB5\x02\x06\x01", 4) + Le32(Hid(6)));
            deep.push_back(Le16(0x0037) + Le32(Hid(6)));
            std::vector<std::string> blocks(1, HeapBlock(0xBC, Hid(1), deep));
            PstHeap heap;
            heap.Load(blocks);
            std::vector<std::pair<std::string, std::string> > records;
            heap.BthRecords(Hid(1), 2, 6, 100, records);
            Expect(records.size() == 2, "A BTH of two levels is read.");
            Expect(ThrowsPstError([&]() { heap.BthRecords(Hid(5), 2, 6, 100, records); }), "A BTH that points back into itself is refused.");
         }

         {
            // A page map that runs past its block.
            std::string broken = pcBlock;
            Put(broken, 0, Le16((uint16_t) (broken.size() - 2)));
            std::vector<std::string> blocks(1, broken);
            PstHeap heap;
            Expect(ThrowsPstError([&]() { heap.Load(blocks); }), "A page map past its block is refused.");
         }

         // A table: one column, three rows, the row index out of the rows'
         // order.
         std::vector<std::string> table;
         table.push_back(std::string("\x7C\x01", 2) + Le16(4) + Le16(4) + Le16(4) + Le16(5) + Le32(Hid(2)) + Le32(Hid(4)) + Le32(0) +
                         Le32(0x67F20003) + Le16(0) + std::string("\x04\x00", 2));
         table.push_back(std::string("\xB5\x04\x04\x00", 4) + Le32(Hid(3)));
         table.push_back(Le32(0x10) + Le32(1) + Le32(0x20) + Le32(0) + Le32(0x30) + Le32(2));
         table.push_back(Le32(0x20) + std::string("\x80", 1) + Le32(0x10) + std::string("\x80", 1) + Le32(0x30) + std::string("\x80", 1));

         MemoryPst pst(1);
         pst.Block(4, pcBlock);
         pst.Block(8, HeapBlock(0x7C, Hid(1), table));
         pst.Node(0x21, 4, 0, 0);
         PstFile file(std::make_shared<PstMemorySource>(pst.Finish()));
         file.Open();

         PstPropertyContext pc;
         pc.Read(file, 4, 0);
         int32_t flags = 0;
         std::string subject;
         Expect(pc.Int32(0x0E07, flags) && flags == 9, "An Int32 is read from its record.");
         Expect(pc.Text(0x0037, 1024, subject) && subject == "Hi", "A string is read from the heap.");
         Expect(!pc.Has(0x1000), "A property that is not there is not found.");
         Expect(ThrowsTooLarge([&]() { std::string value; pc.Bytes(0x0037, 2, value); }), "A value past the caller's limit is refused.");

         PstTable rows;
         rows.Read(file, 8, 0, 10);
         int32_t id = 0;
         Expect(rows.RowCount() == 3 && rows.RowId(0) == 0x10 && rows.RowId(2) == 0x30, "A table's rows by the row index.");
         Expect(rows.CellInt32(0, 0x67F2, id) && id == 0x10, "A row is where the row index says it is.");
         Expect(!rows.CellInt32(0, 0x3001, id), "A column the table does not have is no cell.");
         PstTable few;
         Expect(ThrowsPstError([&]() { few.Read(file, 8, 0, 2); }), "A table with more rows than asked for is refused.");
         Expect(ThrowsPstError([&]() { PstPropertyContext wrong; wrong.Read(file, 8, 0); }), "A table is not a property context.");
      }

      void TestRtf()
      {
         // [MS-OXRTFCP] 3.1.1 and 3.1.2, the specification's own examples.
         const std::string first = Hex("2d0000002b0000004c5a4675f1c5c7a703000a007263706731323542320af32068656c090020627705b06c647d0a800fa0");
         const std::string second = Hex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0");
         std::string rtf, problem;
         Expect(PstRtf::Preload().size() == 207, "The dictionary's preload is 207 bytes.");
         Expect(PstRtf::Decompress(first, 1048576, rtf, problem) && rtf == "{\\rtf1\\ansi\\ansicpg1252\\pard hello world}\r\n",
                "[MS-OXRTFCP]'s first example decompresses.");
         Expect(PstRtf::Decompress(second, 1048576, rtf, problem) && rtf == "{\\rtf1 WXYZWXYZWXYZWXYZWXYZ}",
                "[MS-OXRTFCP]'s second example, a reference into what it is writing, decompresses.");

         std::string flipped = first;
         flipped[20] = (char) (flipped[20] ^ 0x01);
         Expect(!PstRtf::Decompress(flipped, 1048576, rtf, problem), "A compressed body whose CRC fails is refused.");
         Expect(!PstRtf::Decompress(first.substr(0, 30), 1048576, rtf, problem), "A compressed body cut short is refused.");
         Expect(PstRtf::Decompress(first, 10, rtf, problem) && rtf.size() == 10, "Nothing is produced past the limit.");

         const std::string stored = Le32(12 + 10) + Le32(20) + std::string("MELA", 4) + Le32(0) + "0123456789";
         Expect(PstRtf::Decompress(stored, 1048576, rtf, problem) && rtf == "0123456789", "A stored body's RAWSIZE past what follows is clamped.");

         const std::string literal = "{\\rtf1 a body of literals}";
         Expect(PstRtf::Decompress(PstRtf::CompressLiterals(literal), 1048576, rtf, problem) && rtf == literal, "A stream of literals decompresses.");

         // A few dozen bytes that claim 4 GB (RAWSIZE is outside the CRC):
         // what is reserved is what the input can make, not the claim.
         std::string claims = PstRtf::CompressLiterals(literal);
         Put(claims, 4, Le32(0xFFFFFFFF));
         std::string decoded;
         Expect(PstRtf::Decompress(claims, PstStore::MaximumMessageBytes, decoded, problem) && decoded == literal && decoded.capacity() < 64 * 1024,
                "A compressed body's RAWSIZE reserves no more than its input can make.");

         const std::string html =
            "{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 \\deff0{\\fonttbl{\\f0 Arial;}}{\\*\\htmltag19 <html>}\\htmlrtf {\\htmlrtf0 "
            "{\\*\\htmltag84 <b>}\\htmlrtf {\\b\\htmlrtf0 bold\\htmlrtf }\\htmlrtf0 {\\*\\htmltag92 </b>}\\par Caf\\'e9 \\u8364?"
            "{\\*\\mhtmltag84 <img src=\"cid:x\">}{\\*\\htmltag27 </html>}\\htmlrtf }\\htmlrtf0 }";
         Expect(PstRtf::KindOf(html) == PstRtf::KindHtml, "\\fromhtml1 is HTML.");
         Expect(PstRtf::Deencapsulate(html, true) == "<html><b>bold</b>\r\nCaf\xC3\xA9 \xE2\x82\xAC</html>",
                "HTML carried in RTF: the tags, the text \\htmlrtf does not mark, \\'e9 in its code page and \\u8364 - and no \\mhtmltag.");

         const std::string text = "{\\rtf1\\ansi\\fromtext \\deff0{\\fonttbl{\\f0 Arial;}}hello\\par world\\tab x}";
         Expect(PstRtf::KindOf(text) == PstRtf::KindText && PstRtf::Deencapsulate(text, false) == "hello\r\nworld\tx", "Text carried in RTF.");
         Expect(PstRtf::KindOf("{\\rtf1\\ansi plain}") == PstRtf::KindRtf, "An RTF body written in RTF.");

         std::string deep;
         for (int i = 0; i < 10000; i++)
            deep += "{";
         deep += "x";
         PstRtf::Deencapsulate(deep, true);
      }

      void TestCodePages()
      {
         std::string utf8;
         Expect(PstCodePages::CharsetName(1252) == "windows-1252" && PstCodePages::CharsetName(65001) == "utf-8", "The charset of a code page.");
         Expect(!PstCodePages::Known(37) && PstCodePages::CharsetName(37).empty(), "EBCDIC is not in the table.");
         Expect(PstCodePages::Decode(Hex("cff0e8"), 1251, utf8) && utf8 == Hex("d09fd180d0b8"), "cp1251 decoded.");
         Expect(PstCodePages::Decode(Hex("82a0"), 932, utf8) && utf8 == Hex("e38182"), "cp932 decoded.");
         Expect(!PstCodePages::Decode("x", 37, utf8), "A code page not in the table is not decoded.");
      }

      void TestStore()
      {
         Expect(PstStore::WithoutSubjectMarker("\x01\x04RE: x") == "RE: x", "The subject marker and its length go.");
         Expect(PstStore::WithoutSubjectMarker("RE: x") == "RE: x", "A subject without the marker is kept.");
         Expect(PstStore::IsMailMessageClass("IPM.Note") && PstStore::IsMailMessageClass("IPM.Note.SMIME.MultipartSigned") &&
                PstStore::IsMailMessageClass("REPORT.IPM.Note.NDR") && PstStore::IsMailMessageClass("IPM.Schedule.Meeting.Request") &&
                PstStore::IsMailMessageClass("ipm.post"), "Notes, reports, meeting requests and posts are mail.");
         Expect(!PstStore::IsMailMessageClass("IPM.Appointment") && !PstStore::IsMailMessageClass("IPM.Contact") &&
                !PstStore::IsMailMessageClass("IPM.Notes"), "Appointments and contacts are not mail.");
         Expect(PstStore::IsMailFolderClass("") && PstStore::IsMailFolderClass("IPF.Note") && PstStore::IsMailFolderClass("IPF.Note.OutlookHomepage") &&
                PstStore::IsMailFolderClass("IPF.Imap") && !PstStore::IsMailFolderClass("IPF.Contact"), "Mail folders by class.");
         Expect(PstStore::PasswordMatches("fixture-password", 0xF3D02324) && !PstStore::PasswordMatches("wrong", 0xF3D02324),
                "A password is checked by its CRC.");

         // A password outside ASCII whose CRC the file keeps over its UTF-16LE
         // form, the second of the two forms [MS-PST] leaves open.
         const std::string umlaut = "fixture-p\xC3\xA4ssword";
         const uint32_t sixteen = PstFormat::Crc(PstFormat::Utf8ToUtf16(umlaut));
         Expect(PstStore::PasswordMatches(umlaut, sixteen) && !PstStore::PasswordMatches("fixture-password", sixteen) &&
                PstFormat::Crc(umlaut) != sixteen,
                "A password's UTF-16LE form is accepted, and only its own.");
      }

      void TestMime()
      {
         Expect(PstMime::HeaderSafe("a\r\nBcc: x") == "a  Bcc: x", "A line break inside a header value is a space.");
         Expect(PstMime::Mailbox("Evil\r\nBcc: v@x.org", "e@x.org") == "\"Evil  Bcc: v@x.org\" <e@x.org>", "A display name is quoted, not a header.");
         Expect(PstMime::Mailbox("Ann Example", "ann@x.org") == "Ann Example <ann@x.org>" && PstMime::Mailbox("", "a@b.org") == "a@b.org",
                "A mailbox.");
         Expect(PstMime::Mailbox("Bob", "/O=EXAMPLE/CN=BOB") == "Bob:;", "No usable address: the name as an empty group.");
         Expect(PstMime::EncodedWords("\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1") == "=?UTF-8?B?zprOsc67zrfOvM6tz4HOsQ==?=",
                "RFC 2047 encoded words.");
         std::string longName;
         for (int i = 0; i < 40; i++)
            longName += "\xCE\x9A";
         const std::string words = PstMime::EncodedWords(longName);
         Expect(words.find("\r\n ") != std::string::npos && words.find("\xCE") == std::string::npos, "A long name folds into several words.");
         {
            // RFC 2047 section 2: a word of at most 75 characters, a line
            // holding one of at most 76 - the first line with the nine
            // characters of "Subject: " before its word.
            const std::string field = "Subject: " + PstMime::EncodedWords(longName, 9);
            bool within = field.find("\r\n ") != std::string::npos;
            size_t start = 0;
            while (start <= field.size())
            {
               size_t end = field.find("\r\n", start);
               if (end == std::string::npos)
                  end = field.size();
               within = within && end - start <= 76;
               start = end + 2;
            }
            Expect(within && PstMime::EncodedWords(longName, 0) == words, "The first word shares its line with the field's name.");
         }

         Expect(PstMime::QuotedPrintable("a=b\r\n") == "a=3Db\r\n", "= is encoded.");
         Expect(PstMime::QuotedPrintable("a \r\nb") == "a=20\r\nb", "A space before a line break is encoded.");
         const std::string qp = PstMime::QuotedPrintable(std::string(100, 'x'));
         Expect(qp.find("=\r\n") == 75, "A long line is broken softly within 76 characters.");
         Expect(PstMime::Base64("Man") == "TWFu\r\n" && PstMime::Base64("").empty(), "Base64.");
         Expect(PstMime::Crlf("a\nb\rc\r\nd") == "a\r\nb\r\nc\r\nd", "Line endings made CRLF.");
         Expect(PstMime::DateText(0) == "Thu, 01 Jan 1970 00:00:00 +0000" && PstMime::DateText(1709283600) == "Fri, 01 Mar 2024 09:00:00 +0000",
                "Dates in UTC.");
         Expect(PstMime::CleanFileName("C:\\x\\r\xC3\xA9sum\xC3\xA9\r\n.txt") == "r\xC3\xA9sum\xC3\xA9.txt" && PstMime::CleanFileName("") == "attachment",
                "A file name: its last component, no control characters.");
         Expect(PstMime::FileNameParameter("filename", "r\xC3\xA9sum\xC3\xA9.txt") == "filename*=UTF-8''r%C3%A9sum%C3%A9.txt" &&
                PstMime::FileNameParameter("filename", "a.txt") == "filename=\"a.txt\"", "RFC 2231 for a name outside ASCII.");
         Expect(PstMime::WithoutContentFields("From: a@b\r\nContent-Type: multipart/alternative;\r\n\tboundary=\"x\"\r\nSubject: s\r\nMIME-Version: 1.0\r\n\r\nbody") ==
                "From: a@b\r\nSubject: s\r\n", "Transport headers lose MIME-Version and Content-*, their folded lines with them.");

         PstMessage message;
         message.subject = "Hi";
         message.hasBody = true;
         message.body = "text\n";
         message.hasHtml = true;
         message.html = "<p><img src=\"cid:c1\"></p>";
         message.htmlCodePage = 65001;
         message.representingName = "Ann";
         message.representingAddress = "ann@x.org";
         message.submitTime = 133537572000000000ULL;
         PstAttachment picture;
         picture.method = 1;
         picture.fileName = "logo.png";
         picture.mimeType = "image/png";
         picture.contentId = "c1";
         picture.data = "PNG";
         message.attachments.push_back(picture);
         PstAttachment document;
         document.method = 1;
         document.fileName = "notes.txt";
         document.data = "notes";
         message.attachments.push_back(document);

         std::vector<PstWarning> warnings;
         const std::string once = PstMime::Build(message, "stem", warnings);
         const std::string again = PstMime::Build(message, "stem", warnings);
         Expect(once == again, "The rebuild is the same bytes every time.");
         Expect(once.find("From: Ann <ann@x.org>\r\n") == 0 && once.find("Date: Fri, 01 Mar 2024 09:00:00 +0000\r\n") != std::string::npos,
                "The headers from the properties.");
         Expect(once.find("multipart/mixed") != std::string::npos && once.find("multipart/related") != std::string::npos &&
                once.find("multipart/alternative") != std::string::npos && once.find("=_hmPST_stem_") != std::string::npos,
                "mixed, related and alternative, each where it is needed, with boundaries made from the stem.");
         Expect(once.find("Content-ID: <c1>") != std::string::npos, "The picture keeps its Content-ID.");

         PstMessage plain;
         plain.hasBody = true;
         plain.body = "only text";
         const std::string simple = PstMime::Build(plain, "stem", warnings);
         Expect(simple.find("multipart") == std::string::npos && simple.find("Content-Type: text/plain; charset=\"utf-8\"") != std::string::npos,
                "A message of text alone is text/plain alone.");

         PstMessage transported;
         transported.hasTransportHeaders = true;
         transported.transportHeaders = "Received: x\r\nContent-Type: multipart/alternative; boundary=\"stale\"\r\nSubject: kept\r\n\r\n";
         transported.hasBody = true;
         transported.body = "b";
         const std::string rebuilt = PstMime::Build(transported, "stem", warnings);
         Expect(rebuilt.find("stale") == std::string::npos && rebuilt.find("Received: x\r\nSubject: kept\r\nMIME-Version: 1.0\r\n") == 0,
                "Transport headers are kept, a stale boundary is not.");

         // An embedded message carries its transport headers as they are, and
         // one of them begins as this message's first boundary would: the
         // boundary taken is another, which no line of the parts begins with.
         PstMessage outer;
         outer.hasBody = true;
         outer.body = "outer";
         PstAttachment carried;
         carried.method = 5;
         carried.embedded = std::make_shared<PstMessage>();
         carried.embedded->hasTransportHeaders = true;
         carried.embedded->transportHeaders = "Subject: inner\r\n--=_hmPST_stem_1_--: closes the outer part\r\n\r\n";
         carried.embedded->hasBody = true;
         carried.embedded->body = "inner";
         carried.embedded->hasHtml = true;
         carried.embedded->html = "<p>inner</p>";
         carried.embedded->htmlCodePage = 37;
         outer.attachments.push_back(carried);
         std::vector<PstWarning> outerWarnings;
         const std::string nested = PstMime::Build(outer, "stem", outerWarnings);
         Expect(nested.find("boundary=\"=_hmPST_stem_1_\"") == std::string::npos && nested.find("boundary=\"=_hmPST_stem_2_\"") != std::string::npos,
                "A boundary a line of the parts begins with is not used.");

         bool innerWarned = false;
         for (const PstWarning &warning : outerWarnings)
            innerWarned = innerWarned || (warning.word == "html_charset_unknown" && warning.detail.find("in the embedded message") == 0);
         Expect(innerWarned, "What an embedded message was written without is the message's warning too.");

         PstMessage identified;
         identified.hasBody = true;
         identified.body = "b";
         identified.messageId = "<" + std::string(1000, 'x') + "@example.test>";
         identified.inReplyTo = "<short@example.test>";
         std::vector<PstWarning> identifiedWarnings;
         const std::string withIds = PstMime::Build(identified, "stem", identifiedWarnings);
         Expect(withIds.find("Message-ID:") == std::string::npos && withIds.find("In-Reply-To: <short@example.test>\r\n") != std::string::npos &&
                !identifiedWarnings.empty() && identifiedWarnings[0].word == "identifier_too_long",
                "An identifier longer than a header line may be is left out, with a warning.");

         PstMessage opaque;
         opaque.messageClass = "IPM.Note.SMIME";
         PstAttachment p7m;
         p7m.method = 1;
         p7m.data = "0123456789";
         opaque.attachments.push_back(p7m);
         const std::string enveloped = PstMime::Build(opaque, "stem", warnings);
         Expect(enveloped.find("application/pkcs7-mime") != std::string::npos && enveloped.find("smime-type") == std::string::npos,
                "Opaque S/MIME is application/pkcs7-mime with no smime-type the reader cannot know.");

         PstMessage signedMessage;
         signedMessage.messageClass = "IPM.Note.SMIME.MultipartSigned";
         signedMessage.subject = "Signed";
         PstAttachment entity;
         entity.method = 1;
         entity.data = "Content-Type: multipart/signed; boundary=\"b\"\r\n\r\n--b\r\nx\r\n--b--\r\n";
         signedMessage.attachments.push_back(entity);
         const std::string kept = PstMime::Build(signedMessage, "stem", warnings);
         Expect(kept.size() > entity.data.size() && kept.compare(kept.size() - entity.data.size(), entity.data.size(), entity.data) == 0 &&
                kept.find("MIME-Version: 1.0\r\nContent-Type: multipart/signed") != std::string::npos,
                "A clear-signed message's signed entity is kept byte for byte.");
      }

      void TestImportRules()
      {
         typedef MigrationJob J;
         const String dot = _T(".");

         Expect(J::PstFolderName("a.b", dot, false) == _T("a_b"), "The hierarchy delimiter inside a name is an underscore.");
         Expect(J::PstFolderName("#tag", dot, true) == _T("_#tag") && J::PstFolderName("#tag", dot, false) == _T("#tag"),
                "A top-level name starting with # gets an underscore.");
         Expect(J::PstFolderName("", dot, false) == _T("Unnamed folder"), "An empty name is Unnamed folder.");
         Expect(J::PstFolderName(std::string(300, 'x'), dot, false).GetLength() == 255, "A long name is cut to 255.");
         Expect(J::PstFolderName(std::string(64 * 1024, 'x'), dot, false).GetLength() == 255,
                "A name of 64 KB is cut to 255 without encoding it 64,000 times.");

         // Three hundred characters outside ASCII: cut until the encoded form
         // fits, and what it decodes to is the name's beginning.
         std::string accented;
         for (int i = 0; i < 300; i++)
            accented += "\xC3\xA9";
         const String cut = J::PstFolderName(accented, dot, false);
         AnsiString back;
         Unicode::WideToMultiByte(ModifiedUTF7::Decode(AnsiString(cut)), back);
         Expect(cut.GetLength() <= 255 && back.GetLength() > 100 && accented.compare(0, back.GetLength(), back.c_str()) == 0,
                "A long name outside ASCII is cut until its encoded form fits.");

         // A character of two UTF-16 units where the cut falls: 250 letters and
         // the pair would encode to 258, and 250 and the pair's first half alone
         // to 255 - which is not a name. The cut falls before the pair.
         const std::string across = std::string(250, 'a') + "\xF0\x9F\x98\x80" + std::string(50, 'a');
         const String before = J::PstFolderName(across, dot, false);
         AnsiString whole;
         Unicode::WideToMultiByte(ModifiedUTF7::Decode(AnsiString(before)), whole);
         Expect(before.GetLength() <= 255 && PstFormat::IsUtf8(std::string(whole.c_str())) && whole.Find("\xEF\xBF\xBD") < 0 &&
                across.compare(0, whole.GetLength(), whole.c_str()) == 0,
                "The cut never falls between the two halves of a surrogate pair.");

         std::vector<AnsiString> renames;
         std::vector<std::string> names(1, "Inbox");
         std::vector<String> path = J::PstTargetPath(names, dot, renames);
         Expect(path.size() == 1 && path[0] == _T("INBOX") && renames.empty(), "A top-level Inbox is the INBOX.");

         std::vector<std::string> deep;
         for (int i = 0; i < 27; i++)
            deep.push_back("level");
         renames.clear();
         path = J::PstTargetPath(deep, dot, renames);
         Expect(path.size() == 25 && renames.size() == 1, "A folder deeper than 25 levels goes into its ancestor at 25.");

         Expect(J::PstKeyword("Red category") == _T("Red_category") && J::PstKeyword("a(b)") == _T("a_b_") &&
                J::PstKeyword("W\xC3\xB6rk") == _T("W_rk"), "A category as a keyword.");

         // 2024-03-01 09:00 UTC, in this server's time with its offset - what
         // the import keeps is the date and time without the zone, and FETCH
         // gives them back with the server's offset, so the stored time must
         // be the server's.
         const __int64 now = 1790000000;
         const int offset = Time::GetUTCRelationMinutes();
         int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, weekday = 0;
         PstFormat::UnixToCivil(1709283600 + (int64_t) offset * 60, year, month, day, hour, minute, second, weekday);
         static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
         AnsiString zone;
         Unicode::WideToMultiByte(Time::GetUTCRelation(), zone);
         char expected[64];
         snprintf(expected, sizeof(expected), "%02d-%s-%04d %02d:%02d:%02d %s", day, months[(month - 1) % 12], year, hour, minute, second, zone.c_str());
         char stored[64];
         snprintf(stored, sizeof(stored), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
         const String internalDate = J::PstInternalDate(133537572000000000ULL, now);
         AnsiString written;
         Unicode::WideToMultiByte(internalDate, written);
         AnsiString kept;
         Unicode::WideToMultiByte(Time::GetInternalDateFromIMAPInternalDate(internalDate), kept);
         Expect(written == expected, "An internal date from a FILETIME, in the server's time with its offset.");
         Expect(kept == stored, "What the import keeps of it is the server's time, as a delivery's is.");
         Expect(J::PstInternalDate(116444736000000000ULL + 86400ULL * 10000000ULL, now).IsEmpty(), "1970 is no internal date.");
         Expect(J::PstInternalDate(((uint64_t) now + 11644473600ULL + 3 * 86400ULL) * 10000000ULL, now).IsEmpty(), "Nor is three days from now.");
      }
   }

   void
   PstTester::Test()
   {
      TestFormat();
      TestNdb();
      TestLtp();
      TestRtf();
      TestCodePages();
      TestStore();
      TestMime();
      TestImportRules();
      TestBudget();
      TestStoreLimits();
      TestOfflineStoreOrder();
   }
}
