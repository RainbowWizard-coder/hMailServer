// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Where the PST reader's bytes come from: a file, opened read-only and shared,
// or a buffer in memory (the tester's hand-assembled files). Every read names
// a range, and a range that is not wholly inside the source is refused rather
// than read short.

#pragma once

#include <cstdint>
#include <string>

namespace HM
{
   class PstByteSource
   {
   public:
      virtual ~PstByteSource() { }

      virtual uint64_t Size() const = 0;

      // length bytes from offset into bytes. False when the range is not wholly
      // inside the source, or the read failed.
      virtual bool Read(uint64_t offset, size_t length, std::string &bytes) = 0;
   };

   class PstMemorySource : public PstByteSource
   {
   public:
      explicit PstMemorySource(const std::string &bytes) : bytes_(bytes) { }

      virtual uint64_t Size() const { return bytes_.size(); }
      virtual bool Read(uint64_t offset, size_t length, std::string &bytes);

   private:
      std::string bytes_;
   };

   class PstFileSource : public PstByteSource
   {
   public:
      enum OpenResult
      {
         OpenDone = 0,
         OpenMissing = 1,     // no such file, or no such directory on the way
         OpenInUse = 2,       // another program holds it without sharing it (Outlook, while it runs)
         OpenDenied = 3,      // the service's account may not read it
         OpenFailed = 4       // anything else, or a file of no size
      };

      PstFileSource();
      virtual ~PstFileSource();

      // Read-only, with others free to read and write it: Outlook itself holds
      // its data files open, and a file it holds without sharing is InUse.
      OpenResult Open(const std::wstring &path);

      // The path of the file the handle is open on, as the file system resolves
      // it now - links followed, short names and case as they are on disk - or
      // empty where the platform cannot say.
      std::wstring FinalPath() const;

      virtual uint64_t Size() const { return size_; }
      virtual bool Read(uint64_t offset, size_t length, std::string &bytes);

      PstFileSource(const PstFileSource &) = delete;
      PstFileSource &operator=(const PstFileSource &) = delete;

   private:
      void Close_();

#ifdef HM_PLATFORM_POSIX
      int descriptor_;
#else
      void *handle_;
#endif
      uint64_t size_;
   };
}
