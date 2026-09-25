// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstReaderWireTester.h.

#include "StdAfx.h"

#include "PstReaderWireTester.h"
#include "PstReaderProcess.h"
#include "PstReaderWire.h"

#include <cstring>
#include <functional>
#include <new>
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

      bool ThrowsWireError(const std::function<void()> &call)
      {
         try
         {
            call();
         }
         catch (PstWireError &)
         {
            return true;
         }
         return false;
      }

      // A stream over bytes, handing out at most piece of them a read.
      PstWire::ReadFunction StreamOf(const std::string &bytes, size_t &at, size_t piece)
      {
         return [&bytes, &at, piece](char *buffer, size_t length) -> long long
         {
            if (at >= bytes.size())
               return 0;
            size_t count = bytes.size() - at;
            if (count > length)
               count = length;
            if (count > piece)
               count = piece;
            memcpy(buffer, bytes.data() + at, count);
            at += count;
            return (long long) count;
         };
      }

      std::string Frame(unsigned kind, const std::string &payload)
      {
         return PstWire::Header(kind, payload.size()) + payload;
      }

      PstReplyStatus StatusOf(int value)
      {
         PstReplyStatus status;
         status.status = value;
         status.sentence = "the sentence";
         return status;
      }
   }

   void
   PstReaderWireTester::Test()
   {
      // ---- the fields, written and read back

      {
         PstWireWriter writer;
         writer.U8(0xFE);
         writer.U32(0x01020304);
         writer.U64(0x0102030405060708ULL);
         writer.I32(-2);
         writer.Bytes(std::string("a\0b", 3));
         writer.Strings(std::vector<std::string> { "x", "", "yz" });

         const std::string &data = writer.Data();
         Expect(data.size() == 1 + 4 + 8 + 4 + (4 + 3) + (4 + (4 + 1) + (4 + 0) + (4 + 2)), "The fields take the bytes the protocol gives them, no more.");
         Expect((unsigned char) data[1] == 0x04 && (unsigned char) data[4] == 0x01, "A number is little-endian.");

         PstWireReader reader(data);
         Expect(reader.U8() == 0xFE && reader.U32() == 0x01020304 && reader.U64() == 0x0102030405060708ULL && reader.I32() == -2,
                "The numbers read back.");
         Expect(reader.Bytes(3) == std::string("a\0b", 3), "A string of any bytes, a NUL among them, reads back.");
         const std::vector<std::string> strings = reader.Strings(3, 2);
         Expect(strings.size() == 3 && strings[0] == "x" && strings[1].empty() && strings[2] == "yz", "A list of strings, an empty one among them, reads back.");
         reader.End();

         PstWireWriter spanned;
         spanned.U8(1);
         spanned.Bytes("abc");
         PstWireReader spanReader(spanned.Data());
         spanReader.U8();
         size_t offset = 0;
         size_t length = 0;
         spanReader.Span(offset, length);
         spanReader.End();
         Expect(offset == 5 && length == 3 && spanned.Data().substr(offset, length) == "abc", "A span is where a string's own bytes are, without them copied.");

         PstWireWriter trailing;
         trailing.U32(1);
         trailing.U8(0);
         PstWireReader trailingReader(trailing.Data());
         trailingReader.U32();
         Expect(ThrowsWireError([&]() { trailingReader.End(); }), "Bytes after the last field are refused.");

         // A count larger than what follows it can hold is refused before
         // anything is allocated for it.
         PstWireWriter lying;
         lying.U32(0x7FFFFFFF);
         lying.U32(0);
         PstWireReader lyingReader(lying.Data());
         Expect(ThrowsWireError([&]() { lyingReader.Strings(0x7FFFFFFF, 1); }), "A list that counts more strings than its payload holds is refused.");

         PstWireWriter longString;
         longString.U32(1000);
         longString.U8('a');
         PstWireReader longReader(longString.Data());
         Expect(ThrowsWireError([&]() { longReader.Bytes(1000); }), "A string longer than what is left of its payload is refused.");

         // And each is held to the most it may be, before it is copied or
         // anything is reserved for it: a million empty strings where the
         // reader keeps a thousand categories are four megabytes on the wire
         // and would be 32 MB reserved.
         PstWireWriter manyEmpty;
         manyEmpty.U32(1000000);
         manyEmpty.Data().append((size_t) 4000000, '\0');
         PstWireReader manyEmptyReader(manyEmpty.Data());
         Expect(ThrowsWireError([&]() { manyEmptyReader.Strings(PstWire::MaximumCategories, PstWire::MaximumTextBytes); }),
                "A list longer than the reader keeps is refused before anything is reserved, whatever the payload holds.");

         PstWireWriter pastMost;
         pastMost.Bytes(std::string(11, 'x'));
         PstWireReader pastMostReader(pastMost.Data());
         Expect(ThrowsWireError([&]() { pastMostReader.Bytes(10); }), "A string longer than the most it may be is refused.");
         PstWireReader atMostReader(pastMost.Data());
         Expect(atMostReader.Bytes(11).size() == 11, "A string of the most it may be is read.");

         PstWireWriter longElement;
         longElement.Strings(std::vector<std::string> { "ok", std::string(5, 'y') });
         PstWireReader longElementReader(longElement.Data());
         Expect(ThrowsWireError([&]() { longElementReader.Strings(2, 4); }), "A list one of whose strings is longer than the most is refused.");
      }

      // ---- the frame

      {
         const std::string header = PstWire::Header(0x84, 5);
         unsigned kind = 0;
         uint32_t length = 0;
         std::string problem;
         Expect(header.size() == PstWire::HeaderBytes && (unsigned char) header[0] == PstWire::Magic &&
                PstWire::ParseHeader((const unsigned char *) header.data(), kind, length, problem) && kind == 0x84 && length == 5,
                "A header reads back its kind and its length.");

         std::string printed = header;
         printed[0] = 'H';
         Expect(!PstWire::ParseHeader((const unsigned char *) printed.data(), kind, length, problem),
                "A header that does not begin with the protocol's magic is refused: a line some library prints to the reader's output is not a frame.");

         std::string huge = header;
         const uint32_t tooMuch = PstWire::MaximumFrameBytes + 1;
         for (int i = 0; i < 4; i++)
            huge[(size_t) (2 + i)] = (char) (unsigned char) ((tooMuch >> (8 * i)) & 0xFF);
         Expect(!PstWire::ParseHeader((const unsigned char *) huge.data(), kind, length, problem),
                "A frame longer than the protocol carries is refused from its header.");
         Expect(ThrowsWireError([]() { PstWire::Header(1, (size_t) PstWire::MaximumFrameBytes + 1); }), "No frame longer than the protocol carries is written.");
      }

      {
         // Three frames, the last three megabytes, read through pieces of one
         // byte, seven, 64 KB and the whole stream.
         const std::string stream = Frame(0x81, "first") + Frame(0x82, std::string()) + Frame(0x83, std::string(3000000, 'z'));
         const size_t pieces[] = { 1, 7, 65536, stream.size() };
         for (size_t piece : pieces)
         {
            size_t at = 0;
            const PstWire::ReadFunction reading = StreamOf(stream, at, piece);
            unsigned kind = 0;
            std::string payload;
            std::string problem;
            Expect(PstWire::ReadFrame(reading, kind, payload, problem) == PstWire::FrameRead && kind == 0x81 && payload == "first", "The first frame reads whole.");
            Expect(PstWire::ReadFrame(reading, kind, payload, problem) == PstWire::FrameRead && kind == 0x82 && payload.empty(), "A frame of no payload reads.");
            Expect(PstWire::ReadFrame(reading, kind, payload, problem) == PstWire::FrameRead && kind == 0x83 && payload.size() == 3000000 &&
                   payload[2999999] == 'z', "A frame of three megabytes, past one piece, reads whole.");
            Expect(PstWire::ReadFrame(reading, kind, payload, problem) == PstWire::FrameEnded, "A stream that ends between two frames has ended; it is not broken.");
         }

         // Cut inside a frame, at every length: broken, and nothing of it kept.
         const std::string frame = Frame(0x84, "0123456789");
         for (size_t cut = 1; cut < frame.size(); cut++)
         {
            const std::string part = frame.substr(0, cut);
            size_t at = 0;
            unsigned kind = 0;
            std::string payload;
            std::string problem;
            Expect(PstWire::ReadFrame(StreamOf(part, at, 4096), kind, payload, problem) == PstWire::FrameBroken && payload.empty() && !problem.empty(),
                   "A stream cut inside a frame is broken, at every length.");
         }

         // A header that claims 300 MB and is followed by ten bytes: broken,
         // and no more allocated than the megabyte pieces that were read into.
         const std::string claim = PstWire::Header(0x84, 300u * 1024u * 1024u) + "0123456789";
         size_t at = 0;
         unsigned kind = 0;
         std::string payload;
         std::string problem;
         Expect(PstWire::ReadFrame(StreamOf(claim, at, 4096), kind, payload, problem) == PstWire::FrameBroken && payload.empty() &&
                payload.capacity() <= 4u * 1024u * 1024u,
                "A header that claims 300 MB is not taken at its word: the payload is read in pieces as it comes.");

         // A stream that fails, rather than ends, is broken.
         const PstWire::ReadFunction failing = [](char *, size_t) -> long long { return -1; };
         Expect(PstWire::ReadFrame(failing, kind, payload, problem) == PstWire::FrameBroken, "A stream that fails at a frame's start is broken, not ended.");
      }

      // ---- the records

      {
         uint32_t version = 0;
         uint32_t processId = 0;
         bool fileTaken = false;
         PstWireWriter writer;
         PstWire::PutHello(writer, 4242, true);
         PstWireReader reader(writer.Data());
         PstWire::GetHello(reader, version, processId, fileTaken);
         reader.End();
         Expect(version == PstWire::ProtocolVersion && processId == 4242 && fileTaken, "The hello reads back.");
      }

      {
         PstReplyStatus status;
         status.status = PstWire::StatusBudget;
         status.refusalReason = 0;
         status.wholeFile = true;
         status.sentence = "spent";
         status.fileExhausted = true;
         status.itemsUsed = 12345678901ULL;
         status.bytesUsed = 42;
         PstWireWriter writer;
         PstWire::PutStatus(writer, status);

         PstWireReader reader(writer.Data());
         PstReplyStatus back;
         PstWire::GetStatus(reader, back);
         reader.End();
         Expect(back.status == PstWire::StatusBudget && back.wholeFile && back.sentence == "spent" && back.fileExhausted &&
                back.itemsUsed == 12345678901ULL && back.bytesUsed == 42, "A reply's status reads back, with what the file has cost.");

         std::string unknown = writer.Data();
         unknown[0] = (char) 7;
         PstWireReader unknownReader(unknown);
         PstReplyStatus ignored;
         Expect(ThrowsWireError([&]() { PstWire::GetStatus(unknownReader, ignored); }), "A status the protocol does not have is refused.");
      }

      {
         PstOpenRequest request;
         request.password = "p\xC3\xA4ss";
         request.itemsUsed = 7;
         request.bytesUsed = 1ULL << 40;
         request.endAt = std::vector<uint32_t> { 0x2044, 0x86a4 };
         PstWireWriter writer;
         PstWire::PutOpenRequest(writer, request);
         PstWireReader reader(writer.Data());
         PstOpenRequest back;
         PstWire::GetOpenRequest(reader, back);
         reader.End();
         Expect(back.password == request.password && back.itemsUsed == 7 && back.bytesUsed == (1ULL << 40) && back.endAt == request.endAt,
                "The open request reads back: the password, the file's cost so far and the test hook's NIDs.");

         PstOpenReply reply;
         reply.recordKey = std::string(16, '\x5a');
         reply.displayName = "Personal Folders";
         reply.version = 23;
         reply.crypt = 2;
         PstWireWriter replyWriter;
         PstWire::PutOpenReply(replyWriter, reply);
         PstWireReader replyReader(replyWriter.Data());
         PstOpenReply replyBack;
         PstWire::GetOpenReply(replyReader, replyBack);
         replyReader.End();
         Expect(replyBack.recordKey == reply.recordKey && replyBack.displayName == "Personal Folders" && replyBack.version == 23 && replyBack.crypt == 2,
                "The open reply reads back.");
      }

      {
         std::vector<PstFolderInfo> folders(2);
         folders[0].nid = 0x8022;
         folders[0].parentNid = 0x8062;
         folders[0].path = std::vector<std::string> { "Inbox" };
         folders[0].containerClass = "IPF.Note";
         folders[0].contentCount = 15;
         folders[1].nid = 0x80c2;
         folders[1].parentNid = 0x8022;
         folders[1].path = std::vector<std::string> { "Top", "Entw\xC3\xBCrfe" };
         folders[1].containerClass = "IPF.Appointment";
         folders[1].role = PstFolderInfo::RoleDrafts;
         folders[1].contentCount = -1;
         folders[1].mail = false;
         folders[1].hidden = true;
         const std::vector<std::string> problems { "The folder 0x8042 could not be read." };

         PstWireWriter writer;
         PstWire::PutFolders(writer, folders, problems);
         PstWireReader reader(writer.Data());
         std::vector<PstFolderInfo> back;
         std::vector<std::string> problemsBack;
         PstWire::GetFolders(reader, back, problemsBack);
         reader.End();
         Expect(back.size() == 2 && back[0].nid == 0x8022 && back[0].parentNid == 0x8062 && back[0].path.size() == 1 && back[0].path[0] == "Inbox" &&
                back[0].containerClass == "IPF.Note" && back[0].role == PstFolderInfo::RoleNone && back[0].contentCount == 15 && back[0].mail && !back[0].hidden,
                "A mail folder reads back.");
         Expect(back[1].path.size() == 2 && back[1].path[1] == "Entw\xC3\xBCrfe" && back[1].role == PstFolderInfo::RoleDrafts && back[1].contentCount == -1 &&
                !back[1].mail && back[1].hidden && problemsBack == problems,
                "A folder of another class, hidden, with its role and its path, reads back, and so do the problems.");

         std::vector<PstFolderInfo> badRole = folders;
         badRole[0].role = 9;
         PstWireWriter badWriter;
         PstWire::PutFolders(badWriter, badRole, problems);
         PstWireReader badReader(badWriter.Data());
         Expect(ThrowsWireError([&]() { PstWire::GetFolders(badReader, back, problemsBack); }), "A folder of a role there is not is refused.");

         PstWireWriter manyWriter;
         manyWriter.U32(1000000);
         PstWireReader manyReader(manyWriter.Data());
         Expect(ThrowsWireError([&]() { PstWire::GetFolders(manyReader, back, problemsBack); }), "A folder list that counts more folders than it holds is refused.");
      }

      {
         const std::vector<uint32_t> nids { 0x200024, 0x200044, 0x2000a4 };
         PstWireWriter writer;
         PstWire::PutNids(writer, nids);
         PstWireReader reader(writer.Data());
         std::vector<uint32_t> back;
         PstWire::GetNids(reader, back);
         reader.End();
         Expect(back == nids, "A folder's list of messages reads back, in its order.");
      }

      {
         PstMailRead mail;
         mail.outcome = PstMailRead::Built;
         mail.message.messageClass = "IPM.Note";
         mail.message.subject = "Hello";
         mail.message.messageFlags = 9;
         mail.message.flagStatus = 2;
         mail.message.lastVerb = 104;
         mail.message.submitTime = 133541316000000000ULL;
         mail.message.deliveryTime = 133541316600000000ULL;
         mail.message.creationTime = 1;
         mail.message.categories = std::vector<std::string> { "Red", "Work" };
         mail.warnings.push_back(PstWarning("rtf_body_kept", "the body was kept as body.rtf"));
         mail.mime = "From: a@example.test\r\n\r\nbody\r\n";

         PstWireWriter writer;
         PstWire::PutMailHead(writer, mail);
         writer.Bytes(mail.mime);
         const std::string whole = writer.Data();

         PstWireReader reader(whole);
         PstMailRead back;
         PstWire::GetMailHead(reader, back);
         back.mime = reader.Bytes(PstWire::MaximumFrameBytes);
         reader.End();
         Expect(back.outcome == PstMailRead::Built && back.message.messageClass == "IPM.Note" && back.message.subject == "Hello" &&
                back.message.messageFlags == 9 && back.message.flagStatus == 2 && back.message.lastVerb == 104 &&
                back.message.submitTime == 133541316000000000ULL && back.message.deliveryTime == 133541316600000000ULL && back.message.creationTime == 1 &&
                back.message.categories == mail.message.categories && back.warnings.size() == 1 && back.warnings[0].word == "rtf_body_kept" &&
                back.warnings[0].detail == "the body was kept as body.rtf" && back.why.empty() && back.mime == mail.mime,
                "A message's reply reads back: what the job files it by, its warnings and its rebuilt bytes.");

         // Cut at every length, it is refused, never read past its end.
         for (size_t cut = 0; cut < whole.size(); cut++)
         {
            const std::string part = whole.substr(0, cut);
            Expect(ThrowsWireError([&]()
            {
               PstWireReader partReader(part);
               PstMailRead partMail;
               PstWire::GetMailHead(partReader, partMail);
               partReader.Bytes(PstWire::MaximumFrameBytes);
               partReader.End();
            }), "A message's reply cut at any length is refused.");
         }

         std::string outcome = whole;
         outcome[0] = (char) 4;
         PstWireReader outcomeReader(outcome);
         PstMailRead outcomeMail;
         Expect(ThrowsWireError([&]() { PstWire::GetMailHead(outcomeReader, outcomeMail); }), "A message's outcome the protocol does not have is refused.");
      }

      {
         PstItemFoldersReply reply;
         reply.contacts = 0x8182;
         reply.calendar = 0x81a2;
         reply.tasks = 0x81c2;
         reply.namesProblem = "the map is damaged";
         PstWireWriter writer;
         PstWire::PutItemFolders(writer, reply);
         PstWireReader reader(writer.Data());
         PstItemFoldersReply back;
         PstWire::GetItemFolders(reader, back);
         reader.End();
         Expect(back.contacts == 0x8182 && back.calendar == 0x81a2 && back.tasks == 0x81c2 && back.namesProblem == "the map is damaged",
                "The default item folders and the name map's problem read back.");

         PstContactRead contact;
         contact.outcome = PstContactRead::Card;
         contact.messageClass = "IPM.Contact";
         contact.notes = std::vector<std::string> { "the contact's notes ran to 264 KB" };
         contact.card = "BEGIN:VCARD\r\nVERSION:4.0\r\nEND:VCARD\r\n";
         contact.photoLeftOut = true;
         PstWireWriter contactWriter;
         PstWire::PutContact(contactWriter, contact);
         PstWireReader contactReader(contactWriter.Data());
         PstContactRead contactBack;
         PstWire::GetContact(contactReader, contactBack);
         contactReader.End();
         Expect(contactBack.outcome == PstContactRead::Card && contactBack.messageClass == "IPM.Contact" && contactBack.notes == contact.notes &&
                contactBack.card == contact.card && contactBack.photoLeftOut, "A contact's reply reads back.");

         PstCalendarRead item;
         item.outcome = PstCalendarRead::Refused;
         item.messageClass = "IPM.Appointment";
         item.word = "recurrence_unreadable";
         item.why = "the recurrence pattern's version (0x1234) is not one [MS-OXOCAL] describes";
         PstWireWriter itemWriter;
         PstWire::PutCalendarItem(itemWriter, item);
         PstWireReader itemReader(itemWriter.Data());
         PstCalendarRead itemBack;
         PstWire::GetCalendarItem(itemReader, itemBack);
         itemReader.End();
         Expect(itemBack.outcome == PstCalendarRead::Refused && itemBack.messageClass == "IPM.Appointment" && itemBack.notes.empty() &&
                itemBack.text.empty() && itemBack.word == "recurrence_unreadable" && itemBack.why == item.why, "A calendar item's reply reads back.");
      }

      // ---- each record's limits: a Put cuts to them, a Get refuses past them

      {
         // A status's sentence, which reaches the report and the error log.
         PstReplyStatus status = StatusOf(PstWire::StatusDamaged);
         status.sentence = std::string(10000, 's');
         PstWireWriter cut;
         PstWire::PutStatus(cut, status);
         PstWireReader cutReader(cut.Data());
         PstReplyStatus back;
         PstWire::GetStatus(cutReader, back);
         Expect(back.sentence.size() <= PstWire::MaximumSentenceBytes && back.sentence.size() > 4000, "A status's long sentence is written cut to 4 KB.");

         PstWireWriter raw;
         raw.U8(PstWire::StatusDamaged);
         raw.U8(0);
         raw.U8(0);
         raw.Bytes(std::string(PstWire::MaximumSentenceBytes + 1, 's'));
         raw.U8(0);
         raw.U64(0);
         raw.U64(0);
         PstWireReader rawReader(raw.Data());
         Expect(ThrowsWireError([&]() { PstWire::GetStatus(rawReader, back); }), "A status's sentence past 4 KB is refused.");
      }

      {
         // The folders: a path deeper than the reader walks, a name longer than
         // it keeps, more problems than it names.
         std::vector<PstFolderInfo> folders(1);
         folders[0].nid = 0x8022;
         for (int depth = 0; depth < PstStore::MaximumFolderDepth + 6; depth++)
            folders[0].path.push_back(std::string(PstStore::MaximumFolderNameBytes + 10, 'n'));
         std::vector<std::string> problems((size_t) PstWire::MaximumProblems + 50, std::string(PstWire::MaximumSentenceBytes + 10, 'p'));

         PstWireWriter cut;
         PstWire::PutFolders(cut, folders, problems);
         PstWireReader cutReader(cut.Data());
         std::vector<PstFolderInfo> back;
         std::vector<std::string> problemsBack;
         PstWire::GetFolders(cutReader, back, problemsBack);
         cutReader.End();
         Expect(back.size() == 1 && back[0].path.size() == (size_t) PstStore::MaximumFolderDepth &&
                back[0].path[0].size() <= PstStore::MaximumFolderNameBytes && problemsBack.size() == PstWire::MaximumProblems &&
                problemsBack[0].size() <= PstWire::MaximumSentenceBytes,
                "A folder list is written within the reader's own limits: the depth, the names, the problems.");

         auto rawFolders = [](size_t depth, size_t nameBytes, size_t problemCount) -> std::string
         {
            PstWireWriter raw;
            raw.U32(1);
            raw.U32(0x8022);
            raw.U32(0x8002);
            raw.Strings(std::vector<std::string>(depth, std::string(nameBytes, 'n')));
            raw.Bytes("IPF.Note");
            raw.I32(0);
            raw.I32(0);
            raw.U8(1);
            raw.U8(0);
            raw.Strings(std::vector<std::string>(problemCount, "p"));
            return raw.Data();
         };

         const std::string deep = rawFolders((size_t) PstStore::MaximumFolderDepth + 1, 1, 0);
         PstWireReader deepReader(deep);
         Expect(ThrowsWireError([&]() { PstWire::GetFolders(deepReader, back, problemsBack); }), "A path deeper than the reader walks is refused.");

         const std::string longName = rawFolders(1, PstStore::MaximumFolderNameBytes + 1, 0);
         PstWireReader longNameReader(longName);
         Expect(ThrowsWireError([&]() { PstWire::GetFolders(longNameReader, back, problemsBack); }), "A folder's name longer than the reader keeps is refused.");

         const std::string manyProblems = rawFolders(1, 1, PstWire::MaximumProblems + 1);
         PstWireReader manyProblemsReader(manyProblems);
         Expect(ThrowsWireError([&]() { PstWire::GetFolders(manyProblemsReader, back, problemsBack); }), "More problems than the reader names are refused.");

         const std::string fine = rawFolders((size_t) PstStore::MaximumFolderDepth, PstStore::MaximumFolderNameBytes, PstWire::MaximumProblems);
         PstWireReader fineReader(fine);
         PstWire::GetFolders(fineReader, back, problemsBack);
         fineReader.End();
         Expect(back.size() == 1 && back[0].path.size() == (size_t) PstStore::MaximumFolderDepth, "A folder list at every limit is read.");
      }

      {
         // A message: its warnings, its categories, its why and its subject.
         PstMailRead mail;
         mail.outcome = PstMailRead::NotBuilt;
         mail.message.subject = std::string(PstWire::MaximumTextBytes + 10, 's');
         for (size_t i = 0; i < PstWire::MaximumWarnings + 20; i++)
            mail.warnings.push_back(PstWarning("attachment_not_imported", std::string(PstWire::MaximumSentenceBytes + 10, 'd')));
         mail.message.categories = std::vector<std::string>(PstWire::MaximumCategories + 5, "Red");
         mail.why = std::string(PstWire::MaximumSentenceBytes * 2, 'w');

         PstWireWriter cut;
         PstWire::PutMailHead(cut, mail);
         cut.Bytes(std::string());
         PstWireReader cutReader(cut.Data());
         PstMailRead back;
         PstWire::GetMailHead(cutReader, back);
         cutReader.Bytes(PstWire::MaximumFrameBytes);
         cutReader.End();
         Expect(back.warnings.size() == PstWire::MaximumWarnings && back.warnings[0].detail.size() <= PstWire::MaximumSentenceBytes &&
                back.message.categories.size() == PstWire::MaximumCategories && back.why.size() <= PstWire::MaximumSentenceBytes &&
                back.message.subject.size() <= PstWire::MaximumTextBytes,
                "A message's reply is written within the limits: its warnings, categories, why and subject.");

         auto rawMail = [](size_t warnings, size_t categories, size_t whyBytes, size_t subjectBytes) -> std::string
         {
            PstWireWriter raw;
            raw.U8(PstMailRead::NotBuilt);
            raw.Bytes("IPM.Note");
            raw.Bytes(std::string(subjectBytes, 's'));
            raw.I32(0);
            raw.I32(0);
            raw.I32(0);
            raw.U64(0);
            raw.U64(0);
            raw.U64(0);
            raw.Strings(std::vector<std::string>(categories, "c"));
            raw.U32((uint32_t) warnings);
            for (size_t i = 0; i < warnings; i++)
            {
               raw.Bytes("w");
               raw.Bytes("d");
            }
            raw.Bytes(std::string(whyBytes, 'y'));
            return raw.Data();
         };

         const std::string manyWarnings = rawMail(PstWire::MaximumWarnings + 1, 0, 0, 0);
         PstWireReader manyWarningsReader(manyWarnings);
         Expect(ThrowsWireError([&]() { PstWire::GetMailHead(manyWarningsReader, back); }), "More warnings than a message keeps are refused.");

         const std::string manyCategories = rawMail(0, PstWire::MaximumCategories + 1, 0, 0);
         PstWireReader manyCategoriesReader(manyCategories);
         Expect(ThrowsWireError([&]() { PstWire::GetMailHead(manyCategoriesReader, back); }), "More categories than a message keeps are refused.");

         const std::string longWhy = rawMail(0, 0, PstWire::MaximumSentenceBytes + 1, 0);
         PstWireReader longWhyReader(longWhy);
         Expect(ThrowsWireError([&]() { PstWire::GetMailHead(longWhyReader, back); }), "A why past 4 KB is refused.");

         const std::string longSubject = rawMail(0, 0, 0, PstWire::MaximumTextBytes + 1);
         PstWireReader longSubjectReader(longSubject);
         Expect(ThrowsWireError([&]() { PstWire::GetMailHead(longSubjectReader, back); }), "A subject past what the reader keeps is refused.");

         const std::string atLimits = rawMail(PstWire::MaximumWarnings, PstWire::MaximumCategories, PstWire::MaximumSentenceBytes, PstWire::MaximumTextBytes);
         PstWireReader atLimitsReader(atLimits);
         PstWire::GetMailHead(atLimitsReader, back);
         atLimitsReader.End();
         Expect(back.warnings.size() == PstWire::MaximumWarnings && back.message.categories.size() == PstWire::MaximumCategories,
                "A message's reply at every limit is read.");
      }

      {
         // A contact's and an item's notes, an item's word and why, the name
         // map's problem.
         PstContactRead contact;
         contact.notes = std::vector<std::string>(PstWire::MaximumWarnings + 3, std::string(PstWire::MaximumSentenceBytes + 1, 'n'));
         PstWireWriter contactWriter;
         PstWire::PutContact(contactWriter, contact);
         PstWireReader contactReader(contactWriter.Data());
         PstContactRead contactBack;
         PstWire::GetContact(contactReader, contactBack);
         Expect(contactBack.notes.size() == PstWire::MaximumWarnings && contactBack.notes[0].size() <= PstWire::MaximumSentenceBytes,
                "A contact's notes are written within the limits.");

         PstWireWriter rawContact;
         rawContact.U8(PstContactRead::Card);
         rawContact.Bytes("IPM.Contact");
         rawContact.Strings(std::vector<std::string>(PstWire::MaximumWarnings + 1, "n"));
         rawContact.Bytes("BEGIN:VCARD");
         rawContact.U8(0);
         PstWireReader rawContactReader(rawContact.Data());
         Expect(ThrowsWireError([&]() { PstWire::GetContact(rawContactReader, contactBack); }), "More notes than an item keeps are refused.");

         auto rawItem = [](size_t notes, size_t wordBytes, size_t whyBytes) -> std::string
         {
            PstWireWriter raw;
            raw.U8(PstCalendarRead::Refused);
            raw.Bytes("IPM.Appointment");
            raw.Strings(std::vector<std::string>(notes, "n"));
            raw.Bytes(std::string());
            raw.Bytes(std::string(wordBytes, 'w'));
            raw.Bytes(std::string(whyBytes, 'y'));
            return raw.Data();
         };
         PstCalendarRead itemBack;
         const std::string longWord = rawItem(0, PstWire::MaximumSentenceBytes + 1, 0);
         PstWireReader longWordReader(longWord);
         Expect(ThrowsWireError([&]() { PstWire::GetCalendarItem(longWordReader, itemBack); }), "An item's word past 4 KB is refused.");
         const std::string longItemWhy = rawItem(0, 0, PstWire::MaximumSentenceBytes + 1);
         PstWireReader longItemWhyReader(longItemWhy);
         Expect(ThrowsWireError([&]() { PstWire::GetCalendarItem(longItemWhyReader, itemBack); }), "An item's why past 4 KB is refused.");
         const std::string manyNotes = rawItem(PstWire::MaximumWarnings + 1, 0, 0);
         PstWireReader manyNotesReader(manyNotes);
         Expect(ThrowsWireError([&]() { PstWire::GetCalendarItem(manyNotesReader, itemBack); }), "More notes than an item keeps are refused.");

         PstWireWriter rawFolders;
         rawFolders.U32(0);
         rawFolders.U32(0);
         rawFolders.U32(0);
         rawFolders.Bytes(std::string(PstWire::MaximumSentenceBytes + 1, 'm'));
         PstWireReader rawFoldersReader(rawFolders.Data());
         PstItemFoldersReply foldersBack;
         Expect(ThrowsWireError([&]() { PstWire::GetItemFolders(rawFoldersReader, foldersBack); }), "The name map's problem past 4 KB is refused.");

         PstWireWriter rawOpen;
         rawOpen.Bytes(std::string(PstWire::MaximumRecordKeyBytes + 1, 'k'));
         rawOpen.Bytes("Personal Folders");
         rawOpen.I32(23);
         rawOpen.I32(1);
         PstWireReader rawOpenReader(rawOpen.Data());
         PstOpenReply openBack;
         Expect(ThrowsWireError([&]() { PstWire::GetOpenReply(rawOpenReader, openBack); }), "A record key longer than a store's is refused.");
      }

      // ---- each status thrown in the service as what the reader threw

      {
         PstReaderProcess::Rethrow(StatusOf(PstWire::StatusDone));

         bool refused = false;
         try
         {
            PstReplyStatus status = StatusOf(PstWire::StatusRefused);
            status.refusalReason = PstRefusal::ReasonPasswordWrong;
            PstReaderProcess::Rethrow(status);
         }
         catch (PstRefusal &refusal)
         {
            refused = refusal.GetReason() == PstRefusal::ReasonPasswordWrong && std::string(refusal.what()) == "the sentence";
         }
         Expect(refused, "A refusal is a PstRefusal with its reason and its sentence.");

         bool spent = false;
         try
         {
            PstReplyStatus status = StatusOf(PstWire::StatusBudget);
            status.wholeFile = true;
            PstReaderProcess::Rethrow(status);
         }
         catch (PstBudgetExhausted &exhausted)
         {
            spent = exhausted.WholeFile();
         }
         Expect(spent, "A spent budget is PstBudgetExhausted, and says whether it was the whole file's.");

         bool tooLarge = false;
         try
         {
            PstReaderProcess::Rethrow(StatusOf(PstWire::StatusTooLarge));
         }
         catch (PstBudgetExhausted &)
         {
            tooLarge = false;
         }
         catch (PstTooLarge &)
         {
            tooLarge = true;
         }
         Expect(tooLarge, "Too large is PstTooLarge, and not a spent budget.");

         bool damaged = false;
         try
         {
            PstReaderProcess::Rethrow(StatusOf(PstWire::StatusDamaged));
         }
         catch (PstTooLarge &)
         {
            damaged = false;
         }
         catch (PstError &)
         {
            damaged = true;
         }
         Expect(damaged, "Damage is PstError, and not too large.");

         bool memory = false;
         try
         {
            PstReaderProcess::Rethrow(StatusOf(PstWire::StatusMemory));
         }
         catch (std::bad_alloc &)
         {
            memory = true;
         }
         Expect(memory, "Memory short is std::bad_alloc, which the job takes for the server's and tries again.");

         bool failed = false;
         try
         {
            PstReaderProcess::Rethrow(StatusOf(PstWire::StatusFailed));
         }
         catch (PstError &)
         {
            failed = false;
         }
         catch (std::runtime_error &error)
         {
            failed = std::string(error.what()) == "the sentence";
         }
         Expect(failed, "Any other failure is a std::runtime_error with its sentence, and not damage in the file.");
      }
   }
}
