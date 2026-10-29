// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Win32 surface the SERVER CORE uses, for a POSIX build.
//
// This file exists so that the core's own sources compile unchanged on Linux.
// It is not a Windows emulator and must never grow into one: everything here is
// either a type the code names in a signature, or a call the code makes that has
// an exact POSIX equivalent. Anything with no faithful equivalent - the service
// control manager, DPAPI, LogonUser, COM - is NOT here. Those are behind
// HM_PLATFORM_WINDOWS in the sources that need them, with a Linux implementation
// of their own.
//
// Two rules govern edits:
//
//   1. Nothing in this file is compiled on Windows. The Windows build does not
//      include it at all (portable_stdafx.h only reaches it under __linux__), so
//      a mistake here cannot change the shipping server.
//   2. Every definition matches the Win32 one in WIDTH and SIGNEDNESS, because
//      the code stores these in structures and compares them. DWORD is 32 bits
//      on Win64 - "unsigned long" would be 64 here and would silently change
//      arithmetic that has been correct for twenty years.
#pragma once

#if !defined(__linux__) && !defined(__unix__) && !defined(__APPLE__)
#error "hm_platform.h is the POSIX compatibility layer; the Windows build must not include it."
#endif

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <cerrno>
#include <climits>
#include <string>
#include <sys/types.h>
// ---------------------------------------------------------------- printf widths
//
// MSVC's wide printf reads %s as a WIDE string and %S or %hs as a narrow one;
// glibc's reads %s as NARROW and %ls or %S as wide. The tree is written to the
// first convention in hundreds of Format() calls - String::Format(_T("host='%s'"),
// server.c_str()) - and under the second every one of them reads a wchar_t
// buffer as chars: "127.0.0.1" becomes "1", the first character followed by the
// NUL byte of its own encoding. Every wide format in the program reaches
// vswprintf through the functions below, so the format string is rewritten here
// once and the tree stays as it is.
namespace HMPlatform
{
   // The wide format, rewritten for glibc: %s -> %ls, %S and %hs -> %s, %I64 ->
   // %ll, %I -> %z. %% and every other conversion pass through unchanged; flags,
   // width and precision between the % and the conversion are kept. The same
   // rewrite is what musl needs (Alpine). A format that ends inside a conversion
   // - a % at its end, or flags, a width or a length with nothing after them - is
   // rewritten up to its end and no further: up to 6.3.7 both rewriters stepped
   // past the terminating NUL there and went on reading what lay after it, which
   // AddressSanitizer reports of string-format-tests' format ending in %.
   inline std::wstring WideFormatForGlibc(const wchar_t *format)
   {
      std::wstring out;
      if (!format)
         return out;
      out.reserve(::wcslen(format) + 8);
      for (const wchar_t *p = format; *p; ++p)
      {
         if (*p != L'%')
         {
            out.push_back(*p);
            continue;
         }
         if (p[1] == L'%')
         {
            out.append(L"%%");
            ++p;
            continue;
         }
         out.push_back(L'%');
         ++p;
         // flags, width, precision, then an optional length modifier
         while (*p && ::wcschr(L"-+ #0123456789.*", *p))
            out.push_back(*p++);
         if (*p == L'h' && p[1] == L's')
         {
            out.push_back(L's');       // %hs (narrow) -> %s
            p += 1;
            continue;
         }
         if (*p == L'l' || *p == L'h' || *p == L'L' || *p == L'j' || *p == L'z' || *p == L't' || *p == L'I')
         {
            // A length modifier the two agree on (%ld, %lu, %ls, %zu, %I64d
            // handled below); copy it and the conversion as they are.
            if (*p == L'I' && p[1] == L'6' && p[2] == L'4')
            {
               out.append(L"ll");       // MSVC's %I64d is glibc's %lld
               p += 3;
               if (!*p) break;
               out.push_back(*p);
               continue;
            }
            // MSVC's %I alone - %Iu, the tree's size_t - is the width of a size_t
            // or a ptrdiff_t: C's z. glibc printed it all the same, reading the I
            // as a flag of its own (the locale's alternative digits) and the
            // argument as an unsigned int, cut to 32 bits; musl refuses the whole
            // format, and the call wrote nothing - every IMAP FETCH item with a
            // literal, BODY[] {n}, lost its name and size on Alpine (29 September
            // 2026, hmailserver/docs/Alpine.md, "What musl changed").
            if (*p == L'I')
            {
               out.push_back(L'z');
               ++p;
               if (!*p) break;
               out.push_back(*p);
               continue;
            }
            out.push_back(*p++);
            if (*p == L'l') out.push_back(*p++);
            if (!*p) break;
            out.push_back(*p);
            continue;
         }
         if (*p == L's')
         {
            out.append(L"ls");         // %s (wide on MSVC) -> %ls
            continue;
         }
         if (*p == L'S')
         {
            out.push_back(L's');       // %S (narrow on MSVC) -> %s
            continue;
         }
         if (*p == L'c')
         {
            out.append(L"lc");         // %c in a wide format is a wide char on MSVC
            continue;
         }
         if (!*p)
            break;                     // a format ending in its %, flags or width
         out.push_back(*p);
      }
      return out;
   }

