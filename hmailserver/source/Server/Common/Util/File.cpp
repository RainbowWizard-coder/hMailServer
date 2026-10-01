// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "File.h"
#include "ByteBuffer.h"
#include "Unicode.h"

// <io.h> is the MSVC C runtime's low-level file header; it is where _commit and
// _fileno live. The POSIX build reaches the same two calls through <unistd.h>,
// which the platform layer has already included.
#ifdef _MSC_VER
#include <io.h>
#endif
#include <climits>
#include <boost/filesystem.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

#ifdef HM_PLATFORM_POSIX
namespace
{
   // strerror_r comes in two shapes, and which one a C library declares decides
   // what the call returns. glibc's GNU one, which g++ and clang++ select by
   // defining _GNU_SOURCE for C++, returns the message, which may be a static
   // string with the buffer left untouched. The POSIX one - musl's (Alpine), the
   // BSDs' and macOS's - returns 0 or an error number and writes the message into
   // the buffer. An overload for each return type takes whichever the library
   // declares, with no test of the library's name: musl deliberately defines no
   // macro that says it is musl. Built against glibc and musl on 29 September 2026
   // (hmailserver/docs/Alpine.md, "What musl changed"). Whichever overload the
   // library does not call is unused, which [[maybe_unused]] says.
   [[maybe_unused]] const char *StrerrorMessage_(const char *returned, const char *)
   {
      return returned;
   }

   [[maybe_unused]] const char *StrerrorMessage_(int returned, const char *buffer)
   {
      return returned == 0 ? buffer : "unknown error";
   }
}
#endif

namespace HM
{
   

   File::File() :
      file_(nullptr)
   {
      
   }

   File::~File()
   {
      Close();
   }
   
   bool
   File::Close()
   {
      // fwrite answers for the bytes it took into the C runtime's buffer, not for
      // bytes in the file, and fclose writes what is still buffered before it
      // closes: its result is the only report of whether that tail arrived. It
      // was dropped here, so every writer that closed and went on took a short
      // file for a whole one. The stream is gone after fclose whatever it
      // answers (C11 7.21.5.1), so it is forgotten either way.
      if (file_ == nullptr)
         return true;

      const bool closed = fclose(file_) == 0;
      file_ = nullptr;
      return closed;
   }

   bool
   File::IsOpen() const
   {
      if (file_ == nullptr)
         return false;
      
      return true;
   }

   bool
   File::FlushToDisk()
   {
      // Durability barrier: push the C runtime buffer to the OS (fflush) and then
      // force the OS file-system cache to physical storage (_commit -> FlushFileBuffers).
      if (file_ == nullptr)
         return false;

      if (fflush(file_) != 0)
         return false;

#ifdef _MSC_VER
      return _commit(_fileno(file_)) == 0;
#else
      // fsync on the underlying descriptor is _commit's exact equivalent: both
      // ask the operating system to put its cached writes onto the device.
      return ::fsync(::fileno(file_)) == 0;
#endif
   }

   bool
   File::Flush()
   {
      // Visibility, not durability: fflush hands the C runtime buffer to the
      // operating system, at which point every reader of the file sees the
      // bytes. Deliberately no _commit - that forces the OS cache to physical
      // storage, and a caller flushing per log line must not pay a disk sync
      // per log line.
      if (file_ == nullptr)
         return false;

      return fflush(file_) == 0;
   }

   bool
   File::Open(const String &sFilename, OpenType ot)
   {
      if (IsOpen())
      {
         // The file should be closed, before we
         // try to open it again...
         throw std::logic_error(Formatter::FormatAsAnsi("The file {0} is already open.", sFilename));
      }

      std::wstring open_mode;

      switch (ot)
      {
      case OTReadOnly:
         open_mode = _T("rb");
         break;
      case OTCreate:
         open_mode = _T("wb");
         break;
      case OTAppend:
         open_mode = _T("ab");
         break;
      }

#ifdef HM_PLATFORM_POSIX
      // _wfsopen is fopen with a Windows share mode, and _SH_DENYNO is the mode
      // that denies nothing - which is what every open already is here, because
      // POSIX has no mandatory locking to deny with. So there is no share mode to
      // write a POSIX side for and nothing is lost by having none: fopen with the
      // same mode string is the same call.
      //
      // The path and the mode are narrowed the way the rest of the tree narrows a
      // String for a C API. A path that cannot be represented in the filesystem's
      // own encoding could not be opened by any other call here either, and fopen
      // answers null for it - which is the failure this function already returns
      // for two lines below.
      const AnsiString narrow_name = sFilename;
      const AnsiString narrow_mode = open_mode.c_str();

      file_ = ::fopen(narrow_name.c_str(), narrow_mode.c_str());
#else
      file_ = _wfsopen(sFilename.c_str(), open_mode.c_str(), _SH_DENYNO);
#endif

      if (file_ == nullptr)
      {
         return false;
      }

      name_ = sFilename;

      return true;
   }

