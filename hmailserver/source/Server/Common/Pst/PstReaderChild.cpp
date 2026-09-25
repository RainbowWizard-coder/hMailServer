// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstReaderChild.h.

#include "StdAfx.h"

#include "PstReaderChild.h"
#include "PstItems.h"
#include "PstMime.h"
#include "PstReaderWire.h"
#include "PstSource.h"
#include "PstStore.h"

#include <cstdlib>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int PstReaderChild::EndedByTestHook;

   namespace
   {
      // What memory running short in this process is: the reader's own
      // ceiling reached (the service gives it 2 GB on Windows, 4 GB of address
      // space on Linux), which the same message or item reaches again however
      // often it is read - too large for the reader, not the server short of
      // memory for now.
      const char *const OutOfItsMemory =
         "Reading it took more memory than the PST reader's process is given, which reading it again would take again; it is larger than the reader takes.";

#ifdef _WIN32
      long long ReadInput(char *buffer, size_t length)
      {
         const DWORD want = length > (size_t) (1 << 20) ? (DWORD) (1 << 20) : (DWORD) length;
         DWORD got = 0;
         if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, want, &got, NULL))
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
         return (long long) got;
      }

      bool WriteOutput(const char *data, size_t length)
      {
         HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
         while (length > 0)
         {
            const DWORD chunk = length > (size_t) (1 << 20) ? (DWORD) (1 << 20) : (DWORD) length;
            DWORD written = 0;
            if (!WriteFile(output, data, chunk, &written, NULL) || written == 0)
               return false;
            data += written;
            length -= (size_t) written;
         }
         return true;
      }

      uint32_t ProcessId()
      {
         return (uint32_t) GetCurrentProcessId();
      }

      // As a crash would: at once, with nothing written and nothing unwound.
      void EndNow()
      {
         TerminateProcess(GetCurrentProcess(), (UINT) PstReaderChild::EndedByTestHook);
      }
#else
      long long ReadInput(char *buffer, size_t length)
      {
         for (;;)
         {
            const ssize_t got = ::read(0, buffer, length);
            if (got < 0 && errno == EINTR)
               continue;
            return (long long) got;
         }
      }

      bool WriteOutput(const char *data, size_t length)
      {
         while (length > 0)
         {
            const ssize_t written = ::write(1, data, length);
            if (written < 0 && errno == EINTR)
               continue;
            if (written <= 0)
               return false;
            data += written;
            length -= (size_t) written;
         }
         return true;
      }

      uint32_t ProcessId()
      {
         return (uint32_t) ::getpid();
      }

      void EndNow()
      {
         ::kill(::getpid(), SIGKILL);
         ::_exit(PstReaderChild::EndedByTestHook);
      }