   // The narrow format, rewritten for glibc: %S -> %ls, %hs -> %s, %I64 -> %ll,
   // %I -> %z; and, as the wide one, no further than the format's end.
   inline std::string NarrowFormatForGlibc(const char *format)
   {
      std::string out;
      if (!format)
         return out;
      out.reserve(::strlen(format) + 8);
      for (const char *p = format; *p; ++p)
      {
         if (*p != '%')
         {
            out.push_back(*p);
            continue;
         }
         if (p[1] == '%')
         {
            out.append("%%");
            ++p;
            continue;
         }
         out.push_back('%');
         ++p;
         while (*p && ::strchr("-+ #0123456789.*", *p))
            out.push_back(*p++);
         if (*p == 'h' && p[1] == 's')
         {
            out.push_back('s');
            p += 1;
            continue;
         }
         if (*p == 'I' && p[1] == '6' && p[2] == '4')
         {
            out.append("ll");
            p += 3;
            if (!*p) break;
            out.push_back(*p);
            continue;
         }
         // %Iu -> %zu, as in the wide format above.
         if (*p == 'I')
         {
            out.push_back('z');
            ++p;
            if (!*p) break;
            out.push_back(*p);
            continue;
         }
         if (*p == 'S')
         {
            out.append("ls");
            continue;
         }
         if (!*p)
            break;
         out.push_back(*p);
      }
      return out;
   }

