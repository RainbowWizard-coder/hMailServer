// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "Unicode.h"

#ifdef HM_PLATFORM_POSIX
#include <iconv.h>
#include <langinfo.h>
#include <cerrno>
// explicit_bzero, for the measuring scratch below: a write the compiler may not
// remove, as it may a memset over storage nothing reads again. Not OpenSSL's
// cleanse: the MIME fuzz harnesses (build/ci/linux-fuzz.sh) build this file
// and link no libcrypto, which a fuzz target has no business carrying.
#include <string.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
#ifdef HM_PLATFORM_POSIX
   namespace
   {
      // Windows code page identifier to the name iconv knows the same character
      // set by. Only the code pages this server names are here - the table in
      // CodePages::Initialize is the list, and 0 is CP_ACP, "whatever this machine
      // is set to", which nl_langinfo answers. A code page not in this table is
      // reported as a failed conversion rather than guessed at: a mail server that
      // silently decodes a header in the wrong character set produces mojibake
      // that nobody can trace back to the guess.
      //
      // Each name is one every iconv this server is built on knows, where there is
      // one: US-ASCII and not glibc's own canonical ANSI_X3.4-1968, which musl's
      // iconv (Alpine) does not know, so that every us-ascii part - what a mail
      // client labels an empty or plain English body - failed to convert there
      // (a script's added text was lost; hmailserver/docs/Alpine.md, "What musl
      // changed"). glibc and GNU libiconv take both.
      //
      // A table rather than a switch so that CodePageCharacterSets can read it
      // too: what the server converts and what it checks a machine for are one
      // list.
      struct CodePageName
      {
         unsigned int codePage;
         const char *iconvName;
      };

      const CodePageName CodePageNames[] =
      {
         { 950,   "BIG5" },
         { 1250,  "CP1250" },
         { 1251,  "CP1251" },
         { 1252,  "CP1252" },
         { 1253,  "CP1253" },
         { 1254,  "CP1254" },
         { 1255,  "CP1255" },
         { 1256,  "CP1256" },
         { 1257,  "CP1257" },
         { 1258,  "CP1258" },
         { 20127, "US-ASCII" },
         { 28592, "ISO-8859-2" },
         { 28593, "ISO-8859-3" },
         { 28594, "ISO-8859-4" },
         { 28595, "ISO-8859-5" },
         { 28596, "ISO-8859-6" },
         { 28597, "ISO-8859-7" },
         { 28598, "ISO-8859-8" },
         { 28599, "ISO-8859-9" },
         { 28603, "ISO-8859-13" },
         { 28605, "ISO-8859-15" },
         { 50221, "ISO-2022-JP-2" },
         { 65000, "UTF-7" },
         { 65001, "UTF-8" }
      };

      const char *IconvNameForCodePage(unsigned int codePage)
      {
         // nl_langinfo is safe from any thread while nothing changes the
         // locale, and the server never calls setlocale or uselocale.
         if (codePage == 0)
            return ::nl_langinfo(CODESET); // NOLINT(concurrency-mt-unsafe): see above

         for (const CodePageName &row : CodePageNames)
         {
            if (row.codePage == codePage)
               return row.iconvName;
         }

         return 0;
      }

      // One conversion, in the two modes the Win32 pair has: measure, or fill.
      //
      // A fresh iconv descriptor per call, deliberately. A descriptor carries
      // shift state for the stateful encodings (ISO-2022-JP is one of the code
      // pages above), so a shared one would carry the tail of somebody else's
      // string into this conversion - and this is called from every connection
      // thread at once.
      //
      // unitSize is the size of one unit of the OUTPUT: iconv counts bytes and the
      // Win32 functions count wide characters in one direction and bytes in the
      // other, so the byte count is divided by it on the way out.
      int ConvertWithIconv(const char *fromName, const char *toName,
                           const char *source, size_t sourceBytes,
                           char *destination, size_t destinationBytes,
                           size_t unitSize)
      {
         if (fromName == 0 || toName == 0 || source == 0)
            return 0;

         iconv_t descriptor = ::iconv_open(toName, fromName);

         if (descriptor == (iconv_t) -1)
            return 0;

         char *inputPosition = (char *) source;
         size_t inputRemaining = sourceBytes;
         int answer = 0;

         if (destination == 0 || destinationBytes == 0)
         {
            // Measure. iconv has no "how long would this be" call, so the text is
            // converted into a scratch buffer that is emptied as it fills and only
            // the total is kept. E2BIG is that buffer filling up, which is the
            // expected outcome here rather than an error; anything else is not.
            // The scratch is written over however the measure ends: what is
            // measured may be a secret - the stored-secrets key's line is, on its
            // way into the machine's store and out of it - and until 6.3.7 the
            // last of it converted stayed on the thread's stack.
            char scratch[512];
            struct ScratchWipe
            {
               char *bytes;
               size_t size;
               ~ScratchWipe() { ::explicit_bzero(bytes, size); }
            } wipeScratch = {scratch, sizeof(scratch)};
            size_t total = 0;

            while (inputRemaining > 0)
            {
               char *outputPosition = scratch;
               size_t outputRemaining = sizeof(scratch);

               const size_t result = ::iconv(descriptor, &inputPosition, &inputRemaining, &outputPosition, &outputRemaining);

               total += sizeof(scratch) - outputRemaining;

               if (result == (size_t) -1 && errno != E2BIG)
               {
                  ::iconv_close(descriptor);
                  return 0;
               }
            }

            // A stateful target - UTF-7 is one - holds its last group back
            // until told the input has ended. Without this the measure is
            // short by that group, and the conversion below is cut at it.
            {
               char *outputPosition = scratch;
               size_t outputRemaining = sizeof(scratch);

               if (::iconv(descriptor, 0, 0, &outputPosition, &outputRemaining) == (size_t) -1)
               {
                  ::iconv_close(descriptor);
                  return 0;
               }

               total += sizeof(scratch) - outputRemaining;
            }

            answer = (int) (total / unitSize);
         }
         else
         {
            char *outputPosition = destination;
            size_t outputRemaining = destinationBytes;

            const size_t result = ::iconv(descriptor, &inputPosition, &inputRemaining, &outputPosition, &outputRemaining);

            if (result == (size_t) -1)
            {
               // Invalid input, an incomplete sequence at the end, or a
               // destination too small. Win32 answers 0 for all three.
               ::iconv_close(descriptor);
               return 0;
            }

            // The flush that ends a stateful encoding's last group; see above.
            if (::iconv(descriptor, 0, 0, &outputPosition, &outputRemaining) == (size_t) -1)
            {
               ::iconv_close(descriptor);
               return 0;
            }

            answer = (int) ((destinationBytes - outputRemaining) / unitSize);
         }

         ::iconv_close(descriptor);

         return answer;
      }
   }

   std::vector<std::string>
   Unicode::CodePageCharacterSets()
   {
      std::vector<std::string> names;
      for (const CodePageName &row : CodePageNames)
         names.push_back(row.iconvName);
      return names;
   }

   bool
   Unicode::ConvertsCharacterSet(const char *iconvName)
   {
      if (iconvName == 0 || *iconvName == 0)
         return false;

      // glibc finds and loads the character set's converter when the conversion
      // is opened, so an open that succeeds is one that converts, and one whose
      // module is not installed fails here, with EINVAL.
      iconv_t to = ::iconv_open(iconvName, "WCHAR_T");
      if (to == (iconv_t) -1)
         return false;
      ::iconv_close(to);

      iconv_t from = ::iconv_open("WCHAR_T", iconvName);
      if (from == (iconv_t) -1)
         return false;
      ::iconv_close(from);

      return true;
   }

   int
   Unicode::FromCodePage(unsigned int codePage, const char *source, int sourceLength, wchar_t *destination, int destinationLength)
   {
      if (source == 0)
         return 0;

      // A length of -1 means "up to and including the terminator", and the
      // Win32 pair counts the terminator in what it answers. The terminator is
      // not given to iconv: glibc's UTF-7 decoder refuses a NUL byte (it is not
      // a directly encoded character), which turned every modified-UTF-7
      // folder name into its first character and heap garbage. The text is
      // converted, and the terminator written and counted here.
      const bool terminated = sourceLength < 0;
      const size_t sourceBytes = terminated ? ::strlen(source) : (size_t) sourceLength;
      const bool measuring = destination == 0 || destinationLength == 0;
      const size_t room = measuring ? 0 : (size_t) destinationLength - (terminated ? 1 : 0);

      int units = 0;

      if (sourceBytes > 0)
      {
         units = ConvertWithIconv(IconvNameForCodePage(codePage), "WCHAR_T",
                                  source, sourceBytes,
                                  measuring ? 0 : reinterpret_cast<char *>(destination), room * sizeof(wchar_t),
                                  sizeof(wchar_t));
         if (units == 0)
            return 0;
      }

      if (terminated)
      {
         if (!measuring)
            destination[units] = 0;
         units++;
      }

      return units;
   }

   int
   Unicode::ToCodePage(unsigned int codePage, const wchar_t *source, int sourceLength, char *destination, int destinationLength)
   {
      if (source == 0)
         return 0;

      // As FromCodePage: the terminator is written and counted here, not
      // converted.
      const bool terminated = sourceLength < 0;
      const size_t sourceUnits = terminated ? ::wcslen(source) : (size_t) sourceLength;
      const bool measuring = destination == 0 || destinationLength == 0;
      const size_t room = measuring ? 0 : (size_t) destinationLength - (terminated ? 1 : 0);

      int bytes = 0;

      if (sourceUnits > 0)
      {
         bytes = ConvertWithIconv("WCHAR_T", IconvNameForCodePage(codePage),
                                  reinterpret_cast<const char *>(source), sourceUnits * sizeof(wchar_t),
                                  measuring ? 0 : destination, room,
                                  1);
         if (bytes == 0)
            return 0;
      }

      if (terminated)
      {
         if (!measuring)
            destination[bytes] = 0;
         bytes++;
      }

      return bytes;
   }