#endif

      bool WriteFrame(unsigned kind, const std::string &payload, const std::string *tail)
      {
         const std::string header = PstWire::Header(kind, payload.size() + (tail ? tail->size() : 0));
         return WriteOutput(header.data(), header.size()) && WriteOutput(payload.data(), payload.size()) &&
                (!tail || WriteOutput(tail->data(), tail->size()));
      }
   }

   int
   PstReaderChild::Run(const std::string &native)
   {
#ifdef _WIN32
      // No dialog for a fault and no box for a missing drive: a service's child
      // has nobody to click it, and a fault here is to end this process and
      // nothing else.
      SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

      // The file, as the service opened it: the handle or descriptor in decimal.
      char *end = nullptr;
      const unsigned long long value = std::strtoull(native.c_str(), &end, 10);
      const bool numeric = !native.empty() && end != nullptr && *end == '\0';

      std::shared_ptr<PstFileSource> source = std::make_shared<PstFileSource>();
      const bool taken = numeric && source->Adopt((intptr_t) value) == PstFileSource::OpenDone;

      PstWireWriter hello;
      PstWire::PutHello(hello, ProcessId(), taken);
      if (!WriteFrame(PstWire::KindHello, hello.Data(), nullptr) || !taken)
         return 2;

      std::unique_ptr<PstStore> store;
      bool opened = false;
      std::string recordKey;
      std::set<uint32_t> endAt;

      // The test hook (PstReaderChild.h): a request for one of these NIDs ends
      // this process where it stands.
      auto endIfAsked = [&](uint32_t nid)
      {
         if (endAt.find(nid) != endAt.end())
            EndNow();
      };

      for (;;)
      {
         unsigned kind = 0;
         std::string request;
         std::string problem;
         const PstWire::FrameResult frame = PstWire::ReadFrame(ReadInput, kind, request, problem);
         if (frame == PstWire::FrameEnded)
            return 0;
         if (frame != PstWire::FrameRead)
            return 3;

         PstReplyStatus status;
         PstWireWriter body;
         std::string tail;
         bool hasTail = false;

         try
         {
            PstWireReader reader(request);
            if (kind != PstWire::KindOpen && !opened)
               throw std::runtime_error("The PST reader was asked to read before its store was opened.");

            switch (kind)
            {
            case PstWire::KindOpen:
               {
                  PstOpenRequest opening;
                  PstWire::GetOpenRequest(reader, opening);
                  reader.End();

                  endAt.clear();
                  endAt.insert(opening.endAt.begin(), opening.endAt.end());

                  // The limits the service read with, and the work the readers
                  // before this one did on this file in this run.
                  opened = false;
                  store.reset(new PstStore(source));
                  store->Budget().SetFileLimitsFor(source->Size());
                  store->Budget().SetFileUsed(opening.bytesUsed, opening.itemsUsed);
                  store->Open(opening.password);
                  opened = true;
                  recordKey = PstFormat::Hex(store->GetRecordKey());

                  PstOpenReply reply;
                  reply.recordKey = store->GetRecordKey();
                  reply.displayName = store->GetDisplayName();
                  reply.version = (int32_t) store->GetVersion();
                  reply.crypt = (int32_t) store->GetCryptMethod();
                  PstWire::PutOpenReply(body, reply);
                  break;
               }
            case PstWire::KindListFolders:
               {
                  reader.End();
                  std::vector<PstFolderInfo> folders;
                  std::vector<std::string> problems;
                  store->ListFolders(folders, problems);
                  PstWire::PutFolders(body, folders, problems);
                  break;
               }
            case PstWire::KindListMessages:
               {
                  const uint32_t folder = reader.U32();
                  reader.End();
                  endIfAsked(folder);

                  std::vector<uint32_t> messages;
                  store->ListMessages(folder, messages);
                  PstWire::PutNids(body, messages);
                  break;
               }
            case PstWire::KindReadMail:
               {
                  const uint32_t nid = reader.U32();
                  reader.End();
                  endIfAsked(nid);

                  // Read whole, a note or a journal entry taken as a message,
                  // and rebuilt - as the mail phase did in the service, each
                  // step's failure what it was there.
                  PstMessage message;
                  store->ReadMessage(nid, message);
                  bool messageLike = false;
                  if (!PstStore::IsMailMessageClass(message.messageClass))
                     messageLike = store->AsMessage(nid, message);

                  PstMailRead mail;
                  std::string mime;
                  if (!PstStore::IsMailMessageClass(message.messageClass) && !messageLike)
                     mail.outcome = PstMailRead::NotMail;
                  else
                  {
                     mail.warnings = message.warnings;
                     try
                     {
                        mime = PstMime::Build(message, recordKey.substr(0, 8) + "x" + PstFormat::Hex32(nid), mail.warnings);
                        mail.outcome = PstMailRead::Built;
                     }
                     catch (std::bad_alloc &)
                     {
                        throw PstTooLarge(OutOfItsMemory);
                     }
                     catch (std::exception &error)
                     {
                        mail.outcome = PstMailRead::NotBuilt;
                        mail.why = error.what();
                     }
                  }

                  // What the job files the message by; its bodies and parts
                  // are in the rebuilt message and nowhere else.
                  mail.message.messageClass = message.messageClass;
                  mail.message.subject = message.subject;
                  mail.message.messageFlags = message.messageFlags;
                  mail.message.flagStatus = message.flagStatus;
                  mail.message.lastVerb = message.lastVerb;
                  mail.message.submitTime = message.submitTime;
                  mail.message.deliveryTime = message.deliveryTime;
                  mail.message.creationTime = message.creationTime;
                  mail.message.categories = message.categories;

                  PstWire::PutMailHead(body, mail);
                  if (mime.size() > (size_t) PstWire::MaximumFrameBytes)
                     throw PstTooLarge("The message, rebuilt as Internet mail, is larger than the PST reader hands to the service.");
                  body.U32((uint32_t) mime.size());
                  tail.swap(mime);
                  hasTail = true;
                  break;
               }
            case PstWire::KindItemFolders:
               {
                  reader.End();
                  PstItemFoldersReply reply;
                  store->DefaultItemFolders(reply.contacts, reply.calendar, reply.tasks);
                  store->ReadNames();
                  reply.namesProblem = store->NamesProblem();
                  PstWire::PutItemFolders(body, reply);
                  break;
               }
            case PstWire::KindReadContact:
               {
                  const uint32_t nid = reader.U32();
                  reader.End();
                  endIfAsked(nid);

                  PstContact contact;
                  store->ReadContact(nid, contact);

                  PstContactRead contactRead;
                  contactRead.messageClass = contact.messageClass;
                  if (PstItems::IsOfClass(contact.messageClass, "IPM.DistList"))
                     contactRead.outcome = PstContactRead::DistributionList;
                  else if (!PstItems::IsOfClass(contact.messageClass, "IPM.Contact"))
                     contactRead.outcome = PstContactRead::NotAContact;
                  else
                  {
                     for (const PstWarning &warning : contact.warnings)
                        contactRead.notes.push_back(warning.detail);

                     // Its picture left out where the card with it would be
                     // larger than an item the address book takes.
                     const std::string uid = "pst-" + recordKey + "-" + PstFormat::Hex32(nid);
                     contactRead.card = PstItems::ContactText(contact, uid, true);
                     if (contactRead.card.size() > PstItems::MaximumItemTextBytes)
                     {
                        contactRead.card = PstItems::ContactText(contact, uid, false);
                        contactRead.photoLeftOut = true;
                     }
                  }

                  PstWire::PutContact(body, contactRead);
                  break;
               }
            case PstWire::KindReadCalendarItem:
               {
                  const uint32_t nid = reader.U32();
                  const bool taskFolder = reader.U8() != 0;
                  reader.End();
                  endIfAsked(nid);

                  PstCalendarRead itemRead;
                  const std::string fallbackUid = "pst-" + recordKey + "-" + PstFormat::Hex32(nid);
                  if (taskFolder)
                  {
                     PstTask task;
                     store->ReadTask(nid, task);
                     itemRead.messageClass = task.messageClass;
                     if (!PstItems::IsOfClass(task.messageClass, "IPM.Task"))
                        itemRead.outcome = PstCalendarRead::NotAnItem;
                     else
                     {
                        for (const PstWarning &warning : task.warnings)
                           itemRead.notes.push_back(warning.detail);
                        itemRead.text = PstItems::TaskText(task, fallbackUid);
                     }
                  }
                  else
                  {
                     PstAppointment appointment;
                     store->ReadAppointment(nid, appointment);
                     itemRead.messageClass = appointment.messageClass;
                     if (!PstItems::IsOfClass(appointment.messageClass, "IPM.Appointment") &&
                         !PstItems::IsOfClass(appointment.messageClass, "IPM.Schedule.Meeting.Request"))
                        itemRead.outcome = PstCalendarRead::NotAnItem;
                     else
                     {
                        for (const PstWarning &warning : appointment.warnings)
                           itemRead.notes.push_back(warning.detail);
                        if (!PstItems::AppointmentText(appointment, fallbackUid, itemRead.text, itemRead.word, itemRead.why, &store->Budget()))
                           itemRead.outcome = PstCalendarRead::Refused;
                     }
                  }

                  PstWire::PutCalendarItem(body, itemRead);
                  break;
               }
            default:
               // Nothing the service sends: the two are not one program.
               return 3;
            }
         }
         catch (PstRefusal &refusal)
         {
            status.status = PstWire::StatusRefused;
            status.refusalReason = (int) refusal.GetReason();
            status.sentence = refusal.what();
         }
         catch (PstBudgetExhausted &spent)
         {
            status.status = PstWire::StatusBudget;
            status.wholeFile = spent.WholeFile();
            status.sentence = spent.what();
         }
         catch (PstTooLarge &tooLarge)
         {
            status.status = PstWire::StatusTooLarge;
            status.sentence = tooLarge.what();
         }
         catch (PstError &error)
         {
            status.status = PstWire::StatusDamaged;
            status.sentence = error.what();
         }
         catch (std::bad_alloc &)
         {
            status.status = PstWire::StatusTooLarge;
            status.sentence = OutOfItsMemory;
         }
         catch (PstWireError &)
         {
            // A request that is not one: the service and this process are not
            // speaking the same protocol, and nothing it says can be trusted.
            return 3;
         }
         catch (std::exception &error)
         {
            status.status = PstWire::StatusFailed;
            status.sentence = error.what();
         }

         if (status.status != PstWire::StatusDone)
         {
            body.Data().clear();
            tail.clear();
            hasTail = false;
         }

         if (store)
         {
            status.fileExhausted = store->Budget().FileExhausted();
            status.itemsUsed = store->Budget().FileItemsUsed();
            status.bytesUsed = store->Budget().FileBytesUsed();
         }

         PstWireWriter reply;
         PstWire::PutStatus(reply, status);
         reply.Data().append(body.Data());

         if (reply.Data().size() + tail.size() > (size_t) PstWire::MaximumFrameBytes)
         {
            // Only a rebuilt message can come to this: refused as too large,
            // as the service refuses a message past its ceilings.
            status.status = PstWire::StatusTooLarge;
            status.sentence = "The message, rebuilt as Internet mail, is larger than the PST reader hands to the service.";
            tail.clear();
            hasTail = false;
            reply = PstWireWriter();
            PstWire::PutStatus(reply, status);
         }

         if (!WriteFrame(kind | PstWire::KindReply, reply.Data(), hasTail ? &tail : nullptr))
            return 4;
      }
   }
}
