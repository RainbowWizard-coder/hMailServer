// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "MessageStoreObjects.h"
#include "MessageStore.h"
#include "MessageStoreBlobs.h"
#include "MessageStoreMaintenance.h"
#include "MessageStoreObjectClient.h"

#include "../Application/PropertySet.h"
#include "../BO/ScheduledTask.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "../Util/ByteBuffer.h"
#include "../Util/File.h"
#include "../Util/GUIDCreator.h"
#include "../Util/ObjectStoreSignature.h"
#include "../Util/Strings/Formatter.h"
#include "../Util/Unicode.h"

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <utility>

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
      typedef ObjectStoreSignature Sig;
      typedef MessageStoreObjectClient Client;

      // How often the settings are read again, and how long an answer about a
      // key - its size and ETag, and its bytes when they are small - is kept.
      const long long SettingsMilliseconds = 5 * 1000;
      const long long CacheMilliseconds = 30 * 1000;

      // What a read of an object fetches first - enough for most messages whole,
      // and for a header, a size or the first bytes of any - and at a time after
      // that.
      const size_t FirstWindowBytes = 1024 * 1024;
      const size_t ReadWindowBytes = 8 * 1024 * 1024;

      // The committer. A name written through the backend - a rewrite, a blob,
      // a kept copy - is handed over this long after it was written, when its
      // writer is done with it; a message whose row is saved, at once. One pass
      // on the maintenance queue works this long at most. A name the store did
      // not take is tried again after 30 seconds, then twice as long each time,
      // 15 minutes at most; and HM7473 says how many wait, once an hour.
      const long long WriteDelayMilliseconds = 30 * 1000;
      const long long DrainMilliseconds = 50 * 1000;
      const long long RetryFirstMilliseconds = 30 * 1000;
      const long long RetryLongestMilliseconds = 15 * 60 * 1000;
      const long long PendingReportMilliseconds = 60 * 60 * 1000;
      const long long DrainStuckMilliseconds = 10 * 60 * 1000;

      // How many names the committer takes from its list at a time.
      const size_t CommitBatch = 4096;

      // Set while a new name is being made (MessageStoreObjects::Placing).
      thread_local int placing_depth = 0;

      std::atomic<long long> last_missing_report(0);

      long long NowMs()
      {
         return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      std::string Utf8Of(const String &text)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(text, utf8);
         return std::string(utf8.c_str(), (size_t) utf8.GetLength());
      }

      String WideOfUtf8(const std::string &text)
      {
         String wide;
         Unicode::MultiByteToWide(AnsiString(text.c_str()), wide);
         return wide;
      }

      std::string TrimmedText(const std::string &text)
      {
         const size_t first = text.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();
         const size_t last = text.find_last_not_of(" \t\r\n");
         return text.substr(first, last - first + 1);
      }

      std::string LowerAscii(const std::string &text)
      {
         std::string lower = text;
         for (size_t i = 0; i < lower.size(); i++)
         {
            if (lower[i] >= 'A' && lower[i] <= 'Z')
               lower[i] = (char) (lower[i] - 'A' + 'a');
         }
         return lower;
      }

      String TrimSeparators(const String &path)
      {
         String trimmed = path;
         while (trimmed.EndsWith(_T("\\")) || trimmed.EndsWith(_T("/")))
            trimmed = trimmed.Left(trimmed.GetLength() - 1);
         return trimmed;
      }

      // A braced GUID as GUIDCreator writes it, at the end of a file name after
      // one of the marks the store's writers stage a file beside its name with.
      bool IsStagedName(const std::string &fileName)
      {
         static const char *marks[] = { ".writing-", ".encrypting-", ".plaintext-", ".linking-", ".store-" };

         if (fileName.size() < 38 || fileName[fileName.size() - 1] != '}' || fileName[fileName.size() - 38] != '{')
            return false;

         for (size_t m = 0; m < sizeof(marks) / sizeof(marks[0]); m++)
         {
            const std::string mark = marks[m];
            if (fileName.size() > mark.size() + 38 && fileName.compare(fileName.size() - 38 - mark.size(), mark.size(), mark) == 0)
               return true;
         }

         return false;
      }

      // What a local file is now - which file it is, its size and when it was
      // last written, and on POSIX when its inode last changed - so that the
      // committer can tell the file it put from one written in its place
      // meanwhile. Empty when there is no such file. Without the change time,
      // for a file compared across a rename of its own: rename(2) sets it on
      // ext4, XFS, btrfs and tmpfs, and a file renamed over a name is another
      // inode (another file index on Windows) whatever its times say.
      AnsiString LocalStampOf(const String &name, bool withChangeTime = true)
      {
#ifdef HM_PLATFORM_POSIX
         const AnsiString narrow = name.c_str();
         struct stat status;
         if (::stat(narrow.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            return AnsiString();

         std::string stamp = std::to_string((unsigned long long) status.st_dev) + ":" + std::to_string((unsigned long long) status.st_ino) + "/" +
                             std::to_string((long long) status.st_size) + "/" +
                             std::to_string((long long) status.st_mtim.tv_sec) + "." + std::to_string((long long) status.st_mtim.tv_nsec);
         if (withChangeTime)
            stamp += "/" + std::to_string((long long) status.st_ctim.tv_sec) + "." + std::to_string((long long) status.st_ctim.tv_nsec);
         return AnsiString(stamp.c_str());
#else
         (void) withChangeTime;

         // Opened for its attributes alone, sharing everything, so that nobody
         // else's delete or rename is refused for it; the file's index names
         // the file itself, as st_ino does, so that one renamed over the name
         // in the same clock tick with the same size is still another file.
         HANDLE handle = ::CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
            return AnsiString();

         BY_HANDLE_FILE_INFORMATION information;
         const BOOL got = ::GetFileInformationByHandle(handle, &information);
         ::CloseHandle(handle);
         if (!got || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return AnsiString();

         const std::string stamp = std::to_string((unsigned long long) information.nFileSizeHigh) + ":" + std::to_string((unsigned long long) information.nFileSizeLow) + "/" +
                                   std::to_string((unsigned long long) information.ftLastWriteTime.dwHighDateTime) + ":" +
                                   std::to_string((unsigned long long) information.ftLastWriteTime.dwLowDateTime) + "/" +
                                   std::to_string((unsigned long long) information.dwVolumeSerialNumber) + ":" +
                                   std::to_string((unsigned long long) information.nFileIndexHigh) + ":" +
                                   std::to_string((unsigned long long) information.nFileIndexLow);
         return AnsiString(stamp.c_str());
#endif
      }

      // File calls that try once and say nothing, for use while a name's lock
      // or the committer's list is held: FileUtilities' own retry for four
      // seconds and report, and a report runs the OnError event, whose script
      // may store a message on this thread and so ask for the lock it holds.
      boost::filesystem::path FsPath(const String &name)
      {
         return boost::filesystem::path(std::wstring(name.c_str()));
      }

      bool RenameQuietly(const String &from, const String &to)
      {
         boost::system::error_code error;
         boost::filesystem::rename(FsPath(from), FsPath(to), error);
         return !error;
      }

      // A second name for a file, never over one that is there.
      bool LinkQuietly(const String &existing, const String &link)
      {
         boost::system::error_code error;
         boost::filesystem::create_hard_link(FsPath(existing), FsPath(link), error);
         return !error;
      }

      bool RemoveQuietly(const String &name)
      {
         boost::system::error_code error;
         boost::filesystem::remove(FsPath(name), error);
         return !error && !FileUtilities::Exists(name);
      }

      void DirectoryQuietly(const String &directory)
      {
         boost::system::error_code error;
         boost::filesystem::create_directories(FsPath(directory), error);
      }

      bool CopyQuietly(const String &from, const String &to)
      {
         const boost::filesystem::path target = FsPath(to);
         boost::system::error_code error;
         boost::filesystem::create_directories(target.parent_path(), error);
         error.clear();
         boost::filesystem::copy_file(FsPath(from), target, boost::filesystem::copy_options::overwrite_existing, error);
         return !error;
      }

      bool MoveQuietly(const String &from, const String &to)
      {
         const boost::filesystem::path target = FsPath(to);
         boost::system::error_code error;
         boost::filesystem::create_directories(target.parent_path(), error);
         error.clear();
         boost::filesystem::rename(FsPath(from), target, error);
         return !error;
      }

      void Print(bool error, const String &line)
      {
         AnsiString text;
         Unicode::WideToMultiByte(line, text);
         text += "\n";

#ifdef HM_PLATFORM_POSIX
         std::fputs(text.c_str(), error ? stderr : stdout);
         std::fflush(error ? stderr : stdout);
#else
         HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
         if (handle && handle != INVALID_HANDLE_VALUE)
         {
            DWORD written = 0;
            WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
         }
#endif
      }

      void ReportObjectError(int code, const String &where, const String &text)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, where, text);
      }

      // ---- The settings ---------------------------------------------------------

      struct StoreSettingsSnapshot
      {
         StoreSettingsSnapshot() : writes(false), cache_bytes(0) {}

         Client::Settings client;
         bool writes;
         std::string prefix;
         size_t cache_bytes;
      };

      StoreSettingsSnapshot ReadSnapshot()
      {
         StoreSettingsSnapshot snapshot;

         std::shared_ptr<PropertySet> settings = Configuration::Instance()->GetSettings();
         if (!settings)
            return snapshot;

         const std::string service = LowerAscii(TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_SERVICE))));
         if (service == "s3")
            snapshot.client.service = Client::ServiceS3;
         else if (service == "azure")
            snapshot.client.service = Client::ServiceAzure;
         else
            return snapshot;

         snapshot.client.endpoint = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_ENDPOINT)));
         snapshot.client.region = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_REGION)));
         snapshot.client.bucket = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_BUCKET)));
         snapshot.client.addressing = LowerAscii(TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_ADDRESSING))));
         snapshot.client.access_key = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_ACCESS_KEY)));
         snapshot.client.secret = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_SECRET)));
         snapshot.client.sas = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_SAS)));

         String caFile = settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_CA_FILE);
         caFile.Trim();
         snapshot.client.ca_file = caFile;

         std::string prefix = TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_OBJECT_PREFIX)));
         while (!prefix.empty() && prefix[0] == '/')
            prefix.erase(0, 1);
         if (!prefix.empty() && prefix[prefix.size() - 1] != '/')
            prefix += '/';
         snapshot.prefix = prefix;

         const long megabytes = settings->GetLong(PROPERTY_MESSAGE_STORE_OBJECT_CACHE_MB);
         snapshot.cache_bytes = megabytes <= 0 ? 0 : (size_t) (megabytes > 4096 ? 4096 : megabytes) * 1024 * 1024;

         snapshot.writes = LowerAscii(TrimmedText(Utf8Of(settings->GetString(PROPERTY_MESSAGE_STORE_BACKEND)))) == "object";
         return snapshot;
      }

      // ---- What was read a moment ago -------------------------------------------
      //
      // A key's size and ETag, and its bytes when they are small, for half a
      // minute: an IMAP FETCH asks whether a message is there, how large it is,
      // and for its header, its structure and its bytes, and each would be a
      // request of its own. Everything this server writes under a key is
      // forgotten here as it is written; another server sharing the store is
      // seen within the half minute (docs/ObjectStorage.md).

      class StoreObjectAnswers
      {
      public:

         void SetCapacity(size_t bytes)
         {
            boost::mutex::scoped_lock lock(lock_);
            if (capacity_ == bytes)
               return;
            capacity_ = bytes;
            entries_.clear();
            bytes_ = 0;
         }

         void Clear()
         {
            boost::mutex::scoped_lock lock(lock_);
            entries_.clear();
            bytes_ = 0;
         }

         bool FindProperties(const std::string &key, Client::Properties &properties)
         {
            boost::mutex::scoped_lock lock(lock_);
            std::map<std::string, Entry>::iterator found = Live_(key);
            if (found == entries_.end())
               return false;

            properties = found->second.properties;
            return true;
         }

         std::shared_ptr<const std::string> FindBytes(const std::string &key)
         {
            boost::mutex::scoped_lock lock(lock_);
            std::map<std::string, Entry>::iterator found = Live_(key);
            if (found == entries_.end())
               return std::shared_ptr<const std::string>();

            return found->second.bytes;
         }

         void Keep(const std::string &key, const Client::Properties &properties, const std::shared_ptr<const std::string> &bytes)
         {
            boost::mutex::scoped_lock lock(lock_);
            if (capacity_ == 0)
               return;

            Forget_(key);

            Entry entry;
            entry.properties = properties;
            entry.stamp = NowMs();
            if (bytes && bytes->size() <= capacity_ / 4)
            {
               entry.bytes = bytes;
               bytes_ += bytes->size();
            }
            entries_[key] = entry;

            // The oldest go until what is left fits, and never more than a few
            // thousand answers without bytes.
            while (!entries_.empty() && (bytes_ > capacity_ || entries_.size() > 4096))
            {
               std::map<std::string, Entry>::iterator oldest = entries_.begin();
               for (std::map<std::string, Entry>::iterator candidate = entries_.begin(); candidate != entries_.end(); ++candidate)
               {
                  if (candidate->second.stamp < oldest->second.stamp)
                     oldest = candidate;
               }
               Forget_(oldest->first);
            }
         }

         void Forget(const std::string &key)
         {
            boost::mutex::scoped_lock lock(lock_);
            Forget_(key);
         }

      private:

         struct Entry
         {
            Entry() : stamp(0) {}
            Client::Properties properties;
            std::shared_ptr<const std::string> bytes;
            long long stamp;
         };

         std::map<std::string, Entry>::iterator Live_(const std::string &key)
         {
            std::map<std::string, Entry>::iterator found = entries_.find(key);
            if (found == entries_.end())
               return found;

            if (NowMs() - found->second.stamp > CacheMilliseconds)
            {
               Forget_(key);
               return entries_.end();
            }

            return found;
         }

         void Forget_(const std::string &key)
         {
            std::map<std::string, Entry>::iterator found = entries_.find(key);
            if (found == entries_.end())
               return;

            if (found->second.bytes)
               bytes_ -= found->second.bytes->size();
            entries_.erase(found);
         }

         boost::mutex lock_;
         std::map<std::string, Entry> entries_;
         size_t bytes_ = 0;
         size_t capacity_ = 0;
      };

      // ---- A stored object, read in windows ---------------------------------------

      class ObjectReadStream : public MessageStoreBackendStream
      {
      public:

         ObjectReadStream(std::shared_ptr<Client> client, const std::string &key) :
            client_(client),
            key_(key),
            size_(0),
            position_(0),
            window_start_(0)
         {
         }

         // The first window, which is the whole object for all but the largest:
         // one request, and the size and the ETag with it. Every window after it
         // is asked for If-Match that ETag, so that an object rewritten in the
         // middle of a read fails the read instead of splicing two versions.
         Client::Outcome Open(const std::shared_ptr<const std::string> &cached)
         {
            if (cached)
            {
               whole_ = cached;
               size_ = (unsigned __int64) cached->size();
               return Client::OutcomeDone;
            }

            std::string bytes;
            unsigned __int64 total = 0;
            const Client::Outcome outcome = client_->Read(key_, 0, (unsigned __int64) FirstWindowBytes, bytes, total, std::string(), &etag_);
            if (outcome != Client::OutcomeDone)
               return outcome;

            size_ = total;
            if ((unsigned __int64) bytes.size() >= total)
               whole_ = std::make_shared<std::string>(bytes);
            else
            {
               window_.swap(bytes);
               window_start_ = 0;
            }

            return Client::OutcomeDone;
         }

         const std::shared_ptr<const std::string> &Whole() const { return whole_; }
         const std::string &Etag() const { return etag_; }

         unsigned __int64 GetSize() override
         {
            return size_;
         }

         bool SetPosition(unsigned __int64 position) override
         {
            if (position > size_)
               return false;

            position_ = position;
            return true;
         }

         std::shared_ptr<ByteBuffer> ReadChunk(int maxBytes) override
         {
            std::shared_ptr<ByteBuffer> chunk = std::shared_ptr<ByteBuffer>(new ByteBuffer);
            if (maxBytes <= 0 || position_ >= size_)
               return chunk;

            const unsigned __int64 left = size_ - position_;
            size_t wanted = (unsigned __int64) maxBytes < left ? (size_t) maxBytes : (size_t) left;

            if (whole_)
            {
               chunk->Add((const BYTE *) whole_->data() + (size_t) position_, wanted);
               position_ += wanted;
               return chunk;
            }

            while (wanted > 0)
            {
               if (position_ < window_start_ || position_ >= window_start_ + (unsigned __int64) window_.size())
               {
                  std::string bytes;
                  unsigned __int64 total = 0;
                  const Client::Outcome outcome = client_->Read(key_, position_, (unsigned __int64) ReadWindowBytes, bytes, total, etag_);
                  if (outcome != Client::OutcomeDone || bytes.empty())
                  {
                     // A message the row names, opened a moment ago, that
                     // cannot be read now: the store's condition, never a
                     // missing message.
                     MessageStore::NoteUnavailable();
                     return std::shared_ptr<ByteBuffer>();
                  }

                  window_.swap(bytes);
                  window_start_ = position_;
               }

               const size_t offset = (size_t) (position_ - window_start_);
               const size_t available = window_.size() - offset;
               const size_t taken = available < wanted ? available : wanted;

               chunk->Add((const BYTE *) window_.data() + offset, taken);
               position_ += taken;
               wanted -= taken;
            }

            return chunk;
         }

      private:

         std::shared_ptr<Client> client_;
         std::string key_;
         std::string etag_;
         unsigned __int64 size_;
         unsigned __int64 position_;
         std::shared_ptr<const std::string> whole_;
         std::string window_;
         unsigned __int64 window_start_;
      };

      // What a commit of one name came to.
      enum CommitResult
      {
         CommitDone,       // in the store, and gone from the disk
         CommitGone,       // nothing on the disk to commit: removed, moved or committed meanwhile
         CommitChanged,    // written again while it was put: kept, and put again later
         CommitHeld,       // put, and held open by a reader (Windows): taken off the disk on a later pass, not put again
         CommitFailed      // the store did not take it: kept, and tried again later
      };

      // ---- The backend ------------------------------------------------------------

      class MessageStoreObjectBackend : public MessageStoreBackend
      {
      public:

         MessageStoreObjectBackend() :
            configured_(false),
            checked_(0)
         {
         }

         // Whether a store is configured, the settings read again when they are
         // five seconds old.
         bool Refresh()
         {
            const long long now = NowMs();
            if (checked_.load() != 0 && now - checked_.load() < SettingsMilliseconds)
               return configured_.load();

            StoreSettingsSnapshot snapshot = ReadSnapshot();

            // Said after the lock is let go: a report runs the OnError event,
            // whose script may ask the store again on this thread.
            String unusable;
            {
               boost::mutex::scoped_lock lock(lock_);
               checked_ = now;

               if (snapshot.client.service == Client::ServiceNone)
               {
                  configured_ = false;
                  client_.reset();
                  attempted_ = false;
                  snapshot_ = snapshot;
                  return false;
               }

               if (!attempted_ || snapshot.client != snapshot_.client)
               {
                  String problem;
                  client_ = Client::Create(snapshot.client, problem);
                  problem_ = problem;
                  attempted_ = true;

                  // Another store, or the same one reached another way: nothing
                  // it said a moment ago is kept - keys do not name the bucket.
                  cache_.Clear();

                  if (!client_)
                     unusable = problem.IsEmpty() ? String(_T("the settings are incomplete")) : problem;
               }

               snapshot_ = snapshot;
               cache_.SetCapacity(snapshot.cache_bytes);
               configured_ = true;
            }

            if (!unusable.IsEmpty())
            {
               ErrorManager::Instance()->ReportError(ErrorManager::High, 7472, "MessageStoreObjects",
                  "The object store's settings do not describe a store this server can use: " + unusable + ". Nothing that is not on the local disk can be read, and nothing is put into the store: new mail stays on the local disk, where it is read from. Put the settings right; they are read again within five seconds.");
            }

            return true;
         }

         bool Writes()
         {
            boost::mutex::scoped_lock lock(lock_);
            return configured_.load() && snapshot_.writes;
         }

         std::shared_ptr<Client> ClientFor()
         {
            boost::mutex::scoped_lock lock(lock_);
            return client_;
         }

         String Problem()
         {
            boost::mutex::scoped_lock lock(lock_);
            return problem_;
         }

         std::string Prefix()
         {
            boost::mutex::scoped_lock lock(lock_);
            return snapshot_.prefix;
         }

         void Forget(const std::string &key)
         {
            cache_.Forget(key);
         }

         // The name's path under the data directory, as the name spells it; false
         // for a name outside it, and for one directly in it - a queue file.
         bool RelativeOf(const String &name, String &relative)
         {
            const String data = TrimSeparators(IniFileSettings::Instance()->GetDataDirectory());
            if (data.IsEmpty() || name.GetLength() <= data.GetLength() + 1)
               return false;

            const String head = name.Left(data.GetLength());
#ifdef HM_PLATFORM_POSIX
            if (head != data)
               return false;
#else
            if (head.CompareNoCase(data) != 0)
               return false;
#endif

            const int separator = (int) name[data.GetLength()];
            if (separator != '\\' && separator != '/')
               return false;

            relative = name.Mid(data.GetLength() + 1);
            while (relative.Left(1) == _T("\\") || relative.Left(1) == _T("/"))
               relative = relative.Mid(1);

            // Directly in the data directory: the queue, which stays on the disk.
            return relative.Find(_T("\\")) >= 0 || relative.Find(_T("/")) >= 0;
         }

         // A name's key: its path under the data directory, '/' between the
         // parts, lower-cased as StoreDirectoryName lower-cases a directory on
         // POSIX - so that a tree from a Windows server and one from a Linux
         // server give the same keys - UTF-8, after the prefix. False for a
         // name that stays on the local disk: a queue file, a name outside the
         // data directory, and a name staged beside another, which is a
         // working file of the one it is beside and never an object.
         bool KeyOf(const String &name, std::string &key)
         {
            String relative;
            if (!RelativeOf(name, relative))
               return false;

            String lowered = relative;
            lowered.ToLower();

            std::string path = Utf8Of(lowered);
            for (size_t i = 0; i < path.size(); i++)
            {
               if (path[i] == '\\')
                  path[i] = '/';
            }

            const size_t slash = path.rfind('/');
            const std::string fileName = slash == std::string::npos ? path : path.substr(slash + 1);
            if (IsStagedName(fileName))
               return false;

            key = Prefix() + path;
            return true;
         }

         // A directory's prefix of keys: every key under it begins with it.
         bool PrefixOf(const String &directory, std::string &prefix)
         {
            std::string key;
            if (!KeyOf(TrimSeparators(directory) + FileUtilities::PathSeparator + _T("x"), key) || key.size() < 1)
               return false;

            prefix = key.substr(0, key.size() - 1);
            return true;
         }

         // A key's name again, under the data directory.
         String NameOf(const std::string &key)
         {
            std::string relative = key;
            const std::string prefix = Prefix();
            if (!prefix.empty() && relative.compare(0, prefix.size(), prefix) == 0)
               relative = relative.substr(prefix.size());

            String name = WideOfUtf8(relative);
#ifndef HM_PLATFORM_POSIX
            name.Replace(_T("/"), _T("\\"));
#endif
            return FileUtilities::Combine(TrimSeparators(IniFileSettings::Instance()->GetDataDirectory()), name);
         }

         // A blob of the parts held once (MessageStoreBlobs): a counted row of
         // its own, handed to the committer as it is written.
         bool IsBlobName_(const String &name)
         {
            String relative;
            if (!RelativeOf(name, relative))
               return false;

            relative.ToLower();
            return relative.Left(6) == _T("blobs\\") || relative.Left(6) == _T("blobs/");
         }

         // The key's size and ETag, from what was read a moment ago or the store.
         Client::Outcome Head_(const std::shared_ptr<Client> &client, const std::string &key, Client::Properties &properties)
         {
            if (cache_.FindProperties(key, properties))
               return Client::OutcomeDone;

            const Client::Outcome outcome = client->Head(key, properties);
            if (outcome == Client::OutcomeDone)
               cache_.Keep(key, properties, std::shared_ptr<const std::string>());
            return outcome;
         }

         // A client, or none - said on this thread as the store being unavailable.
         std::shared_ptr<Client> ClientOrNote_()
         {
            std::shared_ptr<Client> client = ClientFor();
            if (!client)
               MessageStore::NoteUnavailable();
            return client;
         }

         // Anything but the object or a confirmed 404 is the store's condition:
         // a redirect, a 400, a 405 from a proxy, a 409 - never a message that
         // is gone (docs/ObjectStorage.md).
         static void NoteIf_(Client::Outcome outcome)
         {
            if (outcome != Client::OutcomeDone && outcome != Client::OutcomeAbsent)
               MessageStore::NoteUnavailable();
         }

         MessageStoreBackend &Local()
         {
            return MessageStore::LocalBackend();
         }

         // Every byte of a local file, read for a put.
         static Client::Source FileSource(const std::shared_ptr<File> &file)
         {
            return [file](unsigned __int64 offset, size_t length, std::string &bytes)
            {
               bytes.clear();
               if (offset > (unsigned __int64) INT_MAX || length > (size_t) INT_MAX)
                  return false;

               try
               {
                  if (!file->SetPosition((int) offset))
                     return false;

                  std::shared_ptr<ByteBuffer> chunk = file->ReadChunk((int) length);
                  if (!chunk || chunk->GetSize() != length)
                     return false;

                  bytes.assign(chunk->GetCharBuffer(), chunk->GetSize());
                  return true;
               }
               catch (...)
               {
                  return false;
               }
            };
         }

         // A local file put under a key whole.
         Client::Outcome PutFile_(const std::shared_ptr<Client> &client, const String &name, const std::string &key, String &problem)
         {
            unsigned __int64 size = 0;
            if (!FileUtilities::FileSize64(name, size))
            {
               problem = _T("the local file could not be sized");
               return Client::OutcomeFailed;
            }

            if (size > (unsigned __int64) INT_MAX)
            {
               problem = _T("it is larger than this server reads a file in parts (2 GB)");
               return Client::OutcomeFailed;
            }

            std::shared_ptr<File> file = std::make_shared<File>();
            try
            {
               if (!file->Open(name, File::OTReadOnly))
               {
                  problem = _T("the local file could not be opened");
                  return Client::OutcomeFailed;
               }
            }
            catch (...)
            {
               problem = _T("the local file could not be opened");
               return Client::OutcomeFailed;
            }

            std::string md5;
            const Client::Outcome outcome = client->Put(key, size, FileSource(file), md5);
            file->Close();

            cache_.Forget(key);
            if (outcome != Client::OutcomeDone)
               problem = Client::LastProblem();
            return outcome;
         }

         // ---- One name at a time ----------------------------------------------------
         //
         // A write, a move, a copy onto a name or a removal of one, and the
         // committer's last look at the file it put before it removes it, are
         // one at a time for each name: a file written in the moment between
         // the committer's put and its removal of the file is never removed.

         static const size_t NameLockCount = 64;

         boost::mutex &NameLock_(const std::string &key)
         {
            return name_locks_[std::hash<std::string>()(key) % NameLockCount];
         }

         class NamesLocked
         {
         public:
            NamesLocked(MessageStoreObjectBackend &backend, const std::string *one, const std::string *two) :
               first_(nullptr),
               second_(nullptr)
            {
               boost::mutex *a = one ? &backend.NameLock_(*one) : nullptr;
               boost::mutex *b = two ? &backend.NameLock_(*two) : nullptr;
               if (!a)
               {
                  a = b;
                  b = nullptr;
               }
               if (a == b)
                  b = nullptr;
               if (a && b && b < a)
                  std::swap(a, b);

               first_ = a;
               second_ = b;
               if (first_)
                  first_->lock();
               if (second_)
                  second_->lock();
            }

            ~NamesLocked()
            {
               if (second_)
                  second_->unlock();
               if (first_)
                  first_->unlock();
            }

            NamesLocked(const NamesLocked &) = delete;
            NamesLocked &operator=(const NamesLocked &) = delete;

         private:
            boost::mutex *first_;
            boost::mutex *second_;
         };

         // ---- MessageStoreBackend ---------------------------------------------------

         bool Exists(const String &name) override
         {
            std::string key;
            if (!KeyOf(name, key))
               return Local().Exists(name);

            // On the disk; or, committed and taken off it - even in the moment
            // between two looks at it - in the store.
            if (FileUtilities::Exists(name))
               return true;

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            Client::Properties properties;
            const Client::Outcome outcome = Head_(client, key, properties);
            NoteIf_(outcome);
            return outcome == Client::OutcomeDone;
         }

         bool Size(const String &name, unsigned __int64 &size) override
         {
            size = 0;

            std::string key;
            if (!KeyOf(name, key))
               return Local().Size(name, size);

            // On the disk - unless committed and removed in the moment between.
            if (FileUtilities::Exists(name))
            {
               if (Local().Size(name, size))
                  return true;
               if (FileUtilities::Exists(name))
                  return false;
            }

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            Client::Properties properties;
            const Client::Outcome outcome = Head_(client, key, properties);
            NoteIf_(outcome);
            if (outcome != Client::OutcomeDone)
               return false;

            size = properties.size;
            return true;
         }

         std::shared_ptr<MessageStoreBackendStream> OpenRead(const String &name) override
         {
            std::string key;
            if (!KeyOf(name, key))
               return Local().OpenRead(name);

            if (FileUtilities::Exists(name))
            {
               std::shared_ptr<MessageStoreBackendStream> local = Local().OpenRead(name);
               if (local || FileUtilities::Exists(name))
                  return local;
            }

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return std::shared_ptr<MessageStoreBackendStream>();

            std::shared_ptr<ObjectReadStream> stream = std::make_shared<ObjectReadStream>(client, key);
            const Client::Outcome outcome = stream->Open(cache_.FindBytes(key));
            NoteIf_(outcome);
            if (outcome != Client::OutcomeDone)
               return std::shared_ptr<MessageStoreBackendStream>();

            if (stream->Whole() && !stream->Etag().empty())
            {
               Client::Properties properties;
               properties.size = (unsigned __int64) stream->Whole()->size();
               properties.etag = stream->Etag();
               cache_.Keep(key, properties, stream->Whole());
            }

            return stream;
         }

         bool ReadAll(const String &name, AnsiString &bytes, unsigned __int64 maxBytes) override
         {
            bytes.Empty();

            std::string key;
            if (!KeyOf(name, key))
               return Local().ReadAll(name, bytes, maxBytes);

            if (FileUtilities::Exists(name))
            {
               if (Local().ReadAll(name, bytes, maxBytes))
                  return true;
               if (FileUtilities::Exists(name))
                  return false;
            }

            std::shared_ptr<const std::string> cached = cache_.FindBytes(key);
            if (cached)
            {
               const size_t length = maxBytes > 0 && maxBytes < (unsigned __int64) cached->size() ? (size_t) maxBytes : cached->size();
               bytes = AnsiString(cached->data(), length);
               return true;
            }

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            std::string read;
            std::string etag;
            unsigned __int64 total = 0;
            const Client::Outcome outcome = client->Read(key, 0, maxBytes, read, total, std::string(), &etag);
            NoteIf_(outcome);
            if (outcome != Client::OutcomeDone)
               return false;

            if (maxBytes == 0 || (unsigned __int64) read.size() >= total)
            {
               std::shared_ptr<const std::string> whole = std::make_shared<std::string>(read);
               if (!etag.empty())
               {
                  Client::Properties properties;
                  properties.size = total;
                  properties.etag = etag;
                  cache_.Keep(key, properties, whole);
               }
               bytes = AnsiString(whole->data(), whole->size());
               return true;
            }

            bytes = AnsiString(read.data(), read.size());
            return true;
         }

         // Every write is a local one, under the name, and the name is handed
         // to the committer afterwards - never while it is a new name whose
         // row is not saved yet (MessageStoreObjects::Placing).
         bool WriteAtomically(const String &name, const AnsiString &bytes) override
         {
            std::string key;
            if (!KeyOf(name, key))
               return Local().WriteAtomically(name, bytes);

            // Written beside the name outside its lock, and renamed over it
            // under the lock quietly - and, when that did not take, again the
            // way that waits and says why, outside it (a rename over the name is
            // one the committer tells apart either way: CommitNow_).
            DirectoryQuietly(FileUtilities::GetFilePath(name));
            const String temporary = name + _T(".writing-") + GUIDCreator::GetGUID();

            bool written = false;
            {
               File file;
               try
               {
                  // The close is part of the write, as in the local backend's
                  // WriteAtomically (File::Close).
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

            if (!written)
            {
               FileUtilities::DeleteFile(temporary);
               return false;
            }

            bool renamed = false;
            {
               NamesLocked locked(*this, &key, nullptr);
               renamed = RenameQuietly(temporary, name);
            }

            if (!renamed && !FileUtilities::Move(temporary, name))
            {
               FileUtilities::DeleteFile(temporary);
               return false;
            }

            cache_.Forget(key);
            Written_(name);
            return true;
         }

         bool Copy(const String &from, const String &to) override
         {
            std::string fromKey;
            std::string toKey;
            const bool toKeyed = KeyOf(to, toKey);

            // A name that is never an object - a queue file, a staged one - is
            // copied on the disk, where it is.
            if (!KeyOf(from, fromKey))
            {
               bool copied = false;
               {
                  NamesLocked locked(*this, toKeyed ? &toKey : nullptr, nullptr);
                  copied = CopyQuietly(from, to);
               }

               // Tried again the way that waits and says why, outside the lock.
               if (!copied)
                  copied = Local().Copy(from, to);

               if (!copied)
                  return false;

               if (toKeyed)
               {
                  cache_.Forget(toKey);
                  Written_(to);
               }
               return true;
            }

            // A local file is copied on the disk under its name's lock, which
            // the committer takes before it takes a file off the disk; one it
            // took a moment ago is copied from the store below.
            bool wasLocal = false;
            bool copiedHere = false;
            {
               NamesLocked locked(*this, &fromKey, toKeyed ? &toKey : nullptr);
               if (FileUtilities::Exists(from))
               {
                  wasLocal = true;
                  copiedHere = CopyQuietly(from, to);
               }
            }

            if (wasLocal)
            {
               // Tried again the way that waits and says why, outside the lock.
               if (!copiedHere && FileUtilities::Exists(from))
                  copiedHere = Local().Copy(from, to);

               if (copiedHere)
               {
                  if (toKeyed)
                  {
                     cache_.Forget(toKey);
                     Written_(to);
                  }
                  return true;
               }

               if (FileUtilities::Exists(from))
                  return false;
            }

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            // An object onto another name in the store, by the store.
            if (toKeyed && Writes())
            {
               const Client::Outcome outcome = client->Copy(fromKey, toKey);
               cache_.Forget(toKey);
               NoteIf_(outcome);
               if (outcome != Client::OutcomeDone)
                  return false;

               // A file under the name on the disk is older than what the
               // store now holds under it, and would be read first: removed
               // quietly under the lock, and again the loud way outside it.
               {
                  NamesLocked locked(*this, &toKey, nullptr);
                  if (FileUtilities::Exists(to))
                     RemoveQuietly(to);
               }
               if (FileUtilities::Exists(to))
                  FileUtilities::DeleteFile(to);
               return true;
            }

            // Out of the store to the disk: the queue, an export, a staged
            // name, or a server that stores new mail on the disk again -
            // written as WriteAtomically writes, quietly under the lock.
            AnsiString bytes;
            if (!ReadAll(from, bytes, 0))
               return false;

            return WriteAtomically(to, bytes);
         }

         bool Move(const String &from, const String &to) override
         {
            std::string fromKey;
            std::string toKey;
            const bool fromKeyed = KeyOf(from, fromKey);
            const bool toKeyed = KeyOf(to, toKey);

            // A name that is never an object is moved on the disk; a keyed one
            // on the disk is moved there under both names' locks, which the
            // committer takes before it takes a file off the disk - one it took
            // a moment ago is moved in the store below.
            bool wasLocal = !fromKeyed;
            bool moved = false;
            {
               NamesLocked locked(*this, fromKeyed ? &fromKey : nullptr, toKeyed ? &toKey : nullptr);
               if (!fromKeyed || FileUtilities::Exists(from))
               {
                  wasLocal = true;
                  moved = MoveQuietly(from, to);
               }
            }

            if (wasLocal)
            {
               // Tried again the way that waits and says why, outside the lock.
               if (!moved && FileUtilities::Exists(from))
                  moved = Local().Move(from, to);

               if (!moved && (!fromKeyed || FileUtilities::Exists(from)))
                  return false;
            }

            if (moved)
            {
               if (toKeyed)
               {
                  cache_.Forget(toKey);
                  Written_(to);
               }

               // The name moved away from may still have an object in the
               // store - one committed before the file now moved was written:
               // it goes too, so that the old name holds nothing. One the store
               // does not remove now is named by no row, and costs space only.
               if (fromKeyed && Writes())
               {
                  std::shared_ptr<Client> client = ClientFor();
                  if (client)
                     client->Remove(fromKey);
                  cache_.Forget(fromKey);
               }
               return true;
            }

            if (!Copy(from, to))
               return false;

            // Copied, then the old key removed. A crash between the two leaves
            // the object under both names, never under neither; the old one is
            // named by no row and costs space only (docs/ObjectStorage.md).
            std::shared_ptr<Client> client = ClientFor();
            const Client::Outcome outcome = client ? client->Remove(fromKey) : Client::OutcomeUnavailable;
            cache_.Forget(fromKey);
            if (outcome != Client::OutcomeDone)
            {
               ReportObjectError(7475, _T("MessageStoreObjects::Move"),
                  "The object " + WideOfUtf8(fromKey) + " was copied to " + to + " and could not be removed afterwards (" + Client::LastProblem() + "). The message is under its new name; the old object is left in the store, named by no row.");
            }

            return true;
         }

         bool Link(const String &existing, const String &link) override
         {
            // An object store has no second name for an object, and every
            // caller copies when this answers false (docs/ObjectStorage.md).
            // While the store is only read, a local file is linked on the disk
            // as it always was.
            if (!Writes() && FileUtilities::Exists(existing))
               return Local().Link(existing, link);

            return false;
         }

         bool Remove(const String &name) override
         {
            std::string key;
            if (!KeyOf(name, key))
               return Local().Remove(name);

            // Quietly under the name's lock. A file the committer's own put
            // holds open, which Windows will not remove under it, is marked for
            // the committer, which removes it - and what it put - when its put
            // ends; any other that will not go is tried again the way that
            // waits and says why, outside the lock.
            bool localGone = true;
            bool leftToCommitter = false;
            {
               NamesLocked locked(*this, &key, nullptr);
               if (FileUtilities::Exists(name) && !RemoveQuietly(name))
               {
                  boost::mutex::scoped_lock lock(pending_lock_);
                  if (in_flight_.find(name) != in_flight_.end())
                  {
                     tombstones_.insert(name);
                     leftToCommitter = true;
                  }
               }
            }

            if (!leftToCommitter && FileUtilities::Exists(name))
               localGone = Local().Remove(name);
            cache_.Forget(key);

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            const Client::Outcome outcome = client->Remove(key);
            cache_.Forget(key);
            NoteIf_(outcome);
            return localGone && outcome == Client::OutcomeDone;
         }

         bool Identity(const String &name, AnsiString &identity, unsigned int &names) override
         {
            identity.Empty();
            names = 0;

            std::string key;
            if (!KeyOf(name, key))
               return Local().Identity(name, identity, names);

            if (FileUtilities::Exists(name))
            {
               if (Local().Identity(name, identity, names))
                  return true;
               if (FileUtilities::Exists(name))
                  return false;
            }

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            // Which object it is is the store's ETag, never a size alone: an
            // answer kept without one - what this server wrote or read without
            // being told it - is asked again.
            Client::Properties properties;
            Client::Outcome outcome = Client::OutcomeDone;
            if (!cache_.FindProperties(key, properties) || properties.etag.empty())
            {
               outcome = client->Head(key, properties);
               if (outcome == Client::OutcomeDone)
                  cache_.Keep(key, properties, cache_.FindBytes(key));
            }

            NoteIf_(outcome);
            if (outcome != Client::OutcomeDone || properties.etag.empty())
               return false;

            // The key and what the store says its bytes are now, so that a
            // rewrite is another identity. One name each.
            identity = AnsiString(("object:" + key + ":" + properties.etag + ":" + std::to_string((unsigned long long) properties.size)).c_str());
            names = 1;
            return true;
         }

         bool IsLocal(const String &name) override
         {
            std::string key;
            return !KeyOf(name, key) || FileUtilities::Exists(name);
         }

         void Commit(const String &name) override
         {
            EnqueueCommit_(name, 0);
         }

         bool List(const String &directory, std::vector<String> &names) override
         {
            names.clear();
            MessageStoreBackend::List(directory, names);

            std::string prefix;
            if (!PrefixOf(directory, prefix))
               return true;

            std::shared_ptr<Client> client = ClientOrNote_();
            if (!client)
               return false;

            std::set<std::string> known;
            for (size_t i = 0; i < names.size(); i++)
            {
               std::string key;
               if (KeyOf(names[i], key))
                  known.insert(key);
            }

            std::vector<Client::Listed> objects;
            const Client::Outcome outcome = client->List(prefix, objects);
            NoteIf_(outcome);
            if (outcome != Client::OutcomeDone)
               return false;

            for (size_t i = 0; i < objects.size(); i++)
            {
               // A key is the store's, which the service's account may write: one
               // whose name would lead out of the data directory - a part that is
               // ".." - names no file of this server's, and is never listed as one
               // (MessageStoreMaintenance::IsInDataDirectory).
               if (!known.insert(objects[i].key).second)
                  continue;

               const String name = NameOf(objects[i].key);
               if (MessageStoreMaintenance::IsInDataDirectory(name))
                  names.push_back(name);
            }

            return true;
         }

         // ---- The committer ---------------------------------------------------------

         // A directory held off the committer (MessageStoreObjects::HoldTree):
         // taken once no name under it is being put, two minutes at most.
         void HoldTree(const String &directory)
         {
            const String root = TrimSeparators(directory);
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               held_trees_.push_back(root);
            }

            const long long until = NowMs() + 120 * 1000;
            for (;;)
            {
               {
                  boost::mutex::scoped_lock lock(pending_lock_);
                  bool busy = false;
                  for (std::set<String>::const_iterator name = in_flight_.begin(); name != in_flight_.end() && !busy; ++name)
                     busy = UnderTree_(*name, root);
                  if (!busy)
                     return;
               }

               if (NowMs() > until)
                  return;

               std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
         }

         void ReleaseTree(const String &directory)
         {
            const String root = TrimSeparators(directory);
            boost::mutex::scoped_lock lock(pending_lock_);
            for (std::vector<String>::iterator held = held_trees_.begin(); held != held_trees_.end(); ++held)
            {
               if (*held == root)
               {
                  held_trees_.erase(held);
                  return;
               }
            }
         }

         // Every file on the local disk under a directory, handed over.
         void EnqueueLocalTree(const String &directory)
         {
            if (!Writes())
               return;

            std::vector<String> names;
            Local().List(directory, names);
            for (size_t i = 0; i < names.size(); i++)
            {
               std::string key;
               if (KeyOf(names[i], key))
                  EnqueueCommit_(names[i], WriteDelayMilliseconds);
            }
         }

         // The journal read once, after a start: what was handed over and not
         // yet in the store when the server stopped. Read and merged under the
         // journal's lock, which a compaction takes too, so that none empties
         // the journal while it is being read.
         void LoadJournal()
         {
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (journal_loaded_)
                  return;
            }

            boost::mutex::scoped_lock journal(journal_lock_);
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (journal_loaded_)
                  return;
            }

            const String path = JournalPath_();
            AnsiString bytes;
            if (!path.IsEmpty() && FileUtilities::Exists(path))
               Local().ReadAll(path, bytes, 0);

            std::vector<String> names;
            const std::string text(bytes.data(), bytes.size());
            size_t start = 0;
            while (start < text.size())
            {
               size_t end = text.find('\n', start);
               if (end == std::string::npos)
                  end = text.size();

               const std::string line = TrimmedText(text.substr(start, end - start));
               if (!line.empty())
                  names.push_back(WideOfUtf8(line));
               start = end + 1;
            }

            std::vector<String> present;
            std::set<String> directories;
            for (size_t i = 0; i < names.size(); i++)
            {
               directories.insert(FileUtilities::GetFilePath(names[i]));
               if (FileUtilities::Exists(names[i]) || RecoverSetAside_(names[i]))
                  present.push_back(names[i]);
            }

            // What a crash left set aside and nobody needs: a day old or more.
            for (std::set<String>::const_iterator directory = directories.begin(); directory != directories.end(); ++directory)
               SweepSetAside_(*directory);

            const long long due = NowMs() + WriteDelayMilliseconds;
            boost::mutex::scoped_lock lock(pending_lock_);
            journal_lines_ += names.size();
            for (size_t i = 0; i < present.size(); i++)
            {
               if (pending_.find(present[i]) != pending_.end())
                  continue;

               PendingCommit item;
               item.due = due;
               pending_[present[i]] = item;
            }
            journal_loaded_ = true;
         }

         // A name whose file is gone, and which a crash between the committer
         // setting it aside and removing it left beside it (name.store-{GUID}):
         // the newest put back under the name and handed over again, the
         // others removed. Called from LoadJournal, before any pass runs.
         bool RecoverSetAside_(const String &name)
         {
            const boost::filesystem::path path = FsPath(name);
            const std::wstring mark = path.filename().wstring() + L".store-";

            std::vector<boost::filesystem::path> found;
            boost::system::error_code error;
            for (boost::filesystem::directory_iterator entry(path.parent_path(), error);
                 !error && entry != boost::filesystem::directory_iterator(); entry.increment(error))
            {
               const std::wstring file = entry->path().filename().wstring();
               if (file.compare(0, mark.size(), mark) == 0 && IsStagedName(Utf8Of(String(file))))
                  found.push_back(entry->path());
            }

            if (found.empty())
               return false;

            size_t newest = 0;
            for (size_t i = 1; i < found.size(); i++)
            {
               boost::system::error_code a;
               boost::system::error_code b;
               if (boost::filesystem::last_write_time(found[i], a) > boost::filesystem::last_write_time(found[newest], b))
                  newest = i;
            }

            bool back = false;
            for (size_t i = 0; i < found.size(); i++)
            {
               const String candidate = String(found[i].wstring());
               if (i == newest && !FileUtilities::Exists(name))
                  back = RenameQuietly(candidate, name);
               else
                  RemoveQuietly(candidate);
            }

            return back;
         }

         // A day-old name.store-{GUID} in a directory the journal names.
         void SweepSetAside_(const String &directory)
         {
            const std::time_t dayAgo = std::time(nullptr) - 24 * 60 * 60;

            boost::system::error_code error;
            for (boost::filesystem::directory_iterator entry(FsPath(directory), error);
                 !error && entry != boost::filesystem::directory_iterator(); entry.increment(error))
            {
               const std::wstring file = entry->path().filename().wstring();
               if (file.find(L".store-") == std::wstring::npos || !IsStagedName(Utf8Of(String(file))))
                  continue;

               boost::system::error_code when;
               const std::time_t written = boost::filesystem::last_write_time(entry->path(), when);
               if (!when && written < dayAgo)
                  RemoveQuietly(String(entry->path().wstring()));
            }
         }

         // A pass of the committer on the maintenance queue, unless one is
         // running. One that has run ten minutes - a large put over a slow
         // link - does not hold up the rest: another is started, said in the
         // application log, and takes only names no other pass holds
         // (in_flight_), so that no name is ever put by two at once.
         void DispatchCommits()
         {
            bool beside = false;
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (pending_.empty())
                  return;

               const long long now = NowMs();
               if (passes_ > 0 && now - pass_started_ < DrainStuckMilliseconds)
                  return;

               beside = passes_ > 0;
               passes_++;
               pass_started_ = now;
            }

            if (beside)
            {
               LOG_APPLICATION("Message store: the object store's committer has been on one pass for more than ten minutes; another is started beside it, for the names the first one does not hold.");
            }

            std::shared_ptr<WorkQueue> queue = Application::Instance()->GetMaintenanceWorkQueue();
            if (!queue)
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (passes_ > 0)
                  passes_--;
               return;
            }

            class CommitTask : public Task
            {
            public:
               CommitTask(MessageStoreObjectBackend &backend) : Task("MessageStoreObjectCommit"), backend_(backend) {}
               virtual void DoWork() { backend_.DrainCommits_(); }
            private:
               MessageStoreObjectBackend &backend_;
            };

            queue->AddTask(std::shared_ptr<Task>(new CommitTask(*this)));
         }

         void DrainCommits_()
         {
            bool unexpected = false;
            try
            {
               Drain_();
            }
            catch (const boost::thread_interrupted &)
            {
               // The server stopping: passed on, as the scheduler passes it.
               boost::mutex::scoped_lock lock(pending_lock_);
               if (passes_ > 0)
                  passes_--;
               throw;
            }
            catch (...)
            {
               // Nothing is lost when a pass ends early: what it did not take
               // is still on the list and on the disk. Said below, once the pass
               // is counted out.
               unexpected = true;
            }

            bool report = false;
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (passes_ > 0)
                  passes_--;

               // Once an hour at most: a cause that ends one pass ends the next,
               // a minute later.
               const long long now = NowMs();
               if (unexpected && (unexpected_reported_ == 0 || now - unexpected_reported_ >= PendingReportMilliseconds))
               {
                  unexpected_reported_ = now;
                  report = true;
               }
            }

            // With the list's lock let go, since a report runs the OnError event.
            if (report)
            {
               ReportObjectError(7473, _T("MessageStoreObjects::Commit"),
                  _T("A pass of the object store's committer ended at an unexpected error. Nothing is lost: what it had not put in waits on the local disk, is read from there, and the next pass takes it up. Said once an hour at most."));
            }
         }

      private:

         struct PendingCommit
         {
            PendingCommit() : due(0), tries(0), handed(0) {}
            long long due;
            int tries;
            unsigned int handed;              // how often it was handed over again while on the list
            AnsiString put_stamp;             // the file a put was made of, when it could not be taken off the disk after it
            std::string put_key;              // the key it was put under
            std::weak_ptr<Client> put_by;     // and the client that made that put
         };

         // The names a pass holds: never taken by another pass, and let go when
         // the pass ends, however it ends.
         class InFlight
         {
         public:
            explicit InFlight(MessageStoreObjectBackend &backend) : backend_(backend) {}

            ~InFlight()
            {
               boost::mutex::scoped_lock lock(backend_.pending_lock_);
               for (std::set<String>::const_iterator name = held_.begin(); name != held_.end(); ++name)
                  backend_.in_flight_.erase(*name);
            }

            // Both with pending_lock_ held.
            void Hold(const String &name)
            {
               backend_.in_flight_.insert(name);
               held_.insert(name);
            }

            void Release(const String &name)
            {
               backend_.in_flight_.erase(name);
               held_.erase(name);
            }

            InFlight(const InFlight &) = delete;
            InFlight &operator=(const InFlight &) = delete;

         private:
            MessageStoreObjectBackend &backend_;
            std::set<String> held_;
         };

         // What a pass took of an entry.
         struct TakenCommit
         {
            TakenCommit() : handed(0) {}
            String name;
            unsigned int handed;
            AnsiString put_stamp;
            std::string put_key;
            std::weak_ptr<Client> put_by;
         };

         // A write through the backend handed over - unless it is a new name
         // being made, whose row's save hands it over instead; a blob always.
         void Written_(const String &name)
         {
            if (placing_depth == 0 || IsBlobName_(name))
               EnqueueCommit_(name, WriteDelayMilliseconds);
         }

         void EnqueueCommit_(const String &name, long long delay)
         {
            if (!Writes() || !FileUtilities::Exists(name))
               return;

            LoadJournal();

            const long long due = NowMs() + delay;
            bool added = false;
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               std::map<String, PendingCommit>::iterator found = pending_.find(name);
               if (found == pending_.end())
               {
                  PendingCommit item;
                  item.due = due;
                  pending_[name] = item;
                  added = true;
               }
               else
               {
                  // Written again while on the list: a pass that took it before
                  // this does not take it off (Drain_), and a put made of the
                  // file before is not the file now.
                  found->second.handed++;
                  found->second.put_stamp.Empty();
                  if (found->second.tries == 0 && due < found->second.due)
                     found->second.due = due;
               }
            }

            // Written with the list let go: a journal a scanner holds for a
            // moment holds up this writer, never every other.
            if (added)
               AppendJournal_(name);
         }

         String JournalPath_()
         {
            const String data = TrimSeparators(IniFileSettings::Instance()->GetDataDirectory());
            if (data.IsEmpty())
               return String();

            return FileUtilities::Combine(FileUtilities::Combine(data, _T("_ObjectStore")), _T("pending.txt"));
         }

         // One line appended, under the journal's lock, with file calls that
         // neither wait nor report.
         void AppendJournal_(const String &name)
         {
            const String path = JournalPath_();
            if (path.IsEmpty())
               return;

            AnsiString line;
            Unicode::WideToMultiByte(name, line);
            line += "\n";

            boost::mutex::scoped_lock journal(journal_lock_);
            DirectoryQuietly(FileUtilities::GetFilePath(path));

            try
            {
               File file;
               if (file.Open(path, File::OTAppend))
               {
                  // The line is written when the close has put it there
                  // (File::Close).
                  bool written = file.Write(line);
                  written = file.Close() && written;

                  if (written)
                  {
                     boost::mutex::scoped_lock lock(pending_lock_);
                     journal_lines_++;
                  }
               }
            }
            catch (...)
            {
               // The name is on the list in memory still; only a restart before
               // it is committed leaves it on the disk uncommitted, where it is
               // read from, for the copy verb to put in.
            }
         }

         // The journal emptied when nothing is pending, and written again when
         // it has grown far past what is: decided under the list's lock, done
         // under the journal's alone, with file calls that neither wait nor
         // report.
         void CompactJournal_()
         {
            const String path = JournalPath_();
            if (path.IsEmpty())
               return;

            boost::mutex::scoped_lock journal(journal_lock_);

            bool removeIt = false;
            AnsiString all;
            size_t lines = 0;
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               if (!journal_loaded_)
                  return;

               if (pending_.empty())
               {
                  if (journal_lines_ == 0)
                     return;
                  removeIt = true;
                  journal_lines_ = 0;
               }
               else
               {
                  if (journal_lines_ <= pending_.size() * 4 + 1024)
                     return;

                  for (std::map<String, PendingCommit>::const_iterator item = pending_.begin(); item != pending_.end(); ++item)
                  {
                     AnsiString line;
                     Unicode::WideToMultiByte(item->first, line);
                     all += line;
                     all += "\n";
                  }
                  lines = pending_.size();
               }
            }

            if (removeIt)
            {
               RemoveQuietly(path);
               return;
            }

            const String temporary = path + _T(".writing-") + GUIDCreator::GetGUID();
            bool written = false;
            try
            {
               // A short journal must not take the name of the whole one: the
               // close is part of the write (File::Close).
               File file;
               written = file.Open(temporary, File::OTCreate) && file.Write(all);
               written = file.Close() && written;
            }
            catch (...)
            {
               written = false;
            }

            if (written && RenameQuietly(temporary, path))
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               journal_lines_ = lines;
            }
            else
               RemoveQuietly(temporary);
         }

         static long long RetryAfter_(int tries)
         {
            long long wait = RetryFirstMilliseconds;
            for (int i = 1; i < tries && wait < RetryLongestMilliseconds; i++)
               wait *= 2;
            return wait < RetryLongestMilliseconds ? wait : RetryLongestMilliseconds;
         }

         // One name put into the store and then taken off the disk - unless it
         // was written again, removed or moved meanwhile.
         //
         // The file is never deleted where it is: under the name's lock it is
         // renamed to a name of this pass's own, and only that is deleted. So
         // a writer that renames its own file over the name - MessageData::
         // Write through WriteToFileAtomically, a rule's or a script's rewrite -
         // either does so before the rename, which the stamp taken again of the
         // file renamed away tells, and that file goes back under the name; or
         // after it, when its file is a new one under the name, which is never
         // touched. Every write through this backend takes the same lock. The
         // tree's writers that write into a file in place (WriteToFile, File's
         // OTAppend) write working files and new names, which are handed over
         // only once their rows are saved, so no such write is ever made to a
         // name the committer holds.
         CommitResult CommitNow_(const std::shared_ptr<Client> &client, const TakenCommit &taken, String &problem, AnsiString &putStamp, std::string &putKey)
         {
            const String &name = taken.name;
            putStamp.Empty();
            putKey.clear();

            std::string key;
            if (!KeyOf(name, key))
               return CommitGone;

            // Deleted while an earlier put held it open (Remove): removed now,
            // with what was put, and not put again.
            bool marked = false;
            {
               boost::mutex::scoped_lock lock(pending_lock_);
               marked = tombstones_.find(name) != tombstones_.end();
            }
            if (marked)
            {
               {
                  NamesLocked locked(*this, &key, nullptr);
                  if (FileUtilities::Exists(name) && !RemoveQuietly(name))
                     return CommitChanged;

                  boost::mutex::scoped_lock lock(pending_lock_);
                  tombstones_.erase(name);
               }

               if (!IsBlobName_(name))
                  client->Remove(key);
               cache_.Forget(key);
               return CommitGone;
            }

            const AnsiString before = LocalStampOf(name);
            const AnsiString beforeIdentity = LocalStampOf(name, false);
            if (before.IsEmpty())
               return CommitGone;

            unsigned __int64 size = 0;
            FileUtilities::FileSize64(name, size);

            // Put by an earlier pass through this client, and held open by a
            // reader then: not put again.
            const bool alreadyPut = !taken.put_stamp.IsEmpty() && taken.put_stamp == before && taken.put_key == key && taken.put_by.lock() == client;
            if (!alreadyPut)
            {
               const Client::Outcome outcome = PutFile_(client, name, key, problem);
               if (outcome != Client::OutcomeDone)
                  return CommitFailed;
            }

            const String aside = name + _T(".store-") + GUIDCreator::GetGUID();
            bool undo = false;
            {
               NamesLocked locked(*this, &key, nullptr);

               bool removed = false;
               {
                  boost::mutex::scoped_lock lock(pending_lock_);
                  removed = tombstones_.erase(name) > 0;
               }

               const AnsiString after = LocalStampOf(name);
               std::string keyNow;
               if (removed)
               {
                  // Deleted while it was put, when it could not be removed
                  // under the put: removed now, and what was put with it - or,
                  // when it still will not go, marked again for a later pass.
                  if (!RemoveQuietly(name))
                  {
                     boost::mutex::scoped_lock lock(pending_lock_);
                     tombstones_.insert(name);
                     return CommitChanged;
                  }
                  undo = true;
               }
               else if (after.IsEmpty())
                  undo = true;
               else if (after != before)
                  return CommitChanged;
               else if (ClientFor() != client || !KeyOf(name, keyNow) || keyNow != key)
                  return CommitChanged;   // the settings named another store or prefix during the pass: put there
               else if (!RenameQuietly(name, aside))
               {
                  // Held open by a reader (Windows): taken off the disk on a
                  // later pass, without another put.
                  putStamp = before;
                  putKey = key;
                  return CommitHeld;
               }
            }

            if (undo)
            {
               // A blob's object is its count's to remove (MessageStoreBlobs::
               // DeleteIfNone_): one released during its put, and kept again
               // since from a message whose part it is, is the same bytes.
               if (!IsBlobName_(name))
                  client->Remove(key);
               cache_.Forget(key);
               return CommitGone;
            }

            // The file taken is the one that was put - unless a writer renamed
            // its own over the name in the moment between the stamp and the
            // rename: then it goes back under the name, if nothing newer is
            // there, and is put again.
            if (LocalStampOf(aside, false) != beforeIdentity)
            {
               bool keepAside = false;
               {
                  NamesLocked locked(*this, &key, nullptr);
                  if (!FileUtilities::Exists(name) && !LinkQuietly(aside, name) && !FileUtilities::Exists(name) && !RenameQuietly(aside, name))
                     keepAside = true;

                  // Back under the name by a link, or something newer there
                  // already: the file set aside is not needed.
                  if (!keepAside && FileUtilities::Exists(aside) && FileUtilities::Exists(name))
                     RemoveQuietly(aside);
               }

               // The only copy of that write, and not back under its name yet:
               // moved there the way that waits and says why, outside the lock.
               if (keepAside && !FileUtilities::Exists(name))
                  FileUtilities::Move(aside, name);
               return CommitChanged;
            }

            // What was just put may be read next: its size kept for a moment
            // always, its bytes when they are small; then the file goes.
            Client::Properties properties;
            properties.size = size;
            std::shared_ptr<const std::string> bytes;
            if (size <= (unsigned __int64) ReadWindowBytes)
            {
               AnsiString read;
               if (Local().ReadAll(aside, read, 0) && (unsigned __int64) read.size() == size)
                  bytes = std::make_shared<std::string>(read.data(), read.size());
            }
            cache_.Keep(key, properties, bytes);

            if (!RemoveQuietly(aside))
               Local().Remove(aside);

            return CommitDone;
         }

         void Drain_()
         {
            const long long started = NowMs();
            InFlight held(*this);

            for (;;)
            {
               if (!Writes())
                  break;

               std::shared_ptr<Client> client = ClientFor();
               if (!client || client->Paused())
                  break;

               // Due, and held by no other pass.
               std::vector<TakenCommit> due;
               {
                  boost::mutex::scoped_lock lock(pending_lock_);
                  const long long now = NowMs();
                  for (std::map<String, PendingCommit>::const_iterator item = pending_.begin(); item != pending_.end() && due.size() < CommitBatch; ++item)
                  {
                     if (item->second.due > now || in_flight_.find(item->first) != in_flight_.end() || UnderHeldTree_(item->first))
                        continue;

                     TakenCommit taken;
                     taken.name = item->first;
                     taken.handed = item->second.handed;
                     taken.put_stamp = item->second.put_stamp;
                     taken.put_key = item->second.put_key;
                     taken.put_by = item->second.put_by;
                     due.push_back(taken);
                     held.Hold(item->first);
                  }
               }

               if (due.empty())
                  break;

               bool stop = false;
               for (size_t i = 0; i < due.size(); i++)
               {
                  if (NowMs() - started > DrainMilliseconds || client->Paused())
                  {
                     stop = true;
                     break;
                  }

                  String problem;
                  AnsiString putStamp;
                  std::string putKey;
                  const CommitResult result = CommitNow_(client, due[i], problem, putStamp, putKey);

                  boost::mutex::scoped_lock lock(pending_lock_);
                  held.Release(due[i].name);

                  std::map<String, PendingCommit>::iterator found = pending_.find(due[i].name);
                  if (found == pending_.end())
                     continue;

                  // Handed over again while it was put - written again after the
                  // file was taken: left on the list, to be put again when due.
                  if (found->second.handed != due[i].handed)
                     continue;

                  if (result == CommitDone || result == CommitGone)
                     pending_.erase(found);
                  else if (result == CommitHeld)
                  {
                     found->second.tries = 0;
                     found->second.due = NowMs() + WriteDelayMilliseconds;
                     found->second.put_stamp = putStamp;
                     found->second.put_key = putKey;
                     found->second.put_by = client;
                  }
                  else if (result == CommitChanged)
                  {
                     found->second.tries = 0;
                     found->second.due = NowMs() + WriteDelayMilliseconds;
                     found->second.put_stamp.Empty();
                  }
                  else
                  {
                     found->second.tries++;
                     found->second.due = NowMs() + RetryAfter_(found->second.tries);
                     found->second.put_stamp.Empty();
                     last_problem_ = name_problem_(due[i].name, problem);
                  }
               }

               if (stop || NowMs() - started > DrainMilliseconds)
                  break;
            }

            CompactJournal_();

            // How many wait, once an hour at most - said after the lock is let
            // go, since a report runs the OnError event.
            size_t waiting = 0;
            String problem;
            {
               boost::mutex::scoped_lock lock(pending_lock_);

               for (std::map<String, PendingCommit>::const_iterator item = pending_.begin(); item != pending_.end(); ++item)
               {
                  if (item->second.tries > 0)
                     waiting++;
               }

               const long long now = NowMs();
               if (waiting > 0 && (reported_ == 0 || now - reported_ >= PendingReportMilliseconds))
               {
                  reported_ = now;
                  problem = last_problem_;
               }
               else
                  waiting = 0;
            }

            if (waiting > 0)
            {
               ReportObjectError(7473, _T("MessageStoreObjects::Commit"),
                  Formatter::Format(_T("{0} message file(s) wait on the local disk for the object store to take them; the last one it did not take: {1}. They are read from the local disk meanwhile and nothing is lost; the committer tries each again, less often the longer it waits, and puts it in as soon as the store takes it. Said once an hour at most."),
                     (int) waiting, problem));
            }
         }

         static String name_problem_(const String &name, const String &problem)
         {
            return name + _T(" (") + problem + _T(")");
         }

         // Whether a name is under a directory, as the file system compares
         // names.
         static bool UnderTree_(const String &name, const String &root)
         {
            if (root.IsEmpty() || name.GetLength() <= root.GetLength() + 1)
               return false;

            const int separator = (int) name[root.GetLength()];
            if (separator != '\\' && separator != '/')
               return false;

#ifdef HM_PLATFORM_POSIX
            return name.Left(root.GetLength()) == root;
#else
            return name.Left(root.GetLength()).CompareNoCase(root) == 0;
#endif
         }

         // With pending_lock_ held.
         bool UnderHeldTree_(const String &name) const
         {
            for (size_t i = 0; i < held_trees_.size(); i++)
            {
               if (UnderTree_(name, held_trees_[i]))
                  return true;
            }
            return false;
         }

         boost::mutex lock_;
         bool attempted_ = false;
         std::atomic<bool> configured_;
         std::atomic<long long> checked_;
         StoreSettingsSnapshot snapshot_;
         std::shared_ptr<Client> client_;
         String problem_;
         StoreObjectAnswers cache_;

         boost::mutex name_locks_[NameLockCount];

         // The list: pending_lock_ guards every member below but journal_lock_,
         // which is taken before it, never after.
         boost::mutex journal_lock_;
         boost::mutex pending_lock_;
         std::map<String, PendingCommit> pending_;
         std::set<String> in_flight_;     // held by a pass now
         std::set<String> tombstones_;    // removed while a pass held them open
         std::vector<String> held_trees_; // directories held off the committer (HoldTree)
         bool journal_loaded_ = false;
         size_t journal_lines_ = 0;
         int passes_ = 0;
         long long pass_started_ = 0;
         long long reported_ = 0;
         long long unexpected_reported_ = 0; // when a pass that ended at an unexpected error was last said
         String last_problem_;
      };

      MessageStoreObjectBackend &TheBackend()
      {
         static MessageStoreObjectBackend backend;
         return backend;
      }

      // The committer's minute (MessageStoreObjects::CommitTask).
      class ObjectStoreCommitTask : public ScheduledTask
      {
      public:
         ObjectStoreCommitTask()
         {
            SetReoccurance(ScheduledTask::RunInfinitely);
            SetMinutesBetweenRun(1);
         }

         virtual void DoWork()
         {
            MessageStoreObjects::CommitPending();
         }
      };

      // ---- Moves ------------------------------------------------------------------

      // Whether the objects under two keys are one message's bytes: the same
      // size and the same ETag, or the same MD5 where the store keeps one, or
      // - the same size and neither telling, a copy the store made and gave an
      // ETag of its own - the same bytes read back, up to 64 MB.
      bool SameObject(const std::shared_ptr<Client> &client, const std::string &one, const Client::Properties &first,
                      const std::string &other, const Client::Properties &second)
      {
         if (first.size != second.size)
            return false;

         if (!first.etag.empty() && LowerAscii(first.etag) == LowerAscii(second.etag))
            return true;

         if (!first.content_md5.empty() && first.content_md5 == second.content_md5)
            return true;

         if (first.size > (unsigned __int64) (64 * 1024 * 1024))
            return false;

         std::string a;
         std::string b;
         unsigned __int64 totalA = 0;
         unsigned __int64 totalB = 0;
         return client->Read(one, 0, 0, a, totalA) == Client::OutcomeDone &&
                client->Read(other, 0, 0, b, totalB) == Client::OutcomeDone && a == b;
      }

      // Files still under from on the local disk moved under to, one by one;
      // one whose name to already holds is left where it is and counted.
      void MoveLocalLeftovers(MessageStoreObjectBackend &backend, const String &from, const String &to, unsigned int &moved, unsigned int &left)
      {
         const String fromRoot = TrimSeparators(from);
         const String toRoot = TrimSeparators(to);

         std::vector<String> names;
         MessageStore::LocalBackend().List(fromRoot, names);
         for (size_t i = 0; i < names.size(); i++)
         {
            if (names[i].GetLength() <= fromRoot.GetLength() + 1)
               continue;

            const String target = FileUtilities::Combine(toRoot, names[i].Mid(fromRoot.GetLength() + 1));
            if (FileUtilities::Exists(target))
            {
               left++;
               continue;
            }

            if (backend.Move(names[i], target))
               moved++;
            else
               left++;
         }
      }

      // ---- The copy verb ------------------------------------------------------------

      bool OpenForCommand(const String &verb)
      {
         IniFileSettings::Instance()->LoadSettings();
         if (!IniFileSettings::Instance()->GetDatabaseSettingsExists())
         {
            Print(true, Formatter::Format("[Database] in {0} does not name a database, so there is no store to {1}. Nothing was changed.",
               IniFileSettings::GetInitializationFile(), verb));
            return false;
         }

         String error;
         if (!Application::Instance()->InitInstance(error))
         {
            if (error.IsEmpty())
               error = Application::Instance()->GetLastErrorMessage();

            Print(true, Formatter::Format("The configuration could not be read and the database opened: {0} Nothing was changed.", error));
            Application::Instance()->ExitInstance();
            return false;
         }

         return true;
      }

      // The MD5 of a local file and of each part a put splits it into.
      bool DigestsOf(const String &name, unsigned __int64 &size, std::string &md5, std::vector<std::string> &partMd5s)
      {
         partMd5s.clear();
         md5.clear();

         if (!FileUtilities::FileSize64(name, size) || size > (unsigned __int64) INT_MAX)
            return false;

         File file;
         try
         {
            if (!file.Open(name, File::OTReadOnly))
               return false;

            std::string whole;
            whole.reserve((size_t) size);
            for (unsigned __int64 offset = 0; offset < size; offset += (unsigned __int64) Client::PartBytes)
            {
               const size_t length = (size_t) (size - offset < (unsigned __int64) Client::PartBytes ? size - offset : (unsigned __int64) Client::PartBytes);
               std::shared_ptr<ByteBuffer> chunk = file.ReadChunk((int) length);
               if (!chunk || chunk->GetSize() != length)
                  return false;

               partMd5s.push_back(Sig::Md5Of(chunk->GetCharBuffer(), chunk->GetSize()));
               whole.append(chunk->GetCharBuffer(), chunk->GetSize());
            }

            md5 = Sig::Md5Of(whole.data(), whole.size());
            return true;
         }
         catch (...)
         {
            return false;
         }
      }

      // Whether the object under key is the bytes these digests describe: its
      // size, then its MD5 where the store keeps one (and it is trusted: an
      // Azure blob's Content-MD5 is what this server gave it, which the store
      // keeps without checking, and is not trusted before a local file is
      // removed) or its ETag is one (a single PUT's, a multipart upload's), and
      // where neither, the object read back and its MD5 taken.
      bool Verified(const std::shared_ptr<Client> &client, const std::string &key, const Client::Properties &properties,
                    unsigned __int64 size, const std::string &md5, const std::vector<std::string> &partMd5s, bool trustStoredMd5)
      {
         if (properties.size != size)
            return false;

         if (trustStoredMd5 && !properties.content_md5.empty())
            return properties.content_md5 == md5;

         const std::string etag = LowerAscii(properties.etag);
         if (etag == Sig::HexOf(md5) || etag == Client::ExpectedS3Etag(partMd5s, md5))
            return true;

         std::string bytes;
         unsigned __int64 total = 0;
         if (client->Read(key, 0, 0, bytes, total) != Client::OutcomeDone)
            return false;

         return (unsigned __int64) bytes.size() == size && Sig::Md5Of(bytes.data(), bytes.size()) == md5;
      }

      struct CopyCounts
      {
         CopyCounts() : copied(0), already(0), removed(0), missing(0), failed(0) {}
         int copied;
         int already;
         int removed;
         int missing;
         int failed;
      };

      void CopyToStore(MessageStoreObjectBackend &backend, const std::shared_ptr<Client> &client, const String &name, bool removeSource, CopyCounts &counts)
      {
         std::string key;
         if (!backend.KeyOf(name, key))
            return;

         Client::Properties properties;
         Client::Outcome outcome = client->Head(key, properties);

         if (!FileUtilities::Exists(name))
         {
            if (outcome == Client::OutcomeDone)
               counts.already++;
            else if (outcome == Client::OutcomeAbsent)
            {
               counts.missing++;
               Print(true, "  " + name + ": neither on the local disk nor in the store.");
            }
            else
            {
               counts.failed++;
               Print(true, "  " + name + ": the store could not say whether it holds it (" + Client::LastProblem() + ").");
            }
            return;
         }

         unsigned __int64 size = 0;
         std::string md5;
         std::vector<std::string> partMd5s;
         if (!DigestsOf(name, size, md5, partMd5s))
         {
            counts.failed++;
            Print(true, "  " + name + ": could not be read; left as it is.");
            return;
         }

         bool verified = outcome == Client::OutcomeDone && Verified(client, key, properties, size, md5, partMd5s, !removeSource);
         if (verified)
            counts.already++;
         else if (outcome == Client::OutcomeDone || outcome == Client::OutcomeAbsent)
         {
            String problem;
            outcome = backend.PutFile_(client, name, key, problem);
            if (outcome == Client::OutcomeDone && client->Head(key, properties) == Client::OutcomeDone &&
                Verified(client, key, properties, size, md5, partMd5s, !removeSource))
            {
               verified = true;
               counts.copied++;
            }
            else
            {
               counts.failed++;
               Print(true, "  " + name + ": could not be put into the store and read back as it is (" + (problem.IsEmpty() ? Client::LastProblem() : problem) + "); the local file is left as it is.");
               return;
            }
         }
         else
         {
            counts.failed++;
            Print(true, "  " + name + ": the store could not be asked (" + Client::LastProblem() + "); the local file is left as it is.");
            return;
         }

         if (verified && removeSource)
         {
            if (FileUtilities::DeleteFile(name))
               counts.removed++;
            else
               Print(true, "  " + name + ": is in the store, and the local file could not be removed.");
         }
      }

      void CopyFromStore(MessageStoreObjectBackend &backend, const std::shared_ptr<Client> &client, const String &name, bool removeSource, CopyCounts &counts)
      {
         std::string key;
         if (!backend.KeyOf(name, key))
            return;

         Client::Properties properties;
         const Client::Outcome outcome = client->Head(key, properties);
         if (outcome != Client::OutcomeDone && outcome != Client::OutcomeAbsent)
         {
            counts.failed++;
            Print(true, "  " + name + ": the store could not be asked (" + Client::LastProblem() + ").");
            return;
         }

         bool verified = false;
         if (FileUtilities::Exists(name))
         {
            if (outcome == Client::OutcomeAbsent)
            {
               counts.already++;
               return;
            }

            unsigned __int64 size = 0;
            std::string md5;
            std::vector<std::string> partMd5s;
            if (!DigestsOf(name, size, md5, partMd5s) || !Verified(client, key, properties, size, md5, partMd5s, true))
            {
               counts.failed++;
               Print(true, "  " + name + ": the local file and the object differ; both are left as they are.");
               return;
            }

            counts.already++;
            verified = true;
         }
         else
         {
            if (outcome == Client::OutcomeAbsent)
            {
               counts.missing++;
               Print(true, "  " + name + ": neither on the local disk nor in the store.");
               return;
            }

            std::string bytes;
            unsigned __int64 total = 0;
            if (client->Read(key, 0, 0, bytes, total) != Client::OutcomeDone || (unsigned __int64) bytes.size() != properties.size)
            {
               counts.failed++;
               Print(true, "  " + name + ": could not be read from the store (" + Client::LastProblem() + ").");
               return;
            }

            // Written beside the name and linked to it, which never replaces a
            // file: one the running server wrote under the name while the
            // object was read is newer, and is left as it is.
            const String staged = name + _T(".store-") + GUIDCreator::GetGUID();
            if (!MessageStore::LocalBackend().WriteAtomically(staged, AnsiString(bytes.data(), bytes.size())))
            {
               FileUtilities::DeleteFile(staged);
               counts.failed++;
               Print(true, "  " + name + ": could not be written to the local disk.");
               return;
            }

            bool placed = MessageStore::LocalBackend().Link(staged, name);
            if (!placed && !FileUtilities::Exists(name))
               placed = FileUtilities::Move(staged, name);
            FileUtilities::DeleteFile(staged);

            if (!placed)
            {
               if (FileUtilities::Exists(name))
               {
                  counts.already++;
                  Print(false, "  " + name + ": a file appeared under the name while the object was read; it is newer, and left as it is.");
                  return;
               }

               counts.failed++;
               Print(true, "  " + name + ": could not be written to the local disk.");
               return;
            }

            unsigned __int64 size = 0;
            std::string md5;
            std::vector<std::string> partMd5s;
            if (!DigestsOf(name, size, md5, partMd5s) || md5 != Sig::Md5Of(bytes.data(), bytes.size()) ||
                !Verified(client, key, properties, size, md5, partMd5s, true))
            {
               FileUtilities::DeleteFile(name);
               counts.failed++;
               Print(true, "  " + name + ": what was written to the local disk did not read back as the object; removed again.");
               return;
            }

            counts.copied++;
            verified = true;
         }

         if (verified && removeSource)
         {
            if (client->Remove(key) == Client::OutcomeDone)
               counts.removed++;
            else
               Print(true, "  " + name + ": is on the local disk, and the object could not be removed (" + Client::LastProblem() + ").");
         }
      }
   }

   // ---- MessageStoreObjects ---------------------------------------------------------

   MessageStoreObjects::Placing::Placing()
   {
      placing_depth++;
   }

   MessageStoreObjects::Placing::~Placing()
   {
      placing_depth--;
   }

   MessageStoreObjects::HoldTree::HoldTree(const String &directory) :
      directory_(directory),
      held_(false)
   {
      if (!Active())
         return;

      TheBackend().HoldTree(directory);
      held_ = true;
   }

   MessageStoreObjects::HoldTree::~HoldTree()
   {
      if (held_)
         TheBackend().ReleaseTree(directory_);
   }

   MessageStoreBackend *
   MessageStoreObjects::Active()
   {
      MessageStoreObjectBackend &backend = TheBackend();
      if (!backend.Refresh())
         return nullptr;

      return &backend;
   }

   bool
   MessageStoreObjects::WritesToStore()
   {
      return Active() != nullptr && TheBackend().Writes();
   }

   bool
   MessageStoreObjects::Keyed(const String &name)
   {
      std::string key;
      return Active() != nullptr && TheBackend().KeyOf(name, key);
   }

   void
   MessageStoreObjects::CommitPending()
   {
      if (!WritesToStore())
         return;

      MessageStoreObjectBackend &backend = TheBackend();
      backend.LoadJournal();
      backend.DispatchCommits();
   }

   std::shared_ptr<ScheduledTask>
   MessageStoreObjects::CommitTask()
   {
      return std::shared_ptr<ScheduledTask>(new ObjectStoreCommitTask());
   }

   void
   MessageStoreObjects::ReportMissing(const String &messageFile)
   {
      const long long now = NowMs();
      long long last = last_missing_report.load();
      if (last != 0 && now - last < 60 * 1000)
         return;
      if (!last_missing_report.compare_exchange_strong(last, now))
         return;

      ReportObjectError(7476, _T("PersistentMessage::EnsureFileExistance"),
         "The message " + messageFile + " is neither on the local disk nor in the object store. While an object store is named no placeholder is written in its place - a move under way, a store changed and changed back, or one that answered 404 for a moment would have it written over the message - and IMAP answers NO [UNAVAILABLE] and POP3 -ERR [SYS/TEMP] for it; the consistency check lists it. Said once a minute at most.");
   }

   bool
   MessageStoreObjects::MoveTree(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem)
   {
      moved = 0;

      if (!Active())
         return true;

      MessageStoreObjectBackend &backend = TheBackend();
      std::shared_ptr<Client> client = backend.ClientFor();
      if (!client)
      {
         problem = "the object store cannot be used: " + backend.Problem();
         return false;
      }

      std::string fromPrefix;
      std::string toPrefix;
      if (!backend.PrefixOf(fromDirectory, fromPrefix) || !backend.PrefixOf(toDirectory, toPrefix))
         return true;

      if (fromPrefix == toPrefix)
         return true;

      std::vector<Client::Listed> objects;
      if (client->List(fromPrefix, objects) != Client::OutcomeDone)
      {
         problem = "the objects under " + fromDirectory + " could not be listed: " + Client::LastProblem();
         return false;
      }

      unsigned int conflicts = 0;
      std::string firstConflict;

      for (size_t i = 0; i < objects.size(); i++)
      {
         const std::string &from = objects[i].key;
         const std::string to = toPrefix + from.substr(fromPrefix.size());

         // Never over what the new name already holds.
         Client::Properties there;
         const Client::Outcome existing = client->Head(to, there);
         if (existing == Client::OutcomeDone)
         {
            Client::Properties source;
            const Client::Outcome sourceOutcome = client->Head(from, source);
            if (sourceOutcome == Client::OutcomeAbsent)
               continue;

            if (sourceOutcome != Client::OutcomeDone)
            {
               problem = "the object " + WideOfUtf8(from) + " could not be asked about: " + Client::LastProblem() +
                         ". The objects moved so far are under the new name; the move is finished by making it again.";
               return false;
            }

            if (!SameObject(client, from, source, to, there))
            {
               // Left as they are, both: which is the message is not this
               // server's to guess.
               conflicts++;
               if (firstConflict.empty())
                  firstConflict = from;
               continue;
            }

            // Copied by a move before this one: only the old name goes.
         }
         else if (existing != Client::OutcomeAbsent)
         {
            problem = "whether the object " + WideOfUtf8(to) + " is there could not be asked: " + Client::LastProblem() +
                      ". The objects moved so far are under the new name; the move is finished by making it again.";
            return false;
         }
         else if (client->Copy(from, to) != Client::OutcomeDone)
         {
            problem = "the object " + WideOfUtf8(from) + " could not be copied to " + WideOfUtf8(to) + ": " + Client::LastProblem() +
                      ". The objects moved so far are under the new name; the move is finished by making it again.";
            return false;
         }

         backend.Forget(to);
         moved++;

         if (client->Remove(from) != Client::OutcomeDone)
         {
            ReportObjectError(7475, _T("MessageStoreObjects::MoveTree"),
               "The object " + WideOfUtf8(from) + " was copied to " + WideOfUtf8(to) + " and could not be removed afterwards (" + Client::LastProblem() + "); it is left in the store, named by no row.");
         }
         backend.Forget(from);
      }

      if (conflicts > 0)
      {
         ReportObjectError(7475, _T("MessageStoreObjects::MoveTree"),
            Formatter::Format(_T("{0} object(s) under {1} were not moved to {2}: the new name already holds a different object (the first: {3}). Neither was changed; compare the two and remove the one that is not the message."),
               (int) conflicts, fromDirectory, toDirectory, WideOfUtf8(firstConflict)));
      }

      // What the local disk holds under the new directory now - a mailbox
      // moved there whole, files that were handed over under the old names -
      // is handed over under the new.
      backend.EnqueueLocalTree(toDirectory);
      return true;
   }

   bool
   MessageStoreObjects::SweepLateArrivals(const String &fromDirectory, const String &toDirectory, unsigned int &moved, String &problem)
   {
      moved = 0;

      if (!Active())
         return true;

      // The committer held off the old directory meanwhile (HoldTree).
      HoldTree hold(fromDirectory);

      unsigned int localMoved = 0;
      unsigned int localLeft = 0;
      MoveLocalLeftovers(TheBackend(), fromDirectory, toDirectory, localMoved, localLeft);

      unsigned int objectsMoved = 0;
      const bool objectsDone = MoveTree(fromDirectory, toDirectory, objectsMoved, problem);
      moved = localMoved + objectsMoved;

      if (localLeft > 0)
      {
         const String left = Formatter::Format(_T("{0} file(s) under {1} on the local disk were not moved to {2}: a file of the same name is there already, or the move failed."),
            (int) localLeft, fromDirectory, toDirectory);
         problem = problem.IsEmpty() ? left : problem + _T(" ") + left;
         return false;
      }

      return objectsDone;
   }

   void
   MessageStoreObjects::CommitTree(const String &directory)
   {
      if (!Active())
         return;

      TheBackend().EnqueueLocalTree(directory);
   }

   bool
   MessageStoreObjects::StageInto(const String &stagingDirectory, unsigned int &fetched, String &problem)
   {
      fetched = 0;

      if (!Active())
         return true;

      MessageStoreObjectBackend &backend = TheBackend();
      std::shared_ptr<Client> client = backend.ClientFor();
      if (!client)
      {
         problem = "the object store cannot be used: " + backend.Problem();
         return false;
      }

      // The messages and the blobs; the kept copies are left out of a backup
      // as they always have been (BackupExecuter::IsKeptOutOfBackup_).
      std::vector<MessageStoreMaintenance::StoredFile> files;
      if (!MessageStoreMaintenance::ListStoredFiles(String(), String(), files, problem))
         return false;

      std::vector<String> names;
      for (size_t i = 0; i < files.size(); i++)
         names.push_back(files[i].path);

      std::map<AnsiString, __int64> blobs;
      if (!MessageStoreBlobs::List(blobs))
      {
         problem = _T("the blobs could not be listed from the database");
         return false;
      }
      for (std::map<AnsiString, __int64>::const_iterator blob = blobs.begin(); blob != blobs.end(); ++blob)
         names.push_back(MessageStoreBlobs::PathOf(blob->first));

      for (size_t i = 0; i < names.size(); i++)
      {
         String relative;
         std::string key;
         if (!backend.RelativeOf(names[i], relative) || !backend.KeyOf(names[i], key))
            continue;

         const String staged = FileUtilities::Combine(stagingDirectory, relative);
         if (FileUtilities::Exists(staged))
            continue;

         // On the local disk now, though the tree copy before this did not find
         // it there - written since, or put back: that file. One the committer
         // takes off the disk meanwhile is read from the store below.
         if (FileUtilities::Exists(names[i]))
         {
            if (FileUtilities::Copy(names[i], staged, true))
            {
               fetched++;
               continue;
            }

            if (FileUtilities::Exists(names[i]))
            {
               problem = "the file " + names[i] + " could not be copied to " + staged;
               return false;
            }
         }

         std::string bytes;
         unsigned __int64 total = 0;
         const Client::Outcome outcome = client->Read(key, 0, 0, bytes, total);
         if (outcome == Client::OutcomeAbsent)
            continue;

         if (outcome != Client::OutcomeDone)
         {
            problem = "the object " + WideOfUtf8(key) + " could not be read: " + Client::LastProblem();
            return false;
         }

         if (!MessageStore::LocalBackend().WriteAtomically(staged, AnsiString(bytes.data(), bytes.size())))
         {
            problem = "the object " + WideOfUtf8(key) + " could not be written to " + staged;
            return false;
         }

         fetched++;
      }

      return true;
   }

   int
   MessageStoreObjects::RunCopyCommand(bool toStore, const String &accountAddress, const String &domainName, bool removeSource)
   {
      const String verb = toStore ? String(_T("copy to the object store")) : String(_T("copy from the object store"));
      if (!OpenForCommand(verb))
         return 2;

      // Removing what was copied changes what a running server reads.
      if (removeSource)
      {
         String problem;
         const int taken = MessageStoreMaintenance::TakeStoreLock(true, problem);
         if (taken != 0)
         {
            Print(true, taken == 1
               ? "The hMailServer server is running: " + problem + ". Stop it to copy with --remove-source; the copy without it runs beside the server. Nothing was changed."
               : "Whether the server is running could not be asked: " + problem + ". Nothing was changed.");
            Application::Instance()->ExitInstance();
            return 4;
         }
      }

      int result = 0;
      do
      {
         MessageStoreObjectBackend &backend = TheBackend();
         if (!backend.Refresh())
         {
            Print(true, "No object store is configured: MessageStoreObjectService is not s3 or azure. Name the store and its endpoint, bucket and credentials first (docs/ObjectStorage.md). Nothing was changed.");
            result = 2;
            break;
         }

         std::shared_ptr<Client> client = backend.ClientFor();
         if (!client)
         {
            Print(true, "The object store's settings cannot be used: " + backend.Problem() + ". Nothing was changed.");
            result = 2;
            break;
         }

         if (client->Probe() != Client::OutcomeDone)
         {
            Print(true, "The object store does not answer: " + Client::LastProblem() + ". Nothing was changed.");
            result = 1;
            break;
         }

         std::vector<MessageStoreMaintenance::StoredFile> files;
         String problem;
         if (!MessageStoreMaintenance::ListStoredFiles(accountAddress, domainName, files, problem))
         {
            Print(true, "The stored messages could not be listed: " + problem + ". Nothing was changed.");
            result = accountAddress.IsEmpty() && domainName.IsEmpty() ? 1 : 2;
            break;
         }

         CopyCounts counts;

         std::vector<String> names;
         for (size_t i = 0; i < files.size(); i++)
            names.push_back(files[i].path);

         // The kept copies: one the store could not be asked about is listed
         // too, and copied or counted as failed by name - never left out of
         // the run as though it were not there.
         std::vector<MessageStoreMaintenance::StoredFile> kept;
         if (MessageStoreMaintenance::ListKeptFilesOf(accountAddress, domainName, kept, problem))
         {
            for (size_t i = 0; i < kept.size(); i++)
               names.push_back(kept[i].path);
         }
         else
         {
            counts.failed++;
            Print(true, "  The deleted messages kept for recovery could not be listed: " + problem + ". They were not copied.");
         }

         // The blobs: every one for a run over the whole store, and for a run
         // over an account or a domain the ones its messages and kept copies
         // reference, which it cannot be read without.
         std::set<String> blobNames;
         if (accountAddress.IsEmpty() && domainName.IsEmpty())
         {
            std::map<AnsiString, __int64> blobs;
            if (MessageStoreBlobs::List(blobs))
            {
               for (std::map<AnsiString, __int64>::const_iterator blob = blobs.begin(); blob != blobs.end(); ++blob)
                  blobNames.insert(MessageStoreBlobs::PathOf(blob->first));
            }
            else
            {
               counts.failed++;
               Print(true, "  The blobs could not be listed from the database. They were not copied.");
            }
         }
         else
         {
            for (size_t i = 0; i < names.size(); i++)
            {
               MessageStore::ClearUnavailable();
               std::vector<MessageStoreReference> references;
               if (!MessageStore::GetReferences(names[i], references))
               {
                  if (MessageStore::Unavailable())
                  {
                     counts.failed++;
                     Print(true, "  " + names[i] + ": what it references could not be read (the object store did not answer); its blobs were not copied.");
                  }
                  continue;
               }

               for (size_t r = 0; r < references.size(); r++)
                  blobNames.insert(MessageStoreBlobs::PathOf(references[r].sha256));
            }
         }
         names.insert(names.end(), blobNames.begin(), blobNames.end());

         for (size_t i = 0; i < names.size(); i++)
         {
            if (toStore)
               CopyToStore(backend, client, names[i], removeSource, counts);
            else
               CopyFromStore(backend, client, names[i], removeSource, counts);
         }

         const String scope = !accountAddress.IsEmpty() ? accountAddress :
                              (!domainName.IsEmpty() ? String(_T("the domain ")) + domainName : String(_T("the whole store")));

         Print(false, Formatter::Format("{0} for {1}: {2} name(s) looked at, {3} copied and verified, ", verb, scope, (int) names.size(), counts.copied) +
            Formatter::Format("{0} there already and verified, {1} removed from where they were copied from, {2} missing on both sides, ", counts.already, counts.removed, counts.missing) +
            Formatter::Format("{0} failed.", counts.failed));

         if (toStore && !backend.Writes())
            Print(false, "New mail is still stored on the local disk (MessageStoreBackend is file). Set it to object to store new mail in the object store.");

         result = counts.failed == 0 ? 0 : 1;
      } while (false);

      Application::Instance()->ExitInstance();
      return result;
   }
}