   // ------------------------------------------------------------ wide formatting
   //
   // The text glibc's vswprintf makes of a wide format WideFormatForGlibc has
   // rewritten, made without the C library's multibyte form. glibc's vswprintf
   // copies every wide character - of the format and of a %ls or %lc argument -
   // into its output as it is. musl's writes each through the current locale's
   // multibyte form and reads it back, and the server runs in the C locale (it
   // never calls setlocale or uselocale), where musl's form is byte-transparent:
   // it carries U+0000 to U+007F and 0xDF80 to 0xDFFF, the wide characters
   // musl's C locale makes of the bytes 0x80 to 0xFF, and no other character. Any
   // other character made the whole call fail, and String::Format returned with
   // nothing: on Alpine every log line, protocol reply and message holding a
   // character outside ASCII was empty (hmailserver/docs/Alpine.md, "What musl
   // changed"). A locale of the call's own (newlocale and uselocale, for the
   // calling thread alone) would not do: musl's C.UTF-8 refuses 0xDF80 to 0xDFFF
   // as it refuses every other surrogate and every value above 0x10FFFF, and on
   // musl a Boost.Filesystem path's wstring() of a name holding a byte above
   // 0x7F is made of those - so a log line naming such a file would still be
   // empty, where glibc's has it.
   //
   // So each conversion is made here on its own: text, %ls, %S, %lc and %C by
   // copying the wide characters, as glibc does, with its flags, width and
   // precision (a field is padded with spaces, on the left unless the - flag or a
   // negative * width asks for the right; a precision cuts a string; a null
   // string is "(null)", or nothing under a precision below 6, where musl's
   // vswprintf read a null %ls and ended the process); a narrow %s or %c through
   // the C library's own mbrtowc and btowc, as both C libraries convert one, which
   // in the C locale gives for a byte above 0x7F 0xDF80 + the byte on musl and on
   // glibc a failure of the whole call (%s) or WEOF (%c), as before; and the
   // numeric conversions and %p by the C library's snprintf, whose text in the C
   // locale is ASCII (musl's vswprintf has no %p at all). %% is a percent
   // sign. Anything else - %n, %m, a positional argument, a length the
   // conversion does not take, a format ending in % - is not read here: the
   // whole call goes to the C library's vswprintf as before. Unlike that path,
   // which measured in a buffer that doubled up to 64 KB and answered -1 above
   // it, the text here has no ceiling but the int it is counted in, as on
   // Windows.
   //
   // -1 where glibc's vswprintf answers -1 for the same arguments; otherwise the
   // number of characters in out, which may hold a NUL, as %lc writes one.
   inline int FormatWideForGlibc(std::wstring &out, const wchar_t *format, va_list arguments)
   {
      out.clear();
      if (!format)
         return -1;

      va_list whole;
      va_copy(whole, arguments);

      // A field: text of length characters, padded to width.
      auto field = [&out](const wchar_t *text, size_t length, long long width, bool left)
      {
         const size_t padding = width > 0 && (unsigned long long) width > length ? (size_t) width - length : 0;
         if (!left)
            out.append(padding, L' ');
         out.append(text, length);
         if (left)
            out.append(padding, L' ');
      };

      bool mine = true;
      bool failed = false;
      for (const wchar_t *p = format; *p && mine && !failed; ++p)
      {
         if (*p != L'%')
         {
            out.push_back(*p);
            continue;
         }
         ++p;
         if (*p == L'%')
         {
            out.push_back(L'%');
            continue;
         }

         std::string flags;
         bool left = false;
         while (*p && ::wcschr(L"-+ #0'", *p))
         {
            if (*p == L'-')
               left = true;
            flags.push_back((char) *p);
            ++p;
         }

         long long width = -1;
         if (*p == L'*')
         {
            const int given = va_arg(arguments, int);
            if (given < 0)
            {
               left = true;
               flags.push_back('-');
               width = -(long long) given;
            }
            else
               width = given;
            ++p;
         }
         else if (*p >= L'0' && *p <= L'9')
         {
            width = 0;
            while (*p >= L'0' && *p <= L'9' && width <= INT_MAX)
               width = width * 10 + (*p++ - L'0');
            if (*p == L'$')
               mine = false;           // a positional argument
         }
         if (width > INT_MAX)
            mine = false;

         long long precision = -1;
         if (mine && *p == L'.')
         {
            ++p;
            if (*p == L'*')
            {
               const int given = va_arg(arguments, int);
               precision = given < 0 ? -1 : given;
               ++p;
            }
            else
            {
               precision = 0;
               while (*p >= L'0' && *p <= L'9' && precision <= INT_MAX)
                  precision = precision * 10 + (*p++ - L'0');
               if (precision > INT_MAX)
                  mine = false;
            }
         }

         std::string length;
         while (mine && *p && ::wcschr(L"hlLqjzZt", *p) && length.size() < 2)
            length.push_back((char) *p++);
         if (!mine || !*p)
         {
            mine = false;
            break;
         }

         const wchar_t conversion = *p;
         const bool none = length.empty();
         if ((conversion == L's' && length == "l") || (conversion == L'S' && none))
         {
            const wchar_t *text = va_arg(arguments, const wchar_t *);
            if (!text)
               text = precision < 0 || precision >= 6 ? L"(null)" : L"";
            const size_t count = precision >= 0 ? ::wcsnlen(text, (size_t) precision) : ::wcslen(text);
            field(text, count, width, left);
         }
         else if (conversion == L's' && none)
         {
            const char *text = va_arg(arguments, const char *);
            if (!text)
            {
               const wchar_t *null = precision < 0 || precision >= 6 ? L"(null)" : L"";
               field(null, ::wcslen(null), width, left);
               continue;
            }
            // In the C locale a byte is a character; the count is glibc's, a
            // precision being a number of bytes.
            const size_t count = precision >= 0 ? ::strnlen(text, (size_t) precision) : ::strlen(text);
            std::wstring wide;
            std::mbstate_t state{};
            const char *next = text;
            while (wide.size() < count)
            {
               wchar_t character = 0;
               const size_t used = ::mbrtowc(&character, next, MB_LEN_MAX, &state);
               if (used == (size_t) -1 || used == (size_t) -2)
               {
                  failed = true;
                  break;
               }
               if (used == 0)
                  break;
               wide.push_back(character);
               next += used;
            }
            if (!failed)
               field(wide.data(), wide.size(), width, left);
         }
         else if ((conversion == L'c' && length == "l") || (conversion == L'C' && none))
         {
            const wchar_t character = (wchar_t) va_arg(arguments, wint_t);
            field(&character, 1, width, left);
         }
         else if (conversion == L'c' && none)
         {
            const wchar_t character = (wchar_t) ::btowc((unsigned char) va_arg(arguments, int));
            field(&character, 1, width, left);
         }
         else if (::wcschr(L"diouxXeEfFgGaAp", conversion))
         {
            // The narrow form of the same conversion, with a * width or precision
            // written out: L with an integer and q are glibc's ll, Z its z.
            const bool integer = ::wcschr(L"diouxX", conversion) != nullptr;
            const bool real = ::wcschr(L"eEfFgGaA", conversion) != nullptr;
            std::string size = length;
            if (integer && (size == "L" || size == "q"))
               size = "ll";
            if (integer && size == "Z")
               size = "z";
            if (real && size == "l")
               size = "";
            const bool sized = integer ? (size.empty() || size == "h" || size == "hh" || size == "l" || size == "ll" ||
                                          size == "j" || size == "z" || size == "t")
                                       : real ? (size.empty() || size == "L") : size.empty();
            if (!sized)
            {
               mine = false;
               break;
            }

            std::string spec = "%" + flags;
            if (width >= 0)
               spec += std::to_string(width);
            if (precision >= 0)
               spec += "." + std::to_string(precision);
            spec += size;
            spec.push_back((char) conversion);

            const bool isSigned = conversion == L'd' || conversion == L'i';
            std::string text;
            auto print = [&text, &spec](auto value) -> bool
            {
               const int needed = ::snprintf(nullptr, 0, spec.c_str(), value);
               if (needed < 0)
                  return false;
               text.assign((size_t) needed + 1, '\0');
               ::snprintf(&text[0], text.size(), spec.c_str(), value);
               text.resize((size_t) needed);
               return true;
            };

            bool printed = false;
            if (conversion == L'p')
               printed = print(va_arg(arguments, void *));
            else if (real)
               printed = size == "L" ? print(va_arg(arguments, long double)) : print(va_arg(arguments, double));
            else if (size.empty() || size == "h" || size == "hh")
               printed = isSigned ? print(va_arg(arguments, int)) : print(va_arg(arguments, unsigned int));
            else if (size == "l")
               printed = isSigned ? print(va_arg(arguments, long)) : print(va_arg(arguments, unsigned long));
            else if (size == "ll")
               printed = isSigned ? print(va_arg(arguments, long long)) : print(va_arg(arguments, unsigned long long));
            else if (size == "j")
               printed = isSigned ? print(va_arg(arguments, intmax_t)) : print(va_arg(arguments, uintmax_t));
            else if (size == "z")
               printed = isSigned ? print(va_arg(arguments, ssize_t)) : print(va_arg(arguments, size_t));
            else
               printed = isSigned ? print(va_arg(arguments, ptrdiff_t)) : print(va_arg(arguments, size_t));

            if (!printed)
               failed = true;
            else
               for (char c : text)
                  out.push_back((wchar_t) (unsigned char) c);
         }
         else
            mine = false;              // %n, %m and the rest: the C library's
      }

      if (mine)
      {
         va_end(whole);
         if (failed || out.size() > (size_t) INT_MAX)
         {
            out.clear();
            return -1;
         }
         return (int) out.size();
      }

      // Not read here: the C library's vswprintf, in a buffer that doubles up to
      // 64 KB, as every wide format went before.
      out.clear();
      for (size_t size = 256; size <= 65536; size *= 2)
      {
         std::wstring buffer(size, L'\0');
         va_list copy;
         va_copy(copy, whole);
         const int written = ::vswprintf(&buffer[0], size, format, copy);
         va_end(copy);
         if (written >= 0)
         {
            out.assign(buffer.data(), (size_t) written);
            va_end(whole);
            return written;
         }
      }
      va_end(whole);
      return -1;
   }
}



