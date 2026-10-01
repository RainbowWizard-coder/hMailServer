// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Unicode  
   {
   public:
      Unicode();
      virtual ~Unicode();

      static AnsiString ToANSI(const String &sString);
      static bool WideToMultiByte(const String &sInput, AnsiString &sOutput);
      static bool MultiByteToWide(const AnsiString &sInput, String &sOutput);

      // Every character of the text as UTF-8, for a hash, a MAC or a key, where
      // two different texts must never give the same bytes. AnsiString(String)
      // is not that: on Windows it stops at the first character above U+00FF and
      // the rest of the string becomes NUL bytes, and on Linux each such
      // character becomes '?'. False, with the output empty, when the text
      // holds an unpaired surrogate - which UTF-8 cannot carry, and which
      // WideCharToMultiByte would quietly write as U+FFFD, so that two texts
      // would again give the same bytes - or when the conversion fails. For
      // ASCII text the bytes are the ones AnsiString(String) gave, so a hash
      // made the old way over ASCII is the hash made this way.
      static bool ToUtf8Exact(const String &text, AnsiString &utf8);

      // Much of this server holds text as the bytes it arrived as, one to a
      // character: the SMTP and IMAP command lines are widened byte by byte, so
      // is a REST Basic credential, and an address kept in an AnsiString member
      // (Message::from_address_, Account::address_) is read back that way. For
      // such a String the UTF-8 is those bytes; its characters' UTF-8 would be
      // the text encoded twice. True, with the bytes, when every unit is below
      // U+0100, one is above U+007F and the units as bytes are well-formed UTF-8
      // (RFC 3629: no overlong form, no surrogate, nothing above U+10FFFF) -
      // which Latin-1 text almost never is ("café" is not; its E9 begins a
      // sequence it does not complete). decoded, when given, is the text.
      static bool IsWidenedUtf8(const String &text, AnsiString *bytes = nullptr, String *decoded = nullptr);

      // The UTF-8 a String stands for, whichever of the two it holds: the bytes
      // of a widened one (IsWidenedUtf8), the characters' UTF-8 of any other
      // (ToUtf8Exact). So an address as the SMTP command line gave it and the
      // same address decoded give the same bytes, and a hash or a MAC over them
      // agrees. False, with the output empty, for an unpaired surrogate. For
      // ASCII the bytes are the ones AnsiString(String) gave.
      static bool ToUtf8Canonical(const String &text, AnsiString &utf8);

      // The text as UTF-8 for what leaves the server to be read - a JSON body,
      // a CSV, a mail, a header - where AnsiString(String) is not an encoding
      // of it either: ToUtf8Canonical, and a lone surrogate half, which UTF-8
      // cannot carry, written as U+FFFD on both platforms (WideCharToMultiByte
      // did so on Windows; iconv refused the whole string on Linux).
      static AnsiString ToUtf8(const String &text);

      // Whether the text holds a surrogate half that is not part of a high-low
      // pair. Where wchar_t is four bytes every surrogate value is one, since a
      // character outside the Basic Multilingual Plane is a single unit there.
      static bool HasUnpairedSurrogate(const String &text);

      static unsigned char* CharMoveNext(unsigned char*, bool utf8);

      // The on-disk form of a String, and the way back.
      //
      // Every text file this server writes with a byte order mark - the backup
      // and event logs, the backup index, a Sieve script, the rate-limiter state
      // - is UTF-16LE, because on Windows that is what a String's own bytes are
      // and File::Write used to write those bytes as they lay. On Linux a
      // wchar_t is four bytes, so the same code wrote UTF-32 and read UTF-16 as
      // UTF-32, and a file written by either could not be read by the other.
      // These two are the conversion that both platforms now go through: the
      // bytes are UTF-16LE on either, without a mark, and a String read from
      // them holds the same characters on either.
      //
      // Where wchar_t is two bytes the conversion is a copy, so a file written
      // on Windows is byte for byte the file it was before. Where it is four, a
      // character above U+FFFF becomes a surrogate pair on the way out and a
      // pair becomes one character on the way in. An unpaired surrogate is kept
      // as the unit it is in both directions, so a file this program did not
      // write survives a read and a write unchanged.
      static std::string ToUtf16Le(const std::wstring &text);
      static std::wstring FromUtf16Le(const unsigned char *bytes, size_t byteCount);

#ifdef HM_PLATFORM_POSIX
      // The two conversions Windows spells MultiByteToWideChar and
      // WideCharToMultiByte, over iconv - which is what a POSIX system has for the
      // job, and which knows every code page this server names.
      //
      // They are HERE rather than in the platform compatibility header because
      // they are NOT an exact equivalent of the Win32 pair, and that header's rule
      // is that everything in it is. iconv has no counterpart for lpDefaultChar or
      // lpUsedDefaultChar and nothing that matches MB_ERR_INVALID_CHARS, so what
      // these offer is the part of the pair the server actually uses: a code page,
      // a length, and either a buffer to fill or a request for the size one would
      // need.
      //
      // codePage is the Windows code page identifier, which is what the tree
      // already stores in CodePages and passes about; 0 means the machine's own
      // character set, as CP_ACP does on Windows. The length rules are the Win32
      // ones exactly, because the callers were written against them:
      //
      //   sourceLength < 0        the source is NUL-terminated, and the terminator
      //                           is converted and counted with it.
      //   destinationLength == 0  do not convert; answer the length that would be
      //                           needed, in wide characters or in bytes.
      //   the answer 0            the conversion could not be done - an unknown
      //                           code page, a byte sequence that is not valid in
      //                           it, or a destination too small - which is the
      //                           same 0 the Win32 pair returns and which every
      //                           caller here already tests for.
      static int FromCodePage(unsigned int codePage, const char *source, int sourceLength, wchar_t *destination, int destinationLength);
      static int ToCodePage(unsigned int codePage, const wchar_t *source, int sourceLength, char *destination, int destinationLength);

      // The character sets of those code pages - every one this server names
      // but 0, which is the machine's own - by the names iconv knows them by.
      static std::vector<std::string> CodePageCharacterSets();

      // Whether this machine's iconv converts the character set iconv knows by
      // that name, both ways: to and from the wide characters the two above
      // convert through. Not every glibc carries every converter: Fedora's,
      // EL's and openSUSE's keep most of them in a package of their own, which
      // glibc only recommends (platform/main.cpp says what is done with this).
      static bool ConvertsCharacterSet(const char *iconvName);
#endif

   private:

   };


}