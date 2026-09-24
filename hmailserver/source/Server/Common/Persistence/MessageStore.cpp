// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStore.h"
#include "MessageStoreBlobs.h"
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
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>

#ifdef HM_PLATFORM_POSIX
#include <sys/stat.h>
#endif

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

      // How much of a container's message LoadHeader decodes to find the end of
      // the header: far more than any header, and bounded.
      const size_t LongestHeader = 1024 * 1024;

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

         bool Identity(const String &name, AnsiString &identity, unsigned int &names) override
         {
            identity.Empty();
            names = 0;

#ifdef HM_PLATFORM_POSIX
            const AnsiString narrow = name.c_str();
            struct stat status;
            if (::stat(narrow.c_str(), &status) != 0)
               return false;

            identity = AnsiString(std::to_string((unsigned long long) status.st_dev) + ":" + std::to_string((unsigned long long) status.st_ino));
            names = (unsigned int) status.st_nlink;
            return true;
#else
            HANDLE handle = ::CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (handle == INVALID_HANDLE_VALUE)
               return false;

            BY_HANDLE_FILE_INFORMATION information;
            memset(&information, 0, sizeof(information));
            const BOOL answered = ::GetFileInformationByHandle(handle, &information);
            ::CloseHandle(handle);

            if (!answered)
               return false;

            identity = AnsiString(std::to_string((unsigned long long) information.dwVolumeSerialNumber) + ":" +
                                  std::to_string((unsigned long long) information.nFileIndexHigh) + ":" +
                                  std::to_string((unsigned long long) information.nFileIndexLow));
            names = (unsigned int) information.nNumberOfLinks;
            return true;
#endif
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

      // A file directly in the data directory: a queue file, where
      // PersistentMessage::GetFileName puts a message with no account and no
      // folder. The data directory's trailing separators are trimmed when it is
      // read, and a queue file's name is that directory, a separator and the
      // partial name, so the file's directory is the data directory exactly.
      bool InQueueDirectory(const String &name)
      {
         String directory = FileUtilities::GetFilePath(name);
         String data = IniFileSettings::Instance()->GetDataDirectory();

         while (directory.EndsWith(_T("\\")) || directory.EndsWith(_T("/")))
            directory = directory.Left(directory.GetLength() - 1);
         while (data.EndsWith(_T("\\")) || data.EndsWith(_T("/")))
            data = data.Left(data.GetLength() - 1);

         if (directory.IsEmpty() || data.IsEmpty())
            return false;

#ifdef HM_PLATFORM_POSIX
         return directory == data;
#else
         return directory.CompareNoCase(data) == 0;
#endif
      }

      // The header of a stored file. isContainer says which kind it is; false
      // is answered for a file that cannot be read at all, and for a container
      // whose header is not one this server reads (isContainer set, and the
      // reason in problem). A queue file is never a container, whatever it
      // begins with (MessageStore::MayHoldContainer).
      bool ReadStoredHeader(const String &name, bool &isContainer, MessageStoreHeader &header, String &problem)
      {
         isContainer = false;

         std::shared_ptr<MessageStoreBackendStream> stream = MessageStore::Backend().OpenRead(name);
         if (!stream)
         {
            problem = _T("the file is missing or cannot be opened");
            return false;
         }

         std::shared_ptr<ByteBuffer> start = stream->ReadChunk(16);
         if (!start)
         {
            problem = _T("the file cannot be read");
            return false;
         }

         if (!MessageStoreFormat::HasMagic(start->GetCharBuffer(), start->GetSize()) || !MessageStore::MayHoldContainer(name))
            return true;

         isContainer = true;

         // Bounded before anything more is read: a header past the longest one
         // there can be is damage, and is not read to find that out.
         const unsigned int headerLength = MessageStoreFormat::HeaderLengthOf(start->GetCharBuffer(), start->GetSize());
         if (headerLength < (unsigned int) MessageStoreFormat::FixedHeaderLength ||
             headerLength > (unsigned int) MessageStoreFormat::MaximumHeaderLength ||
             (unsigned __int64) headerLength > stream->GetSize())
         {
            problem = _T("it is cut short inside its header, or its header's length is not a header's");
            return false;
         }

         std::string bytes(start->GetCharBuffer(), start->GetSize());
         if (bytes.size() < headerLength)
         {
            std::shared_ptr<ByteBuffer> rest = stream->ReadChunk((int) (headerLength - bytes.size()));
            if (!rest)
            {
               problem = _T("the file cannot be read");
               return false;
            }
            bytes.append(rest->GetCharBuffer(), rest->GetSize());
         }

         return MessageStoreFormat::ParseHeader(bytes, header, problem);
      }

      // The original bytes of a stored file of either kind, read from the file.
      bool ReadOriginalOf(const String &name, std::string &original, bool &isContainer, String &problem)
      {
         original.clear();
         isContainer = false;

         AnsiString bytes;
         if (!MessageStore::Backend().ReadAll(name, bytes, 0))
         {
            problem = _T("the file is missing or cannot be read");
            return false;
         }

         if (!MessageStoreFormat::HasMagic(bytes.data(), bytes.size()) || !MessageStore::MayHoldContainer(name))
         {
            original.assign(bytes.data(), bytes.size());
            return true;
         }

         isContainer = true;

         MessageStoreHeader header;
         const MessageStoreFormat::BlobReader blob = [] (const MessageStoreReference &reference, std::string &part, String &why)
         {
            return MessageStoreBlobs::Read(reference, part, why);
         };

         return MessageStoreFormat::Decode(std::string(bytes.data(), bytes.size()), blob, original, header, problem);
      }

      // The last containers decoded. A client that reads a message in pieces -
      // BODY[]<start.count>, which Thunderbird and Apple Mail use - or asks for
      // its structure and then its parts would otherwise have it decoded whole,
      // every blob read and hashed and the SHA-256 of the whole taken, once a
      // piece. Keyed by the name, by which file the name is (a rewrite is a new
      // file under the name, so it is never served from here) and by its stored
      // size; at most eight entries and 96 MB, and an entry nobody has asked
      // for in half a minute goes. What is kept here was checked whole when it
      // was decoded.
      class DecodedCache
      {
      public:

         std::shared_ptr<const std::string> Find(const String &name, const AnsiString &identity, unsigned __int64 storedSize)
         {
            boost::mutex::scoped_lock lock(lock_);

            const __int64 now = (__int64) time(nullptr);
            Expire_(now);

            auto found = entries_.find(Key_(name));
            if (found == entries_.end() || found->second.identity != identity || found->second.stored_size != storedSize)
               return std::shared_ptr<const std::string>();

            found->second.used = now;
            return found->second.original;
         }

         void Keep(const String &name, const AnsiString &identity, unsigned __int64 storedSize, const std::shared_ptr<const std::string> &original)
         {
            if (!original || original->size() > MaximumBytes / 2)
               return;

            boost::mutex::scoped_lock lock(lock_);

            const __int64 now = (__int64) time(nullptr);
            const std::wstring key = Key_(name);

            auto existing = entries_.find(key);
            if (existing != entries_.end())
            {
               bytes_ -= existing->second.original->size();
               entries_.erase(existing);
            }

            Entry kept;
            kept.identity = identity;
            kept.stored_size = storedSize;
            kept.original = original;
            kept.used = now;
            entries_[key] = kept;
            bytes_ += original->size();

            Expire_(now);

            // The least recently asked for go until the rest fit.
            while (!entries_.empty() && (entries_.size() > MaximumEntries || bytes_ > MaximumBytes))
            {
               auto oldest = entries_.begin();
               for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate)
               {
                  if (candidate->second.used < oldest->second.used)
                     oldest = candidate;
               }

               bytes_ -= oldest->second.original->size();
               entries_.erase(oldest);
            }
         }

      private:

         struct Entry
         {
            Entry() : stored_size(0), used(0) {}
            AnsiString identity;
            unsigned __int64 stored_size;
            std::shared_ptr<const std::string> original;
            __int64 used;
         };

         static const size_t MaximumEntries = 8;
         static const size_t MaximumBytes = 96 * 1024 * 1024;
         static const __int64 IdleSeconds = 30;

         static std::wstring Key_(const String &name)
         {
            return std::wstring(name.c_str());
         }

         void Expire_(__int64 now)
         {
            for (auto candidate = entries_.begin(); candidate != entries_.end(); )
            {
               if (now - candidate->second.used > IdleSeconds || now < candidate->second.used)
               {
                  bytes_ -= candidate->second.original->size();
                  candidate = entries_.erase(candidate);
               }
               else
                  ++candidate;
            }
         }

         boost::mutex lock_;
         std::map<std::wstring, Entry> entries_;
         size_t bytes_ = 0;
      };

      DecodedCache &Decodes()
      {
         static DecodedCache cache;
         return cache;
      }

      // A container's original bytes, decoded whole and checked - or from the
      // last decodes, when the same file was decoded a moment ago. For a name
      // already found to begin with the magic where a container may be.
      bool DecodedContainer(const String &name, std::shared_ptr<const std::string> &original, String &problem)
      {
         original.reset();

         AnsiString identity;
         unsigned int names = 0;
         unsigned __int64 storedSize = 0;
         const bool keyed = MessageStore::Backend().Identity(name, identity, names) && MessageStore::Backend().Size(name, storedSize);

         if (keyed)
         {
            original = Decodes().Find(name, identity, storedSize);
            if (original)
               return true;
         }

         std::shared_ptr<std::string> decoded = std::shared_ptr<std::string>(new std::string);
         bool isContainer = false;
         if (!ReadOriginalOf(name, *decoded, isContainer, problem))
            return false;

         original = decoded;

         if (keyed && isContainer)
            Decodes().Keep(name, identity, storedSize, original);

         return true;
      }

      std::vector<MessageStoreReference> ReferencesOf(const String &name)
      {
         bool isContainer = false;
         MessageStoreHeader header;
         String problem;
         if (ReadStoredHeader(name, isContainer, header, problem) && isContainer)
            return header.references;

         return std::vector<MessageStoreReference>();
      }

      void ReportStoreError(int code, const String &where, const String &text)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, where, text);
      }
   }

   // ---- MessageStoreReader ---------------------------------------------------------

   MessageStoreReader::MessageStoreReader() :
      in_memory_(false),
      size_(0),
      position_(0),
      source_position_(0),
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

      std::shared_ptr<MessageStoreBackendStream> stream = MessageStore::Backend().OpenRead(messageFile);
      if (!stream)
         return false;

      std::shared_ptr<ByteBuffer> start = stream->ReadChunk((int) MessageStoreFormat::MagicLength);
      if (!start)
         return false;

      if (MessageStoreFormat::HasMagic(start->GetCharBuffer(), start->GetSize()) && MessageStore::MayHoldContainer(messageFile))
      {
         // A container is decoded whole and checked before a byte of it is
         // handed out: a message that is not what it claims is refused here,
         // never streamed half-way. A client reading it in pieces opens it
         // once a piece, and the pieces after the first come from the last
         // decodes.
         stream.reset();

         String problem;
         if (!DecodedContainer(messageFile, decoded_, problem))
         {
            decoded_.reset();
            MessageStore::ReportUnreadable(messageFile, problem);
            return false;
         }

         in_memory_ = true;
         size_ = (unsigned __int64) decoded_->size();
      }
      else
      {
         if (!stream->SetPosition(0))
            return false;

         stream_ = stream;
         size_ = stream_->GetSize();
      }

      name_ = messageFile;
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
      decoded_.reset();
      in_memory_ = false;
      pending_.Empty();
      pending_offset_ = 0;
      position_ = 0;
      source_position_ = 0;
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

      if (!in_memory_ && !stream_->SetPosition(position))
         return false;

      position_ = position;
      source_position_ = position;
      return true;
   }

   std::shared_ptr<ByteBuffer>
   MessageStoreReader::Next_(int maxBytes)
   {
      if (!in_memory_)
      {
         std::shared_ptr<ByteBuffer> chunk = stream_->ReadChunk(maxBytes);
         if (chunk)
            source_position_ += chunk->GetSize();
         return chunk;
      }

      std::shared_ptr<ByteBuffer> chunk = std::shared_ptr<ByteBuffer>(new ByteBuffer);
      if (decoded_ && source_position_ < (unsigned __int64) decoded_->size() && maxBytes > 0)
      {
         const size_t available = decoded_->size() - (size_t) source_position_;
         const size_t taken = std::min(available, (size_t) maxBytes);
         chunk->Add((const BYTE *) decoded_->data() + (size_t) source_position_, taken);
         source_position_ += taken;
      }

      return chunk;
   }

   bool
   MessageStoreReader::Fill_()
   {
      // The next bytes after the ones pending, when none are left. False at the
      // end and on a failed read, which ReadLine answers as fgets does.
      pending_.Empty();
      pending_offset_ = 0;

      std::shared_ptr<ByteBuffer> chunk = Next_(ReadAheadBytes);
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
         std::shared_ptr<ByteBuffer> rest = Next_((int) wanted);
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

   // ---- MessageStorePlainCopy -----------------------------------------------------------

   MessageStorePlainCopy::MessageStorePlainCopy(const String &messageFile) :
      path_(messageFile),
      ready_(true),
      temporary_(false)
   {
      bool isContainer = false;
      MessageStoreHeader header;
      String problem;

      // A file that is not there is a message being composed, which is the
      // caller's to decide; a plain file is itself.
      if (!ReadStoredHeader(messageFile, isContainer, header, problem))
      {
         if (isContainer)
         {
            MessageStore::ReportUnreadable(messageFile, problem);
            ready_ = false;
         }
         return;
      }

      if (!isContainer)
         return;

      bool temporary = false;
      const String plain = MessageStore::PlainFileFor(messageFile, temporary);
      if (plain.IsEmpty())
      {
         ready_ = false;
         return;
      }

      path_ = plain;
      temporary_ = temporary;
      references_ = header.references;
   }

   MessageStorePlainCopy::~MessageStorePlainCopy()
   {
      if (temporary_)
         FileUtilities::DeleteFile(path_);
   }

   void
   MessageStorePlainCopy::StoredFileReplaced()
   {
      if (references_.empty())
         return;

      MessageStoreBlobs::Release(references_);
      references_.clear();
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
   MessageStore::MayHoldContainer(const String &messageFile)
   {
      return !InQueueDirectory(messageFile);
   }

   bool
   MessageStore::DiscardWorkingFile(const String &messageFile)
   {
      // Never placed, so never counted: there is nothing to let go of, and
      // what the file's first bytes claim is not asked.
      return Backend().Remove(messageFile);
   }

   void
   MessageStore::ReportUnreadable(const String &messageFile, const String &problem)
   {
      ReportStoreError(7350, _T("MessageStore"),
         "The stored message " + messageFile + " could not be read: " + problem + ". It is refused rather than read as something it is not, and the file is left as it is.");
   }

   bool
   MessageStore::Exists(const String &messageFile)
   {
      return Backend().Exists(messageFile);
   }

   bool
   MessageStore::FileSize64(const String &messageFile, unsigned __int64 &size)
   {
      size = 0;

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      if (!ReadStoredHeader(messageFile, isContainer, header, problem))
      {
         // A container whose own header is damaged has no size to give: said
         // here, since a caller that sizes before it reads - IMAP's BODY[] -
         // would otherwise ask for nothing and never meet the damage.
         if (isContainer)
            ReportUnreadable(messageFile, problem);
         return false;
      }

      if (!isContainer)
         return Backend().Size(messageFile, size);

      size = header.original_size;
      return true;
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
   MessageStore::ReadOriginal(const String &messageFile, std::string &original, String &problem)
   {
      bool isContainer = false;
      return ReadOriginalOf(messageFile, original, isContainer, problem);
   }

   bool
   MessageStore::ReadAll(const String &messageFile, AnsiString &bytes)
   {
      bytes.Empty();

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      const bool read = ReadStoredHeader(messageFile, isContainer, header, problem);

      if (!isContainer)
         return read && Backend().ReadAll(messageFile, bytes, 0);

      std::shared_ptr<const std::string> original;
      if (!read || !DecodedContainer(messageFile, original, problem))
      {
         ReportUnreadable(messageFile, problem);
         return false;
      }

      bytes = AnsiString(original->data(), original->size());
      return true;
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
      const String name = messageFile;

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      const bool read = ReadStoredHeader(name, isContainer, header, problem);

      // A plain file, or none: LoadFromFile's own answers, OpenFailed among them.
      if (!isContainer)
         return body.LoadFromFile(messageFile);

      std::shared_ptr<const std::string> original;
      if (!read || !DecodedContainer(name, original, problem))
      {
         ReportUnreadable(name, problem);
         return MimeLoadResult::OpenFailed;
      }

      // Parsed from memory, as LoadFromFile parses the buffer it reads: the
      // bytes, with the terminating NUL c_str() guarantees past their end.
      try
      {
         size_t index = 0;
         bool partLoaded = false;
         body.Load(original->c_str(), original->size(), index, partLoaded);
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4228, "MessageStore::LoadMime",
            "An unknown error occurred while parsing the message stored in " + name + ".");
         return MimeLoadResult::ParseFailed;
      }

      return MimeLoadResult::Loaded;
   }

   String
   MessageStore::PlainFileFor(const String &messageFile, bool &temporary)
   {
      temporary = false;

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      const bool read = ReadStoredHeader(messageFile, isContainer, header, problem);
      if (!isContainer)
         return messageFile;

      std::shared_ptr<const std::string> original;
      if (!read || !DecodedContainer(messageFile, original, problem))
      {
         ReportUnreadable(messageFile, problem);
         return String();
      }

      const String copy = FileUtilities::GetTempFileName();
      if (!FileUtilities::WriteToFile(copy, AnsiString(original->data(), original->size())))
      {
         FileUtilities::DeleteFile(copy);
         ReportStoreError(7351, _T("MessageStore::PlainFileFor"),
            "A plain copy of the stored message " + messageFile + " could not be written to " + copy + ".");
         return String();
      }

      temporary = true;
      return copy;
   }

   bool
   MessageStore::CopyStored(const String &from, const String &to)
   {
      // Counted before the copy is made, let go of if it is not: never fewer
      // counts than names (MessageStoreBlobs.h).
      const std::vector<MessageStoreReference> references = ReferencesOf(from);

      String problem;
      if (!references.empty() && !MessageStoreBlobs::AddReferences(references, problem))
      {
         ReportStoreError(7352, _T("MessageStore::CopyStored"),
            "The stored message " + from + " could not be copied to " + to + ": " + problem + ".");
         return false;
      }

      if (!Backend().Copy(from, to))
      {
         MessageStoreBlobs::Release(references);
         return false;
      }

      return true;
   }

   bool
   MessageStore::CopyOriginal(const String &from, const String &to)
   {
      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      const bool read = ReadStoredHeader(from, isContainer, header, problem);
      if (!isContainer)
         return Backend().Copy(from, to);

      std::shared_ptr<const std::string> original;
      if (!read || !DecodedContainer(from, original, problem))
      {
         ReportUnreadable(from, problem);
         return false;
      }

      return Backend().WriteAtomically(to, AnsiString(original->data(), original->size()));
   }

   bool
   MessageStore::Rename(const String &from, const String &to)
   {
      return Backend().Move(from, to);
   }

   bool
   MessageStore::Link(const String &existing, const String &link)
   {
      const std::vector<MessageStoreReference> references = ReferencesOf(existing);

      String problem;
      if (!references.empty() && !MessageStoreBlobs::AddReferences(references, problem))
         return false;

      if (!Backend().Link(existing, link))
      {
         MessageStoreBlobs::Release(references);
         return false;
      }

      return true;
   }

   bool
   MessageStore::Remove(const String &messageFile)
   {
      // Let go of after the name is gone, never before.
      const std::vector<MessageStoreReference> references = ReferencesOf(messageFile);

      if (!Backend().Remove(messageFile))
         return false;

      if (!references.empty())
         MessageStoreBlobs::Release(references);

      return true;
   }

   bool
   MessageStore::SameFile(const String &first, const String &second)
   {
      AnsiString one;
      AnsiString two;
      unsigned int names = 0;
      if (!Backend().Identity(first, one, names) || !Backend().Identity(second, two, names))
         return false;

      return one == two;
   }

   bool
   MessageStore::IsContainer(const String &messageFile)
   {
      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      return ReadStoredHeader(messageFile, isContainer, header, problem) && isContainer;
   }

   bool
   MessageStore::GetReferences(const String &messageFile, std::vector<MessageStoreReference> &references)
   {
      references.clear();

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      if (!ReadStoredHeader(messageFile, isContainer, header, problem))
         return false;

      if (isContainer)
         references = header.references;

      return true;
   }

   AnsiString
   MessageStore::ReadHeaderOfContainer(const String &messageFile, bool reportError)
   {
      std::string start;

      // Decoded whole a moment ago - a client that fetched the message and now
      // asks for its envelope - and checked then, blobs and all.
      AnsiString identity;
      unsigned int names = 0;
      unsigned __int64 storedSize = 0;
      std::shared_ptr<const std::string> decoded;
      if (Backend().Identity(messageFile, identity, names) && Backend().Size(messageFile, storedSize))
         decoded = Decodes().Find(messageFile, identity, storedSize);

      if (decoded)
         start = decoded->substr(0, LongestHeader);
      else
      {
         AnsiString bytes;
         if (!Backend().ReadAll(messageFile, bytes, 0))
            return AnsiString();

         // Every stage undone to its end and checked, and only the start kept:
         // damage anywhere in what the container holds itself is refused here as
         // it is on a read of the whole. The blobs and the SHA-256 of the whole
         // are the whole read's to check (MessageStoreFormat::DecodeStart).
         String problem;
         if (!MessageStoreFormat::DecodeStart(std::string(bytes.data(), bytes.size()), LongestHeader, start, problem))
         {
            if (reportError)
               ReportUnreadable(messageFile, problem);
            return AnsiString();
         }
      }

      // What LoadHeader answers for a plain file: up to and including the
      // first CRLF of the blank line, or all it read when there is none.
      const size_t end = start.find("\r\n\r\n");
      if (end == std::string::npos)
         return AnsiString(start.data(), start.size());

      return AnsiString(start.data(), end + 2);
   }

   AnsiString
   MessageStore::ReadBodyOfContainer(const String &messageFile)
   {
      std::shared_ptr<const std::string> original;
      String problem;
      if (!DecodedContainer(messageFile, original, problem))
      {
         ReportUnreadable(messageFile, problem);
         return AnsiString();
      }

      // What LoadBody answers for a plain file: everything after the blank line.
      const size_t end = original->find("\r\n\r\n");
      if (end == std::string::npos)
         return AnsiString();

      return AnsiString(original->data() + end + 4, original->size() - end - 4);
   }

   bool
   MessageStore::Encode_(const std::string &original, bool compress, unsigned __int64 dedupMinimumBytes,
                         std::string &container, std::vector<MessageStoreReference> &references, String &problem)
   {
      references.clear();

      if (compress && dedupMinimumBytes > 0)
         MessageStoreFormat::FindLargeParts(original, dedupMinimumBytes, references);

      // Not compressed and no part held once: the container with no stage that
      // a message beginning with the magic is wrapped in.
      return MessageStoreFormat::Encode(original, references, compress, container, problem);
   }

   MessageStore::PlaceOutcome
   MessageStore::PlaceFile_(const String &messageFile, bool compress, unsigned __int64 dedupMinimumBytes, String &problem)
   {
      // The bytes came from outside the store - a client's APPEND, a remote
      // server, a migration, an import, the queue - and are the message whatever
      // they begin with: never taken for a container, whatever a header after
      // the magic says. The size and the first eight bytes decide what more is
      // read: with compression off, and for a message too large for a
      // container, nothing unless the message begins with the magic.
      unsigned __int64 size = 0;
      AnsiString start;
      if (!Backend().Size(messageFile, size) || !Backend().ReadAll(messageFile, start, MessageStoreFormat::MagicLength))
      {
         problem = _T("the file is missing or cannot be read");
         return PlaceFailed;
      }

      const bool magic = MessageStoreFormat::HasMagic(start.data(), start.size());

      if (!compress && !magic)
         return PlaceLeft;

      if (size > MessageStoreFormat::MaximumOriginalBytes)
      {
         if (!magic)
            return PlaceLeft;

         problem = _T("it is larger than a container holds, so it cannot be wrapped");
         return PlaceRefused;
      }

      AnsiString bytes;
      if (!Backend().ReadAll(messageFile, bytes, 0) || (unsigned __int64) bytes.size() > MessageStoreFormat::MaximumOriginalBytes)
      {
         problem = _T("the file could not be read whole");
         return magic ? PlaceRefused : PlaceFailed;
      }

      const std::string original(bytes.data(), bytes.size());

      std::string container;
      std::vector<MessageStoreReference> references;

      if (compress && Encode_(original, true, dedupMinimumBytes, container, references, problem))
      {
         // Not worth it: a message the stages do not make smaller, with nothing
         // to hold once, stays as it came - unless it must be wrapped.
         if (!magic && references.empty() && container.size() >= original.size())
            return PlaceLeft;

         // Counted before the name that needs them is written, and let go of if
         // it is not (MessageStoreBlobs.h).
         if (references.empty() || MessageStoreBlobs::Keep(original, references, problem))
         {
            if (Backend().WriteAtomically(messageFile, AnsiString(container.data(), container.size())))
               return PlaceWritten;

            MessageStoreBlobs::Release(references);
            problem = _T("the container could not be written beside the message and renamed over it");
         }
      }

      if (!magic)
         return PlaceFailed;

      // A message that begins with the magic, and was not stored compressed:
      // wrapped, with no stage and no part held once, so that the reader gives
      // back exactly these bytes.
      String wrapProblem;
      if (Encode_(original, false, 0, container, references, wrapProblem) &&
          Backend().WriteAtomically(messageFile, AnsiString(container.data(), container.size())))
         return PlaceWritten;

      if (problem.IsEmpty())
         problem = wrapProblem.IsEmpty() ? String(_T("the wrapped message could not be written beside it and renamed over it")) : wrapProblem;

      return PlaceRefused;
   }

   bool
   MessageStore::WriteContainer(const std::string &original, const String &storedFile, unsigned __int64 dedupMinimumBytes,
                                bool &written, String &problem)
   {
      written = false;

      if ((unsigned __int64) original.size() > MessageStoreFormat::MaximumOriginalBytes)
         return true;

      std::string container;
      std::vector<MessageStoreReference> references;
      if (!Encode_(original, true, dedupMinimumBytes, container, references, problem))
         return false;

      // A message that begins with the magic is always a container; any other
      // is one only when that is worth it.
      const bool magic = MessageStoreFormat::HasMagic(original.data(), original.size());
      if (!magic && references.empty() && container.size() >= original.size())
         return true;

      if (!references.empty() && !MessageStoreBlobs::Keep(original, references, problem))
         return false;

      if (!Backend().WriteAtomically(storedFile, AnsiString(container.data(), container.size())))
      {
         MessageStoreBlobs::Release(references);
         problem = "the container could not be written to " + storedFile;
         return false;
      }

      written = true;
      return true;
   }

   bool
   MessageStore::Relink(const String &storedFile, const String &name)
   {
      // A second name beside the one being replaced, counted by Link, then
      // renamed over it; the name it replaces was a working file's, which
      // counted nothing.
      const String linking = name + _T(".linking-") + GUIDCreator::GetGUID();
      if (!Link(storedFile, linking))
         return false;

      if (!Backend().Move(linking, name))
      {
         Remove(linking);
         return false;
      }

      return true;
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
      const String messageFile = PersistentMessage::GetFileName(account, message);

      // A zero-access mailbox's copy is ciphertext by now (ZeroAccess.h): there
      // is nothing to compress in it and no part to hold once, so it is not
      // compressed - but it is placed like any other, so that bytes beginning
      // with the magic (a message that arrived encrypted is stored as it came)
      // are wrapped rather than taken for a container.
      const bool zeroAccess = account && account->GetZeroAccess() && message->GetAccountID() == account->GetID();

      bool changed = false;
      return PlaceWorkingFile_(messageFile, !zeroAccess && Configuration::Instance()->GetMessageStoreCompression(), changed);
   }

   bool
   MessageStore::PlaceWorkingFile(const String &messageFile)
   {
      bool changed = false;
      return PlaceWorkingFile(messageFile, changed);
   }

   bool
   MessageStore::PlaceWorkingFile(const String &messageFile, bool &changed)
   {
      return PlaceWorkingFile_(messageFile, Configuration::Instance()->GetMessageStoreCompression(), changed);
   }

   bool
   MessageStore::PlaceWorkingFile_(const String &messageFile, bool compress, bool &changed)
   {
      changed = false;

      const unsigned __int64 dedupMinimum = (unsigned __int64) Configuration::Instance()->GetMessageStoreDedupMinKB() * 1024;

      String problem;
      switch (PlaceFile_(messageFile, compress, dedupMinimum, problem))
      {
      case PlaceWritten:
         changed = true;
         return true;

      case PlaceFailed:
         // Plain, and kept as it came, which the store reads as well as any
         // other: a failure costs the saving, never the message.
         ReportStoreError(7351, _T("MessageStore::Place"),
            "The message " + messageFile + " could not be stored in the compressed form, and is kept as it came: " + problem + ".");
         return true;

      case PlaceRefused:
         // Kept, these bytes would be read as a container they are not - and a
         // container names blobs other messages share.
         ReportStoreError(7353, _T("MessageStore::Place"),
            "The message " + messageFile + " begins with the bytes that mark a container of the message store, and could not be stored as the message it is: " + problem + ". It is refused rather than kept.");
         return false;

      case PlaceLeft:
      default:
         return true;
      }
   }
}
