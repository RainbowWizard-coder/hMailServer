// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "MessageStore.h"
#include "MessageStoreBlobs.h"
#include "MessageStoreObjects.h"
#include "PersistentMessage.h"

#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../Mime/Mime.h"
#include "../Util/ByteBuffer.h"
#include "../Util/File.h"
#include "../Util/GUIDCreator.h"
#include "../Util/MailboxCrypto.h"
#include "../Util/MailboxKeys.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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

      // Set on this thread when a door failed because the object store could
      // not be reached (MessageStore::Unavailable).
      thread_local bool store_unavailable = false;

      // Set on this thread when a door met a message the object store confirms
      // it does not have (MessageStore::Missing).
      thread_local bool store_missing = false;

      // Set on this thread when a door refused a stored message it could not
      // read (MessageStore::Unreadable): 1 for damage, 2 for a key closed.
      thread_local int store_unreadable = 0;

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
                  // The close is part of the write: it puts the last of the
                  // bytes in the file when no fsync has (File::Close), and a
                  // temporary that did not take them never takes the name.
                  written = file.Open(temporary, File::OTCreate) &&
                            file.Write(bytes) &&
                            (!IniFileSettings::Instance()->GetMessageStoreFsync() || file.FlushToDisk()) &&
                            file.Close();
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

      // When a stored file last changed, for the decoded cache's key: a file
      // written over in place keeps its identity, and can keep its size.
      AnsiString LastWrittenOf(const String &name)
      {
#ifdef HM_PLATFORM_POSIX
         const AnsiString narrow = name.c_str();
         struct stat status;
         if (::stat(narrow.c_str(), &status) != 0)
            return AnsiString();

         return AnsiString(std::to_string((long long) status.st_mtim.tv_sec) + "." + std::to_string((long long) status.st_mtim.tv_nsec) + "/" +
                           std::to_string((long long) status.st_ctim.tv_sec) + "." + std::to_string((long long) status.st_ctim.tv_nsec));
#else
         WIN32_FILE_ATTRIBUTE_DATA data;
         if (!::GetFileAttributesExW(name.c_str(), GetFileExInfoStandard, &data))
            return AnsiString();

         return AnsiString(std::to_string((unsigned long long) data.ftLastWriteTime.dwHighDateTime) + ":" +
                           std::to_string((unsigned long long) data.ftLastWriteTime.dwLowDateTime));
#endif
      }

      // The last containers decoded. A client that reads a message in pieces -
      // BODY[]<start.count>, which Thunderbird and Apple Mail use - or asks for
      // its structure and then its parts would otherwise have it decoded whole,
      // every blob read and hashed and the SHA-256 of the whole taken, once a
      // piece. Keyed by the name, by which file the name is (a rewrite is a new
      // file under the name, so it is never served from here), by when that
      // file was last written (a file written over in place keeps its identity)
      // and by its stored size; at most eight entries and 96 MB, and an entry nobody has asked
      // for in half a minute goes. What is kept here was checked whole when it
      // was decoded.
      class DecodedCache
      {
      public:

         // Moved on by every Clear: what a decode began before a clear is not
         // kept after it (Keep), since the key it was opened with may be the
         // one that closed.
         unsigned __int64 Generation()
         {
            boost::mutex::scoped_lock lock(lock_);
            return generation_;
         }

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

         void Keep(const String &name, const AnsiString &identity, unsigned __int64 storedSize, const std::shared_ptr<const std::string> &original,
                   unsigned __int64 generation)
         {
            if (!original || original->size() > MaximumBytes / 2)
               return;

            boost::mutex::scoped_lock lock(lock_);

            if (generation != generation_)
               return;

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

         // Everything, at once: a mailbox key closed takes the plaintext it
         // opened with it (MessageStore::ForgetDecodes).
         void Clear()
         {
            boost::mutex::scoped_lock lock(lock_);
            entries_.clear();
            bytes_ = 0;
            generation_++;
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
         unsigned __int64 generation_ = 0;
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

         // Taken before anything is decoded (DecodedCache::Generation).
         const unsigned __int64 generation = Decodes().Generation();

         AnsiString identity;
         unsigned int names = 0;
         unsigned __int64 storedSize = 0;
         bool keyed = MessageStore::Backend().Identity(name, identity, names) && MessageStore::Backend().Size(name, storedSize);
         if (keyed)
         {
            // An object's identity carries its ETag, which every write under
            // its key changes; a file's last write is the local disk's to say.
            const AnsiString written = MessageStore::Backend().IsLocal(name) ? LastWrittenOf(name) : AnsiString("object");
            keyed = !written.IsEmpty();
            identity += "@";
            identity += written;
         }

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
            Decodes().Keep(name, identity, storedSize, original, generation);

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

   // ---- MessageStoreParseSource ---------------------------------------------------------

   MessageStoreParseSource::MessageStoreParseSource(const String &messageFile) :
      ready_(true)
   {
      bool isContainer = false;
      MessageStoreHeader header;
      String problem;

      MessageStore::ClearUnavailable();

      // A file that is not there is a message being composed, which is the
      // caller's to decide; a plain file is itself. An object store that did
      // not answer has said nothing about whether the message is there, and a
      // message taken for one being composed would be written back empty over
      // it: refused.
      if (!ReadStoredHeader(messageFile, isContainer, header, problem))
      {
         if (isContainer)
         {
            MessageStore::ReportUnreadable(messageFile, problem);
            ready_ = false;
         }
         else if (MessageStore::Unavailable())
            ready_ = false;
         return;
      }

      if (!isContainer)
      {
         // A plain message in an object store is parsed from memory, as a
         // container's original is: there is no file to parse it from. And
         // one on the local disk too while a store is named: the committer may
         // take the file off the disk between here and a parse by path, which
         // would find no file and take the message for one being composed.
         // ReadAll reads the disk first and the store after, whichever holds
         // it at that moment (docs/ObjectStorage.md).
         // Never a queue file or a staged one, which the committer never
         // takes, nor one larger than a message is loaded at (MessageData,
         // 80 MB), which is parsed where it is.
         unsigned __int64 localSize = 0;
         const bool whole = MessageStoreObjects::Active() && MessageStoreObjects::Keyed(messageFile) &&
                            (!FileUtilities::FileSize64(messageFile, localSize) || localSize <= (unsigned __int64) 80 * 1024 * 1024);

         if (whole || !MessageStore::Backend().IsLocal(messageFile))
         {
            AnsiString bytes;
            if (!MessageStore::Backend().ReadAll(messageFile, bytes, 0))
            {
               ready_ = false;
               return;
            }

            original_ = std::shared_ptr<const std::string>(new std::string(bytes.data(), bytes.size()));
         }
         return;
      }

      // Decoded into memory, checked whole, and held there: never a file.
      std::shared_ptr<const std::string> original;
      if (!DecodedContainer(messageFile, original, problem))
      {
         MessageStore::ReportUnreadable(messageFile, problem);
         ready_ = false;
         return;
      }

      original_ = original;
      references_ = header.references;
      encrypted_to_ = MessageStoreFormat::EncryptedTo(header);
   }

   void
   MessageStoreParseSource::StoredFileReplaced()
   {
      if (references_.empty())
         return;

      MessageStoreBlobs::Release(references_);
      references_.clear();
   }

   // ---- MessageStore -----------------------------------------------------------------

   bool
   MessageStoreBackend::List(const String &directory, std::vector<String> &names)
   {
      names.clear();

      boost::system::error_code error;
      const boost::filesystem::path rootPath(std::wstring(directory.c_str()));
      if (!boost::filesystem::is_directory(rootPath, error) || error)
         return true;

      for (boost::filesystem::recursive_directory_iterator entry(rootPath, error);
           !error && entry != boost::filesystem::recursive_directory_iterator(); entry.increment(error))
      {
         boost::system::error_code fileError;
         if (boost::filesystem::is_regular_file(entry->path(), fileError) && !fileError)
            names.push_back(String(entry->path().wstring()));
      }

      return !error;
   }

   MessageStoreBackend &
   MessageStore::LocalBackend()
   {
      static LocalFileBackend backend;
      return backend;
   }

   MessageStoreBackend &
   MessageStore::Backend()
   {
      // An object store while one is configured - read first from the local
      // disk, where a working file is, and then from the store - and the local
      // file system otherwise, which a server that never names one never
      // leaves (docs/ObjectStorage.md).
      MessageStoreBackend *objects = MessageStoreObjects::Active();
      return objects ? *objects : LocalBackend();
   }

   bool
   MessageStore::Unavailable()
   {
      return store_unavailable;
   }

   void
   MessageStore::ClearUnavailable()
   {
      store_unavailable = false;
      store_missing = false;
   }

   bool
   MessageStore::Missing()
   {
      return store_missing;
   }

   void
   MessageStore::NoteMissing()
   {
      store_missing = true;
   }

   void
   MessageStore::NoteUnavailable()
   {
      store_unavailable = true;
   }

   bool
   MessageStore::Unreadable()
   {
      return store_unreadable != 0;
   }

   bool
   MessageStore::UnreadableKeyClosed()
   {
      return store_unreadable == 2;
   }

   void
   MessageStore::ClearUnreadable()
   {
      store_unreadable = 0;
   }

   void
   MessageStore::Commit(const String &messageFile)
   {
      Backend().Commit(messageFile);
   }

   bool
   MessageStore::MoveTree(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem)
   {
      return MessageStoreObjects::MoveTree(fromDirectory, toDirectory, moved, problem);
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
      // A message of a mailbox encrypted at rest, read while its key is
      // closed: refused all the same, and not damage - it opens at the
      // account's next sign-in with its password. Said in the debug log only,
      // since a delegate's client or a push can ask for such a message often.
      if (MailboxKeys::IsLockedProblem(problem))
      {
         if (store_unreadable == 0)
            store_unreadable = 2;
         LOG_DEBUG("MessageStore: " + messageFile + " was not read: " + problem + ".");
         return;
      }

      store_unreadable = 1;

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

      // A plain file, or none: LoadFromFile's own answers, OpenFailed among them
      // - unless the object store's committer took the file off the disk in
      // the moment between, when it is read from the store below.
      if (!isContainer && Backend().IsLocal(name))
      {
         MimeLoadResult loaded = body.LoadFromFile(messageFile);

         // Still on the disk: a file renamed over the name in the moment
         // between - opened once more.
         if (loaded == MimeLoadResult::OpenFailed && Backend().IsLocal(name))
            loaded = body.LoadFromFile(messageFile);

         if (loaded != MimeLoadResult::OpenFailed || Backend().IsLocal(name))
            return loaded;
      }

      std::shared_ptr<const std::string> original;
      if (!isContainer)
      {
         // A plain message in an object store, parsed from memory as a
         // container's original is. One the store did not give is OpenFailed,
         // as a file that could not be opened is.
         AnsiString bytes;
         if (!read || !Backend().ReadAll(name, bytes, 0))
            return MimeLoadResult::OpenFailed;

         original = std::shared_ptr<const std::string>(new std::string(bytes.data(), bytes.size()));
      }
      else if (!read || !DecodedContainer(name, original, problem))
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

      // The file itself - but not while an object store is named, whose
      // committer may take it off the disk before the caller opens it: then a
      // temporary copy, as below.
      if (!isContainer && Backend().IsLocal(messageFile) && !MessageStoreObjects::Active())
         return messageFile;

      // A plain message in an object store: its bytes, written out as a
      // container's original is.
      if (!isContainer)
      {
         AnsiString bytes;
         if (!read || !Backend().ReadAll(messageFile, bytes, 0))
            return String();

         const String plain = FileUtilities::GetTempFileName();
         if (!FileUtilities::WriteToFile(plain, bytes))
         {
            FileUtilities::DeleteFile(plain);
            ReportStoreError(7351, _T("MessageStore::PlainFileFor"),
               "A plain copy of the stored message " + messageFile + " could not be written to " + plain + ".");
            return String();
         }

         temporary = true;
         return plain;
      }

      // A message of a mailbox encrypted at rest is not written out in the
      // clear for another process to read (docs/EncryptionAtRest.md): the
      // caller - SpamAssassin's lesson on a move - goes without it.
      if (read && !MessageStoreFormat::EncryptedTo(header).empty())
      {
         LOG_DEBUG("MessageStore: " + messageFile + " is encrypted at rest, and no plain copy of it is written.");
         return String();
      }

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
      // A new name, whose row is saved after this.
      MessageStoreObjects::Placing placing;

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
      PlaceRefusal refusal = PlaceAccepted;
      return Place(account, message, refusal);
   }

   bool
   MessageStore::Place(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, PlaceRefusal &refusal)
   {
      // A copy in the account's own folders - not one a rule filed into a
      // public folder, which is nobody's mailbox - is stored as its mailbox
      // stores mail (MailboxKeys.h).
      std::string publicKey;
      MailboxKeys::Encryption encryption = MailboxKeys::EncryptionNone;
      if (account && message->GetAccountID() == account->GetID())
         encryption = MailboxKeys::EncryptionFor(account, publicKey);

      return PlaceAs(account, message, encryption, publicKey, refusal);
   }

   bool
   MessageStore::PlaceAs(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message,
                         MailboxKeys::Encryption encryption, const std::string &publicKey, PlaceRefusal &refusal)
   {
      refusal = PlaceAccepted;

      // A new name, whose row is saved after this: an object store's
      // committer is handed it then (PersistentMessage::SaveObject), and the
      // placing itself is all on the local disk.
      MessageStoreObjects::Placing placing;

      const String messageFile = PersistentMessage::GetFileName(account, message);

      // A copy in the account's own folders - not one a rule filed into a
      // public folder, which is nobody's mailbox.
      const bool own = account && message->GetAccountID() == account->GetID();

      // A mailbox encrypted at rest (MailboxKeys.h): encrypted to its key,
      // whatever the bytes begin with - they are the message, and the
      // container says so - and refused rather than kept in the clear when it
      // cannot be, or when whether the mailbox is encrypted is not known.
      if (own && encryption == MailboxKeys::EncryptionOn)
         return PlaceEncrypted_(messageFile, publicKey, false, refusal);

      if (own && encryption == MailboxKeys::EncryptionUnknown)
      {
         Scrub(messageFile);
         ReportEncryptionUnknown_(messageFile, _T("MessageStore::Place"));
         refusal = PlaceRefusedUnknown;
         return false;
      }

      if (own && encryption == MailboxKeys::EncryptionDamaged)
      {
         Scrub(messageFile);
         ReportEncryptionDamaged_(messageFile, _T("MessageStore::Place"));
         refusal = PlaceRefusedDamaged;
         return false;
      }

      // A zero-access mailbox's copy is ciphertext by now (ZeroAccess.h): there
      // is nothing to compress in it and no part to hold once, so it is not
      // compressed - but it is placed like any other, so that bytes beginning
      // with the magic (a message that arrived encrypted is stored as it came)
      // are wrapped rather than taken for a container.
      const bool zeroAccess = own && account->GetZeroAccess();

      bool changed = false;
      if (PlaceWorkingFile_(messageFile, !zeroAccess && Configuration::Instance()->GetMessageStoreCompression(), changed))
         return true;

      refusal = PlaceRefusedContent;
      return false;
   }

   bool
   MessageStore::WriteForAccount(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, const AnsiString &bytes)
   {
      PlaceRefusal refusal = PlaceAccepted;
      return WriteForAccount(account, message, bytes, refusal);
   }

   bool
   MessageStore::WriteForAccount(std::shared_ptr<const Account> account, std::shared_ptr<const Message> message, const AnsiString &bytes, PlaceRefusal &refusal)
   {
      refusal = PlaceAccepted;

      // A new name, as in PlaceAs.
      MessageStoreObjects::Placing placing;

      const String messageFile = PersistentMessage::GetFileName(account, message);

      std::string publicKey;
      const MailboxKeys::Encryption encryption = account && message->GetAccountID() == account->GetID()
         ? MailboxKeys::EncryptionFor(account, publicKey)
         : MailboxKeys::EncryptionNone;

      if (encryption == MailboxKeys::EncryptionNone)
      {
         // Written by path, as these writers always have; the row's save
         // commits it to an object store (PersistentMessage::SaveObject).
         if (FileUtilities::WriteToFile(messageFile, bytes))
            return true;

         refusal = PlaceRefusedWrite;
         return false;
      }

      if (encryption == MailboxKeys::EncryptionUnknown)
      {
         ReportEncryptionUnknown_(messageFile, _T("MessageStore::WriteForAccount"));
         refusal = PlaceRefusedUnknown;
         return false;
      }

      if (encryption == MailboxKeys::EncryptionDamaged)
      {
         ReportEncryptionDamaged_(messageFile, _T("MessageStore::WriteForAccount"));
         refusal = PlaceRefusedDamaged;
         return false;
      }

      // Encrypted in memory and written encrypted: the plaintext never
      // reaches the disk.
      String problem;
      bool written = false;
      std::string original(bytes.data(), bytes.size());
      if ((unsigned __int64) original.size() > MessageStoreFormat::MaximumOriginalBytes)
      {
         problem = _T("it is larger than an encrypted container holds (64 MB), and an encrypted mailbox keeps nothing in the clear");
         refusal = PlaceRefusedTooLarge;
      }
      else
      {
         written = WriteEncryptedContainer(original, messageFile, publicKey, problem);
         if (!written)
            refusal = PlaceRefusedWrite;
      }
      MailboxCrypto::Forget(original);

      if (written)
         return true;

      ReportStoreError(MailboxKeys::CodePlace, _T("MessageStore::WriteForAccount"),
         "The message " + messageFile + " is for a mailbox encrypted at rest and could not be stored encrypted: " + problem + ". It was not written, in the clear or otherwise.");
      return false;
   }

   bool
   MessageStore::PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile)
   {
      return PlaceWorkingFileFor(owner, messageFile, false);
   }

   bool
   MessageStore::PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile, bool keepOnRefusal)
   {
      PlaceRefusal refusal = PlaceAccepted;
      return PlaceWorkingFileFor(owner, messageFile, keepOnRefusal, refusal);
   }

   bool
   MessageStore::PlaceWorkingFileFor(std::shared_ptr<const Account> owner, const String &messageFile, bool keepOnRefusal, PlaceRefusal &refusal)
   {
      std::string publicKey;
      const MailboxKeys::Encryption encryption = owner ? MailboxKeys::EncryptionFor(owner, publicKey) : MailboxKeys::EncryptionNone;
      return PlaceWorkingFileAs(owner, messageFile, encryption, publicKey, keepOnRefusal, refusal);
   }

   bool
   MessageStore::PlaceWorkingFileAs(std::shared_ptr<const Account> owner, const String &messageFile, MailboxKeys::Encryption encryption,
                                    const std::string &publicKey, bool keepOnRefusal, PlaceRefusal &refusal)
   {
      refusal = PlaceAccepted;

      // A new name, as in PlaceAs.
      MessageStoreObjects::Placing placing;

      if (owner)
      {
         if (encryption == MailboxKeys::EncryptionOn)
            return PlaceEncrypted_(messageFile, publicKey, keepOnRefusal, refusal);

         if (encryption == MailboxKeys::EncryptionUnknown)
         {
            if (!keepOnRefusal)
               Scrub(messageFile);
            ReportEncryptionUnknown_(messageFile, _T("MessageStore::PlaceWorkingFileFor"));
            refusal = PlaceRefusedUnknown;
            return false;
         }

         if (encryption == MailboxKeys::EncryptionDamaged)
         {
            if (!keepOnRefusal)
               Scrub(messageFile);
            ReportEncryptionDamaged_(messageFile, _T("MessageStore::PlaceWorkingFileFor"));
            refusal = PlaceRefusedDamaged;
            return false;
         }
      }

      if (PlaceWorkingFile(messageFile))
         return true;

      refusal = PlaceRefusedContent;
      return false;
   }

   void
   MessageStore::ReportEncryptionDamaged_(const String &messageFile, const String &where)
   {
      ReportStoreError(MailboxKeys::CodePlace, where,
         "The message " + messageFile + " was not stored: " + MailboxKeys::DamagedSentence());
   }

   void
   MessageStore::ReportEncryptionUnknown_(const String &messageFile, const String &where)
   {
      ReportStoreError(MailboxKeys::CodePlace, where,
         "The message " + messageFile + " could not be stored now: whether its mailbox is encrypted at rest is not known - its key could not be read from the database, or its key row holds no key - and it is not kept in the clear on a guess. The writer's answer is a temporary one.");
   }

   bool
   MessageStore::PlaceEncrypted_(const String &messageFile, const std::string &publicKey, bool keepOnRefusal, PlaceRefusal &refusal)
   {
      refusal = PlaceAccepted;
      MessageStoreObjects::Placing placing;
      String problem;

      unsigned __int64 size = 0;
      AnsiString bytes;
      if (!Backend().Size(messageFile, size))
      {
         problem = _T("the file is missing or cannot be read");
         refusal = PlaceRefusedWrite;
      }
      else if (size > MessageStoreFormat::MaximumOriginalBytes)
      {
         problem = _T("it is larger than an encrypted container holds (64 MB), and an encrypted mailbox keeps nothing in the clear");
         refusal = PlaceRefusedTooLarge;
      }
      else if (!Backend().ReadAll(messageFile, bytes, 0) || (unsigned __int64) bytes.size() > MessageStoreFormat::MaximumOriginalBytes)
      {
         problem = _T("the file could not be read whole");
         refusal = PlaceRefusedWrite;
      }
      else
      {
         // The whole message under the account's key, no part held once - a
         // blob other accounts share cannot be under this one's key - and
         // compressed first when compression is on.
         std::string original(bytes.data(), bytes.size());
         std::string container;
         const bool encoded = MessageStoreFormat::EncodeEncrypted(original, Configuration::Instance()->GetMessageStoreCompression(), publicKey, container, problem);
         MailboxCrypto::Forget(original);

         // Any failure from here on is this server's - a write, a rename, the
         // random generator - and a later attempt may not meet it.
         refusal = PlaceRefusedWrite;

         if (encoded)
         {
            // The container beside the plaintext; the plaintext moved aside and
            // the container renamed into its place; then the plaintext
            // overwritten with zeros and removed. The name holds the message
            // at every moment but between the two renames, and holds it
            // encrypted once this answers; a crash between them leaves the
            // plaintext and the container beside an empty name, which the
            // next start puts right (SweepEncryptionLeftovers).
            const String staged = messageFile + _T(".encrypting-") + GUIDCreator::GetGUID();
            const String aside = messageFile + _T(".plaintext-") + GUIDCreator::GetGUID();

            if (!Backend().WriteAtomically(staged, AnsiString(container.data(), container.size())))
               problem = _T("the encrypted message could not be written beside it");
            else if (!Backend().Move(messageFile, aside))
               problem = _T("the plaintext could not be moved aside for the encrypted message");
            else if (!Backend().Move(staged, messageFile))
            {
               problem = _T("the encrypted message could not be renamed into its place");

               // Put back under its name, for the caller to discard as a
               // refused file. When even that fails, the plaintext beside the
               // name is the refused file: overwritten and removed here, as the
               // caller would have done - unless it is the only copy there is
               // (keepOnRefusal: an import's), which stays where it is, named in
               // the report, and is put back under its name at the next start.
               if (!Backend().Move(aside, messageFile))
               {
                  if (keepOnRefusal)
                     problem += _T(", nor the message put back under its name: it is at ") + aside;
                  else
                  {
                     Scrub(aside);
                     Backend().Remove(aside);
                  }
               }
            }
            else
            {
               Scrub(aside);
               Backend().Remove(aside);
               return true;
            }

            Backend().Remove(staged);
         }
      }

      // Refused: the caller discards the working file, whose plaintext is
      // overwritten first - unless it is the only copy there is (an import's).
      if (!keepOnRefusal)
         Scrub(messageFile);

      ReportStoreError(MailboxKeys::CodePlace, _T("MessageStore::Place"),
         "The message " + messageFile + " is for a mailbox encrypted at rest and could not be stored encrypted: " + problem + ". It is refused rather than kept in the clear.");
      return false;
   }

   bool
   MessageStore::IsEncrypted(const String &messageFile, std::string &publicKey)
   {
      publicKey.clear();

      bool isContainer = false;
      MessageStoreHeader header;
      String problem;
      if (!ReadStoredHeader(messageFile, isContainer, header, problem) || !isContainer)
         return false;

      publicKey = MessageStoreFormat::EncryptedTo(header);
      return !publicKey.empty();
   }

   bool
   MessageStore::WriteEncryptedContainer(const std::string &original, const String &storedFile, const std::string &publicKey, String &problem)
   {
      std::string container;
      if (!MessageStoreFormat::EncodeEncrypted(original, Configuration::Instance()->GetMessageStoreCompression(), publicKey, container, problem))
         return false;

      // Beside the name, read back and compared with what was encrypted -
      // which EncodeEncrypted opened again in memory before it answered - and
      // only then renamed over it: the name holds what it held until what
      // opens is there, and nothing is left beside it when this fails.
      const String staged = storedFile + _T(".encrypting-") + GUIDCreator::GetGUID();
      if (!Backend().WriteAtomically(staged, AnsiString(container.data(), container.size())))
      {
         Backend().Remove(staged);
         problem = "the encrypted message could not be written beside " + storedFile;
         return false;
      }

      AnsiString written;
      if (!Backend().ReadAll(staged, written, 0) || written.size() != container.size() ||
          memcmp(written.data(), container.data(), container.size()) != 0)
      {
         Backend().Remove(staged);
         problem = "what was written beside " + storedFile + " did not read back as it was written";
         return false;
      }

      if (!Backend().Move(staged, storedFile))
      {
         Backend().Remove(staged);
         problem = "the encrypted message could not be renamed over " + storedFile;
         return false;
      }

      return true;
   }

   void
   MessageStore::Scrub(const String &messageFile)
   {
      // One name only: a file several names share is every name's, and the
      // zeros would be too.
      AnsiString identity;
      unsigned int names = 0;
      if (!Backend().Identity(messageFile, identity, names) || names != 1)
         return;

      unsigned __int64 length = 0;
      if (!Backend().Size(messageFile, length) || length == 0)
         return;

      // In place, without truncating: a file opened to be rewritten keeps its
      // blocks, which is what overwriting them means - as ZeroAccess::Scrub
      // does. Truncating first would free them and put the zeros elsewhere.
#ifdef HM_PLATFORM_POSIX
      const AnsiString narrow = messageFile.c_str();
      std::fstream over(narrow.c_str(), std::ios::in | std::ios::out | std::ios::binary);
#else
      std::fstream over(messageFile.c_str(), std::ios::in | std::ios::out | std::ios::binary);
#endif
      if (!over)
         return;

      const std::vector<char> zeros(65536, '\0');
      unsigned __int64 left = length;
      while (left > 0 && over)
      {
         const size_t chunk = left < (unsigned __int64) zeros.size() ? (size_t) left : zeros.size();
         over.write(&zeros[0], (std::streamsize) chunk);
         left -= (unsigned __int64) chunk;
      }

      over.flush();
      over.close();
   }

   namespace
   {
      // A braced GUID as GUIDCreator writes it: {8-4-4-4-12}, hex digits.
      bool IsBracedGuid(const String &text)
      {
         if (text.GetLength() != 38 || text[0] != '{' || text[37] != '}')
            return false;

         for (int i = 1; i < 37; i++)
         {
            const int c = (int) text[i];
            if (i == 9 || i == 14 || i == 19 || i == 24)
            {
               if (c != '-')
                  return false;
               continue;
            }

            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
               return false;
         }

         return true;
      }

      // A leftover's FILE NAME, <name><mark><GUID>, split at the last mark:
      // true only when the name part is not empty and the part after the mark
      // is exactly a braced GUID.
      bool IsLeftoverName(const String &fileName, const String &mark)
      {
         const size_t at = fileName.rfind(mark);
         if (at == String::npos || at == 0)
            return false;

         return IsBracedGuid(fileName.Mid((int) (at + mark.GetLength())));
      }

      struct Leftover
      {
         String path;   // the leftover itself
         String name;   // the stored file it was beside: the path, less the mark and the GUID
      };
   }

   void
   MessageStore::SweepEncryptionLeftovers(const std::vector<String> &directories)
   {
      const String plainMark = _T(".plaintext-");
      const String stagedMark = _T(".encrypting-");
      const int suffixLength = 38;

      int scrubbed = 0;
      int restored = 0;
      int putBack = 0;
      int removed = 0;

      for (const String &directory : directories)
      {
         // One directory at a time, and one that cannot be walked - a name the
         // file system's conversion refuses, a directory gone meanwhile - is
         // reported and passed: the rest are still swept.
         try
         {
            std::vector<Leftover> asides;
            std::vector<Leftover> staged;

            boost::system::error_code error;
            const boost::filesystem::path rootPath(std::wstring(directory.c_str()));
            if (!boost::filesystem::is_directory(rootPath, error) || error)
               continue;

            for (boost::filesystem::recursive_directory_iterator entry(rootPath, error);
                 !error && entry != boost::filesystem::recursive_directory_iterator(); entry.increment(error))
            {
               boost::system::error_code fileError;
               if (!boost::filesystem::is_regular_file(entry->path(), fileError) || fileError)
                  continue;

               // The file's own name, never the directories above it: a mailbox,
               // a domain or a data directory may be named with either mark. A
               // name the conversion refuses skips that file alone - never the
               // leftovers already found in the directory.
               try
               {
                  const String fileName = String(entry->path().filename().wstring());
                  const bool aside = IsLeftoverName(fileName, plainMark);
                  const bool stagedCopy = !aside && IsLeftoverName(fileName, stagedMark);
                  if (!aside && !stagedCopy)
                     continue;

                  Leftover leftover;
                  leftover.path = String(entry->path().wstring());
                  leftover.name = leftover.path.Mid(0, leftover.path.GetLength() - (aside ? plainMark : stagedMark).GetLength() - suffixLength);

                  if (aside)
                     asides.push_back(leftover);
                  else
                     staged.push_back(leftover);
               }
               catch (...)
               {
                  continue;
               }
            }

            for (const Leftover &aside : asides)
            {
               // An object store that could not say whether the name holds its
               // message has not said the name is empty: the plaintext put back
               // under it would hide - and then be committed over - the
               // encrypted message the store holds. Left for the next start
               // (docs/ObjectStorage.md).
               ClearUnavailable();

               // Cut short between the two renames: the name is empty, and the
               // container staged beside it is the message, encrypted and whole
               // (it was written, atomically, before the plaintext was moved).
               if (!Backend().Exists(aside.name) && !Unavailable())
               {
                  for (std::vector<Leftover>::iterator candidate = staged.begin(); candidate != staged.end(); ++candidate)
                  {
                     if (candidate->name == aside.name && Backend().Move(candidate->path, aside.name))
                     {
                        staged.erase(candidate);
                        restored++;
                        break;
                     }
                  }
               }

               ClearUnavailable();
               const bool holds = Backend().Exists(aside.name);
               if (Unavailable())
                  continue;

               if (holds)
               {
                  Scrub(aside.path);
                  Backend().Remove(aside.path);
                  scrubbed++;
               }
               else if (Backend().Move(aside.path, aside.name))
               {
                  // Nothing else holds this message: back under its name as it
                  // was, never overwritten - whatever wrote it sees it again.
                  putBack++;
               }
            }

            // A container staged for a name that holds its message already: a
            // rewrite or a placing cut short before its rename.
            for (const Leftover &leftover : staged)
            {
               if (Backend().Remove(leftover.path))
                  removed++;
            }
         }
         catch (...)
         {
            ReportStoreError(MailboxKeys::CodeSweep, _T("MessageStore::SweepEncryptionLeftovers"),
               "The directory " + directory + " of a mailbox encrypted at rest could not be swept for what an encryption cut short left beside its files; it is swept again at the next start.");
         }
      }

      if (scrubbed + restored + putBack + removed > 0)
      {
         LOG_APPLICATION(Formatter::Format("Mailbox encryption at rest: an encryption cut short had left files beside keyed mailboxes' messages; {0} plaintext copies were overwritten and removed, {1} encrypted messages renamed into their names, {2} messages put back under their names as they were, and {3} staged copies removed.",
            scrubbed, restored, putBack, removed));
      }
   }

   bool
   MessageStore::CopyIntoAccount(const String &from, const String &to, std::shared_ptr<const Account> destination)
   {
      // A new name, whose row - or kept copy's - is saved after this.
      MessageStoreObjects::Placing placing;

      std::string sourceKey;
      IsEncrypted(from, sourceKey);

      // A destination whose encryption is not known gets no copy: it might be
      // a mailbox that keeps nothing in the clear. The destination's own key -
      // switched on or not - is the keyed set's, as the encryption is, so a
      // read of the database that fails cannot make a message under that key
      // look like one to put in the clear.
      std::string destinationKey;
      const MailboxKeys::Encryption destinationEncryption = MailboxKeys::EncryptionFor(destination, destinationKey);
      std::string destinationOwnKey;
      const MailboxKeys::Keyed destinationKeyed = destination ? MailboxKeys::KeyOf(destination->GetID(), destinationOwnKey) : MailboxKeys::KeyedNo;

      if (destinationEncryption == MailboxKeys::EncryptionUnknown || destinationKeyed == MailboxKeys::KeyedUnknown)
      {
         ReportStoreError(MailboxKeys::CodeCopy, _T("MessageStore::CopyIntoAccount"),
            "The stored message " + from + " could not be copied to " + to + ": whether the mailbox it goes into is encrypted at rest could not be read from the database.");
         return false;
      }

      if (destinationEncryption == MailboxKeys::EncryptionDamaged || destinationKeyed == MailboxKeys::KeyedDamaged)
      {
         ReportStoreError(MailboxKeys::CodeCopy, _T("MessageStore::CopyIntoAccount"),
            "The stored message " + from + " could not be copied to " + to + ": " + MailboxKeys::DamagedSentence());
         return false;
      }

      const bool encryptForDestination = destinationEncryption == MailboxKeys::EncryptionOn;

      // The same file for both ends: a copy within one mailbox, or between two
      // that are not encrypted. A copy of a message encrypted to the
      // destination's own key stays encrypted to it, whether the destination
      // still encrypts new mail or not.

      if ((!sourceKey.empty() && sourceKey == destinationOwnKey) || (sourceKey.empty() && !encryptForDestination))
         return CopyStored(from, to);

      // Anything else is re-encoded: out from under the source's key, which
      // must be open, and into the destination's form.
      std::string original;
      String problem;
      if (!ReadOriginal(from, original, problem))
      {
         // Braced: LOG_DEBUG is a macro with an if of its own.
         if (MailboxKeys::IsLockedProblem(problem))
         {
            LOG_DEBUG("MessageStore: " + from + " was not copied to " + to + ": " + problem + ".");
         }
         else
         {
            ReportStoreError(MailboxKeys::CodeCopy, _T("MessageStore::CopyIntoAccount"),
               "The stored message " + from + " could not be copied to " + to + ": " + problem + ".");
         }
         return false;
      }

      bool copied = false;
      if (encryptForDestination)
         copied = WriteEncryptedContainer(original, to, destinationKey, problem);
      else
      {
         // Out of an encrypted mailbox into one that is not, or into a public
         // folder: the message as it arrived, placed as that mailbox places it.
         copied = Backend().WriteAtomically(to, AnsiString(original.data(), original.size())) && PlaceWorkingFile(to);
         if (!copied)
         {
            DiscardWorkingFile(to);
            problem = _T("the copy could not be written");
         }
      }

      MailboxCrypto::Forget(original);

      if (!copied)
      {
         ReportStoreError(MailboxKeys::CodeCopy, _T("MessageStore::CopyIntoAccount"),
            "The stored message " + from + " could not be copied to " + to + ": " + problem + ".");
         return false;
      }

      return true;
   }

   void
   MessageStore::ForgetDecodes()
   {
      Decodes().Clear();
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
      MessageStoreObjects::Placing placing;

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