   int
   File::GetSize()
   {
      // The guard here compared a FILE* against INVALID_HANDLE_VALUE, which is
      // (HANDLE)-1: a closed File has file_ == nullptr, so the test was never true
      // and the "not open" case fell through to file_size() with an empty name_ -
      // which throws boost::filesystem::filesystem_error. ReadFile() calls this
      // OUTSIDE ReadChunk's try block, so that exception would leave File with no
      // HM report of its own.
      if (file_ == nullptr)
         return 0;

      // ...and with the error_code overload, a file that has become unreadable
      // between Open and here answers 0 rather than throwing.
      boost::system::error_code error_code;
      // c_str(), not the String itself: a boost::filesystem::path is built from a
      // wide C string on either platform, while the String class converts to one
      // directly only where path's own character type is wide. It is the idiom the
      // rest of the tree already uses - the same call in FileUtilities::FileSize is
      // written this way - and it names the same file.
      boost::uintmax_t size = boost::filesystem::file_size(name_.c_str(), error_code);

      if (error_code)
         return 0;

      // The result is an int, so this is a 2 GiB ceiling on anything read through
      // ReadFile/ReadTextFile. Saturate rather than truncate: a plain cast turns
      // 2 GiB into a negative number, and ReadChunk would then be asked to allocate
      // (size_t)(negative). Saturating means a file that large is read as far as
      // INT_MAX instead, which is wrong but bounded - see the note in File.h.
      const boost::uintmax_t maxRepresentable = (boost::uintmax_t) INT_MAX;

      if (size > maxRepresentable)
         return INT_MAX;

      return (int) size;
   }

   bool
   File::SetPosition(int position)
   {
      if (file_ == nullptr)
         throw std::logic_error("Attempt to set position on file which has not been opened.");

      return fseek(file_, position, 0) == 0;
   }


   bool 
   File::Write(const String &sWrite)
   {
      if (file_ == nullptr)
         throw std::logic_error("Attempt to write to file which has not been opened.");

      // This is the wide half of the pair that WriteBOF opens: the bytes after a
      // FF FE mark are UTF-16LE, whatever the size of a wchar_t on the machine
      // that wrote them. The String's own bytes used to go to the file as they
      // lay, which is that format only where wchar_t is two bytes; on Linux it
      // is four, and a log or a Sieve script written that way could not be read
      // back by a Windows server, or by this one after an upgrade. The
      // conversion is a copy on Windows, so the file is the file it always was.
      const std::string bytes = Unicode::ToUtf16Le(sWrite);

      size_t result = fwrite(bytes.data(), 1, bytes.size(), file_);

      return result == bytes.size();
   }

   bool 
   File::Write(const AnsiString &sWrite)
   {  
      if (file_ == nullptr)
         throw std::logic_error("Attempt to write to file which has not been opened.");

      AnsiString temp_nonconst = sWrite;
      size_t result = fwrite(temp_nonconst.GetBuffer(), sizeof(char), temp_nonconst.GetLength(), file_);
      return result == temp_nonconst.GetLength();
   }

   bool
   File::Write(const unsigned char *pBuf, size_t iBufLen)
   {
      size_t noOfBytesWritten;

      return Write(pBuf, iBufLen, noOfBytesWritten);
   }

   bool 
   File::Write(const unsigned char *pBuf, size_t iBufLen, size_t& dwNoOfBytesWritten)
   {
      dwNoOfBytesWritten = 0;

      if (file_ == nullptr)
         throw std::logic_error("Attempt to write to file which has not been opened.");

      dwNoOfBytesWritten = fwrite(pBuf, 1, iBufLen, file_);
      return dwNoOfBytesWritten == iBufLen;
   }

   bool 
   File::Write(std::shared_ptr<ByteBuffer> pBuffer, size_t &noOfBytesWritten)
   {
      return Write((const unsigned char*)pBuffer->GetCharBuffer(), pBuffer->GetSize(), noOfBytesWritten);
   }

   bool 
   File::WriteBOF()
   {
      // The UTF-16LE byte order mark. It names the format of everything that
      // Write(const String &) then appends - which is UTF-16LE on every platform,
      // see there - and it is what ReadCompleteTextFile looks for to know that a
      // file has to be decoded rather than read as narrow text.
      unsigned char charByteOrderMarker[2] = {255, 254};

      size_t bytesWritten;
      Write(charByteOrderMarker, 2, bytesWritten);

      if (bytesWritten != 2)
      {
         // Failed to write BOM.
         return false;
      }

      return true;
   }

   bool 
   File::ReadLine(AnsiString &sLine)
   {
      if (file_ == nullptr)
         throw std::logic_error("Attempt to read to file which has not been opened.");

      const int buffer_size = 10000;
      char *line_buffer = sLine.GetBuffer(buffer_size);
      
      bool result = fgets(line_buffer, buffer_size, file_) != 0;

      sLine.ReleaseBuffer();

      return result;
   }