#endif

   Unicode::Unicode()
   {

   }

   Unicode::~Unicode()
   {

   }

   AnsiString 
   Unicode::ToANSI(const String &sString)
   {
      size_t i;
      size_t nLen = (wcslen(sString) + 1) << 1;
      char *pAnsiString = new char [nLen];
#ifdef HM_PLATFORM_POSIX
      // wcstombs is wcstombs_s with the answer in the return value rather than in
      // an out-parameter, and without the guarantee of termination. It answers
      // (size_t) -1 for a character the locale cannot represent and leaves the
      // buffer unspecified, and it does not terminate a result that exactly fills
      // the buffer - so both are terminated by hand, which is what wcstombs_s
      // does for its caller.
      i = ::wcstombs(pAnsiString, sString, nLen);

      if (i == (size_t) -1)
         pAnsiString[0] = '\0';
      else if (i >= nLen)
         pAnsiString[nLen - 1] = '\0';
#else
      wcstombs_s(&i, pAnsiString, nLen, sString, nLen);
#endif
      AnsiString retval = pAnsiString;
      delete [] pAnsiString;

      return retval;
   }

   bool 
   Unicode::WideToMultiByte(const String &sInput, AnsiString &sOutput)
   {
      int iInputLength = sInput.GetLength();

#ifdef HM_PLATFORM_POSIX
      // 65001 is CP_UTF8. See Unicode::ToCodePage: the same two calls, measure
      // then fill, with the same length rules and the same 0 for a failure.
      int nNeedSize = ToCodePage(65001, sInput, iInputLength, NULL, 0);
#else
      int nNeedSize = WideCharToMultiByte(CP_UTF8, 0, sInput, iInputLength, NULL, 0, NULL, NULL );
#endif

      if (nNeedSize == 0)
      {
         // Either the input was empty or the conversion failed. An empty input
         // is a valid (empty) result; anything else is an error.
         sOutput = "";
         return iInputLength == 0;
      }

#ifdef HM_PLATFORM_POSIX
      int nWritten = ToCodePage(65001, sInput, iInputLength, sOutput.GetBuffer(nNeedSize), nNeedSize);
#else
      int nWritten = WideCharToMultiByte( CP_UTF8, 0, sInput, iInputLength, sOutput.GetBuffer(nNeedSize), nNeedSize, NULL, NULL );
#endif
      if (nWritten == 0)
         return false;

      // GetBuffer resized the string to the requested capacity, so trim it back
      // to the exact number of bytes written. Otherwise the result carries a
      // trailing padding byte that callers measuring GetLength() (for example
      // DPAPI protection of stored secrets) would erroneously include.
      sOutput.ReleaseBuffer(nWritten);

      return true;
   }

   bool
   Unicode::ToUtf8Exact(const String &text, AnsiString &utf8)
   {
      utf8.Empty();

      if (HasUnpairedSurrogate(text))
         return false;

      if (!WideToMultiByte(text, utf8))
      {
         utf8.Empty();
         return false;
      }

      return true;
   }

   bool
   Unicode::IsWidenedUtf8(const String &text, AnsiString *bytes, String *decoded)
   {
      const int length = text.GetLength();
      bool high = false;
      for (int i = 0; i < length; i++)
      {
         const unsigned int unit = (unsigned int) text[i];
         if (unit > 0xFF)
            return false;
         if (unit > 0x7F)
            high = true;
      }

      if (!high)
         return false;

      // RFC 3629, section 4, byte by byte.
      for (int i = 0; i < length; )
      {
         const unsigned int lead = (unsigned int) text[i];
         int trail = 0;
         unsigned int low = 0x80, highest = 0xBF;

         if (lead <= 0x7F)
         {
            i++;
            continue;
         }
         else if (lead >= 0xC2 && lead <= 0xDF)
            trail = 1;
         else if (lead == 0xE0)
         {
            trail = 2;
            low = 0xA0;
         }
         else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF)
            trail = 2;
         else if (lead == 0xED)
         {
            trail = 2;
            highest = 0x9F;
         }
         else if (lead == 0xF0)
         {
            trail = 3;
            low = 0x90;
         }
         else if (lead >= 0xF1 && lead <= 0xF3)
            trail = 3;
         else if (lead == 0xF4)
         {
            trail = 3;
            highest = 0x8F;
         }
         else
            return false;

         if (i + trail >= length)
            return false;

         for (int k = 1; k <= trail; k++)
         {
            const unsigned int next = (unsigned int) text[i + k];
            const unsigned int minimum = k == 1 ? low : 0x80;
            const unsigned int maximum = k == 1 ? highest : 0xBF;
            if (next < minimum || next > maximum)
               return false;
         }

         i += trail + 1;
      }

      AnsiString raw;
      raw.resize(length);
      for (int i = 0; i < length; i++)
         raw[i] = (char) (unsigned char) text[i];

      if (decoded != nullptr && !MultiByteToWide(raw, *decoded))
         return false;

      if (bytes != nullptr)
         *bytes = raw;

      return true;
   }

   bool
   Unicode::ToUtf8Canonical(const String &text, AnsiString &utf8)
   {
      if (IsWidenedUtf8(text, &utf8))
         return true;

      return ToUtf8Exact(text, utf8);
   }

   AnsiString
   Unicode::ToUtf8(const String &text)
   {
      AnsiString bytes;
      if (ToUtf8Canonical(text, bytes))
         return bytes;

      String repaired;
      const int length = text.GetLength();
      for (int i = 0; i < length; i++)
      {
         const unsigned int unit = (unsigned int) text[i];

         if (unit >= 0xD800 && unit <= 0xDBFF && sizeof(wchar_t) == 2 && i + 1 < length &&
             (unsigned int) text[i + 1] >= 0xDC00 && (unsigned int) text[i + 1] <= 0xDFFF)
         {
            repaired += text[i];
            repaired += text[i + 1];
            i++;
            continue;
         }

         repaired += (unit >= 0xD800 && unit <= 0xDFFF) ? (wchar_t) 0xFFFD : text[i];
      }

      if (!WideToMultiByte(repaired, bytes))
         bytes.Empty();

      return bytes;
   }

   bool
   Unicode::HasUnpairedSurrogate(const String &text)
   {
      const int length = text.GetLength();
      for (int i = 0; i < length; i++)
      {
         const unsigned int unit = (unsigned int) text[i];

         if (unit >= 0xD800 && unit <= 0xDBFF && sizeof(wchar_t) == 2 && i + 1 < length)
         {
            const unsigned int low = (unsigned int) text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
               i++;
               continue;
            }
         }

         if (unit >= 0xD800 && unit <= 0xDFFF)
            return true;
      }

      return false;
   }

   bool
   Unicode::MultiByteToWide(const AnsiString &sInput, String &sOutput)
   {
      int iInputLength = sInput.GetLength();

      // Empty in, empty out, and answered here rather than below: MultiByteToWideChar
      // returns 0 for cbMultiByte == 0, which is indistinguishable from its error
      // return, so an empty string used to look like a failed conversion - and left
      // sOutput holding whatever it held before.
      if (iInputLength == 0)
      {
         sOutput.Empty();
         return true;
      }

#ifdef HM_PLATFORM_POSIX
      // 65001 is CP_UTF8. See Unicode::FromCodePage.
      int nNeedSize = FromCodePage(65001, sInput, iInputLength, NULL, 0);
#else
      int nNeedSize = MultiByteToWideChar( CP_UTF8, 0, sInput, iInputLength, NULL, 0);
#endif

      if (nNeedSize == 0)
         return false;

      // resize, not GetBuffer alone. CStdStr::GetBuf only ever GROWS -
      // "if (size() < nMinLen) resize(nMinLen)" - and nothing here called
      // ReleaseBuffer to set the final length, so converting a SHORTER value into a
      // String that already held a longer one left the old tail in place.
      //
      // That is not theoretical. SMTPConnection::ProtocolAUTH_ passes the session
      // members username_ and password_ into DecodeSaslPlain, and
      // ResetLoginCredentials_ clears username_ but not password_. So on one
      // connection: AUTH PLAIN with password "LongPassword123" fails, the client
      // retries with "x", and the server validated "xongPassword123" - a credential
      // the client never sent. The user could never authenticate on that connection,
      // and every mangled attempt fed RegisterFailedLogin towards an auto-ban of a
      // legitimate address.
      sOutput.resize(nNeedSize);

#ifdef HM_PLATFORM_POSIX
      if( FromCodePage(65001, sInput, iInputLength, sOutput.GetBuffer(nNeedSize), nNeedSize) == 0 )
#else
      if( MultiByteToWideChar( CP_UTF8, 0, sInput, iInputLength, sOutput.GetBuffer(nNeedSize), nNeedSize ) == 0 )
#endif
         return false;

      return true;
   }

   std::string
   Unicode::ToUtf16Le(const std::wstring &text)
   {
      std::string bytes;
      bytes.reserve(text.size() * 2);

      for (size_t index = 0; index < text.size(); index++)
      {
         unsigned int codePoint = (unsigned int) text[index];

         if (codePoint >= 0x10000 && codePoint <= 0x10FFFF)
         {
            // Only reachable where a wchar_t can hold such a value, which is
            // where it is four bytes. On Windows every character is already a
            // code unit and the loop below writes it as it is - a surrogate half
            // included, so the bytes are exactly the bytes of the String.
            codePoint -= 0x10000;
            const unsigned int high = 0xD800 + (codePoint >> 10);
            const unsigned int low = 0xDC00 + (codePoint & 0x3FF);

            bytes.push_back((char) (high & 0xFF));
            bytes.push_back((char) ((high >> 8) & 0xFF));
            bytes.push_back((char) (low & 0xFF));
            bytes.push_back((char) ((low >> 8) & 0xFF));
         }
         else
         {
            // A value above U+10FFFF is not a character and has no UTF-16 form.
            // No decoder here produces one, so this is a defence against memory
            // that was never a String, and it is written as the replacement
            // character rather than as its low sixteen bits, which would be some
            // other character altogether.
            if (codePoint > 0x10FFFF)
               codePoint = 0xFFFD;

            bytes.push_back((char) (codePoint & 0xFF));
            bytes.push_back((char) ((codePoint >> 8) & 0xFF));
         }
      }

      return bytes;
   }

   std::wstring
   Unicode::FromUtf16Le(const unsigned char *bytes, size_t byteCount)
   {
      std::wstring text;

      if (bytes == 0)
         return text;

      text.reserve(byteCount / 2);

      // Whether a surrogate pair can be joined into one wchar_t, decided by the
      // platform rather than by the input: on Windows a wchar_t is a UTF-16 code
      // unit and the pair IS the character, so the two units are kept as they
      // are and the result is what a copy of the bytes would have been.
      const bool joinsPairs = sizeof(wchar_t) >= 4;

      size_t index = 0;

      // A trailing odd byte is not a code unit and is left out, which is what
      // dividing the byte count by two always did.
      while (index + 1 < byteCount)
      {
         unsigned int unit = (unsigned int) bytes[index] | ((unsigned int) bytes[index + 1] << 8);
         index += 2;

         if (joinsPairs && unit >= 0xD800 && unit <= 0xDBFF && index + 1 < byteCount)
         {
            const unsigned int low = (unsigned int) bytes[index] | ((unsigned int) bytes[index + 1] << 8);

            if (low >= 0xDC00 && low <= 0xDFFF)
            {
               unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
               index += 2;
            }
         }

         text.push_back((wchar_t) unit);
      }

      return text;
   }

   unsigned char*
   Unicode::CharMoveNext(unsigned char* input, bool utf8)
   {
      if (utf8)
      {
         unsigned char* string = input;
         string++;
         while ((*string & 0xc0) == 0x80)
            string++;

         return string;
      }
      else
      {
         return input + 1;
      }
   }

}