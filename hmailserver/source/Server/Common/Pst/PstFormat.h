// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST reader (docs/Migration.md, "Outlook data files (PST)"): the pieces of
// Microsoft's published format every layer of it stands on.
//
// THE READER IS THIS PROJECT'S OWN. It is written from Microsoft's published
// specifications and nothing else - [MS-PST] (the file: the NDB layer of pages,
// blocks and B-trees, the LTP layer of heaps, property contexts and tables, and
// the messaging layer of stores, folders and messages), [MS-OXRTFCP] (compressed
// RTF), [MS-OXRTFEX] (HTML and text carried in RTF), [MS-OXCMSG] and
// [MS-OXPROPS] (the properties) and [MS-OXCMAIL] (what a message's properties
// mean as Internet mail). No third-party PST code is vendored, copied or
// translated.
//
// A PST IS UNTRUSTED INPUT, and the reader runs inside the service. It can be
// any file an administrator was handed, of any size, and every length, offset,
// count and level in it is a claim to be checked before it is believed. The
// rules every layer keeps:
//
//   - Every read is bounded by the file (PstByteSource::Read refuses a range
//     outside it), and every structure by the block it is in (the U8..U64 reads
//     below throw rather than read past a buffer).
//   - Nothing is allocated from a size the file declares until that size has
//     been checked against a limit the caller gave and against what the input
//     can hold.
//   - Every tree walk - the node and block B-trees, a data tree, a subnode tree,
//     a heap's B-trees and the folder hierarchy - has a depth cap and refuses a
//     page, block or node it has already been to within that walk.
//   - And because a file can still make the reader do the same legitimate work
//     again and again - a table whose every row names one large value, an
//     embedded message that holds its own parent - every read goes through the
//     WORK BUDGET below: each page, block, heap allocation, table cell, subnode,
//     attachment and embedded message read is counted, with the bytes it
//     decodes, against a limit per unit (one message, the store opened, one
//     folder of the list of folders, one folder's list of messages) and a limit
//     for the whole file, and the job's stop is asked at every one of them. So
//     a reading ends - with a reason, or because the job was told to stop -
//     within a bounded amount of work, whatever the file holds.
//   - What does not hold throws PstError with a sentence; the caller decides
//     what that costs - one attachment, one message, one folder, or (for the
//     header and the store) the file. A budget that runs out throws
//     PstBudgetExhausted, which none of a message's per-part catches takes (a
//     folder of the list whose own budget runs out is that folder's problem,
//     and the whole file's is the file's); a stop throws PstStopped, which is
//     not a PstError at all.

