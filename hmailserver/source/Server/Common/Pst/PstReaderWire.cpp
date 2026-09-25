// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstReaderWire.h.

#include "StdAfx.h"

#include "PstReaderWire.h"

#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const unsigned char PstWire::Magic;
   const uint32_t PstWire::ProtocolVersion;
   const size_t PstWire::HeaderBytes;
   const uint32_t PstWire::MaximumFrameBytes;
   const size_t PstWire::MaximumSentenceBytes;
   const size_t PstWire::MaximumTextBytes;
   const size_t PstWire::MaximumRecordKeyBytes;
   const size_t PstWire::MaximumWarnings;
   const size_t PstWire::MaximumCategories;
   const size_t PstWire::MaximumProblems;
   const size_t PstWire::MaximumPasswordBytes;

   namespace
   {
      // The least a folder can take on the wire: its NID, its parent's, the
      // count of its path, its class's length, its role, its count and two
      // flags.
      const size_t SmallestFolder = 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1;

      // The least a warning can take: two empty strings.
      const size_t SmallestWarning = 4 + 4;

      // What a Put function writes of a string: at most maximum bytes, cut at
      // a character, as the reader cuts what it keeps.
      std::string Within(const std::string &value, size_t maximum)
      {
         return value.size() <= maximum ? value : PstFormat::CutUtf8(value, maximum);
      }

      // And of a list: at most maximumCount of them, each within maximumBytes.
      std::vector<std::string> Within(const std::vector<std::string> &values, size_t maximumCount, size_t maximumBytes)
      {
         std::vector<std::string> kept;
         const size_t count = values.size() < maximumCount ? values.size() : maximumCount;
         kept.reserve(count);
         for (size_t i = 0; i < count; i++)
            kept.push_back(Within(values[i], maximumBytes));
         return kept;
      }
   }

   // ------------------------------------------------------------ the writer

   void
   PstWireWriter::U8(unsigned value)
   {
      data_.push_back((char) (unsigned char) (value & 0xFF));
   }

   void
   PstWireWriter::U32(uint32_t value)
   {
      for (int i = 0; i < 4; i++)
         data_.push_back((char) (unsigned char) ((value >> (8 * i)) & 0xFF));
   }

   void
   PstWireWriter::U64(uint64_t value)
   {
      for (int i = 0; i < 8; i++)
         data_.push_back((char) (unsigned char) ((value >> (8 * i)) & 0xFF));
   }

   void
   PstWireWriter::I32(int32_t value)
   {
      U32((uint32_t) value);
   }

   void
   PstWireWriter::Bytes(const std::string &value)
   {
      if (value.size() > (size_t) PstWire::MaximumFrameBytes)
         throw PstWireError("A field is larger than a frame may be.");

      U32((uint32_t) value.size());
      data_.append(value);
   }

   void
   PstWireWriter::Strings(const std::vector<std::string> &values)
   {
      if (values.size() > (size_t) PstWire::MaximumFrameBytes)
         throw PstWireError("A list is longer than a frame may hold.");

      U32((uint32_t) values.size());
      for (const std::string &value : values)
         Bytes(value);
   }

   // ------------------------------------------------------------ the reader

   void
   PstWireReader::Need_(size_t bytes) const
   {
      if (at_ > data_.size() || bytes > data_.size() - at_)
         throw PstWireError("A reply from the PST reader ends inside a field.");
   }

   uint8_t
   PstWireReader::U8()
   {
      Need_(1);
      return (uint8_t) (unsigned char) data_[at_++];
   }

   uint32_t
   PstWireReader::U32()
   {
      Need_(4);
      uint32_t value = 0;
      for (int i = 0; i < 4; i++)
         value |= (uint32_t) (unsigned char) data_[at_ + (size_t) i] << (8 * i);
      at_ += 4;
      return value;
   }

   uint64_t
   PstWireReader::U64()
   {
      Need_(8);
      uint64_t value = 0;
      for (int i = 0; i < 8; i++)
         value |= (uint64_t) (unsigned char) data_[at_ + (size_t) i] << (8 * i);
      at_ += 8;
      return value;
   }

   int32_t
   PstWireReader::I32()
   {
      return (int32_t) U32();
   }

   void
   PstWireReader::Span(size_t &offset, size_t &length)
   {
      const uint32_t declared = U32();
      Need_((size_t) declared);
      offset = at_;
      length = (size_t) declared;
      at_ += length;
   }

   std::string
   PstWireReader::Bytes(size_t maximum)
   {
      Need_(4);
      uint32_t declared = 0;
      for (int i = 0; i < 4; i++)
         declared |= (uint32_t) (unsigned char) data_[at_ + (size_t) i] << (8 * i);
      if ((size_t) declared > maximum)
         throw PstWireError("A reply from the PST reader holds a string longer than the reader keeps (" + std::to_string(declared) +
                            " bytes, of at most " + std::to_string(maximum) + ").");

      size_t offset = 0;
      size_t length = 0;
      Span(offset, length);
      return data_.substr(offset, length);
   }

   uint32_t
   PstWireReader::Count(size_t smallest, size_t maximum)
   {
      const uint32_t count = U32();
      if ((size_t) count > maximum)
         throw PstWireError("A reply from the PST reader counts more elements than the reader keeps (" + std::to_string(count) +
                            ", of at most " + std::to_string(maximum) + ").");

      const size_t each = smallest == 0 ? 1 : smallest;
      if ((size_t) count > Left() / each)
         throw PstWireError("A reply from the PST reader counts more elements than it holds.");
      return count;
   }

   std::vector<std::string>
   PstWireReader::Strings(size_t maximumCount, size_t maximumBytes)
   {
      // Checked before anything is reserved: the count against the most there
      // may be and against what the payload holds.
      const uint32_t count = Count(4, maximumCount);
      std::vector<std::string> values;
      values.reserve((size_t) count);
      for (uint32_t i = 0; i < count; i++)
         values.push_back(Bytes(maximumBytes));
      return values;
   }

   void
   PstWireReader::End() const
   {
      if (at_ != data_.size())
         throw PstWireError("A reply from the PST reader holds more than its fields.");
   }

   // ------------------------------------------------------------ frames

   std::string
   PstWire::Header(unsigned kind, size_t length)
   {
      if (length > (size_t) MaximumFrameBytes)
         throw PstWireError("A frame is larger than the protocol carries.");

      std::string header;
      header.push_back((char) Magic);
      header.push_back((char) (unsigned char) (kind & 0xFF));
      const uint32_t value = (uint32_t) length;
      for (int i = 0; i < 4; i++)
         header.push_back((char) (unsigned char) ((value >> (8 * i)) & 0xFF));
      return header;
   }

   bool
   PstWire::ParseHeader(const unsigned char *header, unsigned &kind, uint32_t &length, std::string &problem)
   {
      kind = 0;
      length = 0;

      if (header[0] != Magic)
      {
         problem = "it wrote something that is not a frame of the protocol";
         return false;
      }

      kind = header[1];
      for (int i = 0; i < 4; i++)
         length |= (uint32_t) header[2 + i] << (8 * i);

      if (length > MaximumFrameBytes)
      {
         problem = "it announced a frame of " + std::to_string(length) + " bytes, more than the protocol carries";
         return false;
      }

      return true;
   }

   PstWire::FrameResult
   PstWire::ReadFrame(const ReadFunction &source, unsigned &kind, std::string &payload, std::string &problem)
   {
      payload.clear();
      kind = 0;

      // Exactly length bytes into buffer; false at an end or an error, with
      // got saying how many came and failed whether it was an error.
      bool failed = false;
      auto readExactly = [&](char *buffer, size_t length, size_t &got) -> bool
      {
         got = 0;
         while (got < length)
         {
            const long long count = source(buffer + got, length - got);
            if (count <= 0)
            {
               failed = count < 0;
               return false;
            }
            got += (size_t) count;
         }
         return true;
      };

      unsigned char header[HeaderBytes];
      size_t got = 0;
      if (!readExactly((char *) header, HeaderBytes, got))
      {
         if (got == 0 && !failed)
            return FrameEnded;
         problem = failed ? "its output could not be read" : "it ended inside a frame's header";
         return FrameBroken;
      }

      uint32_t length = 0;
      if (!ParseHeader(header, kind, length, problem))
         return FrameBroken;

      // In pieces, as the bytes come: a header that claims 300 MB and is
      // followed by nothing costs a megabyte, not 300.
      const size_t piece = 1024 * 1024;
      while (payload.size() < (size_t) length)
      {
         const size_t before = payload.size();
         const size_t want = (size_t) length - before < piece ? (size_t) length - before : piece;
         payload.resize(before + want);
         if (!readExactly(&payload[before], want, got))
         {
            payload.clear();
            problem = failed ? "its output could not be read" : "it ended inside a frame";
            return FrameBroken;
         }
      }

      return FrameRead;
   }

   // ------------------------------------------------------------ records

   void
   PstWire::PutHello(PstWireWriter &writer, uint32_t processId, bool fileTaken)
   {
      writer.U32(ProtocolVersion);
      writer.U32(processId);
      writer.U8(fileTaken ? 1 : 0);
   }

   void
   PstWire::GetHello(PstWireReader &reader, uint32_t &version, uint32_t &processId, bool &fileTaken)
   {
      version = reader.U32();
      processId = reader.U32();
      fileTaken = reader.U8() != 0;
   }

   void
   PstWire::PutStatus(PstWireWriter &writer, const PstReplyStatus &status)
   {
      writer.U8((unsigned) status.status);
      writer.U8((unsigned) status.refusalReason);
      writer.U8(status.wholeFile ? 1 : 0);
      writer.Bytes(Within(status.sentence, MaximumSentenceBytes));
      writer.U8(status.fileExhausted ? 1 : 0);
      writer.U64(status.itemsUsed);
      writer.U64(status.bytesUsed);
   }

   void
   PstWire::GetStatus(PstWireReader &reader, PstReplyStatus &status)
   {
      status = PstReplyStatus();
      status.status = reader.U8();
      status.refusalReason = reader.U8();
      status.wholeFile = reader.U8() != 0;
      status.sentence = reader.Bytes(MaximumSentenceBytes);
      status.fileExhausted = reader.U8() != 0;
      status.itemsUsed = reader.U64();
      status.bytesUsed = reader.U64();

      if (status.status > StatusFailed)
         throw PstWireError("A reply from the PST reader has a status the protocol does not have.");
   }

   void
   PstWire::PutOpenRequest(PstWireWriter &writer, const PstOpenRequest &request)
   {
      writer.Bytes(request.password);
      writer.U64(request.itemsUsed);
      writer.U64(request.bytesUsed);
      PutNids(writer, request.endAt);
   }

   void
   PstWire::GetOpenRequest(PstWireReader &reader, PstOpenRequest &request)
   {
      request = PstOpenRequest();
      request.password = reader.Bytes(MaximumPasswordBytes);
      request.itemsUsed = reader.U64();
      request.bytesUsed = reader.U64();
      GetNids(reader, request.endAt);
   }

   void
   PstWire::PutOpenReply(PstWireWriter &writer, const PstOpenReply &reply)
   {
      writer.Bytes(reply.recordKey);
      writer.Bytes(Within(reply.displayName, MaximumTextBytes));
      writer.I32(reply.version);
      writer.I32(reply.crypt);
   }

   void
   PstWire::GetOpenReply(PstWireReader &reader, PstOpenReply &reply)
   {
      reply = PstOpenReply();
      reply.recordKey = reader.Bytes(MaximumRecordKeyBytes);
      reply.displayName = reader.Bytes(MaximumTextBytes);
      reply.version = reader.I32();
      reply.crypt = reader.I32();
   }

   void
   PstWire::PutFolders(PstWireWriter &writer, const std::vector<PstFolderInfo> &folders, const std::vector<std::string> &problems)
   {
      writer.U32((uint32_t) folders.size());
      for (const PstFolderInfo &folder : folders)
      {
         writer.U32(folder.nid);
         writer.U32(folder.parentNid);
         writer.Strings(Within(folder.path, (size_t) PstStore::MaximumFolderDepth, PstStore::MaximumFolderNameBytes));
         writer.Bytes(Within(folder.containerClass, PstStore::MaximumFolderNameBytes));
         writer.I32((int32_t) folder.role);
         writer.I32(folder.contentCount);
         writer.U8(folder.mail ? 1 : 0);
         writer.U8(folder.hidden ? 1 : 0);
      }
      writer.Strings(Within(problems, MaximumProblems, MaximumSentenceBytes));
   }

   void
   PstWire::GetFolders(PstWireReader &reader, std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems)
   {
      folders.clear();
      problems.clear();

      const uint32_t count = reader.Count(SmallestFolder, PstStore::MaximumFolders);
      folders.reserve((size_t) count);
      for (uint32_t i = 0; i < count; i++)
      {
         PstFolderInfo folder;
         folder.nid = reader.U32();
         folder.parentNid = reader.U32();
         folder.path = reader.Strings((size_t) PstStore::MaximumFolderDepth, PstStore::MaximumFolderNameBytes);
         folder.containerClass = reader.Bytes(PstStore::MaximumFolderNameBytes);
         folder.role = reader.I32();
         folder.contentCount = reader.I32();
         folder.mail = reader.U8() != 0;
         folder.hidden = reader.U8() != 0;

         if (folder.role < PstFolderInfo::RoleNone || folder.role > PstFolderInfo::RoleJunk)
            throw PstWireError("A reply from the PST reader gives a folder a role there is not.");

         folders.push_back(folder);
      }

      problems = reader.Strings(MaximumProblems, MaximumSentenceBytes);
   }

   void
   PstWire::PutNids(PstWireWriter &writer, const std::vector<uint32_t> &nids)
   {
      writer.U32((uint32_t) nids.size());
      for (uint32_t nid : nids)
         writer.U32(nid);
   }

   void
   PstWire::GetNids(PstWireReader &reader, std::vector<uint32_t> &nids)
   {
      nids.clear();
      const uint32_t count = reader.Count(4, PstStore::MaximumMessagesPerFolder);
      nids.reserve((size_t) count);
      for (uint32_t i = 0; i < count; i++)
         nids.push_back(reader.U32());
   }

   void
   PstWire::PutMailHead(PstWireWriter &writer, const PstMailRead &mail)
   {
      writer.U8((unsigned) mail.outcome);
      writer.Bytes(Within(mail.message.messageClass, MaximumTextBytes));
      writer.Bytes(Within(mail.message.subject, MaximumTextBytes));
      writer.I32(mail.message.messageFlags);
      writer.I32(mail.message.flagStatus);
      writer.I32(mail.message.lastVerb);
      writer.U64(mail.message.submitTime);
      writer.U64(mail.message.deliveryTime);
      writer.U64(mail.message.creationTime);
      writer.Strings(Within(mail.message.categories, MaximumCategories, MaximumTextBytes));

      // The rebuild adds a few warnings of its own to the reader's hundred, and
      // its embedded messages' up to twice as many: past that, the first kept.
      const size_t warnings = mail.warnings.size() < MaximumWarnings ? mail.warnings.size() : MaximumWarnings;
      writer.U32((uint32_t) warnings);
      for (size_t i = 0; i < warnings; i++)
      {
         writer.Bytes(Within(mail.warnings[i].word, MaximumSentenceBytes));
         writer.Bytes(Within(mail.warnings[i].detail, MaximumSentenceBytes));
      }

      writer.Bytes(Within(mail.why, MaximumSentenceBytes));
   }

   void
   PstWire::GetMailHead(PstWireReader &reader, PstMailRead &mail)
   {
      mail = PstMailRead();
      mail.outcome = reader.U8();
      if (mail.outcome > PstMailRead::NotBuiltShortOfMemory)
         throw PstWireError("A reply from the PST reader says a message came out in a way there is not.");

      mail.message.messageClass = reader.Bytes(MaximumTextBytes);
      mail.message.subject = reader.Bytes(MaximumTextBytes);
      mail.message.messageFlags = reader.I32();
      mail.message.flagStatus = reader.I32();
      mail.message.lastVerb = reader.I32();
      mail.message.submitTime = reader.U64();
      mail.message.deliveryTime = reader.U64();
      mail.message.creationTime = reader.U64();
      mail.message.categories = reader.Strings(MaximumCategories, MaximumTextBytes);

      const uint32_t warnings = reader.Count(SmallestWarning, MaximumWarnings);
      mail.warnings.reserve((size_t) warnings);
      for (uint32_t i = 0; i < warnings; i++)
      {
         PstWarning warning;
         warning.word = reader.Bytes(MaximumSentenceBytes);
         warning.detail = reader.Bytes(MaximumSentenceBytes);
         mail.warnings.push_back(warning);
      }

      mail.why = reader.Bytes(MaximumSentenceBytes);
   }

   void
   PstWire::PutItemFolders(PstWireWriter &writer, const PstItemFoldersReply &reply)
   {
      writer.U32(reply.contacts);
      writer.U32(reply.calendar);
      writer.U32(reply.tasks);
      writer.Bytes(Within(reply.namesProblem, MaximumSentenceBytes));
   }

   void
   PstWire::GetItemFolders(PstWireReader &reader, PstItemFoldersReply &reply)
   {
      reply = PstItemFoldersReply();
      reply.contacts = reader.U32();
      reply.calendar = reader.U32();
      reply.tasks = reader.U32();
      reply.namesProblem = reader.Bytes(MaximumSentenceBytes);
   }

   void
   PstWire::PutContact(PstWireWriter &writer, const PstContactRead &contact)
   {
      writer.U8((unsigned) contact.outcome);
      writer.Bytes(Within(contact.messageClass, MaximumTextBytes));
      writer.Strings(Within(contact.notes, MaximumWarnings, MaximumSentenceBytes));
      writer.Bytes(contact.card);
      writer.U8(contact.photoLeftOut ? 1 : 0);
   }

   void
   PstWire::GetContact(PstWireReader &reader, PstContactRead &contact)
   {
      contact = PstContactRead();
      contact.outcome = reader.U8();
      if (contact.outcome > PstContactRead::NotAContact)
         throw PstWireError("A reply from the PST reader says a contact came out in a way there is not.");

      contact.messageClass = reader.Bytes(MaximumTextBytes);
      contact.notes = reader.Strings(MaximumWarnings, MaximumSentenceBytes);
      contact.card = reader.Bytes(MaximumFrameBytes);
      contact.photoLeftOut = reader.U8() != 0;
   }

   void
   PstWire::PutCalendarItem(PstWireWriter &writer, const PstCalendarRead &item)
   {
      writer.U8((unsigned) item.outcome);
      writer.Bytes(Within(item.messageClass, MaximumTextBytes));
      writer.Strings(Within(item.notes, MaximumWarnings, MaximumSentenceBytes));
      writer.Bytes(item.text);
      writer.Bytes(Within(item.word, MaximumSentenceBytes));
      writer.Bytes(Within(item.why, MaximumSentenceBytes));
   }

   void
   PstWire::GetCalendarItem(PstWireReader &reader, PstCalendarRead &item)
   {
      item = PstCalendarRead();
      item.outcome = reader.U8();
      if (item.outcome > PstCalendarRead::Refused)
         throw PstWireError("A reply from the PST reader says an item came out in a way there is not.");

      item.messageClass = reader.Bytes(MaximumTextBytes);
      item.notes = reader.Strings(MaximumWarnings, MaximumSentenceBytes);
      item.text = reader.Bytes(MaximumFrameBytes);
      item.word = reader.Bytes(MaximumSentenceBytes);
      item.why = reader.Bytes(MaximumSentenceBytes);
   }
}
