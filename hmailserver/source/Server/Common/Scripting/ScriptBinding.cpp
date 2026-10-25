// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

// The JavaScript event-script engine and the binding of the server's objects into
// it. ScriptBinding.h says what a run is and what it is allowed; this file is the
// how. POSIX only: see the header for why.
//
// THE BINDING. Each object a handler is given is a JavaScript object of a class
// of its own (Message, Client, Result, EventLog, FetchAccount, Account, and the
// Recipients collection with its Recipient items), whose prototype carries one
// accessor per COM property and one function per COM method, calling exactly what
// the COM class calls: InterfaceMessage goes through MessageData, InterfaceEventLog
// is Logger::LogEvent, and so on - the COM file each dispatcher mirrors is named
// above it. The object's opaque pointer is a small holder owning a shared_ptr to
// the C++ object, deleted by the class finalizer when the runtime goes, which is
// at the end of the event: nothing a script keeps can outlive what it points at.
//
// Two COM shapes have no JavaScript spelling and are given one:
//   message.HeaderValue('X') = 'v'   is message.SetHeaderValue('X', 'v')
//   message.Flag(n) = true           is message.SetFlag(n, true)
// and their reading forms, message.HeaderValue('X') and message.Flag(n), are
// functions. Everything a COM interface has that this binding does not is listed by
// ScriptBinding::NotOnJavaScript with the reason in EventScripts.md.
//
// NO C++ EXCEPTION CROSSES THE ENGINE. quickjs.c is C, built without unwind tables,
// so an exception thrown through its frames ends the process. Every function the
// engine calls runs its body inside Guard_, which turns a C++ exception into a
// JavaScript one; a boost::thread_interrupted - the server stopping - is recorded,
// stops the script at the next interrupt poll, and is thrown again from C++ once
// the engine has returned.

#include "StdAfx.h"

#ifdef HM_PLATFORM_POSIX

#include "ScriptBinding.h"
#include "ScriptObjectContainer.h"
#include "ScriptServer.h"
#include "Result.h"
#include "ClientInfo.h"

#include "../BO/Account.h"
#include "../BO/FetchAccount.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentFetchAccount.h"
#include "../Persistence/PersistentMessage.h"
#include "../Tracking/ChangeNotification.h"
#include "../Tracking/NotificationServer.h"
#include "../Util/MessageUtilities.h"
#include "../Util/Time.h"
#include "../../IMAP/MessagesContainer.h"
#include "../../SMTP/RecipientParser.h"

#include "../../quickjs/quickjs.h"