#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <pthread.h>
#include <dirent.h>

// ---------------------------------------------------------------- basic types
//
// Widths are Win64's, not the host's natural ones. See rule 2 above.
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef uint32_t            DWORD;
typedef uint32_t            ULONG;
typedef int32_t             LONG;
typedef int64_t             LONGLONG;
typedef uint64_t            ULONGLONG;
// __int64 is an MSVC KEYWORD, not a type name, so the tree writes "unsigned
// __int64" in 245 places. A typedef could not carry that; a macro can, and it
// gives the same 64-bit width on both platforms.
#define __int64 long long
typedef unsigned int        UINT;
typedef int                 INT;
typedef unsigned char       UCHAR;
typedef char                CHAR;
typedef wchar_t             WCHAR;
typedef size_t              SIZE_T;
typedef ptrdiff_t           SSIZE_T;
typedef void *              LPVOID;
typedef const void *        LPCVOID;
typedef void *              HANDLE;
typedef void *              HMODULE;
typedef void *              HINSTANCE;
typedef void *              HWND;
typedef void *              HKEY;
typedef int32_t             HRESULT;
typedef short               VARIANT_BOOL;
// An OLE date: days since 30 December 1899, the fraction being the time of day.
// The tree's DateTime class (Common/Util/VariantDateTime.h) carries it.
typedef double              DATE;
// A locale identifier. The tree passes one to the string class's locale-aware
// comparisons; on POSIX the C++ locale does that work, so the value is carried
// but never interpreted.
typedef uint32_t            LCID;
typedef uint16_t            LANGID;
// The locale the string class asks for when a comparison is "the user's". POSIX
// has no numeric locale identifiers; the value is carried and the C++ locale
// does the work, so any constant will do as long as it is the same one.
#define LANG_USER_DEFAULT   ((LANGID) 0x0400)
#define LANG_NEUTRAL        ((LANGID) 0x0000)
#define SUBLANG_DEFAULT     0x01
#define SORT_DEFAULT        0x0
#define MAKELANGID(primary, sub) ((LANGID) (((WORD) (sub) << 10) | (WORD) (primary)))
#define MAKELCID(language, sort) ((LCID) ((((DWORD) ((WORD) (sort))) << 16) | ((DWORD) ((WORD) (language)))))
typedef int64_t             LONG64;
typedef uint64_t            ULONG64;
typedef uint64_t            DWORD64;
typedef int32_t             LONG32;
typedef uint32_t            DWORD32;
// MSVC's <errno.h> defines errno_t; glibc does not, and the string class and
// the _s functions name it in their signatures.
typedef int                 errno_t;

