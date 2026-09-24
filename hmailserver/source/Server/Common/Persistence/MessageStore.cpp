// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStore.h"
#include "PersistentMessage.h"

#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../Mime/Mime.h"
#include "../Util/ByteBuffer.h"
#include "../Util/File.h"
#include "../Util/GUIDCreator.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // What the reader asks the backend for at a time, and what ReadLine keeps
      // ahead of the caller.
      const int ReadAheadBytes = 65536;

      // File::ReadLine reads into a buffer of 10000 with fgets, which keeps one
      // for the terminating NUL.
      const size_t LongestLine = 9999;

      class LocalFileStream : public MessageStoreBackendStream
      {
      public:

         LocalFileStream() :
            size_(0)
         {
         }

         bool Open(const String &name)
         {
            if (!FileUtilities::FileSize64(name, size_))
               return false;

            try
            {
               return file_.Open(name, File::OTReadOnly);
            }
            catch (...)
            {
               return false;
            }
         }

         unsigned __int64 GetSize() override
         {
            return size_;
         }

         bool SetPosition(unsigned __int64 position) override
         {
            // File positions are an int, as File::GetSize's are; a message
            // past 2 GiB cannot be reached here any more than it could by the
            // File every caller used before.
            if (position > (unsigned __int64) INT_MAX)
               return false;

            try
            {
               return file_.SetPosition((int) position);
            }
            catch (...)
            {
               return false;
            }
         }

         std::shared_ptr<ByteBuffer> ReadChunk(int maxBytes) override
         {
            try
            {
               return file_.ReadChunk(maxBytes);
            }
            catch (...)
            {
               // File::ReadChunk has reported it (HM4208) before throwing.
               return std::shared_ptr<ByteBuffer>();
            }
         }

      private:

         File file_;
         unsigned __int64 size_;
      };

      // The local file system: the one backend there is.
      class LocalFileBackend : public MessageStoreBackend
      {
      public:

         bool Exists(const String &name) override
         {
            return FileUtilities::Exists(name);
         }

         bool Size(const String &name, unsigned __int64 &size) override
         {
            return FileUtilities::FileSize64(name, size);
         }

         std::shared_ptr<MessageStoreBackendStream> OpenRead(const String &name) override
         {
            std::shared_ptr<LocalFileStream> stream = std::shared_ptr<LocalFileStream>(new LocalFileStream());
            if (!stream->Open(name))
               return std::shared_ptr<MessageStoreBackendStream>();

            return stream;
         }

         bool ReadAll(const String &name, AnsiString &bytes, unsigned __int64 maxBytes) override
         {
            bytes.Empty();

            std::shared_ptr<MessageStoreBackendStream> stream = OpenRead(name);
            if (!stream)
               return false;

            unsigned __int64 wanted = stream->GetSize();
            if (maxBytes > 0 && wanted > maxBytes)
               wanted = maxBytes;

            if (wanted > (unsigned __int64) INT_MAX)
               return false;

            if (wanted == 0)
               return true;

            std::shared_ptr<ByteBuffer> buffer = stream->ReadChunk((int) wanted);
            if (!buffer)
               return false;

            bytes = AnsiString(buffer->GetCharBuffer(), buffer->GetSize());
            return true;
         }

         bool WriteAtomically(const String &name, const AnsiString &bytes) override
         {
            if (!MakeDirectoryFor_(name))
               return false;

            const String temporary = name + _T(".writing-") + GUIDCreator::GetGUID();

            bool written = false;
            {
               File file;
               try
               {
                  written = file.Open(temporary, File::OTCreate) &&
                            file.Write(bytes) &&
                            (!IniFileSettings::Instance()->GetMessageStoreFsync() || file.FlushToDisk());
               }
               catch (...)
               {
                  written = false;
               }
            }

            if (!written || !FileUtilities::Move(temporary, name))
            {
               FileUtilities::DeleteFile(temporary);
               return false;
            }

            return true;
         }

         bool Copy(const String &from, const String &to) override
         {
            return FileUtilities::Copy(from, to, true);
         }

         bool Move(const String &from, const String &to) override
         {
            if (!MakeDirectoryFor_(to))
               return false;

            return FileUtilities::Move(from, to);
         }

         bool Link(const String &existing, const String &link) override
         {
            if (!MakeDirectoryFor_(link))
               return false;

#ifdef HM_PLATFORM_POSIX
            // link(2) names the EXISTING file first and answers 0 for success.
            const AnsiString narrowExisting = existing.c_str();
            const AnsiString narrowLink = link.c_str();
            return ::link(narrowExisting.c_str(), narrowLink.c_str()) == 0;
#else
            return ::CreateHardLink(link, existing, NULL) != FALSE;
#endif
         }

         bool Remove(const String &name) override
         {
            return FileUtilities::DeleteFile(name);
         }

      private:

         bool MakeDirectoryFor_(const String &name)
         {
            const String directory = FileUtilities::GetFilePath(name);
            if (directory.IsEmpty() || FileUtilities::Exists(directory))
               return true;

            return FileUtilities::CreateDirectory(directory);
         }
      };
   }

   // ---- MessageStoreReader ---------------------------------------------------------

   MessageStoreReader::MessageStoreReader() :
      size_(0),
      position_(0),
      pending_offset_(0),
      open_(false)
   {
   }

   MessageStoreReader::~MessageStoreReader()
   {
      Close();
   }

   bool
   MessageStoreReader::Open(const String &messageFile)
   {
      Close();

      stream_ = MessageStore::Backend().OpenRead(messageFile);
      if (!stream_)
         return false;

      name_ = messageFile;
      size_ = stream_->GetSize();
      open_ = true;
      return true;
   }

   bool
   MessageStoreReader::Open(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      return Open(PersistentMessage::GetFileName(account, message));
   }

   bool
   MessageStoreReader::IsOpen() const
   {
      return open_;
   }

   void
   MessageStoreReader::Close()
   {
      stream_.reset();
      pending_.Empty();
      pending_offset_ = 0;
      position_ = 0;
      size_ = 0;
      open_ = false;
   }

   unsigned __int64
   MessageStoreReader::GetSize() const
   {
      return size_;
   }

   bool
   MessageStoreReader::SetPosition(unsigned __int64 position)
   {
      if (!open_)
         throw std::logic_error("Attempt to set position on file which has not been opened.");

      pending_.Empty();
      pending_offset_ = 0;

      if (!stream_->SetPosition(position))
         return false;

      position_ = position;
      return true;
   }

   bool
   MessageStoreReader::Fill_()
   {
      // The next bytes after position_, when none are pending. False at the
      // end and on a failed read, which ReadLine answers as fgets does.
      pending_.Empty();
      pending_offset_ = 0;

      std::shared_ptr<ByteBuffer> chunk = stream_->ReadChunk(ReadAheadBytes);
      if (!chunk || chunk->GetSize() == 0)
         return false;

      pending_ = AnsiString(chunk->GetCharBuffer(), chunk->GetSize());
      return true;
   }

   std::shared_ptr<ByteBuffer>
   MessageStoreReader::ReadChunk(int maxBytes)
   {
      if (!open_)
         throw std::logic_error("Attempt to read from file which has not been opened.");

      if (maxBytes < 0)
         throw std::runtime_error(Formatter::FormatAsAnsi("Refusing to read a negative number of bytes ({0}) from file {1}.", maxBytes, name_));

      std::shared_ptr<ByteBuffer> result = std::shared_ptr<ByteBuffer>(new ByteBuffer);

      size_t wanted = (size_t) maxBytes;

      // What ReadLine read ahead comes first: those bytes are the next ones.
      if (pending_offset_ < pending_.size() && wanted > 0)
      {
         const size_t available = pending_.size() - pending_offset_;
         const size_t taken = std::min(available, wanted);

         result->Add((const BYTE *) pending_.c_str() + pending_offset_, taken);
         pending_offset_ += taken;
         position_ += taken;
         wanted -= taken;
      }

      if (wanted > 0)
      {
         std::shared_ptr<ByteBuffer> rest = stream_->ReadChunk((int) wanted);
         if (!rest)
            throw std::runtime_error(Formatter::FormatAsAnsi("Unable to read file {0}.", name_));

         if (rest->GetSize() > 0)
         {
            result->Add(rest->GetBuffer(), rest->GetSize());
            position_ += rest->GetSize();
         }
      }

      return result;
   }

   bool
   MessageStoreReader::ReadLine(AnsiString &line)
   {
      if (!open_)
         throw std::logic_error("Attempt to read to file which has not been opened.");

      std::string collected;
      bool newlineFound = false;

      while (collected.size() < LongestLine && !newlineFound)
      {
         if (pending_offset_ >= pending_.size() && !Fill_())
            break;

         const char *start = pending_.c_str() + pending_offset_;
         size_t taken = std::min(pending_.size() - pending_offset_, LongestLine - collected.size());

         const void *newline = memchr(start, '\n', taken);
         if (newline != nullptr)
         {
            taken = (size_t) ((const char *) newline - start) + 1;
            newlineFound = true;
         }

         collected.append(start, taken);
         pending_offset_ += taken;
         position_ += taken;
      }

      if (collected.empty())
      {
         line.Empty();
         return false;
      }

      // fgets fills the buffer and File::ReadLine's ReleaseBuffer measures it
      // with strlen, so a NUL in a line ends its text there.
      const size_t nul = collected.find('\0');
      if (nul != std::string::npos)
         collected.resize(nul);

      line = AnsiString(collected.c_str(), collected.size());
      return true;
   }

   // ---- MessageStore -----------------------------------------------------------------

   MessageStoreBackend &
   MessageStore::Backend()
   {
      static LocalFileBackend backend;
      return backend;
   }

   String
   MessageStore::WorkingFile(std::shared_ptr<const Message> message)
   {
      return PersistentMessage::GetFileName(message);
   }

   String
   MessageStore::WorkingFile(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      return PersistentMessage::GetFileName(account, message);
   }

   bool
   MessageStore::Exists(const String &messageFile)
   {
      return Backend().Exists(messageFile);
   }

   bool
   MessageStore::FileSize64(const String &messageFile, unsigned __int64 &size)
   {
      return Backend().Size(messageFile, size);
   }

   bool
   MessageStore::StoredFileSize64(const String &messageFile, unsigned __int64 &size)
   {
      return Backend().Size(messageFile, size);
   }

   long
   MessageStore::FileSize(const String &messageFile)
   {
      unsigned __int64 size = 0;
      if (!FileSize64(messageFile, size))
         return 0;

      // Saturated, never truncated: FileUtilities::FileSize says why.
      if (size > (unsigned __int64) LONG_MAX)
         return LONG_MAX;

      return (long) size;
   }

   bool
   MessageStore::ReadAll(const String &messageFile, AnsiString &bytes)
   {
      return Backend().ReadAll(messageFile, bytes, 0);
   }

   void
   MessageStore::ReadRange(const String &messageFile, BYTE *buffer, int start, int count)
   {
      // FileUtilities::ReadFileToBuf's contract, which IMAPFetch was written
      // against: nothing asked for is nothing done, a negative start is the
      // beginning, and a file that cannot be opened or positioned throws.
      if (count <= 0)
         return;

      if (start < 0)
         start = 0;

      MessageStoreReader reader;
      if (!reader.Open(messageFile))
         throw std::logic_error(Formatter::FormatAsAnsi("Unable to open file {0}", messageFile));

      if (!reader.SetPosition((unsigned __int64) start))
         throw std::runtime_error(Formatter::FormatAsAnsi("Unable to seek to offset {0} in file {1}", start, messageFile));

      std::shared_ptr<ByteBuffer> bytes = reader.ReadChunk(count);

      const size_t bytesRead = bytes->GetSize();
      const size_t requested = (size_t) count;

      if (bytesRead > 0)
         memcpy(buffer, bytes->GetBuffer(), bytesRead < requested ? bytesRead : requested);

      if (bytesRead < requested)
         memset(buffer + bytesRead, 0, requested - bytesRead);
   }

   MimeLoadResult
   MessageStore::LoadMime(const AnsiString &messageFile, MimeBody &body)
   {
      return body.LoadFromFile(messageFile);
   }

   String
   MessageStore::PlainFileFor(const String &messageFile, bool &temporary)
   {
      temporary = false;
      return messageFile;
   }

   bool
   MessageStore::CopyStored(const String &from, const String &to)
   {
      return Backend().Copy(from, to);
   }

   bool
   MessageStore::CopyOriginal(const String &from, const String &to)
   {
      return Backend().Copy(from, to);
   }

   bool
   MessageStore::Rename(const String &from, const String &to)
   {
      return Backend().Move(from, to);
   }

   bool
   MessageStore::Link(const String &existing, const String &link)
   {
      return Backend().Link(existing, link);
   }

   bool
   MessageStore::Remove(const String &messageFile)
   {
      return Backend().Remove(messageFile);
   }

   AnsiString
   MessageStore::LoadHeader(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, bool reportError)
   {
      return PersistentMessage::LoadHeader(PersistentMessage::GetFileName(account, message), reportError);
   }

   long
   MessageStore::Size(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      return FileSize(PersistentMessage::GetFileName(account, message));
   }

   __int64
   MessageStore::StoredSize(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      unsigned __int64 size = 0;
      if (!StoredFileSize64(PersistentMessage::GetFileName(account, message), size))
         return 0;

      return (__int64) size;
   }

   bool
   MessageStore::Delete(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      return Remove(PersistentMessage::GetFileName(account, message));
   }

   bool
   MessageStore::Place(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message)
   {
      // The working file IS the stored file while the store has one form.
      return true;
   }
}
