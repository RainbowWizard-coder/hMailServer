// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the PST reader's process and the service say to each other
// (docs/Migration.md, "Outlook data files (PST)"; PstReaderProcess.h is the
// service's half, PstReaderChild.h the reader's).
//
// THE FRAME. Both ways, one frame at a time, request and reply in turn:
//
//    offset 0   one byte, 0xA7 - not ASCII, so that a line some library prints
//               to the reader's standard output is never taken for a frame
//    offset 1   one byte, the kind: a request's (1 to 7), or its reply's, which
//               is the request's with the top bit set; 0x80 is the reader's
//               hello, the one frame nobody asked for
//    offset 2   four bytes, little-endian: how many bytes follow, at most
//               MaximumFrameBytes
//    offset 6   the payload
//
// THE PAYLOAD is a run of fields in an order each kind fixes, with no names and
// no padding: a byte, a 32- or 64-bit number little-endian, or a string - four
// bytes of length, then that many bytes - and a list, which is four bytes of
// count and then its elements. Nothing is optional: every field of a kind is
// always there, so that a payload is either exactly its kind's fields or it is
// refused, with trailing bytes refused as well.
//
// A REPLY begins with its status: whether the request was done, or what the
// reader threw instead - a refusal of the file with its reason, damage in it
// (PstError), a value or a message too large (PstTooLarge), a work budget spent
// (PstBudgetExhausted, and whether it was the whole file's), memory short, any
// other failure - with its sentence; then whether the whole file's budget is
// spent, and what the file has cost so far, so that a reader started again goes
// on from there. The body follows only a reply that was done.
//
// NOTHING IN A FRAME IS TRUSTED by the side that reads it. The service reads what
// a process that has just parsed a hostile file wrote: a frame is held to
// MaximumFrameBytes before any of it is read, and a payload is read in pieces as
// it arrives, never allocated whole from the length its header claims. Every
// list is held to the most the reader itself keeps - the folders, a path's depth,
// the problems, a message's or an item's warnings and categories - and to what
// is left of its frame, before anything is reserved for it; every string to the
// most the reader keeps of it - a folder's name, a class or a subject, and 4 KB
// for a sentence (a status's, a why, a word, a problem, a warning, a note),
// which is what reaches the report and the error log. Anything that does not
// hold throws PstWireError, and the service takes that as the reader having
// failed, as if it had ended. The Put functions write within the same limits -
// a list cut to its most, a string cut at a character - so that no reply the
// reader writes is one the service refuses; only a reader that is not this
// program's, or not itself any more, writes past them.

#pragma once