// BSTR is an OLE string - a length-prefixed wide buffer. CStdString has one
// member that returns one (AllocSysString) and the COM API passes them about.
// Nothing in the core BUILD uses it: the type is declared so the class compiles,
// and SysAllocString/SysFreeString are the smallest honest implementations -
// they allocate the same shape (a four-byte length ahead of the characters) so a
// caller that reads a length behind the pointer is not lied to.
typedef wchar_t *           BSTR;
typedef const wchar_t *     LPCOLESTR;
typedef wchar_t *           LPOLESTR;
typedef wchar_t             OLECHAR;

inline BSTR SysAllocStringLen(const wchar_t *text, unsigned int characters)
{
   const size_t bytes = (size_t) characters * sizeof(wchar_t);
   unsigned char *block = (unsigned char *) ::malloc(sizeof(uint32_t) + bytes + sizeof(wchar_t));
   if (!block)
      return nullptr;
   const uint32_t length = (uint32_t) bytes;
   ::memcpy(block, &length, sizeof(uint32_t));
   wchar_t *characters_out = reinterpret_cast<wchar_t *>(block + sizeof(uint32_t));
   if (text)
      ::wmemcpy(characters_out, text, characters);
   characters_out[characters] = L'\0';
   return reinterpret_cast<BSTR>(block + sizeof(uint32_t));
}

inline BSTR SysAllocString(const wchar_t *text)
{
   return SysAllocStringLen(text, text ? (unsigned int) ::wcslen(text) : 0u);
}

inline void SysFreeString(BSTR text)
{
   if (text)
      ::free(reinterpret_cast<unsigned char *>(text) - sizeof(uint32_t));
}

inline unsigned int SysStringLen(BSTR text)
{
   if (!text)
      return 0;
   uint32_t bytes = 0;
   ::memcpy(&bytes, reinterpret_cast<unsigned char *>(text) - sizeof(uint32_t), sizeof(uint32_t));
   return (unsigned int) (bytes / sizeof(wchar_t));
}

typedef char *              LPSTR;
typedef const char *        LPCSTR;
typedef wchar_t *           LPWSTR;
typedef const wchar_t *     LPCWSTR;
typedef wchar_t             TCHAR;
typedef wchar_t *           LPTSTR;
typedef const wchar_t *     LPCTSTR;
typedef const wchar_t *     PCWSTR;
typedef wchar_t *           PWSTR;
typedef BYTE *              LPBYTE;
typedef DWORD *             LPDWORD;

// The server is built UNICODE on Windows, so _T() is a wide literal there and
// HM::String is CStdStr<wchar_t>. It must stay wide here or half the string
// handling would change width and this would be a different program.
#ifndef _T
#define _T(x) L##x
#endif
#ifndef TEXT
#define TEXT(x) L##x
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif

// Calling conventions. On x86 these picked a stack discipline; on x64 there is
// one convention and Windows itself defines them all away. The tree still writes
// __stdcall in a few signatures.
#define WINAPI
#define PASCAL
#define STDMETHODCALLTYPE
#define APIENTRY
#define CALLBACK
#define __declspec(x)
#define __stdcall
#define __cdecl

// HRESULT values the core tests for. The COM API itself is not built here; these
// exist because helper code in Common/ returns them.
#ifndef S_OK
#define S_OK          ((HRESULT) 0x00000000L)
#define S_FALSE       ((HRESULT) 0x00000001L)
#define E_FAIL        ((HRESULT) 0x80004005L)
#define E_INVALIDARG  ((HRESULT) 0x80070057L)
#define E_OUTOFMEMORY ((HRESULT) 0x8007000EL)
#define E_NOTIMPL     ((HRESULT) 0x80004001L)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)
#endif

// ---------------------------------------------------------------- COM shapes
//
// Declarations only, and deliberately incomplete. Server/COM is not part of this
// build, but a handful of core headers name a COM interface in a signature -
// ErrorManager::GetNativeErrorCode(IErrorInfo*) is the one that reaches every
// translation unit through the precompiled header. A forward declaration lets
// the declaration parse; the DEFINITION of any such function stays behind
// HM_PLATFORM_WINDOWS, so a Linux build cannot call one by accident: it would
// fail to link, which is the right answer rather than a silent stub.
struct IUnknown;
struct IDispatch;
struct IErrorInfo;
// A certificate context is a Windows CryptoAPI handle. Nothing in the POSIX
// build produces one - certificates come through OpenSSL - so the declaration
// exists only so that a Windows-only signature parses.
typedef const struct _CERT_CONTEXT *PCCERT_CONTEXT;

typedef const struct _CERT_CHAIN_CONTEXT *PCCERT_CHAIN_CONTEXT;
struct ITypeInfo;
typedef struct tagVARIANT VARIANT;
typedef VARIANT *LPVARIANT;

// ---------------------------------------------------------------- sockets
//
// Berkeley sockets are the Win32 ones minus the handle type and the separate
// close call, which is the whole of the difference the core sees.
typedef int SOCKET;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR   (-1)
#endif

