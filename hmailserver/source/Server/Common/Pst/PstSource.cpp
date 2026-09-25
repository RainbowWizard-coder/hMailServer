// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstSource.h.

#include "StdAfx.h"

#include "PstSource.h"
#include "PstFormat.h"
#include "../Util/Unicode.h"

#ifdef HM_PLATFORM_POSIX
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <vector>

namespace HM
{
   bool
   PstMemorySource::Read(uint64_t offset, size_t length, std::string &bytes)
   {
      if (!PstFormat::Fits(offset, length, bytes_.size()))
         return false;

      bytes.assign(bytes_, (size_t) offset, length);
      return true;
   }

#ifdef HM_PLATFORM_POSIX

   PstFileSource::PstFileSource() :
      descriptor_(-1),
      size_(0)
   {
   }

   PstFileSource::~PstFileSource()
   {
      Close_();
   }

   void
   PstFileSource::Close_()
   {
      if (descriptor_ >= 0)
         ::close(descriptor_);
      descriptor_ = -1;
      size_ = 0;
   }

   PstFileSource::OpenResult
   PstFileSource::Open(const std::wstring &path)
   {
      Close_();

      // The path is the one Resolve checked, links already followed; a link
      // swapped in for its last name since is not followed (O_NOFOLLOW), and
      // a FIFO swapped in does not hold the job, and every stop of the service
      // with it, until a writer comes (O_NONBLOCK). Only a regular file is
      // read, and it is read blocking.
      const std::string name = PstFormat::WideToUtf8(path);
      const int descriptor = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
      if (descriptor < 0)
      {
         if (errno == ENOENT || errno == ENOTDIR)
            return OpenMissing;
         if (errno == EACCES || errno == EPERM)
            return OpenDenied;
         return OpenFailed;
      }

      struct stat status;
      if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0)
      {
         ::close(descriptor);
         return OpenFailed;
      }

      const int flags = ::fcntl(descriptor, F_GETFL);
      if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0)
      {
         ::close(descriptor);
         return OpenFailed;
      }

      descriptor_ = descriptor;
      size_ = (uint64_t) status.st_size;
      return OpenDone;
   }

   std::wstring
   PstFileSource::FinalPath() const
   {
      if (descriptor_ < 0)
         return std::wstring();

      // What the descriptor is open on, as the kernel names it: the link in
      // /proc, resolved.
      char link[64];
      snprintf(link, sizeof(link), "/proc/self/fd/%d", descriptor_);
      char resolved[PATH_MAX + 1];
      if (::realpath(link, resolved) == nullptr)
         return std::wstring();

      const std::string utf8(resolved);
      String wide;
      Unicode::MultiByteToWide(AnsiString(utf8), wide);
      return wide;
   }

   bool
   PstFileSource::Read(uint64_t offset, size_t length, std::string &bytes)
   {
      if (descriptor_ < 0 || !PstFormat::Fits(offset, length, size_))
         return false;

      bytes.resize(length);
      size_t done = 0;
      while (done < length)
      {
         const ssize_t read = ::pread(descriptor_, &bytes[done], length - done, (off_t) (offset + done));
         if (read < 0 && errno == EINTR)
            continue;
         if (read <= 0)
            return false;
         done += (size_t) read;
      }
      return true;
   }

#else

   PstFileSource::PstFileSource() :
      handle_(INVALID_HANDLE_VALUE),
      size_(0)
   {
   }

   PstFileSource::~PstFileSource()
   {
      Close_();
   }

   void
   PstFileSource::Close_()
   {
      if (handle_ != INVALID_HANDLE_VALUE)
         CloseHandle((HANDLE) handle_);
      handle_ = INVALID_HANDLE_VALUE;
      size_ = 0;
   }

   PstFileSource::OpenResult
   PstFileSource::Open(const std::wstring &path)
   {
      Close_();

      HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
      if (handle == INVALID_HANDLE_VALUE)
      {
         const DWORD error = GetLastError();
         if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME)
            return OpenMissing;
         if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
            return OpenInUse;
         if (error == ERROR_ACCESS_DENIED)
            return OpenDenied;
         return OpenFailed;
      }

      LARGE_INTEGER size;
      BY_HANDLE_FILE_INFORMATION information;
      if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 ||
          !GetFileInformationByHandle(handle, &information) ||
          (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      {
         CloseHandle(handle);
         return OpenFailed;
      }

      handle_ = handle;
      size_ = (uint64_t) size.QuadPart;
      return OpenDone;
   }

   std::wstring
   PstFileSource::FinalPath() const
   {
      if (handle_ == INVALID_HANDLE_VALUE)
         return std::wstring();

      const DWORD needed = GetFinalPathNameByHandleW((HANDLE) handle_, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (needed == 0 || needed > 32768)
         return std::wstring();

      std::vector<wchar_t> buffer(needed + 1);
      const DWORD written = GetFinalPathNameByHandleW((HANDLE) handle_, &buffer[0], needed + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (written == 0 || written > needed)
         return std::wstring();

      return std::wstring(&buffer[0], written);
   }

   bool
   PstFileSource::Read(uint64_t offset, size_t length, std::string &bytes)
   {
      if (handle_ == INVALID_HANDLE_VALUE || !PstFormat::Fits(offset, length, size_))
         return false;

      bytes.resize(length);
      size_t done = 0;
      while (done < length)
      {
         const uint64_t at = offset + done;
         OVERLAPPED position = {};
         position.Offset = (DWORD) (at & 0xFFFFFFFF);
         position.OffsetHigh = (DWORD) (at >> 32);

         const size_t left = length - done;
         const DWORD want = left > (size_t) (1 << 20) ? (DWORD) (1 << 20) : (DWORD) left;
         DWORD read = 0;
         if (!ReadFile((HANDLE) handle_, &bytes[done], want, &read, &position) || read == 0)
            return false;
         done += read;
      }
      return true;
   }

#endif
}