#include "PstStore.h"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace HM
{
   // A frame, or a payload, that is not what the protocol says.
   class PstWireError : public std::runtime_error
   {
   public:
      explicit PstWireError(const std::string &what) : std::runtime_error(what) { }
   };

   class PstWireWriter
   {
   public:
      void U8(unsigned value);
      void U32(uint32_t value);
      void U64(uint64_t value);
      void I32(int32_t value);

      // A string of any bytes: four bytes of length, then the bytes.
      void Bytes(const std::string &value);
      void Strings(const std::vector<std::string> &values);

      std::string &Data() { return data_; }
      const std::string &Data() const { return data_; }

   private:
      std::string data_;
   };

   class PstWireReader
   {
   public:
      explicit PstWireReader(const std::string &data, size_t from = 0) : data_(data), at_(from) { }

      uint8_t U8();
      uint32_t U32();
      uint64_t U64();
      int32_t I32();

      // A string of at most maximum bytes; a longer one is refused before it
      // is copied.
      std::string Bytes(size_t maximum);

      // A list of at most maximumCount strings of at most maximumBytes each,
      // refused before anything is reserved for it.
      std::vector<std::string> Strings(size_t maximumCount, size_t maximumBytes);

      // Where the next string's own bytes are, and how many, without copying
      // them: for the rebuilt message, the last and largest field of its reply.
      void Span(size_t &offset, size_t &length);

      // A count of elements, each at least smallest bytes: refused when it is
      // more than maximum, or than the payload can hold.
      uint32_t Count(size_t smallest, size_t maximum);

      // Throws when bytes are left over.
      void End() const;

      size_t Offset() const { return at_; }
      size_t Left() const { return data_.size() - at_; }

   private:
      void Need_(size_t bytes) const;

      const std::string &data_;
      size_t at_;
   };

   // What the reader answers a request with, before the reply's body.
   struct PstReplyStatus
   {
      PstReplyStatus() : status(0), refusalReason(0), wholeFile(false), fileExhausted(false), itemsUsed(0), bytesUsed(0) { }

      int status;                // PstWire::Status
      int refusalReason;         // PstRefusal::Reason, for StatusRefused
      bool wholeFile;            // for StatusBudget: the whole file's budget, not one unit's
      std::string sentence;      // what() of what was thrown
      bool fileExhausted;        // the whole file's budget is spent
      uint64_t itemsUsed;        // what the whole file has cost so far
      uint64_t bytesUsed;
   };

   struct PstOpenRequest
   {
      PstOpenRequest() : itemsUsed(0), bytesUsed(0) { }

      std::string password;
      uint64_t itemsUsed;        // what the file cost the readers before this one
      uint64_t bytesUsed;
      std::vector<uint32_t> endAt;   // the test hook: NIDs a request for which ends the reader
   };

   struct PstOpenReply
   {
      PstOpenReply() : version(0), crypt(0) { }

      std::string recordKey;     // the store's record key, its sixteen bytes
      std::string displayName;
      int32_t version;
      int32_t crypt;
   };

   // A message read, taken as a message where it is a note or a journal entry,
   // and rebuilt as Internet mail - what the mail phase files it by.
   struct PstMailRead
   {
      enum Outcome
      {
         Built = 0,               // mime holds the rebuilt message
         NotMail = 1,             // an appointment, a contact, a task: only the class is here
         NotBuilt = 2,            // read, and the rebuild failed: why says how
         NotBuiltShortOfMemory = 3 // the service's reading of it; this program's reader answers memory short as too large
      };

      PstMailRead() : outcome(Built) { }

      int outcome;
      PstMessage message;        // the class, subject, flags, times and categories; no body and no parts
      std::vector<PstWarning> warnings;   // the message's, and the rebuild's
      std::string why;
      std::string mime;
   };

   // A contact read, and made a vCard.
   struct PstContactRead
   {
      enum Outcome
      {
         Card = 0,
         DistributionList = 1,
         NotAContact = 2
      };

      PstContactRead() : outcome(Card), photoLeftOut(false) { }

      int outcome;
      std::string messageClass;
      std::vector<std::string> notes;     // what the contact came in without
      std::string card;
      bool photoLeftOut;         // the card with its picture was larger than an item may be
   };

   // An appointment or a task read, and made iCalendar.
   struct PstCalendarRead
   {
      enum Outcome
      {
         Text = 0,
         NotAnItem = 1,
         Refused = 2               // word and why say why no text could be made
      };

      PstCalendarRead() : outcome(Text) { }

      int outcome;
      std::string messageClass;
      std::vector<std::string> notes;
      std::string text;
      std::string word;
      std::string why;
   };

   struct PstItemFoldersReply
   {
      PstItemFoldersReply() : contacts(0), calendar(0), tasks(0) { }

      uint32_t contacts;
      uint32_t calendar;
      uint32_t tasks;
      std::string namesProblem;
   };

   class PstWire
   {
   public:
      static const unsigned char Magic = 0xA7;
      static const uint32_t ProtocolVersion = 1;
      static const size_t HeaderBytes = 6;

      // The most a frame may carry: a message of MaximumMessageBytes rebuilt,
      // its attachments in base64, with room to spare. A message whose rebuild
      // is larger is refused by the reader as too large.
      static const uint32_t MaximumFrameBytes = 320u * 1024u * 1024u;

      // The most of each thing a reply carries: what the reader keeps of it,
      // and what the service reads (the header, NOTHING IN A FRAME).
      static const size_t MaximumSentenceBytes = 4096;             // a status's sentence, a why, a word, a problem, a warning, a note
      static const size_t MaximumTextBytes = 256 * 1024;           // a class, a subject, a store's name, a category: 64 KB of UTF-16 or eight-bit text as UTF-8
      static const size_t MaximumRecordKeyBytes = 64;
      static const size_t MaximumWarnings = 2 * PstStore::MaximumWarnings;   // a message's with its rebuild's, an item's notes
      static const size_t MaximumCategories = 1024;
      static const size_t MaximumProblems = PstStore::MaximumProblems + 1;  // and the line that counts the rest
      static const size_t MaximumPasswordBytes = 64 * 1024;

      enum Kind
      {
         KindOpen = 0x01,
         KindListFolders = 0x02,
         KindListMessages = 0x03,
         KindReadMail = 0x04,
         KindItemFolders = 0x05,
         KindReadContact = 0x06,
         KindReadCalendarItem = 0x07,
         KindReply = 0x80,         // or'd with the request's kind; alone, the hello
         KindHello = 0x80
      };

      enum Status
      {
         StatusDone = 0,
         StatusRefused = 1,        // PstRefusal
         StatusDamaged = 2,        // PstError
         StatusTooLarge = 3,       // PstTooLarge
         StatusBudget = 4,         // PstBudgetExhausted
         StatusMemory = 5,         // std::bad_alloc; this program's reader answers it as StatusTooLarge (its own ceiling)
         StatusFailed = 6          // anything else
      };

      // The six bytes before a payload. Throws PstWireError past MaximumFrameBytes.
      static std::string Header(unsigned kind, size_t length);

      // A header read: false, with the reason, for six bytes that are not one.
      static bool ParseHeader(const unsigned char *header, unsigned &kind, uint32_t &length, std::string &problem);

      // Reads up to length bytes into buffer: how many, 0 at the end of the
      // stream, negative on an error.
      typedef std::function<long long(char *buffer, size_t length)> ReadFunction;

      enum FrameResult
      {
         FrameRead,                // kind and payload are the frame's
         FrameEnded,               // the stream ended where a frame would begin
         FrameBroken               // it ended inside one, failed, or held something that is not one
      };

      // One whole frame. The payload is read in pieces of at most a megabyte as
      // they arrive, so a header that claims more than follows allocates no more
      // than has come.
      static FrameResult ReadFrame(const ReadFunction &source, unsigned &kind, std::string &payload, std::string &problem);

      // The records, each written and read back field for field.
      static void PutHello(PstWireWriter &writer, uint32_t processId, bool fileTaken);
      static void GetHello(PstWireReader &reader, uint32_t &version, uint32_t &processId, bool &fileTaken);

      static void PutStatus(PstWireWriter &writer, const PstReplyStatus &status);
      static void GetStatus(PstWireReader &reader, PstReplyStatus &status);

      static void PutOpenRequest(PstWireWriter &writer, const PstOpenRequest &request);
      static void GetOpenRequest(PstWireReader &reader, PstOpenRequest &request);
      static void PutOpenReply(PstWireWriter &writer, const PstOpenReply &reply);
      static void GetOpenReply(PstWireReader &reader, PstOpenReply &reply);

      static void PutFolders(PstWireWriter &writer, const std::vector<PstFolderInfo> &folders, const std::vector<std::string> &problems);
      static void GetFolders(PstWireReader &reader, std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems);

      static void PutNids(PstWireWriter &writer, const std::vector<uint32_t> &nids);
      static void GetNids(PstWireReader &reader, std::vector<uint32_t> &nids);

      // Everything of a mail reply but the rebuilt message itself, which is its
      // last field, a string: written after this by the caller (the reader
      // writes it straight from where it was built, never copied into the
      // payload), and read after this with Span or Bytes.
      static void PutMailHead(PstWireWriter &writer, const PstMailRead &mail);
      static void GetMailHead(PstWireReader &reader, PstMailRead &mail);

      static void PutItemFolders(PstWireWriter &writer, const PstItemFoldersReply &reply);
      static void GetItemFolders(PstWireReader &reader, PstItemFoldersReply &reply);

      static void PutContact(PstWireWriter &writer, const PstContactRead &contact);
      static void GetContact(PstWireReader &reader, PstContactRead &contact);

      static void PutCalendarItem(PstWireWriter &writer, const PstCalendarRead &item);
      static void GetCalendarItem(PstWireReader &reader, PstCalendarRead &item);
   };
}