// Win32 spells the Berkeley structures in capitals and adds an "IN_ADDR" union
// whose members the tree names. These are aliases, not copies: the layout is the
// system's own, so a pointer handed to a POSIX call is the right shape.
typedef struct sockaddr     SOCKADDR;
typedef struct sockaddr *   LPSOCKADDR;
typedef struct sockaddr_in  SOCKADDR_IN;
typedef struct sockaddr_in6 SOCKADDR_IN6;
typedef struct sockaddr_storage SOCKADDR_STORAGE;
typedef struct hostent *    PHOSTENT;
typedef struct hostent *    LPHOSTENT;
typedef struct in_addr      IN_ADDR;
typedef struct in6_addr     IN6_ADDR;
typedef struct addrinfo     ADDRINFOW;
typedef int                 socklen_t_win;

inline int closesocket(SOCKET s)
{
   return ::close(s);
}

// ---------------------------------------------------------------- time
typedef struct _SYSTEMTIME
{
   WORD wYear;
   WORD wMonth;
   WORD wDayOfWeek;
   WORD wDay;
   WORD wHour;
   WORD wMinute;
   WORD wSecond;
   WORD wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef struct _FILETIME
{
   DWORD dwLowDateTime;
   DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

namespace HMPlatform
{
   inline void FillSystemTime(SYSTEMTIME *out, const struct tm &parts, long milliseconds)
   {
      out->wYear = (WORD) (parts.tm_year + 1900);
      out->wMonth = (WORD) (parts.tm_mon + 1);
      out->wDayOfWeek = (WORD) parts.tm_wday;
      out->wDay = (WORD) parts.tm_mday;
      out->wHour = (WORD) parts.tm_hour;
      out->wMinute = (WORD) parts.tm_min;
      out->wSecond = (WORD) parts.tm_sec;
      out->wMilliseconds = (WORD) milliseconds;
   }
}

inline void GetLocalTime(LPSYSTEMTIME out)
{
   struct timeval now = {};
   ::gettimeofday(&now, nullptr);
   struct tm parts = {};
   ::localtime_r(&now.tv_sec, &parts);
   HMPlatform::FillSystemTime(out, parts, now.tv_usec / 1000);
}

inline void GetSystemTime(LPSYSTEMTIME out)
{
   struct timeval now = {};
   ::gettimeofday(&now, nullptr);
   struct tm parts = {};
   ::gmtime_r(&now.tv_sec, &parts);
   HMPlatform::FillSystemTime(out, parts, now.tv_usec / 1000);
}

// Milliseconds since boot, 64-bit, monotonic - which is what every caller in the
// core actually wants and what GetTickCount64 gives them on Windows.
inline ULONGLONG GetTickCount64()
{
   struct timespec ts = {};
   ::clock_gettime(CLOCK_MONOTONIC, &ts);
   return (ULONGLONG) ts.tv_sec * 1000ULL + (ULONGLONG) (ts.tv_nsec / 1000000);
}

inline DWORD GetTickCount()
{
   return (DWORD) GetTickCount64();
}

inline void Sleep(DWORD milliseconds)
{
   struct timespec request;
   request.tv_sec = (time_t) (milliseconds / 1000);
   request.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
   while (::nanosleep(&request, &request) == -1 && errno == EINTR)
      ;
}

// ---------------------------------------------------------------- last error
//
// Win32 keeps a per-thread last-error code; errno is the same idea with the same
// threading rules, so the core's GetLastError() calls read the errno its own
// POSIX call just set.
inline DWORD GetLastError()
{
   return (DWORD) errno;
}

inline void SetLastError(DWORD code)
{
   errno = (int) code;
}

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS           0L
#define ERROR_FILE_NOT_FOUND    ENOENT
#define ERROR_PATH_NOT_FOUND    ENOENT
#define ERROR_ACCESS_DENIED     EACCES
#define ERROR_ALREADY_EXISTS    EEXIST
#define ERROR_FILE_EXISTS       EEXIST
#define ERROR_NOT_ENOUGH_MEMORY ENOMEM
#define ERROR_INVALID_PARAMETER EINVAL
#endif

// ---------------------------------------------------------------- interlocked
//
// The core uses these for reference counts and counters. __atomic with
// SEQ_CST is at least as strong as the Win32 interlocked family, which is
// what the code assumes.
inline LONG InterlockedIncrement(LONG volatile *target)
{
   return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

inline LONG InterlockedDecrement(LONG volatile *target)
{
   return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

inline LONG InterlockedExchange(LONG volatile *target, LONG value)
{
   return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

inline LONGLONG InterlockedCompareExchange64(LONGLONG volatile *target, LONGLONG replacement, LONGLONG comparand)
{
   // Win32 returns the value that was there, whether or not the exchange
   // happened; __atomic_compare_exchange_n reports success and writes the seen
   // value back through its expected pointer, so the two are the same call with
   // the answer in a different place.
   LONGLONG expected = comparand;
   __atomic_compare_exchange_n(target, &expected, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
   return expected;
}

inline LONG InterlockedCompareExchange(LONG volatile *target, LONG replacement, LONG comparand)
{
   LONG expected = comparand;
   __atomic_compare_exchange_n(target, &expected, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
   return expected;
}

inline LONGLONG InterlockedIncrement64(LONGLONG volatile *target)
{
   return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------- ids, debug
inline DWORD GetCurrentThreadId()
{
   // The kernel's thread id, the number ps -L and gdb show for the thread:
   // unique among live threads, stable for one, and positive. pthread_self()
   // is a pointer, and its low 32 bits printed as a negative number in every
   // log line - which the log-format fixture read as the format having changed.
   return (DWORD) ::syscall(SYS_gettid);
}

inline DWORD GetCurrentProcessId()
{
   return (DWORD) ::getpid();
}

inline void OutputDebugStringW(LPCWSTR)
{
   // Windows sends this to an attached debugger and nowhere else. There is no
   // equivalent, and a stderr write would put debugger noise into a service's
   // journal, so it is deliberately a no-op.
}

inline void OutputDebugStringA(LPCSTR)
{
}

#ifdef UNICODE
#define OutputDebugString OutputDebugStringW
#else
#define OutputDebugString OutputDebugStringA
#endif

// ---------------------------------------------------------------- string calls
//
// The wide C runtime calls the core uses under their MSVC names. The behaviour
// difference that matters - MSVC's _s functions return an error code and clamp
// - is preserved: these return 0 on success and leave the destination
// terminated.
inline int _stricmp(const char *a, const char *b)
{
   return ::strcasecmp(a, b);
}

inline int _strnicmp(const char *a, const char *b, size_t n)
{
   return ::strncasecmp(a, b, n);
}

inline int _wcsicmp(const wchar_t *a, const wchar_t *b)
{
   return ::wcscasecmp(a, b);
}

inline int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n)
{
   return ::wcsncasecmp(a, b, n);
}

inline int strcpy_s(char *destination, size_t size, const char *source)
{
   if (!destination || !source || size == 0)
      return EINVAL;
   const size_t length = ::strlen(source);
   if (length + 1 > size)
   {
      destination[0] = '\0';
      return ERANGE;
   }
   ::memcpy(destination, source, length + 1);
   return 0;
}

inline int strncpy_s(char *destination, size_t size, const char *source, size_t count)
{
   if (!destination || !source || size == 0)
      return EINVAL;
   const size_t length = ::strnlen(source, count);
   if (length + 1 > size)
   {
      destination[0] = '\0';
      return ERANGE;
   }
   ::memcpy(destination, source, length);
   destination[length] = '\0';
   return 0;
}

inline int wcscpy_s(wchar_t *destination, size_t size, const wchar_t *source)
{
   if (!destination || !source || size == 0)
      return EINVAL;
   const size_t length = ::wcslen(source);
   if (length + 1 > size)
   {
      destination[0] = L'\0';
      return ERANGE;
   }
   ::wmemcpy(destination, source, length + 1);
   return 0;
}

inline int _memicmp(const void *a, const void *b, size_t count)
{
   const unsigned char *left = (const unsigned char *) a;
   const unsigned char *right = (const unsigned char *) b;
   for (size_t i = 0; i < count; i++)
   {
      const int difference = ::tolower(left[i]) - ::tolower(right[i]);
      if (difference != 0)
         return difference;
   }
   return 0;
}

// MSVC's rand_s is a cryptographically strong random number, not the C library's
// rand(). getrandom() is the same promise on Linux; the fallback path reads
// /dev/urandom, which is what getrandom() is a syscall for.
inline errno_t rand_s(unsigned int *value)
{
   if (!value)
      return EINVAL;
   FILE *source = ::fopen("/dev/urandom", "rb");
   if (!source)
      return EIO;
   const size_t read = ::fread(value, sizeof(unsigned int), 1, source);
   (void) ::fclose(source);
   return read == 1 ? 0 : EIO;
}

inline int sscanf_s(const char *buffer, const char *format, ...)
{
   // MSVC's _s scanf takes a buffer size after each %s; nothing in this tree
   // passes one, so the plain form is exactly equivalent here.
   va_list arguments;
   va_start(arguments, format);
   const int assigned = ::vsscanf(buffer, HMPlatform::NarrowFormatForGlibc(format).c_str(), arguments);
   va_end(arguments);
   return assigned;
}

inline int localtime_s(struct tm *result, const time_t *time)
{
   // MSVC's argument order is (out, in) and it returns an error code; POSIX
   // localtime_r is (in, out) and returns the pointer. The core calls the MSVC
   // form, so that is what this provides.
   return ::localtime_r(time, result) == nullptr ? EINVAL : 0;
}

inline int gmtime_s(struct tm *result, const time_t *time)
{
   return ::gmtime_r(time, result) == nullptr ? EINVAL : 0;
}

// ---------------------------------------------------------------- MSVC CRT
//
// The string class and a few utilities call the C runtime under Microsoft's
// names - the _s "secure" family and the TCHAR-generic _t family. These are
// those names over the POSIX functions, with Microsoft's semantics where they
// differ: the _s printf family returns the number of characters written and -1
// on truncation, and the _vsc family returns the length a format would need.
#include <cwctype>


inline int vsprintf_s(char *buffer, size_t size, const char *format, va_list arguments)
{
   const std::string rewritten = HMPlatform::NarrowFormatForGlibc(format);
   const int written = ::vsnprintf(buffer, size, rewritten.c_str(), arguments);
   if (written < 0 || (size_t) written >= size)
   {
      if (size > 0)
         buffer[0] = '\0';
      return -1;
   }
   return written;
}

inline int _vsnprintf_s(char *buffer, size_t size, size_t, const char *format, va_list arguments)
{
   return vsprintf_s(buffer, size, format, arguments);
}

inline int _vscprintf(const char *format, va_list arguments)
{
   va_list copy;
   va_copy(copy, arguments);
   const int needed = ::vsnprintf(nullptr, 0, HMPlatform::NarrowFormatForGlibc(format).c_str(), copy);
   va_end(copy);
   return needed;
}

// The wide forms make their text with HMPlatform::FormatWideForGlibc, which gives
// on musl the text glibc's vswprintf gives (above). _vscwprintf is the length the
// text needs, -1 where it cannot be made; _vstprintf writes it and its NUL, or,
// as vswprintf does, answers -1 when it does not fit, with the buffer emptied.
inline int _vscwprintf(const wchar_t *format, va_list arguments)
{
   std::wstring text;
   va_list copy;
   va_copy(copy, arguments);
   const int written = HMPlatform::FormatWideForGlibc(text, HMPlatform::WideFormatForGlibc(format).c_str(), copy);
   va_end(copy);
   return written;
}

inline int _vstprintf(wchar_t *buffer, size_t size, const wchar_t *format, va_list arguments)
{
   std::wstring text;
   const int written = HMPlatform::FormatWideForGlibc(text, HMPlatform::WideFormatForGlibc(format).c_str(), arguments);
   if (written < 0 || (size_t) written >= size)
   {
      if (size > 0)
         buffer[0] = L'\0';
      return -1;
   }
   ::wmemcpy(buffer, text.data(), (size_t) written);
   buffer[written] = L'\0';
   return written;
}

inline int vswprintf_s(wchar_t *buffer, size_t size, const wchar_t *format, va_list arguments)
{
   return _vstprintf(buffer, size, format, arguments);
}

inline long _ttol(const wchar_t *text)
{
   return ::wcstol(text, nullptr, 10);
}

inline __int64 _atoi64(const char *text)
{
   return (__int64) ::strtoll(text, nullptr, 10);
}

inline long _tstol(const wchar_t *text)
{
   return ::wcstol(text, nullptr, 10);
}

inline double _tstof(const wchar_t *text)
{
   return ::wcstod(text, nullptr);
}

inline int _ttoi(const wchar_t *text)
{
   return (int) ::wcstol(text, nullptr, 10);
}

inline __int64 _ttoi64(const wchar_t *text)
{
   return (__int64) ::wcstoll(text, nullptr, 10);
}

inline double _ttof(const wchar_t *text)
{
   return ::wcstod(text, nullptr);
}

inline long _atol_l_unused(const char *) { return 0; }   // placeholder, never called

inline size_t _tcslen(const wchar_t *text)
{
   return ::wcslen(text);
}

inline int _tcscmp(const wchar_t *a, const wchar_t *b)
{
   return ::wcscmp(a, b);
}

inline int _tcsicmp(const wchar_t *a, const wchar_t *b)
{
   return ::wcscasecmp(a, b);
}

inline int _tcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
   return ::wcsncmp(a, b, n);
}

inline wchar_t *_tcschr(const wchar_t *text, wchar_t c)
{
   return ::wcschr((wchar_t *) text, c);
}

inline wchar_t *_tcsstr(const wchar_t *text, const wchar_t *needle)
{
   return ::wcsstr((wchar_t *) text, needle);
}

inline int _totlower(int c)
{
   return ::towlower((wint_t) c);
}

inline int _totupper(int c)
{
   return ::towupper((wint_t) c);
}

inline errno_t _itot_s(int value, wchar_t *buffer, size_t size, int radix)
{
   if (radix != 10)
      return EINVAL;
   return ::swprintf(buffer, size, L"%d", value) < 0 ? ERANGE : 0;
}

inline int sprintf_s(char *buffer, size_t size, const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   const int written = vsprintf_s(buffer, size, format, arguments);
   va_end(arguments);
   return written;
}

inline int swprintf_s(wchar_t *buffer, size_t size, const wchar_t *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   const int written = _vstprintf(buffer, size, format, arguments);
   va_end(arguments);
   return written;
}