#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace HM
{
   // Something in the file is not as [MS-PST] says it must be. what() is a
   // sentence for a report, in English.
   class PstError : public std::runtime_error
   {
   public:
      explicit PstError(const std::string &what) : std::runtime_error(what) { }
   };

   // A value, or a message, larger than the limit the caller read it with:
   // refused before it is allocated.
   class PstTooLarge : public PstError
   {
   public:
      explicit PstTooLarge(const std::string &what) : PstError(what) { }
   };

   // The work budget ran out (PstBudget): one message, one list, or the whole
   // file cost more reading than any file Outlook writes needs. wholeFile says
   // which. A PstTooLarge, so a message it ends is counted too_large; the
   // reader's own per-part catches (an attachment, the recipients) let it
   // through rather than turning it into a warning.
   class PstBudgetExhausted : public PstTooLarge
   {
   public:
      PstBudgetExhausted(const std::string &what, bool wholeFile) : PstTooLarge(what), whole_file_(wholeFile) { }

      bool WholeFile() const { return whole_file_; }

   private:
      bool whole_file_;
   };

   // The job was told to stop - a pause, a cancel, the service stopping - and
   // the reader gave up between two items. Deliberately not a PstError, so that
   // nothing in the reader takes it for damage in the file.
   class PstStopped : public std::runtime_error
   {
   public:
      PstStopped() : std::runtime_error("The job was told to stop while the file was being read.") { }
   };

   // The file as a whole is not read: a kind of file this reader does not take,
   // a header that is damaged, a password missing or wrong. what() is the
   // sentence an administrator reads, saying what to do.
   class PstRefusal : public PstError
   {
   public:
      enum Reason
      {
         ReasonNotPst = 1,
         ReasonOffline = 2,
         // 3 was an ANSI file, which is read now (PstFileAnsi.cpp).
         ReasonFourK = 4,
         ReasonProtected = 5,
         ReasonUnknownFormat = 6,
         ReasonDamaged = 7,
         ReasonPasswordNeeded = 8,
         ReasonPasswordWrong = 9
      };

      PstRefusal(Reason reason, const std::string &sentence) : PstError(sentence), reason_(reason) { }

      Reason GetReason() const { return reason_; }

   private:
      Reason reason_;
   };

   // [MS-PST] 2.2.2.1: the type of a node, the low five bits of its NID.
   enum PstNidType
   {
      PstNidTypeHid = 0x00,
      PstNidTypeInternal = 0x01,
      PstNidTypeNormalFolder = 0x02,
      PstNidTypeSearchFolder = 0x03,
      PstNidTypeNormalMessage = 0x04,
      PstNidTypeAttachment = 0x05,
      PstNidTypeAssocMessage = 0x08,
      PstNidTypeHierarchyTable = 0x0D,
      PstNidTypeContentsTable = 0x0E,
      PstNidTypeAssocContentsTable = 0x0F,
      PstNidTypeLtp = 0x1F
   };

   // [MS-PST] 2.4.1: the nodes every store has.
   const uint32_t PstNidMessageStore = 0x21;
   const uint32_t PstNidNameToIdMap = 0x61;
   const uint32_t PstNidRootFolder = 0x122;
   const uint32_t PstNidAttachmentTable = 0x671;
   const uint32_t PstNidRecipientTable = 0x692;

   // [MS-OXCDATA] 2.11.1: the property types this reader takes.
   const uint16_t PstTypeInteger16 = 0x0002;
   const uint16_t PstTypeInteger32 = 0x0003;
   const uint16_t PstTypeFloating32 = 0x0004;
   const uint16_t PstTypeFloating64 = 0x0005;
   const uint16_t PstTypeCurrency = 0x0006;
   const uint16_t PstTypeFloatingTime = 0x0007;
   const uint16_t PstTypeErrorCode = 0x000A;
   const uint16_t PstTypeBoolean = 0x000B;
   const uint16_t PstTypeObject = 0x000D;
   const uint16_t PstTypeInteger64 = 0x0014;
   const uint16_t PstTypeString8 = 0x001E;
   const uint16_t PstTypeString = 0x001F;
   const uint16_t PstTypeTime = 0x0040;
   const uint16_t PstTypeGuid = 0x0048;
   const uint16_t PstTypeBinary = 0x0102;
   const uint16_t PstTypeMultipleString8 = 0x101E;
   const uint16_t PstTypeMultipleString = 0x101F;
   const uint16_t PstTypeMultipleBinary = 0x1102;

   // THE WORK BUDGET. Every page, block, heap allocation, table cell, subnode,
   // attachment and embedded message the reader reads is one item, and the bytes
   // it decodes are counted with it. A unit - one message read whole, the store
   // opened, one folder of the list of folders, one folder's list of messages -
   // may cost UnitItems items and UnitBytes bytes; the whole file, in one run of
   // a phase of the job, what SetFileLimits gives (0 is no limit) - which each
   // phase sets with SetFileLimitsFor: FileItems items and one more for every
   // FileBytesPerItem bytes of the file, and FileBytes bytes and FileBytesPerByte
   // more for every byte of it. Past either, PstBudgetExhausted. Every item asks
   // the stop check first, and a stop throws PstStopped.
   class PstBudget
   {
   public:
      PstBudget();

      void SetFileLimits(uint64_t bytes, uint64_t items);
      void SetStop(const std::function<bool()> &stop);

      // The whole file's limits for a file of this many bytes (at most 1 TB of
      // it counted), as every phase of a PST job reads one.
      void SetFileLimitsFor(uint64_t fileSize);

      // A new unit: its own counts start again from nothing.
      void BeginUnit();

      void Bytes(uint64_t bytes);
      void Item();

      // PstStopped when the job has been told to stop.
      void CheckStop();

      bool FileExhausted() const { return file_exhausted_; }
      uint64_t UnitItemsUsed() const { return unit_items_; }

      static const uint64_t UnitBytes = 512ULL * 1024 * 1024;
      static const uint64_t UnitItems = 4000000;

      static const uint64_t FileItems = 10000000;
      static const uint64_t FileBytesPerItem = 4;
      static const uint64_t FileBytes = 1024ULL * 1024 * 1024;
      static const uint64_t FileBytesPerByte = 64;

   private:
      void Exhausted_(bool wholeFile);

      std::function<bool()> stop_;
      uint64_t file_bytes_limit_;
      uint64_t file_items_limit_;
      uint64_t file_bytes_;
      uint64_t file_items_;
      uint64_t unit_bytes_;
      uint64_t unit_items_;
      bool file_exhausted_;
   };

   class PstFormat
   {
   public:
      // [MS-PST] 5.3: the CRC-32 table, a register that starts at the given
      // value (0 for every CRC the format stores) and no final inversion.
      static uint32_t Crc(const unsigned char *data, size_t length, uint32_t initial = 0);
      static uint32_t Crc(const std::string &data, uint32_t initial = 0);

      // [MS-PST] 5.1: NDB_CRYPT_PERMUTE. A block is decoded with mpbbI and was
      // encoded with mpbbR; the encoder is here for the tester.
      static void DecodePermute(std::string &data);
      static void EncodePermute(std::string &data);

      // [MS-PST] 5.2: NDB_CRYPT_CYCLIC, keyed by the low 32 bits of the block's
      // BID. The same function encodes and decodes.
      static void CryptCyclic(std::string &data, uint32_t key);

      // mpbbCrypt: mpbbR, mpbbS and mpbbI, 256 bytes each.
      static const unsigned char *CryptTable();

      // Little-endian reads from a buffer. Each checks that the bytes are there
      // and throws PstError when they are not, naming what was being read.
      static uint8_t U8(const std::string &data, size_t offset, const char *what);
      static uint16_t U16(const std::string &data, size_t offset, const char *what);
      static uint32_t U32(const std::string &data, size_t offset, const char *what);
      static uint64_t U64(const std::string &data, size_t offset, const char *what);

      // Whether offset + length lies within size, without overflowing.
      static bool Fits(uint64_t offset, uint64_t length, uint64_t size);

      // UTF-16LE to UTF-8. A surrogate that is not half of a pair becomes
      // U+FFFD; a trailing odd byte is not a code unit and is left out.
      static std::string Utf16ToUtf8(const std::string &utf16);

      // A wide string (UTF-16 where wchar_t is two bytes, UTF-32 where it is
      // four) to UTF-8, the same way.
      static std::string WideToUtf8(const std::wstring &text);

      static void AppendUtf8(std::string &out, uint32_t codePoint);

      // UTF-8 back to UTF-16LE: the password check's second form.
      static std::string Utf8ToUtf16(const std::string &utf8);

      // Whether bytes are valid UTF-8 (no overlong forms, no surrogates).
      static bool IsUtf8(const std::string &bytes);

      // A FILETIME (100-nanosecond intervals since 1601) as seconds since
      // 1970. False for 0, and for a value before 1601 or past 9999.
      static bool FileTimeToUnix(uint64_t fileTime, int64_t &unixSeconds);

      // A civil date and time in UTC from seconds since 1970.
      static void UnixToCivil(int64_t unixSeconds, int &year, int &month, int &day, int &hour, int &minute, int &second, int &weekday);

      // [MS-PST] 2.2.2.2 and 2.4.3.2: the NID a PST EntryID names - its last four
      // bytes, after four bytes of flags and the store's sixteen-byte record key.
      // False when it is not 24 bytes, or names another store.
      static bool NidOfEntryId(const std::string &entryId, const std::string &recordKey, uint32_t &nid);

      // Bytes as lower-case hex.
      static std::string Hex(const std::string &bytes);
      static std::string Hex32(uint32_t value);

      // UTF-8 cut to at most maximum bytes, at a character boundary, with
      // "..." after it when anything was cut (and the three dots within the
      // maximum).
      static std::string CutUtf8(const std::string &utf8, size_t maximum);

      // UTF-8 with every character past U+FFFF made U+FFFD: what a three-byte
      // utf8 column (MySQL's hm_migrationfolders) can hold, for text that is
      // only shown.
      static std::string BmpOnly(const std::string &utf8);
   };
}