#include <malloc.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace HM
{
   namespace
   {
      // ------------------------------------------------------------ text
      //
      // A String is UTF-32 here (wchar_t is four bytes) and the engine speaks
      // UTF-8. Written out rather than taken from Unicode::WideToMultiByte because
      // a JavaScript string may hold a lone surrogate, which iconv refuses and the
      // engine encodes as the three bytes WTF-8 gives it; both directions below
      // carry one through as the code unit it is, and turn anything that is not
      // UTF-8 at all into U+FFFD rather than into an empty string.

      std::string ToUtf8_(const String &text)
      {
         std::string out;
         out.reserve(text.size());

         // Indexed through the buffer: String's own operator[] takes an int or an
         // unsigned int, and a size_t matches both equally well.
         const wchar_t *characters = text.c_str();
         const size_t count = text.size();

         for (size_t i = 0; i < count; i++)
         {
            uint32_t c = static_cast<uint32_t>(characters[i]);

            if (c > 0x10FFFF)
               c = 0xFFFD;

            if (c < 0x80)
            {
               out += static_cast<char>(c);
            }
            else if (c < 0x800)
            {
               out += static_cast<char>(0xC0 | (c >> 6));
               out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
               out += static_cast<char>(0xE0 | (c >> 12));
               out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
               out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
               out += static_cast<char>(0xF0 | (c >> 18));
               out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
               out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
               out += static_cast<char>(0x80 | (c & 0x3F));
            }
         }

         return out;
      }

      String FromUtf8_(const char *bytes, size_t length)
      {
         std::wstring out;
         out.reserve(length);

         size_t i = 0;
         while (i < length)
         {
            unsigned char lead = static_cast<unsigned char>(bytes[i]);
            uint32_t c = 0;
            size_t following = 0;
            uint32_t smallest = 0;

            if (lead < 0x80)
            {
               out += static_cast<wchar_t>(lead);
               i++;
               continue;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
               c = lead & 0x1F;
               following = 1;
               smallest = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
               c = lead & 0x0F;
               following = 2;
               smallest = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
               c = lead & 0x07;
               following = 3;
               smallest = 0x10000;
            }
            else
            {
               out += static_cast<wchar_t>(0xFFFD);
               i++;
               continue;
            }

            bool valid = i + following < length;
            for (size_t k = 1; valid && k <= following; k++)
            {
               unsigned char next = static_cast<unsigned char>(bytes[i + k]);
               if ((next & 0xC0) != 0x80)
                  valid = false;
               else
                  c = (c << 6) | (next & 0x3F);
            }

            if (!valid || c < smallest || c > 0x10FFFF)
            {
               out += static_cast<wchar_t>(0xFFFD);
               i++;
               continue;
            }

            out += static_cast<wchar_t>(c);
            i += following + 1;
         }

         return String(out);
      }

      JSValue NewString_(JSContext *ctx, const String &text)
      {
         std::string utf8 = ToUtf8_(text);
         return JS_NewStringLen(ctx, utf8.data(), utf8.size());
      }

      // The value as text, as JavaScript's String() would give it. False, with the
      // exception pending, when it cannot be made one (a Symbol, a throwing
      // toString).
      bool ReadString_(JSContext *ctx, JSValueConst value, String &out)
      {
         size_t length = 0;
         const char *bytes = JS_ToCStringLen(ctx, &length, value);
         if (!bytes)
            return false;

         out = FromUtf8_(bytes, length);
         JS_FreeCString(ctx, bytes);
         return true;
      }

      // Drops whatever exception is pending: used only where a failure to read a
      // diagnostic is itself not worth reporting.
      void ClearException_(JSContext *ctx)
      {
         JSValue pending = JS_GetException(ctx);
         JS_FreeValue(ctx, pending);
      }

      // ------------------------------------------------------------ one run

      // What the allocator and the interrupt handler share with the code that
      // started the run. It outlives the runtime: Engine declares it first.
      class RunState
      {
      public:
         explicit RunState(int timeoutSeconds) :
            has_deadline(timeoutSeconds > 0),
            interrupted(false),
            refused_at_ceiling(false),
            thread_interruption(false)
         {
            if (has_deadline)
               deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
         }

         std::chrono::steady_clock::time_point deadline;
         bool has_deadline;

         // The deadline passed and the interrupt handler stopped the script.
         bool interrupted;

         // The allocator refused a block because the run was at the ceiling.
         bool refused_at_ceiling;

         // A callback caught boost::thread_interrupted: the server is stopping.
         bool thread_interruption;
      };

      // The allocator, which is quickjs.c's own default (js_def_malloc and its two
      // companions) with one addition: when a block is refused because the run is
      // at JS_SetMemoryLimit's ceiling, it says so, so that a script stopped by the
      // ceiling can be told apart from one that simply threw. The accounting is the
      // default's exactly - the usable size of each block plus the same eight bytes
      // of overhead quickjs.c counts on Linux (MALLOC_OVERHEAD) - because the
      // engine's own memory reporting reads the same counters.
      const size_t AllocationOverhead = 8;

      size_t UsableSize_(const void *pointer)
      {
         return ::malloc_usable_size(const_cast<void *>(pointer));
      }

      void *Allocate_(JSMallocState *state, size_t size)
      {
         if (state->malloc_size + size > state->malloc_limit)
         {
            static_cast<RunState *>(state->opaque)->refused_at_ceiling = true;
            return nullptr;
         }

         void *pointer = ::malloc(size == 0 ? 1 : size);
         if (!pointer)
            return nullptr;

         state->malloc_count++;
         state->malloc_size += UsableSize_(pointer) + AllocationOverhead;
         return pointer;
      }

      void Free_(JSMallocState *state, void *pointer)
      {
         if (!pointer)
            return;

         state->malloc_count--;
         state->malloc_size -= UsableSize_(pointer) + AllocationOverhead;
         ::free(pointer);
      }

      void *Reallocate_(JSMallocState *state, void *pointer, size_t size)
      {
         if (!pointer)
         {
            if (size == 0)
               return nullptr;

            return Allocate_(state, size);
         }

         size_t oldSize = UsableSize_(pointer);

         if (size == 0)
         {
            state->malloc_count--;
            state->malloc_size -= oldSize + AllocationOverhead;
            ::free(pointer);
            return nullptr;
         }

         if (state->malloc_size + size - oldSize > state->malloc_limit)
         {
            static_cast<RunState *>(state->opaque)->refused_at_ceiling = true;
            return nullptr;
         }

         void *moved = ::realloc(pointer, size);
         if (!moved)
            return nullptr;

         state->malloc_size += UsableSize_(moved) - oldSize;
         return moved;
      }

      const JSMallocFunctions Allocator =
      {
         Allocate_,
         Free_,
         Reallocate_,
         UsableSize_
      };

      // Polled by the engine every ten thousand interpreter steps, and by the
      // regular expression matcher as it backtracks. Returning 1 makes the engine
      // throw an "interrupted" error that no catch in the script can hold.
      int Interrupt_(JSRuntime * /*runtime*/, void *opaque)
      {
         RunState *state = static_cast<RunState *>(opaque);

         if (state->thread_interruption)
            return 1;

         if (!state->has_deadline)
            return 0;

         if (std::chrono::steady_clock::now() < state->deadline)
            return 0;

         state->interrupted = true;
         return 1;
      }

      // No C++ exception may leave a function the engine called; see the top of
      // this file.
      template <typename Body>
      JSValue Guard_(JSContext *ctx, Body body)
      {
         try
         {
            return body();
         }
         catch (boost::thread_interrupted &)
         {
            RunState *state = static_cast<RunState *>(JS_GetContextOpaque(ctx));
            if (state)
               state->thread_interruption = true;

            return JS_ThrowInternalError(ctx, "%s", "the server is stopping");
         }
         catch (std::exception &error)
         {
            return JS_ThrowInternalError(ctx, "%s", error.what());
         }
         catch (...)
         {
            return JS_ThrowInternalError(ctx, "%s", "the server could not complete the call");
         }
      }

      // ------------------------------------------------------------ arguments

      bool Missing_(JSContext *ctx)
      {
         JS_ThrowTypeError(ctx, "%s", "an argument this function needs is missing");
         return false;
      }

      bool TextArgument_(JSContext *ctx, int argc, JSValueConst *argv, int index, String &value)
      {
         if (index >= argc)
            return Missing_(ctx);

         return ReadString_(ctx, argv[index], value);
      }

      // A number a member takes - Result.Value and Result.Parameter, a flag, a
      // folder's id, a recipient's index - is a Number with no fraction inside 32
      // bits, and nothing else is taken for one. JS_ToInt32 alone made anything a
      // number: NaN, 'deny', null, undefined and an object became 0, and true 1.
      // And 0 is what OnClientValidatePassword reads as "let the client in without
      // checking the password", so a handler bug - Result.Value = parseInt(x) for
      // an x that is not a number, or a word - opened the account on Linux where on
      // Windows COM refuses the assignment as a type mismatch, the handler stops
      // with a Script Error and Result keeps its default. The same here, and for
      // everything that is not already a whole number: a TypeError, the handler
      // stops there, and the event goes on with the Result it had. That is
      // stricter than COM, which turns undefined into 0 and a fraction into the
      // nearest whole number, and deliberately so; a boolean is refused too -
      // JScript's true is -1 to COM, which no event reads as anything, where 1
      // would delete or refuse a message.
      bool NumberArgument_(JSContext *ctx, int argc, JSValueConst *argv, int index, int32_t &value)
      {
         if (index >= argc)
            return Missing_(ctx);

         double number = 0;

         if (!JS_IsNumber(argv[index]) || JS_ToFloat64(ctx, &number, argv[index]) != 0 ||
             !std::isfinite(number) || number != std::floor(number) ||
             number < -2147483648.0 || number > 2147483647.0)
         {
            JS_ThrowTypeError(ctx, "%s",
               "a whole number is needed here - a Number with no fraction, within 32 bits - and a string, a boolean, "
               "null, undefined, NaN or an object is not one");
            return false;
         }

         value = static_cast<int32_t>(number);
         return true;
      }

      bool FlagArgument_(JSContext *ctx, int argc, JSValueConst *argv, int index, bool &value)
      {
         if (index >= argc)
            return Missing_(ctx);

         int flag = JS_ToBool(ctx, argv[index]);
         if (flag < 0)
            return false;

         value = flag != 0;
         return true;
      }

      // ------------------------------------------------------------ the classes

      JSClassID MessageClass = 0;
      JSClassID ClientClass = 0;
      JSClassID ResultClass = 0;
      JSClassID EventLogClass = 0;
      JSClassID FetchAccountClass = 0;
      JSClassID AccountClass = 0;
      JSClassID RecipientsClass = 0;
      JSClassID RecipientClass = 0;

      std::once_flag ClassIdsAllocated;

      // A class id is process-wide in QuickJS and a class is registered per
      // runtime, so the ids are taken once and every run registers the classes
      // under them.
      void AllocateClassIds_()
      {
         std::call_once(ClassIdsAllocated, []()
         {
            JS_NewClassID(&MessageClass);
            JS_NewClassID(&ClientClass);
            JS_NewClassID(&ResultClass);
            JS_NewClassID(&EventLogClass);
            JS_NewClassID(&FetchAccountClass);
            JS_NewClassID(&AccountClass);
            JS_NewClassID(&RecipientsClass);
            JS_NewClassID(&RecipientClass);
         });
      }

      class MessageHolder
      {
      public:
         explicit MessageHolder(std::shared_ptr<Message> value) :
            message(value)
         {
         }

         // InterfaceMessage::GetMessageData_: the MIME content, loaded on first use -
         // from the account's folder when the message is in one - and kept until
         // RefreshContent drops it.
         std::shared_ptr<MessageData> Data()
         {
            if (!data)
            {
               std::shared_ptr<const Account> account;

               if (message->GetAccountID() > 0)
                  account = CacheContainer::Instance()->GetAccount(message->GetAccountID());

               data = std::shared_ptr<MessageData>(new MessageData());
               data->LoadFromMessage(account, message);
            }

            return data;
         }

         std::shared_ptr<Message> message;
         std::shared_ptr<MessageData> data;
      };

      class ClientHolder
      {
      public:
         explicit ClientHolder(std::shared_ptr<ClientInfo> value) : client(value) {}
         std::shared_ptr<ClientInfo> client;
      };

      class ResultHolder
      {
      public:
         explicit ResultHolder(std::shared_ptr<Result> value) : result(value) {}
         std::shared_ptr<Result> result;
      };

      class FetchAccountHolder
      {
      public:
         explicit FetchAccountHolder(std::shared_ptr<FetchAccount> value) : fetch_account(value) {}
         std::shared_ptr<FetchAccount> fetch_account;
      };

      class AccountHolder
      {
      public:
         explicit AccountHolder(std::shared_ptr<Account> value) : account(value) {}
         std::shared_ptr<Account> account;
      };

      class RecipientsHolder
      {
      public:
         explicit RecipientsHolder(std::shared_ptr<Message> value) : message(value) {}
         std::shared_ptr<Message> message;
      };

      class RecipientHolder
      {
      public:
         explicit RecipientHolder(std::shared_ptr<MessageRecipient> value) : recipient(value) {}
         std::shared_ptr<MessageRecipient> recipient;
      };

      void FinalizeMessage_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<MessageHolder *>(JS_GetOpaque(value, MessageClass)); }
      void FinalizeClient_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<ClientHolder *>(JS_GetOpaque(value, ClientClass)); }
      void FinalizeResult_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<ResultHolder *>(JS_GetOpaque(value, ResultClass)); }
      void FinalizeFetchAccount_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<FetchAccountHolder *>(JS_GetOpaque(value, FetchAccountClass)); }
      void FinalizeAccount_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<AccountHolder *>(JS_GetOpaque(value, AccountClass)); }
      void FinalizeRecipients_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<RecipientsHolder *>(JS_GetOpaque(value, RecipientsClass)); }
      void FinalizeRecipient_(JSRuntime * /*runtime*/, JSValue value) { delete static_cast<RecipientHolder *>(JS_GetOpaque(value, RecipientClass)); }

      // An object of the class with the holder as its opaque pointer. The holder
      // belongs to the object from here on - its finalizer deletes it - and is
      // deleted here if the object cannot be made.
      template <class Holder>
      JSValue Wrap_(JSContext *ctx, JSClassID classId, Holder *holder)
      {
         JSValue object = JS_NewObjectClass(ctx, static_cast<int>(classId));
         if (JS_IsException(object))
         {
            delete holder;
            return object;
         }

         JS_SetOpaque(object, holder);
         return object;
      }

      // ------------------------------------------------------------ member tables

      enum MemberKind
      {
         KindRead = 0,
         KindReadWrite = 1,
         KindMethod = 2
      };

      class MemberSpec
      {
      public:
         const char *name;
         MemberKind kind;
         int arguments;   // a method's declared parameter count (its length)
         int id;          // the dispatcher's case, handed to it as the function's magic
      };

      const char *KindName_(MemberKind kind)
      {
         switch (kind)
         {
         case KindRead: return "read";
         case KindReadWrite: return "read/write";
         default: return "method";
         }
      }

      // ---- Message: COM/InterfaceMessage.cpp

      enum class MessageMember : int
      {
         ID, Filename, Subject, From, Date, Body, HTMLBody, Save, To, AddRecipient, FromAddress, State, Size,
         ClearRecipients, CC, Recipients, HeaderValue, SetHeaderValue, HasBodyType, EncodeFields, Flag, SetFlag,
         InternalDate, RefreshContent, DeliveryAttempt, Charset, Copy, UID
      };

      const MemberSpec MessageMembers[] =
      {
         { "ID", KindRead, 0, static_cast<int>(MessageMember::ID) },
         { "Filename", KindRead, 0, static_cast<int>(MessageMember::Filename) },
         { "Subject", KindReadWrite, 0, static_cast<int>(MessageMember::Subject) },
         { "From", KindReadWrite, 0, static_cast<int>(MessageMember::From) },
         { "Date", KindReadWrite, 0, static_cast<int>(MessageMember::Date) },
         { "Body", KindReadWrite, 0, static_cast<int>(MessageMember::Body) },
         { "HTMLBody", KindReadWrite, 0, static_cast<int>(MessageMember::HTMLBody) },
         { "Save", KindMethod, 0, static_cast<int>(MessageMember::Save) },
         { "To", KindRead, 0, static_cast<int>(MessageMember::To) },
         { "AddRecipient", KindMethod, 2, static_cast<int>(MessageMember::AddRecipient) },
         { "FromAddress", KindReadWrite, 0, static_cast<int>(MessageMember::FromAddress) },
         { "State", KindRead, 0, static_cast<int>(MessageMember::State) },
         { "Size", KindRead, 0, static_cast<int>(MessageMember::Size) },
         { "ClearRecipients", KindMethod, 0, static_cast<int>(MessageMember::ClearRecipients) },
         { "CC", KindRead, 0, static_cast<int>(MessageMember::CC) },
         { "Recipients", KindRead, 0, static_cast<int>(MessageMember::Recipients) },
         { "HeaderValue", KindMethod, 1, static_cast<int>(MessageMember::HeaderValue) },
         { "SetHeaderValue", KindMethod, 2, static_cast<int>(MessageMember::SetHeaderValue) },
         { "HasBodyType", KindMethod, 1, static_cast<int>(MessageMember::HasBodyType) },
         { "EncodeFields", KindReadWrite, 0, static_cast<int>(MessageMember::EncodeFields) },
         { "Flag", KindMethod, 1, static_cast<int>(MessageMember::Flag) },
         { "SetFlag", KindMethod, 2, static_cast<int>(MessageMember::SetFlag) },
         { "InternalDate", KindRead, 0, static_cast<int>(MessageMember::InternalDate) },
         { "RefreshContent", KindMethod, 0, static_cast<int>(MessageMember::RefreshContent) },
         { "DeliveryAttempt", KindRead, 0, static_cast<int>(MessageMember::DeliveryAttempt) },
         { "Charset", KindReadWrite, 0, static_cast<int>(MessageMember::Charset) },
         { "Copy", KindMethod, 1, static_cast<int>(MessageMember::Copy) },
         { "UID", KindRead, 0, static_cast<int>(MessageMember::UID) },
      };

      JSValue NewRecipients_(JSContext *ctx, std::shared_ptr<Message> message)
      {
         return Wrap_(ctx, RecipientsClass, new RecipientsHolder(message));
      }

      // The account whose folder the message is in, or none: what InterfaceMessage
      // hands PersistentMessage::GetFileName in three places.
      std::shared_ptr<const Account> AccountOf_(std::shared_ptr<Message> message)
      {
         std::shared_ptr<const Account> account;

         if (message->GetAccountID() > 0)
            account = CacheContainer::Instance()->GetAccount(message->GetAccountID());

         return account;
      }

      JSValue MessageGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         MessageHolder *holder = static_cast<MessageHolder *>(JS_GetOpaque2(ctx, self, MessageClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            std::shared_ptr<Message> message = holder->message;

            switch (static_cast<MessageMember>(magic))
            {
            case MessageMember::ID:
               return JS_NewInt64(ctx, message->GetID());
            case MessageMember::Filename:
               return NewString_(ctx, PersistentMessage::GetFileName(AccountOf_(message), message));
            case MessageMember::Subject:
               return NewString_(ctx, holder->Data()->GetSubject());
            case MessageMember::From:
               return NewString_(ctx, holder->Data()->GetFrom());
            case MessageMember::Date:
               return NewString_(ctx, holder->Data()->GetSentTime());
            case MessageMember::Body:
               return NewString_(ctx, holder->Data()->GetBody());
            case MessageMember::HTMLBody:
               return NewString_(ctx, holder->Data()->GetHTMLBody());
            case MessageMember::To:
               return NewString_(ctx, holder->Data()->GetTo());
            case MessageMember::FromAddress:
               return NewString_(ctx, message->GetFromAddress());
            case MessageMember::State:
               return JS_NewInt32(ctx, static_cast<int32_t>(message->GetState()));
            case MessageMember::Size:
               return JS_NewInt32(ctx, message->GetSize() / 1024);
            case MessageMember::CC:
               return NewString_(ctx, holder->Data()->GetCC());
            case MessageMember::Recipients:
               return NewRecipients_(ctx, message);
            case MessageMember::EncodeFields:
               return JS_NewBool(ctx, holder->Data()->GetEncodeFields());
            case MessageMember::InternalDate:
               // COM answers a VARIANT date; here it is the server's own form of
               // the same moment, yyyy-mm-dd hh:mm:ss, which new Date() reads.
               return NewString_(ctx, message->GetCreateTime());
            case MessageMember::DeliveryAttempt:
               return JS_NewInt32(ctx, static_cast<int32_t>(message->GetNoOfRetries()) + 1);
            case MessageMember::Charset:
               return NewString_(ctx, holder->Data()->GetCharset());
            case MessageMember::UID:
               return JS_NewInt64(ctx, static_cast<int64_t>(message->GetUID()));
            default:
               return JS_ThrowTypeError(ctx, "%s", "Message has no such property");
            }
         });
      }

      JSValue MessageSet_(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
      {
         MessageHolder *holder = static_cast<MessageHolder *>(JS_GetOpaque2(ctx, self, MessageClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            MessageMember member = static_cast<MessageMember>(magic);

            if (member == MessageMember::EncodeFields)
            {
               bool on = false;
               if (!FlagArgument_(ctx, argc, argv, 0, on))
                  return JS_EXCEPTION;

               holder->Data()->SetEncodeFields(on);
               return JS_UNDEFINED;
            }

            String value;
            if (!TextArgument_(ctx, argc, argv, 0, value))
               return JS_EXCEPTION;

            switch (member)
            {
            case MessageMember::Subject:
               holder->Data()->SetSubject(value);
               break;
            case MessageMember::From:
               holder->Data()->SetFrom(value);
               break;
            case MessageMember::Date:
               holder->Data()->SetSentTime(value);
               break;
            case MessageMember::Body:
               holder->Data()->SetBody(value);
               break;
            case MessageMember::HTMLBody:
               holder->Data()->SetHTMLBody(value);
               break;
            case MessageMember::FromAddress:
               holder->message->SetFromAddress(value);
               break;
            case MessageMember::Charset:
               holder->Data()->SetCharset(value);
               break;
            default:
               return JS_ThrowTypeError(ctx, "%s", "the property is read-only");
            }

            return JS_UNDEFINED;
         });
      }

      // InterfaceMessage::Save and SaveNewMessageToIMAPFolder_, case for case. Where
      // COM answers S_FALSE - a save the database refused, reported by the
      // persistence layer itself - the script is not told either.
      JSValue MessageSave_(JSContext *ctx, MessageHolder *holder)
      {
         std::shared_ptr<Message> message = holder->message;

         String sentTime = holder->Data()->GetSentTime();
         if (sentTime.IsEmpty())
         {
            sentTime = Time::GetCurrentMimeDate();
            holder->Data()->SetSentTime(sentTime);
         }

         std::shared_ptr<const Account> account = AccountOf_(message);
         const String fileName = PersistentMessage::GetFileName(account, message);

         // As COM's Save writes it: a new message for a folder of a mailbox
         // encrypted at rest is encrypted in memory, never written in the clear.
         const bool intoMailbox = message->GetState() == Message::Created && message->GetFolderID() != 0 && account;
         const bool written = intoMailbox ? holder->Data()->WriteToMailbox(fileName, account) : holder->Data()->Write(fileName);
         if (!written)
            return JS_ThrowInternalError(ctx, "%s", "Unable to write to message file.");

         switch (message->GetState())
         {
         case Message::Created:
            if (message->GetFolderID() == 0 && message->GetAccountID() == 0)
            {
               message->SetState(Message::Delivering);

               if (!PersistentMessage::SaveObject(message))
                  return JS_ThrowInternalError(ctx, "%s", "Message could not be saved in database.");

               Application::Instance()->SubmitPendingEmail();
            }
            else
            {
               message->SetState(Message::Delivered);

               if (!PersistentMessage::SaveObject(message))
                  return JS_UNDEFINED;

               MessagesContainer::Instance()->SetFolderNeedsRefresh(message->GetFolderID());

               std::shared_ptr<ChangeNotification> notification =
                  std::shared_ptr<ChangeNotification>(new ChangeNotification(message->GetAccountID(), message->GetFolderID(), ChangeNotification::NotificationMessageAdded));

               Application::Instance()->GetNotificationServer()->SendNotification(notification);
            }
            break;
         case Message::Delivering:
            // The delivery that is running saves the row; the file is what changed.
            break;
         case Message::Delivered:
            PersistentMessage::SaveObject(message);
            break;
         default:
            return JS_ThrowInternalError(ctx, "%s", "The message could not be saved. It is in an unknown state.");
         }

         return JS_UNDEFINED;
      }

      JSValue MessageCall_(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
      {
         MessageHolder *holder = static_cast<MessageHolder *>(JS_GetOpaque2(ctx, self, MessageClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            std::shared_ptr<Message> message = holder->message;

            switch (static_cast<MessageMember>(magic))
            {
            case MessageMember::Save:
               return MessageSave_(ctx, holder);
            case MessageMember::AddRecipient:
               {
                  String name;
                  String address;
                  if (!TextArgument_(ctx, argc, argv, 0, name) || !TextArgument_(ctx, argc, argv, 1, address))
                     return JS_EXCEPTION;

                  bool recipientOK = false;
                  RecipientParser recipientParser;
                  recipientParser.CreateMessageRecipientList(address, message->GetRecipients(), recipientOK);

                  String thisAddress = "\"" + name + "\"" + " <" + address + ">";
                  String to = holder->Data()->GetTo();

                  if (!to.IsEmpty())
                     to += ",";

                  to += thisAddress;
                  holder->Data()->SetTo(to);
                  return JS_UNDEFINED;
               }
            case MessageMember::ClearRecipients:
               message->GetRecipients()->Clear();
               holder->Data()->SetTo("");
               holder->Data()->SetCC("");
               holder->Data()->SetBCC("");
               return JS_UNDEFINED;
            case MessageMember::HeaderValue:
               {
                  String field;
                  if (!TextArgument_(ctx, argc, argv, 0, field))
                     return JS_EXCEPTION;

                  return NewString_(ctx, holder->Data()->GetFieldValue(field));
               }
            case MessageMember::SetHeaderValue:
               {
                  String field;
                  String value;
                  if (!TextArgument_(ctx, argc, argv, 0, field) || !TextArgument_(ctx, argc, argv, 1, value))
                     return JS_EXCEPTION;

                  holder->Data()->SetFieldValue(field, value);
                  return JS_UNDEFINED;
               }
            case MessageMember::HasBodyType:
               {
                  String bodyType;
                  if (!TextArgument_(ctx, argc, argv, 0, bodyType))
                     return JS_EXCEPTION;

                  return JS_NewBool(ctx, holder->Data()->GetHasBodyType(bodyType));
               }
            case MessageMember::Flag:
               {
                  int32_t flag = 0;
                  if (!NumberArgument_(ctx, argc, argv, 0, flag))
                     return JS_EXCEPTION;

                  int flags = message->GetFlags();
                  return JS_NewBool(ctx, (flags & flag) > 0);
               }
            case MessageMember::SetFlag:
               {
                  int32_t flag = 0;
                  bool on = false;
                  if (!NumberArgument_(ctx, argc, argv, 0, flag) || !FlagArgument_(ctx, argc, argv, 1, on))
                     return JS_EXCEPTION;

                  int flags = message->GetFlags();

                  if (on)
                     flags = flags | flag;
                  else
                     flags = flags & ~flag;

                  message->SetFlags(static_cast<short>(flags));
                  return JS_UNDEFINED;
               }
            case MessageMember::RefreshContent:
               holder->data.reset();
               return JS_UNDEFINED;
            case MessageMember::Copy:
               {
                  int32_t folderId = 0;
                  if (!NumberArgument_(ctx, argc, argv, 0, folderId))
                     return JS_EXCEPTION;

                  // The source message must be in an account; COM answers S_FALSE.
                  if (message->GetAccountID() == 0)
                     return JS_UNDEFINED;

                  if (!MessageUtilities::CopyToIMAPFolder(message, folderId))
                     return JS_ThrowInternalError(ctx, "%s", "Unable to copy message.");

                  return JS_UNDEFINED;
               }
            default:
               return JS_ThrowTypeError(ctx, "%s", "Message has no such method");
            }
         });
      }

      // ---- Message.Recipients: COM/InterfaceRecipients.cpp and InterfaceRecipient.cpp

      enum class RecipientsMember : int { Count, Item };

      const MemberSpec RecipientsMembers[] =
      {
         { "Count", KindRead, 0, static_cast<int>(RecipientsMember::Count) },
         { "Item", KindMethod, 1, static_cast<int>(RecipientsMember::Item) },
      };

      JSValue RecipientsGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         RecipientsHolder *holder = static_cast<RecipientsHolder *>(JS_GetOpaque2(ctx, self, RecipientsClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            if (static_cast<RecipientsMember>(magic) == RecipientsMember::Count)
               return JS_NewInt32(ctx, holder->message->GetRecipients()->GetCount());

            return JS_ThrowTypeError(ctx, "%s", "Recipients has no such property");
         });
      }

      JSValue RecipientsCall_(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
      {
         RecipientsHolder *holder = static_cast<RecipientsHolder *>(JS_GetOpaque2(ctx, self, RecipientsClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            if (static_cast<RecipientsMember>(magic) != RecipientsMember::Item)
               return JS_ThrowTypeError(ctx, "%s", "Recipients has no such method");

            int32_t index = 0;
            if (!NumberArgument_(ctx, argc, argv, 0, index))
               return JS_EXCEPTION;

            // A copy, as InterfaceRecipients takes one: the item it hands out stays
            // the recipient it was, whatever the list does afterwards.
            // owned by this call: the recipients of the message this event is about, reached only on the thread that fired the event, for as long as the handler runs
            std::vector<std::shared_ptr<MessageRecipient> > recipients = holder->message->GetRecipients()->GetVector();

            // COM refuses an index at or past the end with DISP_E_BADINDEX, and does
            // not check a negative one at all; both are refused here.
            if (index < 0 || static_cast<size_t>(index) >= recipients.size())
               return JS_ThrowRangeError(ctx, "%s", "Recipients.Item: the index is outside the collection");

            return Wrap_(ctx, RecipientClass, new RecipientHolder(recipients[static_cast<size_t>(index)]));
         });
      }

      enum class RecipientMember : int { Address, IsLocalUser, OriginalAddress };

      const MemberSpec RecipientMembers[] =
      {
         { "Address", KindRead, 0, static_cast<int>(RecipientMember::Address) },
         { "IsLocalUser", KindRead, 0, static_cast<int>(RecipientMember::IsLocalUser) },
         { "OriginalAddress", KindRead, 0, static_cast<int>(RecipientMember::OriginalAddress) },
      };

      JSValue RecipientGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         RecipientHolder *holder = static_cast<RecipientHolder *>(JS_GetOpaque2(ctx, self, RecipientClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            switch (static_cast<RecipientMember>(magic))
            {
            case RecipientMember::Address:
               return NewString_(ctx, holder->recipient->GetAddress());
            case RecipientMember::IsLocalUser:
               return JS_NewBool(ctx, holder->recipient->GetLocalAccountID() > 0);
            case RecipientMember::OriginalAddress:
               return NewString_(ctx, holder->recipient->GetOriginalAddress());
            default:
               return JS_ThrowTypeError(ctx, "%s", "Recipient has no such property");
            }
         });
      }

      // ---- Client: COM/InterfaceClient.cpp

      enum class ClientMember : int
      {
         Port, IPAddress, Username, HELO, Authenticated, EncryptedConnection, CipherVersion, CipherName, CipherBits, SessionID
      };

      const MemberSpec ClientMembers[] =
      {
         { "Port", KindRead, 0, static_cast<int>(ClientMember::Port) },
         { "IPAddress", KindRead, 0, static_cast<int>(ClientMember::IPAddress) },
         { "Username", KindRead, 0, static_cast<int>(ClientMember::Username) },
         { "HELO", KindRead, 0, static_cast<int>(ClientMember::HELO) },
         { "Authenticated", KindRead, 0, static_cast<int>(ClientMember::Authenticated) },
         { "EncryptedConnection", KindRead, 0, static_cast<int>(ClientMember::EncryptedConnection) },
         { "CipherVersion", KindRead, 0, static_cast<int>(ClientMember::CipherVersion) },
         { "CipherName", KindRead, 0, static_cast<int>(ClientMember::CipherName) },
         { "CipherBits", KindRead, 0, static_cast<int>(ClientMember::CipherBits) },
         { "SessionID", KindRead, 0, static_cast<int>(ClientMember::SessionID) },
      };

      JSValue ClientGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         ClientHolder *holder = static_cast<ClientHolder *>(JS_GetOpaque2(ctx, self, ClientClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            std::shared_ptr<ClientInfo> client = holder->client;

            switch (static_cast<ClientMember>(magic))
            {
            case ClientMember::Port:
               return JS_NewInt64(ctx, client->GetPort());
            case ClientMember::IPAddress:
               return NewString_(ctx, client->GetIPAddress());
            case ClientMember::Username:
               return NewString_(ctx, client->GetUsername());
            case ClientMember::HELO:
               return NewString_(ctx, client->GetHELO());
            case ClientMember::Authenticated:
               return JS_NewBool(ctx, client->GetIsAuthenticated());
            case ClientMember::EncryptedConnection:
               return JS_NewBool(ctx, client->GetIsEncryptedConnection());
            case ClientMember::CipherVersion:
               return NewString_(ctx, client->GetCipherVersion());
            case ClientMember::CipherName:
               return NewString_(ctx, client->GetCipherName());
            case ClientMember::CipherBits:
               return JS_NewInt64(ctx, client->GetCipherBits());
            case ClientMember::SessionID:
               return JS_NewInt64(ctx, client->GetSessionID());
            default:
               return JS_ThrowTypeError(ctx, "%s", "Client has no such property");
            }
         });
      }

      // ---- Result: COM/InterfaceResult.cpp

      enum class ResultMember : int { Value, Message, Parameter };

      const MemberSpec ResultMembers[] =
      {
         { "Value", KindReadWrite, 0, static_cast<int>(ResultMember::Value) },
         { "Message", KindReadWrite, 0, static_cast<int>(ResultMember::Message) },
         { "Parameter", KindReadWrite, 0, static_cast<int>(ResultMember::Parameter) },
      };

      JSValue ResultGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         ResultHolder *holder = static_cast<ResultHolder *>(JS_GetOpaque2(ctx, self, ResultClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            switch (static_cast<ResultMember>(magic))
            {
            case ResultMember::Value:
               return JS_NewInt64(ctx, holder->result->GetValue());
            case ResultMember::Message:
               return NewString_(ctx, holder->result->GetMessage());
            case ResultMember::Parameter:
               return JS_NewInt32(ctx, holder->result->GetParameter());
            default:
               return JS_ThrowTypeError(ctx, "%s", "Result has no such property");
            }
         });
      }

      JSValue ResultSet_(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
      {
         ResultHolder *holder = static_cast<ResultHolder *>(JS_GetOpaque2(ctx, self, ResultClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            switch (static_cast<ResultMember>(magic))
            {
            case ResultMember::Value:
               {
                  int32_t value = 0;
                  if (!NumberArgument_(ctx, argc, argv, 0, value))
                     return JS_EXCEPTION;

                  holder->result->SetValue(value);
                  return JS_UNDEFINED;
               }
            case ResultMember::Message:
               {
                  String value;
                  if (!TextArgument_(ctx, argc, argv, 0, value))
                     return JS_EXCEPTION;

                  holder->result->SetMessage(value);
                  return JS_UNDEFINED;
               }
            case ResultMember::Parameter:
               {
                  int32_t value = 0;
                  if (!NumberArgument_(ctx, argc, argv, 0, value))
                     return JS_EXCEPTION;

                  holder->result->SetParameter(value);
                  return JS_UNDEFINED;
               }
            default:
               return JS_ThrowTypeError(ctx, "%s", "Result has no such property");
            }
         });
      }

      // ---- EventLog: COM/InterfaceEventLog.cpp

      enum class EventLogMember : int { Write };

      const MemberSpec EventLogMembers[] =
      {
         { "Write", KindMethod, 1, static_cast<int>(EventLogMember::Write) },
      };

      JSValue EventLogCall_(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
      {
         if (JS_GetClassID(self) != EventLogClass)
            return JS_ThrowTypeError(ctx, "%s", "EventLog.Write was called on something that is not the EventLog");

         return Guard_(ctx, [&]() -> JSValue
         {
            if (static_cast<EventLogMember>(magic) != EventLogMember::Write)
               return JS_ThrowTypeError(ctx, "%s", "EventLog has no such method");

            String text;
            if (!TextArgument_(ctx, argc, argv, 0, text))
               return JS_EXCEPTION;

            Logger::Instance()->LogEvent(text);
            return JS_UNDEFINED;
         });
      }

      // ---- FetchAccount: COM/InterfaceFetchAccount.cpp, reading only

      enum class FetchAccountMember : int
      {
         ID, Name, ServerAddress, Port, ServerType, Username, MinutesBetweenFetch, DaysToKeepMessages, AccountID,
         Enabled, ProcessMIMERecipients, ProcessMIMEDate, UseSSL, NextDownloadTime, UseAntiSpam, UseAntiVirus,
         EnableRouteRecipients, IsLocked, ConnectionSecurity, MIMERecipientHeaders, MirrorFolders
      };

      const MemberSpec FetchAccountMembers[] =
      {
         { "ID", KindRead, 0, static_cast<int>(FetchAccountMember::ID) },
         { "Name", KindRead, 0, static_cast<int>(FetchAccountMember::Name) },
         { "ServerAddress", KindRead, 0, static_cast<int>(FetchAccountMember::ServerAddress) },
         { "Port", KindRead, 0, static_cast<int>(FetchAccountMember::Port) },
         { "ServerType", KindRead, 0, static_cast<int>(FetchAccountMember::ServerType) },
         { "Username", KindRead, 0, static_cast<int>(FetchAccountMember::Username) },
         { "MinutesBetweenFetch", KindRead, 0, static_cast<int>(FetchAccountMember::MinutesBetweenFetch) },
         { "DaysToKeepMessages", KindRead, 0, static_cast<int>(FetchAccountMember::DaysToKeepMessages) },
         { "AccountID", KindRead, 0, static_cast<int>(FetchAccountMember::AccountID) },
         { "Enabled", KindRead, 0, static_cast<int>(FetchAccountMember::Enabled) },
         { "ProcessMIMERecipients", KindRead, 0, static_cast<int>(FetchAccountMember::ProcessMIMERecipients) },
         { "ProcessMIMEDate", KindRead, 0, static_cast<int>(FetchAccountMember::ProcessMIMEDate) },
         { "UseSSL", KindRead, 0, static_cast<int>(FetchAccountMember::UseSSL) },
         { "NextDownloadTime", KindRead, 0, static_cast<int>(FetchAccountMember::NextDownloadTime) },
         { "UseAntiSpam", KindRead, 0, static_cast<int>(FetchAccountMember::UseAntiSpam) },
         { "UseAntiVirus", KindRead, 0, static_cast<int>(FetchAccountMember::UseAntiVirus) },
         { "EnableRouteRecipients", KindRead, 0, static_cast<int>(FetchAccountMember::EnableRouteRecipients) },
         { "IsLocked", KindRead, 0, static_cast<int>(FetchAccountMember::IsLocked) },
         { "ConnectionSecurity", KindRead, 0, static_cast<int>(FetchAccountMember::ConnectionSecurity) },
         { "MIMERecipientHeaders", KindRead, 0, static_cast<int>(FetchAccountMember::MIMERecipientHeaders) },
         { "MirrorFolders", KindRead, 0, static_cast<int>(FetchAccountMember::MirrorFolders) },
      };

      JSValue FetchAccountGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         FetchAccountHolder *holder = static_cast<FetchAccountHolder *>(JS_GetOpaque2(ctx, self, FetchAccountClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            std::shared_ptr<FetchAccount> account = holder->fetch_account;

            switch (static_cast<FetchAccountMember>(magic))
            {
            case FetchAccountMember::ID:
               return JS_NewInt64(ctx, account->GetID());
            case FetchAccountMember::Name:
               return NewString_(ctx, account->GetName());
            case FetchAccountMember::ServerAddress:
               return NewString_(ctx, account->GetServerAddress());
            case FetchAccountMember::Port:
               return JS_NewInt32(ctx, account->GetPort());
            case FetchAccountMember::ServerType:
               return JS_NewInt32(ctx, static_cast<int32_t>(account->GetServerType()));
            case FetchAccountMember::Username:
               return NewString_(ctx, account->GetUsername());
            case FetchAccountMember::MinutesBetweenFetch:
               return JS_NewInt32(ctx, account->GetMinutesBetweenTry());
            case FetchAccountMember::DaysToKeepMessages:
               return JS_NewInt32(ctx, account->GetDaysToKeep());
            case FetchAccountMember::AccountID:
               return JS_NewInt64(ctx, account->GetAccountID());
            case FetchAccountMember::Enabled:
               return JS_NewBool(ctx, account->GetActive());
            case FetchAccountMember::ProcessMIMERecipients:
               return JS_NewBool(ctx, account->GetProcessMIMERecipients());
            case FetchAccountMember::ProcessMIMEDate:
               return JS_NewBool(ctx, account->GetProcessMIMEDate());
            case FetchAccountMember::UseSSL:
               return JS_NewBool(ctx, account->GetConnectionSecurity() == CSSSL);
            case FetchAccountMember::NextDownloadTime:
               return NewString_(ctx, account->GetNextTry());
            case FetchAccountMember::UseAntiSpam:
               return JS_NewBool(ctx, account->GetUseAntiSpam());
            case FetchAccountMember::UseAntiVirus:
               return JS_NewBool(ctx, account->GetUseAntiVirus());
            case FetchAccountMember::EnableRouteRecipients:
               return JS_NewBool(ctx, account->GetEnableRouteRecipients());
            case FetchAccountMember::IsLocked:
               return JS_NewBool(ctx, PersistentFetchAccount::IsLocked(account->GetID()));
            case FetchAccountMember::ConnectionSecurity:
               return JS_NewInt32(ctx, static_cast<int32_t>(account->GetConnectionSecurity()));
            case FetchAccountMember::MIMERecipientHeaders:
               return NewString_(ctx, account->GetMIMERecipientHeaders());
            case FetchAccountMember::MirrorFolders:
               return JS_NewBool(ctx, account->GetMirrorFolders());
            default:
               return JS_ThrowTypeError(ctx, "%s", "FetchAccount has no such property");
            }
         });
      }

      // ---- Account: COM/InterfaceAccount.cpp, reading only and thinner than COM

      enum class AccountMember : int
      {
         ID, Address, DomainID, Active, IsAD, ADDomain, ADUsername, MaxSize, AdminLevel, PersonFirstName,
         PersonLastName, VacationMessageIsOn, VacationSubject, VacationMessage, ForwardEnabled, ForwardAddress,
         ForwardKeepOriginal, SignatureEnabled, AntiSpamEnabled, MessageRetentionDays, RestrictedDelivery,
         ZeroAccess, ExternalId
      };

      const MemberSpec AccountMembers[] =
      {
         { "ID", KindRead, 0, static_cast<int>(AccountMember::ID) },
         { "Address", KindRead, 0, static_cast<int>(AccountMember::Address) },
         { "DomainID", KindRead, 0, static_cast<int>(AccountMember::DomainID) },
         { "Active", KindRead, 0, static_cast<int>(AccountMember::Active) },
         { "IsAD", KindRead, 0, static_cast<int>(AccountMember::IsAD) },
         { "ADDomain", KindRead, 0, static_cast<int>(AccountMember::ADDomain) },
         { "ADUsername", KindRead, 0, static_cast<int>(AccountMember::ADUsername) },
         { "MaxSize", KindRead, 0, static_cast<int>(AccountMember::MaxSize) },
         { "AdminLevel", KindRead, 0, static_cast<int>(AccountMember::AdminLevel) },
         { "PersonFirstName", KindRead, 0, static_cast<int>(AccountMember::PersonFirstName) },
         { "PersonLastName", KindRead, 0, static_cast<int>(AccountMember::PersonLastName) },
         { "VacationMessageIsOn", KindRead, 0, static_cast<int>(AccountMember::VacationMessageIsOn) },
         { "VacationSubject", KindRead, 0, static_cast<int>(AccountMember::VacationSubject) },
         { "VacationMessage", KindRead, 0, static_cast<int>(AccountMember::VacationMessage) },
         { "ForwardEnabled", KindRead, 0, static_cast<int>(AccountMember::ForwardEnabled) },
         { "ForwardAddress", KindRead, 0, static_cast<int>(AccountMember::ForwardAddress) },
         { "ForwardKeepOriginal", KindRead, 0, static_cast<int>(AccountMember::ForwardKeepOriginal) },
         { "SignatureEnabled", KindRead, 0, static_cast<int>(AccountMember::SignatureEnabled) },
         { "AntiSpamEnabled", KindRead, 0, static_cast<int>(AccountMember::AntiSpamEnabled) },
         { "MessageRetentionDays", KindRead, 0, static_cast<int>(AccountMember::MessageRetentionDays) },
         { "RestrictedDelivery", KindRead, 0, static_cast<int>(AccountMember::RestrictedDelivery) },
         { "ZeroAccess", KindRead, 0, static_cast<int>(AccountMember::ZeroAccess) },
         { "ExternalId", KindRead, 0, static_cast<int>(AccountMember::ExternalId) },
      };

      JSValue AccountGet_(JSContext *ctx, JSValueConst self, int /*argc*/, JSValueConst * /*argv*/, int magic)
      {
         AccountHolder *holder = static_cast<AccountHolder *>(JS_GetOpaque2(ctx, self, AccountClass));
         if (!holder)
            return JS_EXCEPTION;

         return Guard_(ctx, [&]() -> JSValue
         {
            std::shared_ptr<Account> account = holder->account;

            switch (static_cast<AccountMember>(magic))
            {
            case AccountMember::ID:
               return JS_NewInt64(ctx, account->GetID());
            case AccountMember::Address:
               return NewString_(ctx, account->GetAddress());
            case AccountMember::DomainID:
               return JS_NewInt64(ctx, account->GetDomainID());
            case AccountMember::Active:
               return JS_NewBool(ctx, account->GetActive());
            case AccountMember::IsAD:
               return JS_NewBool(ctx, account->GetIsAD());
            case AccountMember::ADDomain:
               return NewString_(ctx, account->GetADDomain());
            case AccountMember::ADUsername:
               return NewString_(ctx, account->GetADUsername());
            case AccountMember::MaxSize:
               return JS_NewInt64(ctx, account->GetAccountMaxSize());
            case AccountMember::AdminLevel:
               return JS_NewInt32(ctx, static_cast<int32_t>(account->GetAdminLevel()));
            case AccountMember::PersonFirstName:
               return NewString_(ctx, account->GetPersonFirstName());
            case AccountMember::PersonLastName:
               return NewString_(ctx, account->GetPersonLastName());
            case AccountMember::VacationMessageIsOn:
               // What get_VacationMessageIsOn answers: the switch as scheduled
               // today, not the stored flag alone.
               return JS_NewBool(ctx, PersistentAccount::GetIsVacationMessageOn(account));
            case AccountMember::VacationSubject:
               return NewString_(ctx, account->GetVacationSubject());
            case AccountMember::VacationMessage:
               return NewString_(ctx, account->GetVacationMessage());
            case AccountMember::ForwardEnabled:
               return JS_NewBool(ctx, account->GetForwardEnabled());
            case AccountMember::ForwardAddress:
               return NewString_(ctx, account->GetForwardAddress());
            case AccountMember::ForwardKeepOriginal:
               return JS_NewBool(ctx, account->GetForwardKeepOriginal());
            case AccountMember::SignatureEnabled:
               return JS_NewBool(ctx, account->GetEnableSignature());
            case AccountMember::AntiSpamEnabled:
               return JS_NewBool(ctx, account->GetAntiSpamEnabled());
            case AccountMember::MessageRetentionDays:
               return JS_NewInt32(ctx, account->GetMessageRetentionDays());
            case AccountMember::RestrictedDelivery:
               return JS_NewInt32(ctx, account->GetRestrictedDelivery());
            case AccountMember::ZeroAccess:
               return JS_NewBool(ctx, account->GetZeroAccess());
            case AccountMember::ExternalId:
               return NewString_(ctx, account->GetExternalId());
            default:
               return JS_ThrowTypeError(ctx, "%s", "Account has no such property");
            }
         });
      }

      // A setter that refuses, for a property that is read-only on JavaScript -
      // read-only on the COM object too, or read-only here only (the Account and
      // FetchAccount properties NotOnJavaScript names as "write"). An assignment
      // to it throws rather than being dropped in silence, as VBScript's does.
      JSValue ReadOnly_(JSContext *ctx, JSValueConst /*self*/, int /*argc*/, JSValueConst * /*argv*/, int /*magic*/, JSValue *data)
      {
         return Guard_(ctx, [&]() -> JSValue
         {
            String name;
            if (!ReadString_(ctx, data[0], name))
               return JS_EXCEPTION;

            std::string utf8 = ToUtf8_(name);
            return JS_ThrowTypeError(ctx, "%s is read-only", utf8.c_str());
         });
      }

      // ------------------------------------------------------------ one table

      class ClassBinding
      {
      public:
         const char *object;
         JSClassID *class_id;
         JSClassFinalizer *finalizer;
         const MemberSpec *members;
         size_t count;
         JSCFunctionMagic *get;
         JSCFunctionMagic *set;
         JSCFunctionMagic *call;
      };

      template <size_t N>
      constexpr size_t CountOf_(const MemberSpec (&)[N])
      {
         return N;
      }

      const ClassBinding Classes[] =
      {
         { "Message", &MessageClass, FinalizeMessage_, MessageMembers, CountOf_(MessageMembers), MessageGet_, MessageSet_, MessageCall_ },
         { "Client", &ClientClass, FinalizeClient_, ClientMembers, CountOf_(ClientMembers), ClientGet_, nullptr, nullptr },
         { "Result", &ResultClass, FinalizeResult_, ResultMembers, CountOf_(ResultMembers), ResultGet_, ResultSet_, nullptr },
         { "EventLog", &EventLogClass, nullptr, EventLogMembers, CountOf_(EventLogMembers), nullptr, nullptr, EventLogCall_ },
         { "FetchAccount", &FetchAccountClass, FinalizeFetchAccount_, FetchAccountMembers, CountOf_(FetchAccountMembers), FetchAccountGet_, nullptr, nullptr },
         { "Account", &AccountClass, FinalizeAccount_, AccountMembers, CountOf_(AccountMembers), AccountGet_, nullptr, nullptr },
         { "Recipients", &RecipientsClass, FinalizeRecipients_, RecipientsMembers, CountOf_(RecipientsMembers), RecipientsGet_, nullptr, RecipientsCall_ },
         { "Recipient", &RecipientClass, FinalizeRecipient_, RecipientMembers, CountOf_(RecipientMembers), RecipientGet_, nullptr, nullptr },
      };

      bool DefineMembers_(JSContext *ctx, JSValueConst prototype, const ClassBinding &binding)
      {
         for (size_t i = 0; i < binding.count; i++)
         {
            const MemberSpec &member = binding.members[i];

            if (member.kind == KindMethod)
            {
               JSValue method = JS_NewCFunctionMagic(ctx, binding.call, member.name, member.arguments, JS_CFUNC_generic_magic, member.id);
               if (JS_IsException(method))
                  return false;

               // Takes the method.
               if (JS_DefinePropertyValueStr(ctx, prototype, member.name, method, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
                  return false;

               continue;
            }

            JSValue getter = JS_NewCFunctionMagic(ctx, binding.get, member.name, 0, JS_CFUNC_generic_magic, member.id);
            JSValue setter = JS_UNDEFINED;

            if (member.kind == KindReadWrite)
            {
               setter = JS_NewCFunctionMagic(ctx, binding.set, member.name, 1, JS_CFUNC_generic_magic, member.id);
            }
            else
            {
               std::string qualified = std::string(binding.object) + "." + member.name;
               JSValue name = JS_NewStringLen(ctx, qualified.data(), qualified.size());
               if (!JS_IsException(name))
               {
                  setter = JS_NewCFunctionData(ctx, ReadOnly_, 1, 0, 1, &name);
                  JS_FreeValue(ctx, name);
               }
               else
               {
                  setter = JS_EXCEPTION;
               }
            }

            if (JS_IsException(getter) || JS_IsException(setter))
            {
               JS_FreeValue(ctx, getter);
               JS_FreeValue(ctx, setter);
               return false;
            }

            JSAtom atom = JS_NewAtom(ctx, member.name);
            if (atom == JS_ATOM_NULL)
            {
               JS_FreeValue(ctx, getter);
               JS_FreeValue(ctx, setter);
               return false;
            }

            // Takes the getter and the setter.
            int defined = JS_DefinePropertyGetSet(ctx, prototype, atom, getter, setter, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
            JS_FreeAtom(ctx, atom);

            if (defined < 0)
               return false;
         }

         return true;
      }

      // CreateObject (VBScript) and new ActiveXObject (JScript) make COM objects,
      // and COM is Windows. They exist here only to say so in the error a script
      // gets, rather than a bare "not defined".
      JSValue NoComObjects_(JSContext *ctx, JSValueConst /*self*/, int /*argc*/, JSValueConst * /*argv*/)
      {
         return JS_ThrowTypeError(ctx, "%s",
            "CreateObject and ActiveXObject are not available to JavaScript event scripts: they make COM objects, and COM is Windows. "
            "The server's own objects are passed to each handler; see EventScripts.md.");
      }

      // ------------------------------------------------------------ the engine

      class Engine
      {
      public:
         explicit Engine(int timeoutSeconds) :
            state(timeoutSeconds),
            runtime(nullptr),
            context(nullptr)
         {
         }

         ~Engine()
         {
            if (context)
               JS_FreeContext(context);

            if (runtime)
               JS_FreeRuntime(runtime);
         }

         // False when the runtime, the context or the classes could not be made.
         bool Open()
         {
            AllocateClassIds_();

            runtime = JS_NewRuntime2(&Allocator, &state);
            if (!runtime)
               return false;

            JS_SetMemoryLimit(runtime, ScriptBinding::MemoryCeiling);
            JS_SetMaxStackSize(runtime, ScriptBinding::StackCeiling);
            JS_SetInterruptHandler(runtime, Interrupt_, &state);
            JS_SetCanBlock(runtime, 0);

            for (const ClassBinding &binding : Classes)
            {
               JSClassDef definition = {};
               definition.class_name = binding.object;
               definition.finalizer = binding.finalizer;

               if (JS_NewClass(runtime, *binding.class_id, &definition) < 0)
                  return false;
            }

            context = JS_NewContext(runtime);
            if (!context)
               return false;

            JS_SetContextOpaque(context, &state);

            for (const ClassBinding &binding : Classes)
            {
               JSValue prototype = JS_NewObject(context);
               if (JS_IsException(prototype))
                  return false;

               if (!DefineMembers_(context, prototype, binding))
               {
                  JS_FreeValue(context, prototype);
                  return false;
               }

               // Takes the prototype.
               JS_SetClassProto(context, *binding.class_id, prototype);
            }

            JSValue global = JS_GetGlobalObject(context);
            bool defined = true;

            const char *refused[] = { "CreateObject", "ActiveXObject" };
            for (const char *name : refused)
            {
               JSValue function = JS_NewCFunction2(context, NoComObjects_, name, 1, JS_CFUNC_constructor_or_func, 0);
               if (JS_IsException(function) || JS_DefinePropertyValueStr(context, global, name, function, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
                  defined = false;
            }

            JS_FreeValue(context, global);
            return defined;
         }

         RunState state;
         JSRuntime *runtime;
         JSContext *context;
      };

      // ------------------------------------------------------------ diagnostics

      String PropertyText_(JSContext *ctx, JSValueConst object, const char *name)
      {
         String text;
         JSValue value = JS_GetPropertyStr(ctx, object, name);

         if (JS_IsException(value))
         {
            ClearException_(ctx);
            return text;
         }

         if (!JS_IsUndefined(value) && !ReadString_(ctx, value, text))
            ClearException_(ctx);

         JS_FreeValue(ctx, value);
         return text;
      }

      int PropertyNumber_(JSContext *ctx, JSValueConst object, const char *name)
      {
         int32_t number = 0;
         JSValue value = JS_GetPropertyStr(ctx, object, name);

         if (JS_IsException(value))
         {
            ClearException_(ctx);
            return 0;
         }

         if (JS_IsNumber(value) && JS_ToInt32(ctx, &number, value) != 0)
         {
            ClearException_(ctx);
            number = 0;
         }

         JS_FreeValue(ctx, value);
         return number;
      }

      // The first "<file>:<line>:<column>" in a stack, which is the frame the error
      // was raised in: a runtime error carries its position only there.
      void FindPosition_(const String &stack, const String &fileName, int &line, int &column)
      {
         String marker = fileName + _T(":");
         int at = stack.Find(marker);
         if (at < 0)
            return;

         const wchar_t *p = stack.c_str() + at + marker.GetLength();
         int foundLine = 0;
         while (*p >= L'0' && *p <= L'9')
            foundLine = foundLine * 10 + (*p++ - L'0');

         if (foundLine == 0)
            return;

         line = foundLine;

         if (*p == L':')
         {
            p++;
            int foundColumn = 0;
            while (*p >= L'0' && *p <= L'9')
               foundColumn = foundColumn * 10 + (*p++ - L'0');
            column = foundColumn;
         }
      }

      // A stack on one line: its frames trimmed and joined with " | ", at most
      // MaximumFrames of them - a runaway recursion's stack overflow otherwise
      // puts thousands of frames on a single log line.
      const int MaximumFrames = 20;

      String FlattenStack_(const String &stack)
      {
         String flat;
         int frames = 0;
         int omitted = 0;
         int start = 0;

         while (start <= stack.GetLength())
         {
            int end = stack.Find(_T('\n'), start);
            if (end < 0)
               end = stack.GetLength();

            String frame = stack.Mid(start, end - start);
            frame.Replace(_T("\r"), _T(""));
            frame.Trim();

            if (!frame.IsEmpty())
            {
               if (frames < MaximumFrames)
               {
                  if (!flat.IsEmpty())
                     flat += _T(" | ");
                  flat += frame;
                  frames++;
               }
               else
               {
                  omitted++;
               }
            }

            start = end + 1;
         }

         if (omitted > 0)
         {
            String more;
            more.Format(_T(" | ... and %d more frames"), omitted);
            flat += more;
         }

         return flat;
      }

      // One line for the ERROR log from the exception pending in the context, and
      // the exception cleared. The shape follows the Windows host's
      // CScriptSiteBasic::HandleScriptError - "Script Error: Source: ... - Line: n
      // Column: n" - so that a log search written for one finds the other, and
      // carries the stack after it, flattened onto the line.
      void Describe_(Engine &engine, const String &fileName, ScriptRunOutcome &outcome)
      {
         JSContext *ctx = engine.context;

         // The script has stopped; what is read below is its error object, which
         // may itself need a little memory to turn into text even when the run was
         // stopped at the ceiling.
         JS_SetMemoryLimit(engine.runtime, ScriptBinding::MemoryCeiling + 4 * 1024 * 1024);

         JSValue exception = JS_GetException(ctx);

         String name;
         String message;
         String stack;
         int line = 0;
         int column = 0;

         if (JS_IsError(ctx, exception))
         {
            name = PropertyText_(ctx, exception, "name");
            message = PropertyText_(ctx, exception, "message");
            stack = PropertyText_(ctx, exception, "stack");
            line = PropertyNumber_(ctx, exception, "lineNumber");
            column = PropertyNumber_(ctx, exception, "columnNumber");
         }
         else
         {
            name = _T("a thrown value that is not an Error");
            if (!ReadString_(ctx, exception, message))
               ClearException_(ctx);
         }

         JS_FreeValue(ctx, exception);

         if (line == 0)
            FindPosition_(stack, fileName, line, column);

         String flatStack = FlattenStack_(stack);
         message.Replace(_T("\r"), _T(" "));
         message.Replace(_T("\n"), _T(" "));

         String lineText;
         lineText.Format(_T("Script Error: Source: JavaScript - Error: %s - Description: %s - Line: %d Column: %d - Stack: %s"),
            name.c_str(), message.c_str(), line, column, flatStack.c_str());

         outcome.error = lineText;
         outcome.error_line = line;
      }

      bool RunJobs_(Engine &engine, const String &fileName, ScriptRunOutcome &outcome)
      {
         while (JS_IsJobPending(engine.runtime))
         {
            JSContext *jobContext = nullptr;

            if (JS_ExecutePendingJob(engine.runtime, &jobContext) < 0)
            {
               Describe_(engine, fileName, outcome);
               return false;
            }

            // A chain of promise jobs each too short to reach an interrupt poll
            // still runs inside the deadline.
            if (Interrupt_(engine.runtime, &engine.state))
            {
               outcome.error = _T("Script Error: Source: JavaScript - Error: InternalError - Description: interrupted - Line: 0 Column: 0 - Stack: ");
               return false;
            }
         }

         return true;
      }

      // Evaluates the whole script as global code, then the promise jobs its top
      // level queued.
      bool Evaluate_(Engine &engine, const String &script, const String &fileName, ScriptRunOutcome &outcome)
      {
         std::string source = ToUtf8_(script);
         std::string file = ToUtf8_(fileName);

         // JS_Eval reads source[length] and needs it to be the terminating NUL,
         // which std::string::c_str guarantees.
         JSValue result = JS_Eval(engine.context, source.c_str(), source.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL);
         if (JS_IsException(result))
         {
            Describe_(engine, fileName, outcome);
            return false;
         }

         JS_FreeValue(engine.context, result);
         return RunJobs_(engine, fileName, outcome);
      }

      // A name a handler may have: ASCII letters, digits, _ and $, not starting with a
      // digit, at most 255 characters. Anything else is not looked up at all. The one
      // test the Windows engines' call text is held to (ScriptServer::
      // IsPlainProcedureName), and the one a rule's Run function name is held to when
      // the rule is saved (RuleGuard::WhyScriptFunctionMayNotBeStored) - so a name
      // saved is a name looked up here, where this was a copy of it until 13 October
      // 2026.
      bool IsIdentifier_(const String &name)
      {
         return ScriptServer::IsPlainProcedureName(name);
      }

      // The function named, or undefined: looked up by evaluating the name, so that
      // a handler declared with let, const or class is found as well as a function
      // declaration or a var - the first two are not properties of the global
      // object. JS_EXCEPTION only when the lookup itself failed.
      JSValue Lookup_(Engine &engine, const String &functionName)
      {
         if (!IsIdentifier_(functionName))
            return JS_UNDEFINED;

         std::string name = ToUtf8_(functionName);
         std::string expression = "(typeof " + name + " === 'function') ? " + name + " : undefined";

         JSValue value = JS_Eval(engine.context, expression.c_str(), expression.size(), "<handler lookup>", JS_EVAL_TYPE_GLOBAL);
         if (!JS_IsException(value))
            return value;

         // The deadline, the server stopping and the memory ceiling are the
         // caller's to report, with the exception still pending.
         if (engine.state.interrupted || engine.state.thread_interruption || engine.state.refused_at_ceiling)
            return value;

         // A reserved word that passed IsIdentifier_ - "class", "if" - is not a
         // handler's name, and the expression built from it is a SyntaxError:
         // that one failure is the answer "no such handler". Anything else is a
         // lookup that failed, and answering "no" to it would switch the handler
         // off without a word; it goes back to the caller, pending, as it is.
         JSValue exception = JS_GetException(engine.context);

         bool reserved = JS_IsError(engine.context, exception) &&
            PropertyText_(engine.context, exception, "name") == String(_T("SyntaxError"));

         if (reserved)
         {
            JS_FreeValue(engine.context, exception);
            return JS_UNDEFINED;
         }

         // Takes the exception back.
         JS_Throw(engine.context, exception);
         return JS_EXCEPTION;
      }

      void Conclude_(Engine &engine, ScriptRunOutcome &outcome)
      {
         outcome.interrupted = engine.state.interrupted;
         outcome.out_of_memory = !outcome.completed && engine.state.refused_at_ceiling;
      }

      const wchar_t *EngineFailure = _T("Script Error: Source: JavaScript - Error: InternalError - Description: the JavaScript engine could not be started (out of memory) - Line: 0 Column: 0 - Stack: ");

      // An object for one entry of the container, or null for an entry with no
      // C++ object behind it (an event fired before there is a message).
      JSValue NewBoundObject_(JSContext *ctx, const ScriptObject &entry)
      {
         switch (entry.eType)
         {
         case ScriptObject::OTResult:
            {
               std::shared_ptr<Result> result = std::static_pointer_cast<Result>(entry.pObject);
               return result ? Wrap_(ctx, ResultClass, new ResultHolder(result)) : JS_NULL;
            }
         case ScriptObject::OTMessage:
            {
               std::shared_ptr<Message> message = std::static_pointer_cast<Message>(entry.pObject);
               return message ? Wrap_(ctx, MessageClass, new MessageHolder(message)) : JS_NULL;
            }
         case ScriptObject::OTClient:
            {
               std::shared_ptr<ClientInfo> client = std::static_pointer_cast<ClientInfo>(entry.pObject);
               return client ? Wrap_(ctx, ClientClass, new ClientHolder(client)) : JS_NULL;
            }
         case ScriptObject::OTEventLog:
            return JS_NewObjectClass(ctx, static_cast<int>(EventLogClass));
         case ScriptObject::OTFetchAccount:
            {
               std::shared_ptr<FetchAccount> account = std::static_pointer_cast<FetchAccount>(entry.pObject);
               return account ? Wrap_(ctx, FetchAccountClass, new FetchAccountHolder(account)) : JS_NULL;
            }
         case ScriptObject::OTAccount:
            {
               std::shared_ptr<Account> account = std::static_pointer_cast<Account>(entry.pObject);
               return account ? Wrap_(ctx, AccountClass, new AccountHolder(account)) : JS_NULL;
            }
         default:
            return JS_NULL;
         }
      }
   }

   ScriptRunOutcome::ScriptRunOutcome() :
      completed(false),
      interrupted(false),
      out_of_memory(false),
      engine_failed(false),
      function_exists(false),
      error_line(0)
   {
   }

   ScriptRunOutcome
   ScriptBinding::RunScript(const String &script, const String &fileName, int timeoutSeconds)
   {
      ScriptRunOutcome outcome;
      bool stopping = false;

      {
         Engine engine(timeoutSeconds);

         if (!engine.Open())
         {
            outcome.engine_failed = true;
            outcome.error = EngineFailure;
            return outcome;
         }

         outcome.completed = Evaluate_(engine, script, fileName, outcome);

         Conclude_(engine, outcome);
         stopping = engine.state.thread_interruption;
      }

      if (stopping)
         throw boost::thread_interrupted();

      return outcome;
   }

   ScriptRunOutcome
   ScriptBinding::FindFunction(const String &script, const String &fileName, const String &functionName, int timeoutSeconds)
   {
      ScriptRunOutcome outcome;
      bool stopping = false;

      {
         Engine engine(timeoutSeconds);

         if (!engine.Open())
         {
            outcome.engine_failed = true;
            outcome.error = EngineFailure;
            return outcome;
         }

         if (Evaluate_(engine, script, fileName, outcome))
         {
            JSValue function = Lookup_(engine, functionName);

            if (JS_IsException(function))
            {
               Describe_(engine, fileName, outcome);
            }
            else
            {
               outcome.completed = true;
               outcome.function_exists = JS_IsFunction(engine.context, function) != 0;
               JS_FreeValue(engine.context, function);
            }
         }

         Conclude_(engine, outcome);
         stopping = engine.state.thread_interruption;
      }

      if (stopping)
         throw boost::thread_interrupted();

      return outcome;
   }

   ScriptRunOutcome
   ScriptBinding::CallHandler(const String &script, const String &fileName, const String &functionName,
      const std::vector<String> &objectArguments, std::shared_ptr<ScriptObjectContainer> objects, int timeoutSeconds)
   {
      ScriptRunOutcome outcome;
      bool stopping = false;

      {
         Engine engine(timeoutSeconds);

         if (!engine.Open())
         {
            outcome.engine_failed = true;
            outcome.error = EngineFailure;
            return outcome;
         }

         JSContext *ctx = engine.context;
         JSValue global = JS_GetGlobalObject(ctx);

         // The objects, bound before the script runs - as the Windows host adds its
         // named items before it runs the script - so that the top level can see
         // them too. Each is kept here as well, to be passed as an argument: the
         // handler's parameter and the global are the same object.
         std::map<String, JSValue> bound;
         bool boundAll = true;

         if (objects)
         {
            std::vector<String> names = objects->GetObjectNames();

            for (const String &name : names)
            {
               std::shared_ptr<ScriptObject> entry = objects->FindObject(name);
               JSValue value = entry ? NewBoundObject_(ctx, *entry) : JS_NULL;

               if (JS_IsException(value))
               {
                  boundAll = false;
                  break;
               }

               bound[name] = JS_DupValue(ctx, value);

               std::string utf8 = ToUtf8_(name);
               if (JS_DefinePropertyValueStr(ctx, global, utf8.c_str(), value, JS_PROP_C_W_E) < 0)
               {
                  boundAll = false;
                  break;
               }
            }
         }

         if (!boundAll)
         {
            ClearException_(ctx);
            outcome.engine_failed = true;
            outcome.error = EngineFailure;
         }
         else if (Evaluate_(engine, script, fileName, outcome))
         {
            JSValue function = Lookup_(engine, functionName);

            if (JS_IsException(function))
            {
               Describe_(engine, fileName, outcome);
            }
            else if (!JS_IsFunction(ctx, function))
            {
               String text;
               text.Format(_T("Script Error: Source: JavaScript - Error: ReferenceError - Description: %s is not a function defined in the event script - Line: 0 Column: 0 - Stack: "),
                  functionName.c_str());
               outcome.error = text;
               JS_FreeValue(ctx, function);
            }
            else
            {
               outcome.function_exists = true;

               std::vector<JSValue> argv;

               for (const String &name : objectArguments)
               {
                  auto found = bound.find(name);
                  argv.push_back(found == bound.end() ? JS_NULL : JS_DupValue(ctx, (*found).second));
               }

               if (objects)
               {
                  for (const ScriptArgument &argument : objects->GetArguments())
                  {
                     if (argument.kind == ScriptArgument::KindText)
                        argv.push_back(NewString_(ctx, argument.text));
                     else
                        argv.push_back(JS_NewInt64(ctx, argument.number));
                  }
               }

               bool argumentsMade = true;
               for (const JSValue &value : argv)
               {
                  if (JS_IsException(value))
                     argumentsMade = false;
               }

               JSValue returned = argumentsMade
                  ? JS_Call(ctx, function, global, static_cast<int>(argv.size()), argv.empty() ? nullptr : argv.data())
                  : JS_EXCEPTION;

               for (JSValue &value : argv)
                  JS_FreeValue(ctx, value);

               if (JS_IsException(returned))
               {
                  Describe_(engine, fileName, outcome);
               }
               else
               {
                  // An async handler, or one that returns a promise, is waited for:
                  // its jobs run here, inside the same deadline, and a rejection is
                  // an error like any other.
                  bool jobsRan = RunJobs_(engine, fileName, outcome);

                  if (jobsRan)
                  {
                     int promiseState = static_cast<int>(JS_PromiseState(ctx, returned));

                     if (promiseState == JS_PROMISE_REJECTED)
                     {
                        JS_Throw(ctx, JS_PromiseResult(ctx, returned));
                        Describe_(engine, fileName, outcome);
                     }
                     else if (promiseState == JS_PROMISE_PENDING)
                     {
                        String text;
                        text.Format(_T("Script Error: Source: JavaScript - Error: Error - Description: %s returned a promise that had not settled when its jobs ran out - Line: 0 Column: 0 - Stack: "),
                           functionName.c_str());
                        outcome.error = text;
                     }
                     else
                     {
                        outcome.completed = true;

                        if (JS_IsString(returned) && !ReadString_(ctx, returned, outcome.returned))
                           ClearException_(ctx);
                     }
                  }
               }

               JS_FreeValue(ctx, returned);
               JS_FreeValue(ctx, function);
            }
         }

         for (auto &entry : bound)
            JS_FreeValue(ctx, entry.second);

         JS_FreeValue(ctx, global);

         Conclude_(engine, outcome);
         stopping = engine.state.thread_interruption;
      }

      if (stopping)
         throw boost::thread_interrupted();

      return outcome;
   }

   // ------------------------------------------------------------------ the contract

   std::vector<ScriptBinding::Member>
   ScriptBinding::Contract()
   {
      std::vector<Member> members;

      for (const ClassBinding &binding : Classes)
      {
         for (size_t i = 0; i < binding.count; i++)
         {
            Member member;
            member.object = binding.object;
            member.name = binding.members[i].name;
            member.kind = KindName_(binding.members[i].kind);
            members.push_back(member);
         }
      }

      return members;
   }

   std::vector<ScriptBinding::Member>
   ScriptBinding::NotOnJavaScript()
   {
      // What each COM interface a handler is given exposes to VBScript and JScript
      // and this binding does not. "write" is a COM property that is here but
      // read-only; every other kind is absent altogether. EventScripts.md gives the
      // reason beside each group.
      static const Member absent[] =
      {
         // Message: the two collection objects whose members read and write files
         // (Attachment.SaveAs, Attachments.Add) or hold positions into the header
         // list (MessageHeader) - use HeaderValue and SetHeaderValue.
         { "Message", "Attachments", "read" },
         { "Message", "Headers", "read" },

         // FetchAccount: the account a download is running under, given to read.
         { "FetchAccount", "Name", "write" },
         { "FetchAccount", "ServerAddress", "write" },
         { "FetchAccount", "Port", "write" },
         { "FetchAccount", "ServerType", "write" },
         { "FetchAccount", "Username", "write" },
         { "FetchAccount", "Password", "read/write" },
         { "FetchAccount", "MinutesBetweenFetch", "write" },
         { "FetchAccount", "DaysToKeepMessages", "write" },
         { "FetchAccount", "Save", "method" },
         { "FetchAccount", "AccountID", "write" },
         { "FetchAccount", "Enabled", "write" },
         { "FetchAccount", "ProcessMIMERecipients", "write" },
         { "FetchAccount", "DownloadNow", "method" },
         { "FetchAccount", "ProcessMIMEDate", "write" },
         { "FetchAccount", "UseSSL", "write" },
         { "FetchAccount", "Delete", "method" },
         { "FetchAccount", "UseAntiSpam", "write" },
         { "FetchAccount", "UseAntiVirus", "write" },
         { "FetchAccount", "EnableRouteRecipients", "write" },
         { "FetchAccount", "ConnectionSecurity", "write" },
         { "FetchAccount", "MIMERecipientHeaders", "write" },
         { "FetchAccount", "MirrorFolders", "write" },

         // Account: the account a password is being checked for, given to read,
         // and thinner than COM - the properties an administrator's tool reads and
         // the operations it performs are COM, REST and the Control Panel.
         { "Account", "Active", "write" },
         { "Account", "ADDomain", "write" },
         { "Account", "Address", "write" },
         { "Account", "DomainID", "write" },
         { "Account", "IsAD", "write" },
         { "Account", "Password", "read/write" },
         { "Account", "MoveTo", "method" },
         { "Account", "InUseReason", "read" },
         { "Account", "LastMove", "read" },
         { "Account", "Size", "read" },
         { "Account", "Save", "method" },
         { "Account", "ADUsername", "write" },
         { "Account", "DeleteMessages", "method" },
         { "Account", "Messages", "read" },
         { "Account", "MaxSize", "write" },
         { "Account", "MessageRetentionDays", "write" },
         { "Account", "VacationMessageIsOn", "write" },
         { "Account", "VacationMessage", "write" },
         { "Account", "VacationSubject", "write" },
         { "Account", "FetchAccounts", "read" },
         { "Account", "AdminLevel", "write" },
         { "Account", "Rules", "read" },
         { "Account", "ValidatePassword", "method" },
         { "Account", "UnlockMailbox", "method" },
         { "Account", "IMAPFolders", "read" },
         { "Account", "QuotaUsed", "read" },
         { "Account", "PushSubscriptionCount", "read" },
         { "Account", "RemovePushSubscriptions", "method" },
         { "Account", "ForwardEnabled", "write" },
         { "Account", "ForwardAddress", "write" },
         { "Account", "ForwardKeepOriginal", "write" },
         { "Account", "SignatureEnabled", "write" },
         { "Account", "SignaturePlainText", "read/write" },
         { "Account", "SignatureHTML", "read/write" },
         { "Account", "LastLogonTime", "read" },
         { "Account", "VacationMessageExpires", "read/write" },
         { "Account", "VacationMessageExpiresDate", "read/write" },
         { "Account", "PersonFirstName", "write" },
         { "Account", "PersonLastName", "write" },
         { "Account", "Delete", "method" },
         { "Account", "VacationMessageAbortSpamFlagged", "read/write" },
         { "Account", "VacationMessageDays", "read/write" },
         { "Account", "VacationInternalSubject", "read/write" },
         { "Account", "VacationInternalMessage", "read/write" },
         { "Account", "VacationMessageInternalOnly", "read/write" },
         { "Account", "ForwardAbortSpamFlagged", "read/write" },
         { "Account", "SieveScript", "read/write" },
         { "Account", "AppPasswords", "read" },
         { "Account", "TOTPEnabled", "read" },
         { "Account", "EnrolTOTP", "method" },
         { "Account", "DisableTOTP", "method" },
         { "Account", "ExportMessages", "method" },
         { "Account", "ExportArchive", "method" },
         { "Account", "AntiSpamEnabled", "write" },
         { "Account", "SpamMarkThreshold", "read/write" },
         { "Account", "SpamDeleteThreshold", "read/write" },
         { "Account", "GetDevices", "method" },
         { "Account", "RevokeDevice", "method" },
         { "Account", "VacationMessageBeginDate", "read/write" },
         { "Account", "RoleName", "read/write" },
         { "Account", "RoleDomains", "read/write" },
         { "Account", "RolePermissions", "read" },
         { "Account", "OpenPGPPublish", "read/write" },
         { "Account", "OpenPGPKeys", "read" },
         { "Account", "RecoveryAddress", "read" },
         { "Account", "RecoveryAddressVerified", "read" },
         { "Account", "RecoveryCodesUnused", "read" },
         { "Account", "ClearRecoveryAddress", "method" },
         { "Account", "ClearRecoveryCodes", "method" },
         { "Account", "DirectoryTitle", "read/write" },
         { "Account", "DirectoryDepartment", "read/write" },
         { "Account", "DirectoryPhone", "read/write" },
         { "Account", "DirectoryMobile", "read/write" },
         { "Account", "DirectoryOffice", "read/write" },
         { "Account", "DirectoryOptOut", "read/write" },
         { "Account", "DirectoryNote", "read/write" },
         { "Account", "DirectoryPhoto", "read/write" },
         { "Account", "DirectoryPhotoType", "read" },
         { "Account", "RestrictedDelivery", "write" },
         { "Account", "SettingOverride", "read/write" },
         { "Account", "EffectiveSetting", "read" },
         { "Account", "EffectiveSettingSource", "read" },
         { "Account", "ExternalId", "write" },
      };

      return std::vector<Member>(std::begin(absent), std::end(absent));
   }

   // ------------------------------------------------------------------ the tester

   namespace
   {
      void Expect_(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error("ScriptBindingTester: " + what);
      }

      std::string Narrow_(const String &text)
      {
         return ToUtf8_(text);
      }

      bool Contains_(const String &text, const String &part)
      {
         return text.Find(part) >= 0;
      }

      // The handler's answer, failing the test with the script's own error when it
      // did not complete.
      String Answer_(const String &script, const String &handler, const std::vector<String> &objectArguments,
         std::shared_ptr<ScriptObjectContainer> objects, const std::string &what)
      {
         ScriptRunOutcome outcome = ScriptBinding::CallHandler(script, _T("EventHandlers.js"), handler, objectArguments, objects, 30);
         Expect_(outcome.completed, what + " did not complete: " + Narrow_(outcome.error));
         return outcome.returned;
      }

      std::shared_ptr<ScriptObjectContainer> EveryObject_(std::shared_ptr<Result> &result, std::shared_ptr<ClientInfo> &client, std::shared_ptr<Message> &message)
      {
         std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);

         result = std::shared_ptr<Result>(new Result);
         client = std::shared_ptr<ClientInfo>(new ClientInfo);
         message = std::shared_ptr<Message>(new Message(false));

         std::shared_ptr<MessageRecipient> recipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
         recipient->SetAddress(_T("grace@example.test"));
         recipient->SetOriginalAddress(_T("hopper@example.test"));
         message->GetRecipients()->Add(recipient);

         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account);
         account->SetAddress(_T("ada@example.test"));

         std::shared_ptr<FetchAccount> fetchAccount = std::shared_ptr<FetchAccount>(new FetchAccount);
         fetchAccount->SetName(_T("Collector"));

         container->AddObject(_T("Result"), result, ScriptObject::OTResult);
         container->AddObject(_T("HMAILSERVER_CLIENT"), client, ScriptObject::OTClient);
         container->AddObject(_T("HMAILSERVER_MESSAGE"), message, ScriptObject::OTMessage);
         container->AddObject(_T("HMAILSERVER_ACCOUNT"), account, ScriptObject::OTAccount);
         container->AddObject(_T("HMAILSERVER_FETCHACCOUNT"), fetchAccount, ScriptObject::OTFetchAccount);
         return container;
      }
   }

   void
   ScriptBindingTester::Test()
   {
      TestContract_();
      TestValues_();
      TestLimits_();
      TestSandbox_();
      TestErrors_();
      TestLanguage_();
   }

   void
   ScriptBindingTester::TestContract_()
   {
      std::shared_ptr<Result> result;
      std::shared_ptr<ClientInfo> client;
      std::shared_ptr<Message> message;
      std::shared_ptr<ScriptObjectContainer> container = EveryObject_(result, client, message);

      // Every own member of each object's prototype, as object.name=kind. Read
      // from the descriptors, so no getter runs: the message here has no file.
      // The kind is the contract's own word: a method; an accessor whose setter
      // is the member's own (named after it, JS_NewCFunctionMagic) is read/write;
      // one whose setter is ReadOnly_ (JS_NewCFunctionData, which names nothing)
      // is read. So a read member bound with a setter, or a read/write one that
      // refuses assignment, fails here, not only a member missing altogether.
      String script =
         _T("function Probe() {\n")
         _T("   var objects = { Message: HMAILSERVER_MESSAGE, Client: HMAILSERVER_CLIENT, Result: Result, EventLog: EventLog,\n")
         _T("      FetchAccount: HMAILSERVER_FETCHACCOUNT, Account: HMAILSERVER_ACCOUNT,\n")
         _T("      Recipients: HMAILSERVER_MESSAGE.Recipients, Recipient: HMAILSERVER_MESSAGE.Recipients.Item(0) };\n")
         _T("   var lines = [];\n")
         _T("   for (var object in objects) {\n")
         _T("      var prototype = Object.getPrototypeOf(objects[object]);\n")
         _T("      for (var name of Object.getOwnPropertyNames(prototype)) {\n")
         _T("         var d = Object.getOwnPropertyDescriptor(prototype, name);\n")
         _T("         lines.push(object + '.' + name + '=' + (typeof d.value === 'function' ? 'method' : (d.get && d.set ? (d.set.name === '' ? 'read' : 'read/write') : 'other')));\n")
         _T("      }\n")
         _T("   }\n")
         _T("   return lines.join('\\n');\n")
         _T("}\n");

      String answer = Answer_(script, _T("Probe"), std::vector<String>(), container, "the contract probe");

      std::set<std::string> seen;
      std::string all = Narrow_(answer);
      size_t start = 0;
      while (start <= all.size())
      {
         size_t end = all.find('\n', start);
         if (end == std::string::npos)
            end = all.size();

         if (end > start)
            seen.insert(all.substr(start, end - start));

         start = end + 1;
      }

      std::vector<ScriptBinding::Member> contract = ScriptBinding::Contract();

      for (const ScriptBinding::Member &member : contract)
      {
         std::string expected = std::string(member.object) + "." + member.name + "=" + member.kind;

         Expect_(seen.count(expected) == 1, "the contract names " + expected + " and the object does not have it");
      }

      Expect_(seen.size() == contract.size(), "an object has a member the contract does not name (" +
         std::to_string(seen.size()) + " members, " + std::to_string(contract.size()) + " in the contract)");

      // What is not on JavaScript is not there - or, for "write", is there and
      // refuses an assignment without touching the object.
      String writes;
      for (const ScriptBinding::Member &member : ScriptBinding::NotOnJavaScript())
      {
         std::string kind = member.kind;
         std::string prefix = std::string(member.object) + "." + member.name + "=";

         if (kind == "write")
         {
            Expect_(seen.count(prefix + "read") == 1, prefix + " should be there to read, and refuse an assignment");
            writes += String(member.object) + _T(".") + String(member.name) + _T(",");
         }
         else
         {
            Expect_(seen.count(prefix + "method") == 0 && seen.count(prefix + "read") == 0 && seen.count(prefix + "read/write") == 0,
               prefix + " is listed as not on JavaScript and is there");
         }
      }

      // And every member the contract calls read, whatever object it is on:
      // assigning to it must throw "is read-only" and change nothing.
      for (const ScriptBinding::Member &member : contract)
      {
         if (std::string(member.kind) == "read")
            writes += String(member.object) + _T(".") + String(member.name) + _T(",");
      }

      String refusals =
         _T("function Refusals(list) {\n")
         _T("   var objects = { FetchAccount: HMAILSERVER_FETCHACCOUNT, Account: HMAILSERVER_ACCOUNT, Client: HMAILSERVER_CLIENT, Message: HMAILSERVER_MESSAGE,\n")
         _T("      Result: Result, Recipients: HMAILSERVER_MESSAGE.Recipients, Recipient: HMAILSERVER_MESSAGE.Recipients.Item(0) };\n")
         _T("   var taken = [];\n")
         _T("   for (var entry of list.split(',')) {\n")
         _T("      if (!entry) continue;\n")
         _T("      var parts = entry.split('.');\n")
         _T("      try { objects[parts[0]][parts[1]] = 0; taken.push(entry); } catch (e) { if (!/is read-only/.test(e.message)) taken.push(entry + ':' + e.message); }\n")
         _T("   }\n")
         _T("   for (var readOnly of ['Client.IPAddress', 'Message.ID', 'Message.Filename']) {\n")
         _T("      var p = readOnly.split('.');\n")
         _T("      try { objects[p[0]][p[1]] = 0; taken.push(readOnly); } catch (e) { }\n")
         _T("   }\n")
         _T("   return taken.join(',');\n")
         _T("}\n");

      std::shared_ptr<ScriptObjectContainer> second = EveryObject_(result, client, message);
      second->AddTextArgument(writes);
      String taken = Answer_(refusals, _T("Refusals"), std::vector<String>(), second, "the read-only probe");
      Expect_(taken.IsEmpty(), "an assignment to a read-only property was taken: " + Narrow_(taken));
   }

   void
   ScriptBindingTester::TestValues_()
   {
      std::shared_ptr<Result> result;
      std::shared_ptr<ClientInfo> client;
      std::shared_ptr<Message> message;
      std::shared_ptr<ScriptObjectContainer> container = EveryObject_(result, client, message);

      client->SetIPAddress(_T("192.0.2.7"));
      client->SetPort(25);
      client->SetUsername(_T("ada@example.test"));
      client->SetIsAuthenticated(true);
      client->SetHELO(_T("mx.example.test"));
      client->SetSessionID(41);

      // The values after the objects, exactly as given: nothing is quoted, so a
      // quote, a backslash and a line break arrive as themselves.
      String awkward = _T("it's \"quoted\" \\ and\r\nmulti-line \u00e9");
      container->AddTextArgument(awkward);
      container->AddNumberArgument(5019);

      String script =
         _T("function Values(client, message, text, number) {\n")
         _T("   Result.Value = 2;\n")
         _T("   Result.Message = 'refused: ' + client.Username;\n")
         _T("   Result.Parameter = 7;\n")
         _T("   return [client === HMAILSERVER_CLIENT, message, client.IPAddress, client.Port, client.Authenticated, client.HELO,\n")
         _T("      client.SessionID, typeof number, number, Result.Value, Result.Parameter, text].join('|');\n")
         _T("}\n");

      std::vector<String> objectArguments;
      objectArguments.push_back(_T("HMAILSERVER_CLIENT"));
      objectArguments.push_back(_T("NO_SUCH_OBJECT"));

      String answer = Answer_(script, _T("Values"), objectArguments, container, "the values handler");

      Expect_(answer == String(_T("true||192.0.2.7|25|true|mx.example.test|41|number|5019|2|7|")) + awkward,
         "the handler did not read what it was given: " + Narrow_(answer));

      Expect_(result->GetValue() == 2, "Result.Value did not reach the C++ Result");
      Expect_(result->GetParameter() == 7, "Result.Parameter did not reach the C++ Result");
      Expect_(result->GetMessage() == String(_T("refused: ada@example.test")), "Result.Message did not reach the C++ Result");

      // What is not a whole number is refused with a TypeError and leaves Result
      // as it was - the 2 above - rather than becoming the 0 that lets a client in
      // without its password. A whole number written as 1.0 is a whole number.
      String refusals =
         _T("function Refused() {\n")
         _T("   var answers = [];\n")
         _T("   for (var value of ['deny', '0', NaN, null, undefined, true, false, {}, [], 1.5, Infinity, 2147483648]) {\n")
         _T("      try { Result.Value = value; answers.push('taken:' + String(value)); } catch (e) { answers.push(e.name); }\n")
         _T("   }\n")
         _T("   try { Result.Parameter = 'five'; answers.push('parameter taken'); } catch (e) { answers.push(e.name); }\n")
         _T("   answers.push(Result.Value, Result.Parameter);\n")
         _T("   Result.Value = 1.0;\n")
         _T("   answers.push(Result.Value);\n")
         _T("   return answers.join(',');\n")
         _T("}\n");

      result->SetValue(2);
      answer = Answer_(refusals, _T("Refused"), std::vector<String>(), container, "the Result refusals handler");
      Expect_(answer == String(_T("TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,TypeError,2,7,1")),
         "Result took something that is not a whole number: " + Narrow_(answer));
      Expect_(result->GetValue() == 1, "Result.Value 1.0 did not reach the C++ Result as 1");

      // The recipients are the message's own.
      String recipients =
         _T("function Recipients(message) {\n")
         _T("   var r = message.Recipients;\n")
         _T("   var first = r.Item(0);\n")
         _T("   var outside;\n")
         _T("   try { r.Item(1); outside = 'taken'; } catch (e) { outside = e.name; }\n")
         _T("   return [r.Count, first.Address, first.OriginalAddress, first.IsLocalUser, outside].join('|');\n")
         _T("}\n");

      std::vector<String> messageArgument;
      messageArgument.push_back(_T("HMAILSERVER_MESSAGE"));
      answer = Answer_(recipients, _T("Recipients"), messageArgument, container, "the recipients handler");
      Expect_(answer == String(_T("1|grace@example.test|hopper@example.test|false|RangeError")), "Message.Recipients did not read the recipients: " + Narrow_(answer));
   }

   void
   ScriptBindingTester::TestLimits_()
   {
      std::vector<String> none;
      std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);

      // A loop, and a loop that tries to catch its own interruption: both stop at
      // the deadline.
      auto started = std::chrono::steady_clock::now();
      ScriptRunOutcome outcome = ScriptBinding::CallHandler(_T("function Loop() { for (;;) { try { for (;;) {} } catch (e) {} } }"), _T("EventHandlers.js"), _T("Loop"), none, container, 1);
      auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
      Expect_(outcome.interrupted && !outcome.completed, "a loop was not interrupted at the timeout");
      Expect_(seconds < 10, "the interrupted loop took " + std::to_string(seconds) + " seconds to stop");

      // Catastrophic backtracking: the regular expression matcher polls the same
      // handler.
      outcome = ScriptBinding::CallHandler(_T("function Backtrack() { return /^(a+)+$/.test('a'.repeat(40) + 'b') ? 'matched' : 'no'; }"), _T("EventHandlers.js"), _T("Backtrack"), none, container, 1);
      Expect_(outcome.interrupted && !outcome.completed, "a runaway regular expression was not interrupted");

      // The top level is under the deadline too.
      outcome = ScriptBinding::RunScript(_T("for (;;) {}"), _T("EventHandlers.js"), 1);
      Expect_(outcome.interrupted && !outcome.completed, "a top level that never ends was not interrupted");

      // The memory ceiling.
      outcome = ScriptBinding::CallHandler(_T("function Grow() { var kept = []; for (;;) kept.push('x'.repeat(4096) + kept.length); }"), _T("EventHandlers.js"), _T("Grow"), none, container, 60);
      Expect_(outcome.out_of_memory && !outcome.completed, "an allocation past the ceiling was not stopped: " + Narrow_(outcome.error));

      // The stack: a runaway recursion is an error, not the thread's end - an
      // InternalError "stack overflow" in this engine (JS_ThrowStackOverflow),
      // where QuickJS-ng and the browsers throw a RangeError. EventScripts.md says
      // which, because a handler that catches it by type has to name the right one.
      outcome = ScriptBinding::CallHandler(_T("function Deep(n) { return Deep(n + 1) + 1; }\nfunction Recurse() { return Deep(0); }"), _T("EventHandlers.js"), _T("Recurse"), none, container, 30);
      Expect_(!outcome.completed && Contains_(outcome.error, _T("InternalError")) && Contains_(outcome.error, _T("stack overflow")),
         "a runaway recursion was not stopped as an InternalError stack overflow: " + Narrow_(outcome.error));

      // With the limit off (ScriptTimeout=0) a script that ends, ends.
      outcome = ScriptBinding::RunScript(_T("var total = 0; for (var i = 0; i < 1000; i++) total += i;"), _T("EventHandlers.js"), 0);
      Expect_(outcome.completed, "a script with no timeout did not complete");
   }

   void
   ScriptBindingTester::TestSandbox_()
   {
      std::vector<String> none;
      std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);

      // No std, no os, no host helpers; import() is refused and a static import
      // is not even global code.
      String script =
         _T("var imported = 'pending';\n")
         _T("import('os').then(function () { imported = 'loaded'; }, function (e) { imported = 'refused'; });\n")
         _T("function Globals() {\n")
         _T("   return [typeof std, typeof os, typeof require, typeof process, typeof print, typeof console, typeof scriptArgs, imported].join(',');\n")
         _T("}\n");

      String answer = Answer_(script, _T("Globals"), none, container, "the sandbox probe");
      Expect_(answer == String(_T("undefined,undefined,undefined,undefined,undefined,undefined,undefined,refused")),
         "the script could reach something outside the sandbox: " + Narrow_(answer));

      ScriptRunOutcome outcome = ScriptBinding::RunScript(_T("import * as os from 'os';"), _T("EventHandlers.js"), 5);
      Expect_(!outcome.completed && Contains_(outcome.error, _T("SyntaxError")), "a static import was not refused");

      // Atomics.wait may not block a server thread.
      answer = Answer_(_T("function Wait() { try { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10); return 'blocked'; } catch (e) { return e.message; } }"),
         _T("Wait"), none, container, "the Atomics.wait probe");
      Expect_(Contains_(answer, _T("block")), "Atomics.wait was allowed to block: " + Narrow_(answer));

      // CreateObject and new ActiveXObject say why they cannot work.
      answer = Answer_(_T("function Com() { var a, b; try { CreateObject('WScript.Shell'); a = 'made'; } catch (e) { a = e.message; } try { new ActiveXObject('Scripting.FileSystemObject'); b = 'made'; } catch (e) { b = e.message; } return a + '|' + b; }"),
         _T("Com"), none, container, "the CreateObject probe");
      Expect_(Contains_(answer, _T("not available to JavaScript event scripts")) && !Contains_(answer, _T("made")),
         "CreateObject or ActiveXObject did not refuse clearly: " + Narrow_(answer));
   }

   void
   ScriptBindingTester::TestErrors_()
   {
      std::vector<String> none;
      std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);

      // A syntax error names its line.
      ScriptRunOutcome outcome = ScriptBinding::RunScript(_T("function OnAcceptMessage(client, message) {\n   var first = 1;\n   var second = ;\n}\n"), _T("EventHandlers.js"), 5);
      Expect_(!outcome.completed && outcome.error_line == 3, "a syntax error on line 3 was reported on line " + std::to_string(outcome.error_line));
      Expect_(Contains_(outcome.error, _T("SyntaxError")) && Contains_(outcome.error, _T("Line: 3")), "the syntax error's line was not in the report: " + Narrow_(outcome.error));

      // A runtime error carries its line and its stack.
      outcome = ScriptBinding::CallHandler(_T("function Throws() {\n   var nothing = null;\n   return nothing.property;\n}\n"), _T("EventHandlers.js"), _T("Throws"), none, container, 5);
      Expect_(!outcome.completed && outcome.function_exists, "a throwing handler completed");
      Expect_(Contains_(outcome.error, _T("TypeError")) && Contains_(outcome.error, _T("Line: 3")) && Contains_(outcome.error, _T("Throws")),
         "the runtime error lacked its type, its line or its stack: " + Narrow_(outcome.error));

      // A handler that is not there.
      outcome = ScriptBinding::CallHandler(_T("function Other() {}"), _T("EventHandlers.js"), _T("Missing"), none, container, 5);
      Expect_(!outcome.completed && !outcome.function_exists && Contains_(outcome.error, _T("Missing")), "a missing handler was not reported");

      // A rejected async handler is an error like any other.
      outcome = ScriptBinding::CallHandler(_T("async function Later() { await null; throw new Error('late failure'); }"), _T("EventHandlers.js"), _T("Later"), none, container, 5);
      Expect_(!outcome.completed && Contains_(outcome.error, _T("late failure")), "a rejected async handler was not reported: " + Narrow_(outcome.error));

      // Handlers are found however they are declared.
      outcome = ScriptBinding::FindFunction(_T("const OnHELO = (client) => {};\nlet OnClientLogon = function (client) {};\nvar OnError = 5;"), _T("EventHandlers.js"), _T("OnHELO"), 5);
      Expect_(outcome.completed && outcome.function_exists, "a handler declared with const was not found");
      outcome = ScriptBinding::FindFunction(_T("const OnHELO = (client) => {};\nlet OnClientLogon = function (client) {};\nvar OnError = 5;"), _T("EventHandlers.js"), _T("OnError"), 5);
      Expect_(outcome.completed && !outcome.function_exists, "a var holding a number was taken for a handler");
      outcome = ScriptBinding::FindFunction(_T("function OnHELO(client) {}"), _T("EventHandlers.js"), _T("class"), 5);
      Expect_(outcome.completed && !outcome.function_exists, "a reserved word was taken for a handler");

      // A search whose run of the top level throws has not answered the question -
      // ScriptServer refuses the load on it rather than register the script with
      // the handler switched off - so it is not completed, and it says why.
      outcome = ScriptBinding::FindFunction(_T("function OnHELO(client) {}\nthrow new Error('the top level failed on this run');"), _T("EventHandlers.js"), _T("OnHELO"), 5);
      Expect_(!outcome.completed && !outcome.interrupted && Contains_(outcome.error, _T("the top level failed on this run")),
         "a search whose top level threw was reported as complete: " + Narrow_(outcome.error));
   }

   void
   ScriptBindingTester::TestLanguage_()
   {
      std::vector<String> none;
      std::shared_ptr<ScriptObjectContainer> container = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);

      // ES2023, and the years before it that JScript never had: a class with a
      // private field, template literals, optional chaining, nullish coalescing,
      // arrows, spread, findLast, at, Object.hasOwn, toReversed.
      String script =
         _T("class Greeter {\n")
         _T("   #name;\n")
         _T("   constructor(name) { this.#name = name; }\n")
         _T("   get greeting() { return `hello ${this.#name}`; }\n")
         _T("}\n")
         _T("const pick = (object) => object?.inner?.value ?? 'none';\n")
         _T("function Modern() {\n")
         _T("   const list = [1, 2, 3, 4];\n")
         _T("   return [new Greeter('ada').greeting, pick({ inner: { value: 'v' } }), pick(null), list.findLast((x) => x % 2 === 1),\n")
         _T("      list.at(-1), Object.hasOwn({ a: 1 }, 'a'), [...'abc'].toReversed().join('')].join(',');\n")
         _T("}\n");

      String answer = Answer_(script, _T("Modern"), none, container, "the ES2023 handler");
      Expect_(answer == String(_T("hello ada,v,none,3,4,true,cba")), "the ES2023 handler answered " + Narrow_(answer));
   }
}

#endif