   std::shared_ptr<ByteBuffer> 
   File::ReadFile()
   {
      int iFileSize = GetSize();

      return ReadChunk(iFileSize);
   }

   std::shared_ptr<ByteBuffer> 
   File::ReadTextFile()
   {
      try
      {
         std::shared_ptr<ByteBuffer> pFileContents = ReadFile();

         const BYTE pByte = '\0';
         pFileContents->Add(&pByte, 1);
         return pFileContents;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5042, "File::ReadTextFile", "An unknown error occurred while reading a text file from disk.");
         throw;
      }

      std::shared_ptr<ByteBuffer> pFileContents = std::shared_ptr<ByteBuffer>(new ByteBuffer);
      return pFileContents;
   }

   std::shared_ptr<ByteBuffer> 
   File::ReadChunk(int iMaxSize)
   {  
      try
      {
         std::shared_ptr<ByteBuffer> pFileContents = std::shared_ptr<ByteBuffer>(new ByteBuffer);

         if (file_ == nullptr)
            throw std::logic_error("Attempt to read from file which has not been opened.");

         // Allocate takes a size_t, so a negative iMaxSize becomes a request for
         // very nearly SIZE_MAX bytes. Refuse it here with something that names the
         // cause, rather than letting it surface as a bad_alloc whose message says
         // nothing about where the number came from.
         if (iMaxSize < 0)
            throw std::runtime_error(Formatter::FormatAsAnsi("Refusing to read a negative number of bytes ({0}) from file {1}.", iMaxSize, name_));

         // Create a buffer to hold the file
         pFileContents->Allocate(iMaxSize);

         // fread fails reading large files. If the file is too large, fread will read zero bytes and the
         // errno will be set to invalid argument. The below code therefore reads the file in chunks.
         size_t remaining_bytes = iMaxSize;
         BYTE *buffer_position = (BYTE*)pFileContents->GetBuffer();
         
         size_t total_bytes_read = 0;

         while (remaining_bytes > 0)
         {
            size_t bytes_to_read = std::min((size_t) FileChunkSize, remaining_bytes);
            size_t bytes_actually_read = fread((void*)buffer_position, 1, bytes_to_read, file_);

            total_bytes_read += bytes_actually_read;
                
            if (bytes_actually_read != bytes_to_read)
            {
               if (feof(file_))
               {
                  // we've reached end of file.
                  break;
               }
            
               char error_msg[255];
#ifdef HM_PLATFORM_POSIX
               // strerror_r is the POSIX spelling of strerror_s, and glibc's is the
               // GNU one: it is allowed to answer with a static message and leave
               // the buffer untouched, so what it RETURNS is the message and the
               // buffer is only where it may have put it. Copying the return value
               // back is what makes the line below print a reason rather than
               // whatever was on the stack. musl's is the POSIX one, which returns
               // a number: StrerrorMessage_, above, answers the message for both.
               const char *reported = StrerrorMessage_(::strerror_r(errno, error_msg, sizeof(error_msg)), error_msg);

               if (reported != error_msg)
               {
                  ::strncpy(error_msg, reported, sizeof(error_msg) - 1);
                  error_msg[sizeof(error_msg) - 1] = '\0';
               }
#else
               strerror_s(error_msg, 255, errno);
#endif
               // The casts are not decoration. FormatArgument has a constructor for
               // unsigned int and one for unsigned __int64 and none for size_t,
               // which on Win64 is exactly the second of those and on this platform
               // is unsigned long - a distinct type that converts equally well to
               // both, so the call is ambiguous. Naming the 64-bit one keeps the
               // whole range of a size and is the type Windows was already picking.
               throw std::runtime_error(Formatter::FormatAsAnsi("Unable to read file {0}. Expected bytes: {1}, Actual read bytes: {2}. Error: {3}", name_, (unsigned __int64) bytes_to_read, (unsigned __int64) bytes_actually_read, error_msg));
            }

            buffer_position += bytes_actually_read;
            remaining_bytes -= bytes_actually_read;
            
         }

         pFileContents->DecreaseSize(iMaxSize - total_bytes_read);
         return pFileContents;
      }
      catch (boost::system::system_error& error)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "File::ReadFile()", "An error occured when reading file.", error);
         throw;
      }
      catch (std::exception& error)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "File::ReadFile()", "An error occured when reading file.", error);
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "File::ReadFile()", "An error occured when reading file.");
         throw;
      }

   }

   String 
   File::GetName() const
   {
      return name_;
   }

   bool 
   File::Write(File &sourceFile)
   {
      while (std::shared_ptr<ByteBuffer> sourceData = sourceFile.ReadChunk(FileChunkSize))
      {
         if (sourceData->GetSize() == 0)
         {
            // Done reading source file.
            return true;
         }

         size_t bytesWritten = 0;
         if (!Write(sourceData, bytesWritten))
            return false;

         if (bytesWritten != sourceData->GetSize())
            return false;
      }

      return true;
   }

}