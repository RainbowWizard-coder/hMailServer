// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "./SURBL.h"

#include "../../Common/AntiSpam/AntiSpamDiagnostics.h"
#include "../../Common/BO/MessageData.h"
#include "../../Common/BO/SURBLServer.h"
#include "../../Common/TCPIP/DNSResolver.h"
#include "../../SMTP/BLCheck.h"

#include "DMARC/PublicSuffixList.h"
#include "SenderListCache.h"
#include "SenderPattern.h"
#include <boost/regex.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The longest name DNS carries, and the longest label (RFC 1035 section
      // 2.3.4). The Windows resolver refuses a longer one outright, and a
      // lookup that fails abandons the list for the message.
      const size_t maxNameLength = 253;
      const size_t maxLabelLength = 63;

      // Where a link's authority ends and its path, query or fragment begins.
      // The backslash is the WHATWG URL standard's: for http and https it is a
      // slash, so http://good.example\@evil.example/ goes to good.example.
      bool IsPathStart_(wchar_t c)
      {
         return c == L'/' || c == L'?' || c == L'#' || c == L'\\';
      }

      // What ends a link in running text: ASCII white space and the control
      // characters, and Unicode's spaces.
      bool IsWhiteSpace_(wchar_t c)
      {
         return c <= 0x20 || c == 0x7F || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
                c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
      }

      // A character of a name this server can ask a list about.
      bool IsHostCharacter_(wchar_t c)
      {
         return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
      }

      int HexValue_(wchar_t c)
      {
         if (c >= L'0' && c <= L'9')
            return c - L'0';
         if (c >= L'a' && c <= L'f')
            return c - L'a' + 10;
         if (c >= L'A' && c <= L'F')
            return c - L'A' + 10;
         return -1;
      }

      bool IsAsciiAlphanumeric_(wchar_t c)
      {
         return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
      }

      const unsigned int replacementCharacter = 0xFFFD;

      struct CodePointRange
      {
         unsigned int first;
         unsigned int last;
      };

      // The code points UTS 46 marks "ignored" - IdnaMappingTable.txt of
      // Unicode 18.0.0, every line with that status, as the table writes
      // them. A browser deletes them from a host before it looks the name up
      // (the WHATWG URL standard's "domain to ASCII" maps the host by UTS 46),
      // so http://ev<SOFT HYPHEN>il.example/ and a zero-width space inside a
      // name go to evil.example. The rest of UTS 46 - the characters it maps
      // to others (fullwidth letters, U+3002 for a dot), NFC and Punycode - is
      // not done here (Roadmap3, "Internationalised link hosts").
      const CodePointRange ignoredInAHost[] =
      {
         { 0x00AD, 0x00AD },     // SOFT HYPHEN
         { 0x034F, 0x034F },     // COMBINING GRAPHEME JOINER
         { 0x115F, 0x1160 },     // HANGUL CHOSEONG FILLER..HANGUL JUNGSEONG FILLER
         { 0x17B4, 0x17B5 },     // KHMER VOWEL INHERENT AQ..KHMER VOWEL INHERENT AA
         { 0x180B, 0x180E },     // MONGOLIAN FREE VARIATION SELECTOR ONE..MONGOLIAN VOWEL SEPARATOR
         { 0x180F, 0x180F },     // MONGOLIAN FREE VARIATION SELECTOR FOUR
         { 0x200B, 0x200B },     // ZERO WIDTH SPACE
         { 0x2060, 0x2063 },     // WORD JOINER..INVISIBLE SEPARATOR
         { 0x2064, 0x2064 },     // INVISIBLE PLUS
         { 0x206A, 0x206F },     // INHIBIT SYMMETRIC SWAPPING..NOMINAL DIGIT SHAPES
         { 0x3164, 0x3164 },     // HANGUL FILLER
         { 0xFE00, 0xFE0F },     // VARIATION SELECTOR-1..VARIATION SELECTOR-16
         { 0xFEFF, 0xFEFF },     // ZERO WIDTH NO-BREAK SPACE
         { 0xFFA0, 0xFFA0 },     // HALFWIDTH HANGUL FILLER
         { 0x1BCA0, 0x1BCA3 },   // SHORTHAND FORMAT LETTER OVERLAP..SHORTHAND FORMAT UP STEP
         { 0x1D173, 0x1D17A },   // MUSICAL SYMBOL BEGIN BEAM..MUSICAL SYMBOL END PHRASE
         { 0xE0100, 0xE01EF },   // VARIATION SELECTOR-17..VARIATION SELECTOR-256
      };

      bool IsIgnoredInAHost_(unsigned int codePoint)
      {
         for (const CodePointRange &range : ignoredInAHost)
            if (codePoint >= range.first && codePoint <= range.last)
               return true;
         return false;
      }

      // text[i] as the number it holds, through wchar_t's own unsigned type: a
      // UTF-16 unit on Windows, where wchar_t is unsigned, and the code point
      // elsewhere, where it is signed and a value past U+10FFFF stays one.
      unsigned int UnitAt_(const std::wstring &text, size_t i)
      {
         return static_cast<std::make_unsigned<wchar_t>::type>(text[i]);
      }

      // The code point at text[i], and how many wchar_t it takes. On Windows a
      // wchar_t is a UTF-16 unit: a pair of surrogates is one code point, and a
      // surrogate on its own is U+FFFD, as a browser reads a string. Elsewhere
      // a wchar_t is the code point.
      unsigned int CodePointAt_(const std::wstring &text, size_t i, size_t &units)
      {
         units = 1;
         const unsigned int unit = UnitAt_(text, i);

         if (sizeof(wchar_t) == 2 && unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size())
         {
            const unsigned int low = UnitAt_(text, i + 1);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
               units = 2;
               return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            }
         }

         if ((unit >= 0xD800 && unit <= 0xDFFF) || unit > 0x10FFFF)
            return replacementCharacter;

         return unit;
      }

      // A code point appended as one wchar_t, or as a pair of surrogates where a
      // wchar_t is a UTF-16 unit and the code point is past U+FFFF.
      void AppendCodePoint_(unsigned int codePoint, std::wstring &text)
      {
         if (sizeof(wchar_t) == 2 && codePoint > 0xFFFF)
         {
            const unsigned int offset = codePoint - 0x10000;
            text += static_cast<wchar_t>(0xD800 + (offset >> 10));
            text += static_cast<wchar_t>(0xDC00 + (offset & 0x3FF));
            return;
         }

         text += static_cast<wchar_t>(codePoint);
      }

      void AppendByte_(unsigned int byte, std::string &bytes)
      {
         bytes += static_cast<char>(static_cast<unsigned char>(byte));
      }

      void AppendUtf8_(unsigned int codePoint, std::string &bytes)
      {
         if (codePoint < 0x80)
         {
            AppendByte_(codePoint, bytes);
         }
         else if (codePoint < 0x800)
         {
            AppendByte_(0xC0 | (codePoint >> 6), bytes);
            AppendByte_(0x80 | (codePoint & 0x3F), bytes);
         }
         else if (codePoint < 0x10000)
         {
            AppendByte_(0xE0 | (codePoint >> 12), bytes);
            AppendByte_(0x80 | ((codePoint >> 6) & 0x3F), bytes);
            AppendByte_(0x80 | (codePoint & 0x3F), bytes);
         }
         else
         {
            AppendByte_(0xF0 | (codePoint >> 18), bytes);
            AppendByte_(0x80 | ((codePoint >> 12) & 0x3F), bytes);
            AppendByte_(0x80 | ((codePoint >> 6) & 0x3F), bytes);
            AppendByte_(0x80 | (codePoint & 0x3F), bytes);
         }
      }

      // The Encoding standard's UTF-8 decoder, as the URL standard runs it on
      // a percent-decoded host: a byte sequence that is not UTF-8 is U+FFFD,
      // one for each maximal part of a sequence that is wrong.
      std::vector<unsigned int> DecodeUtf8_(const std::string &bytes)
      {
         std::vector<unsigned int> codePoints;
         codePoints.reserve(bytes.size());

         unsigned int codePoint = 0;
         unsigned int bytesNeeded = 0;
         unsigned int bytesSeen = 0;
         unsigned int lowerBoundary = 0x80;
         unsigned int upperBoundary = 0xBF;

         size_t i = 0;
         while (i < bytes.size())
         {
            const unsigned int byte = static_cast<unsigned char>(bytes[i]);

            if (bytesNeeded == 0)
            {
               i++;

               if (byte <= 0x7F)
               {
                  codePoints.push_back(byte);
               }
               else if (byte >= 0xC2 && byte <= 0xDF)
               {
                  bytesNeeded = 1;
                  codePoint = byte & 0x1F;
               }
               else if (byte >= 0xE0 && byte <= 0xEF)
               {
                  if (byte == 0xE0)
                     lowerBoundary = 0xA0;
                  if (byte == 0xED)
                     upperBoundary = 0x9F;
                  bytesNeeded = 2;
                  codePoint = byte & 0xF;
               }
               else if (byte >= 0xF0 && byte <= 0xF4)
               {
                  if (byte == 0xF0)
                     lowerBoundary = 0x90;
                  if (byte == 0xF4)
                     upperBoundary = 0x8F;
                  bytesNeeded = 3;
                  codePoint = byte & 0x7;
               }
               else
               {
                  codePoints.push_back(replacementCharacter);
               }

               continue;
            }

            if (byte < lowerBoundary || byte > upperBoundary)
            {
               // The sequence ends before this byte, which is read again as
               // the start of the next.
               codePoint = bytesNeeded = bytesSeen = 0;
               lowerBoundary = 0x80;
               upperBoundary = 0xBF;
               codePoints.push_back(replacementCharacter);
               continue;
            }

            i++;
            lowerBoundary = 0x80;
            upperBoundary = 0xBF;
            codePoint = (codePoint << 6) | (byte & 0x3F);
            bytesSeen++;

            if (bytesSeen != bytesNeeded)
               continue;

            codePoints.push_back(codePoint);
            codePoint = bytesNeeded = bytesSeen = 0;
         }

         if (bytesNeeded != 0)
            codePoints.push_back(replacementCharacter);

         return codePoints;
      }

      // The URL standard's percent-decoding of a string, then its UTF-8 decoding:
      // each character as its UTF-8 bytes and each %XX as its byte, the bytes
      // read as UTF-8. So %C2%AD is a soft hyphen, and a byte that is not
      // part of a UTF-8 sequence is U+FFFD.
      std::vector<unsigned int> PercentDecodedCodePoints_(const std::wstring &value)
      {
         std::string bytes;
         bytes.reserve(value.size());

         for (size_t i = 0; i < value.size();)
         {
            if (value[i] == L'%' && i + 2 < value.size())
            {
               const int high = HexValue_(value[i + 1]);
               const int low = HexValue_(value[i + 2]);
               if (high >= 0 && low >= 0)
               {
                  AppendByte_(static_cast<unsigned int>(high * 16 + low), bytes);
                  i += 3;
                  continue;
               }
            }

            size_t units = 1;
            AppendUtf8_(CodePointAt_(value, i, units), bytes);
            i += units;
         }

         return DecodeUtf8_(bytes);
      }

      std::wstring PercentDecoded_(const std::wstring &value)
      {
         std::wstring decoded;
         decoded.reserve(value.size());
         for (unsigned int codePoint : PercentDecodedCodePoints_(value))
            AppendCodePoint_(codePoint, decoded);
         return decoded;
      }

      struct NamedReference
      {
         const wchar_t *name;
         unsigned int character;
      };

      // Every named character reference HTML defines for an ASCII character -
      // the 45 written with their semicolon and the 8 legacy ones without it,
      // the 53 of html.spec.whatwg.org/entities.json - the two for the
      // no-break space, which ends a link in text, and the 14 that stand for
      // one code point UTS 46 ignores (entities.json again), which a browser
      // deletes from a host: &shy; is a soft hyphen, &ZeroWidthSpace; and its
      // four other names a zero-width space. No named reference stands for an
      // ASCII letter, digit or hyphen; a numeric one can stand for any.
      const NamedReference namedReferences[] =
      {
         { L"shy;", 0x00AD }, { L"ZeroWidthSpace;", 0x200B }, { L"NegativeVeryThinSpace;", 0x200B },
         { L"NegativeThinSpace;", 0x200B }, { L"NegativeMediumSpace;", 0x200B }, { L"NegativeThickSpace;", 0x200B },
         { L"NoBreak;", 0x2060 }, { L"ApplyFunction;", 0x2061 }, { L"af;", 0x2061 }, { L"InvisibleTimes;", 0x2062 },
         { L"it;", 0x2062 }, { L"InvisibleComma;", 0x2063 }, { L"ic;", 0x2063 },
         { L"Tab;", L'\t' }, { L"NewLine;", L'\n' }, { L"excl;", L'!' }, { L"quot;", L'"' },
         { L"QUOT;", L'"' }, { L"num;", L'#' }, { L"dollar;", L'$' }, { L"percnt;", L'%' },
         { L"amp;", L'&' }, { L"AMP;", L'&' }, { L"apos;", L'\'' }, { L"lpar;", L'(' },
         { L"rpar;", L')' }, { L"ast;", L'*' }, { L"midast;", L'*' }, { L"plus;", L'+' },
         { L"comma;", L',' }, { L"period;", L'.' }, { L"sol;", L'/' }, { L"colon;", L':' },
         { L"semi;", L';' }, { L"lt;", L'<' }, { L"LT;", L'<' }, { L"equals;", L'=' },
         { L"gt;", L'>' }, { L"GT;", L'>' }, { L"quest;", L'?' }, { L"commat;", L'@' },
         { L"lbrack;", L'[' }, { L"lsqb;", L'[' }, { L"bsol;", L'\\' }, { L"rbrack;", L']' },
         { L"rsqb;", L']' }, { L"Hat;", L'^' }, { L"UnderBar;", L'_' }, { L"lowbar;", L'_' },
         { L"DiacriticalGrave;", L'`' }, { L"grave;", L'`' }, { L"lbrace;", L'{' }, { L"lcub;", L'{' },
         { L"VerticalLine;", L'|' }, { L"verbar;", L'|' }, { L"vert;", L'|' }, { L"rbrace;", L'}' },
         { L"rcub;", L'}' }, { L"nbsp;", 0xA0 }, { L"NonBreakingSpace;", 0xA0 },
         // Legacy: read without the semicolon, except where an '=' or a letter
         // or digit follows, as in an attribute's value.
         { L"amp", L'&' }, { L"AMP", L'&' }, { L"quot", L'"' }, { L"QUOT", L'"' },
         { L"lt", L'<' }, { L"LT", L'<' }, { L"gt", L'>' }, { L"GT", L'>' }, { L"nbsp", 0xA0 },
         { L"shy", 0x00AD },
      };

      // Reads the character reference at text[position] ('&'), as the HTML
      // tokenizer does: numeric, decimal or hexadecimal, with or without its
      // semicolon, and the named ones above. True, with the code point and the
      // reference's length, when there is one.
      bool ReadCharacterReference_(const std::wstring &text, size_t position, unsigned int &character, size_t &length)
      {
         const size_t textLength = text.size();

         if (position + 1 < textLength && text[position + 1] == L'#')
         {
            size_t i = position + 2;
            const bool hexadecimal = i < textLength && (text[i] == L'x' || text[i] == L'X');
            if (hexadecimal)
               i++;

            const size_t digitsStart = i;
            unsigned long long value = 0;
            while (i < textLength)
            {
               const int digit = hexadecimal ? HexValue_(text[i]) : (text[i] >= L'0' && text[i] <= L'9' ? text[i] - L'0' : -1);
               if (digit < 0)
                  break;

               // Anything past U+10FFFF is U+FFFD however long it is.
               if (value <= 0x10FFFF)
                  value = value * (hexadecimal ? 16 : 10) + digit;
               i++;
            }

            if (i == digitsStart)
               return false;

            if (i < textLength && text[i] == L';')
               i++;

            // NUL, a surrogate, a code point past Unicode's last, and the C1
            // controls - which HTML reads as windows-1252, never as ASCII -
            // are U+FFFD here. A code point outside the Basic Multilingual
            // Plane is itself, a pair of surrogates on Windows: it is never
            // ASCII and never a space, but VARIATION SELECTOR-17 to -256 and a
            // few others are code points a browser deletes from a host.
            if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) || (value >= 0x80 && value <= 0x9F))
               character = replacementCharacter;
            else
               character = static_cast<unsigned int>(value);

            length = i - position;
            return true;
         }

         for (const NamedReference &reference : namedReferences)
         {
            const size_t nameLength = wcslen(reference.name);
            if (position + 1 + nameLength > textLength)
               continue;

            if (wcsncmp(text.c_str() + position + 1, reference.name, nameLength) != 0)
               continue;

            if (reference.name[nameLength - 1] != L';')
            {
               const size_t next = position + 1 + nameLength;
               if (next < textLength && (text[next] == L'=' || IsAsciiAlphanumeric_(text[next])))
                  continue;
            }

            character = reference.character;
            length = 1 + nameLength;
            return true;
         }

         return false;
      }

      // The HTML with its character references read, and for each character
      // whether a reference wrote it: a quote written as &quot; inside an
      // attribute's value is part of the value, not its end.
      void DecodeCharacterReferences_(const std::wstring &html, std::wstring &text, std::vector<bool> &fromReference)
      {
         text.reserve(html.size());
         fromReference.reserve(html.size());

         for (size_t i = 0; i < html.size(); i++)
         {
            unsigned int character = 0;
            size_t length = 0;
            if (html[i] == L'&' && ReadCharacterReference_(html, i, character, length))
            {
               // One flag for each wchar_t written: a pair of surrogates on
               // Windows is two.
               const size_t before = text.size();
               AppendCodePoint_(character, text);
               fromReference.insert(fromReference.end(), text.size() - before, true);
               i += length - 1;
               continue;
            }

            text += html[i];
            fromReference.push_back(false);
         }
      }

      std::vector<std::wstring> SplitOnDots_(const std::wstring &value)
      {
         std::vector<std::wstring> parts;
         size_t start = 0;
         for (;;)
         {
            const size_t dot = value.find(L'.', start);
            if (dot == std::wstring::npos)
            {
               parts.push_back(value.substr(start));
               return parts;
            }

            parts.push_back(value.substr(start, dot - start));
            start = dot + 1;
         }
      }

      std::wstring WithoutTrailingDots_(const std::wstring &value)
      {
         size_t end = value.size();
         while (end > 0 && value[end - 1] == L'.')
            end--;
         return value.substr(0, end);
      }

      // The WHATWG URL standard's IPv4 number parser: decimal, 0x hexadecimal,
      // or 0 octal, of any length. Values past 2^32 are held there, which no
      // part of an address can be.
      bool ParseIPv4Number_(const std::wstring &input, unsigned long long &value)
      {
         if (input.empty())
            return false;

         std::wstring digits = input;
         unsigned int radix = 10;
         if (digits.size() >= 2 && digits[0] == L'0' && (digits[1] == L'x' || digits[1] == L'X'))
         {
            digits = digits.substr(2);
            radix = 16;
         }
         else if (digits.size() >= 2 && digits[0] == L'0')
         {
            digits = digits.substr(1);
            radix = 8;
         }

         value = 0;
         for (size_t i = 0; i < digits.size(); i++)
         {
            const int digit = HexValue_(digits[i]);
            if (digit < 0 || static_cast<unsigned int>(digit) >= radix)
               return false;

            if (value <= 0xFFFFFFFFULL)
               value = value * radix + static_cast<unsigned int>(digit);
         }

         if (value > 0xFFFFFFFFULL)
            value = 0x100000000ULL;

         return true;
      }

      // The WHATWG host parser's "ends in a number": a host whose last label
      // is a number is an IPv4 address, or no host at all.
      bool EndsInANumber_(const std::wstring &host)
      {
         std::vector<std::wstring> parts = SplitOnDots_(host);
         if (parts.back().empty())
         {
            if (parts.size() == 1)
               return false;
            parts.pop_back();
         }

         const std::wstring &last = parts.back();
         if (last.empty())
            return false;

         bool digitsOnly = true;
         for (wchar_t c : last)
            if (c < L'0' || c > L'9')
               digitsOnly = false;

         if (digitsOnly)
            return true;

         unsigned long long ignored = 0;
         return last.size() >= 2 && last[0] == L'0' && (last[1] == L'x' || last[1] == L'X') && ParseIPv4Number_(last, ignored);
      }

      // The WHATWG IPv4 parser: one to four parts, the last filling the bytes
      // the others left, so 0x7f.1, 0177.0.0.1 and 2130706433 are all
      // 127.0.0.1. False where a browser fails the host.
      bool ParseIPv4_(const std::wstring &host, String &dotted)
      {
         std::vector<std::wstring> parts = SplitOnDots_(host);
         if (parts.back().empty() && parts.size() > 1)
            parts.pop_back();

         if (parts.size() > 4)
            return false;

         std::vector<unsigned long long> numbers;
         for (const std::wstring &part : parts)
         {
            unsigned long long number = 0;
            if (!ParseIPv4Number_(part, number))
               return false;
            numbers.push_back(number);
         }

         for (size_t i = 0; i + 1 < numbers.size(); i++)
            if (numbers[i] > 255)
               return false;

         unsigned long long limit = 1;
         for (size_t i = 0; i < 5 - numbers.size(); i++)
            limit *= 256;

         if (numbers.back() >= limit)
            return false;

         unsigned long long address = numbers.back();
         for (size_t i = 0; i + 1 < numbers.size(); i++)
         {
            unsigned long long weight = 1;
            for (size_t j = 0; j < 3 - i; j++)
               weight *= 256;
            address += numbers[i] * weight;
         }

         dotted.Format(_T("%u.%u.%u.%u"), static_cast<unsigned int>((address >> 24) & 0xFF), static_cast<unsigned int>((address >> 16) & 0xFF),
            static_cast<unsigned int>((address >> 8) & 0xFF), static_cast<unsigned int>(address & 0xFF));
         return true;
      }

      // A name this server asks a list about: two labels or more, each of 1 to
      // 63 host characters, 253 characters at most.
      bool IsName_(const std::wstring &name)
      {
         if (name.empty() || name.size() > maxNameLength)
            return false;

         std::vector<std::wstring> labels = SplitOnDots_(name);
         if (labels.size() < 2)
            return false;

         for (const std::wstring &label : labels)
         {
            if (label.empty() || label.size() > maxLabelLength)
               return false;

            for (wchar_t c : label)
               if (!IsHostCharacter_(c))
                  return false;
         }

         return true;
      }

      // A candidate host in the form it is handed over: an address in dotted
      // decimal, or a name without its trailing dots and its "www.". Empty
      // when it is neither.
      String Canonical_(const std::wstring &candidate)
      {
         const std::wstring name = WithoutTrailingDots_(candidate);

         if (EndsInANumber_(candidate))
         {
            String dotted;
            if (ParseIPv4_(candidate, dotted))
               return dotted;
            return String();
         }

         if (!IsName_(name))
            return String();

         // As the regular expression before 6.3.6 dropped it, and as the URL
         // entries of the sender lists do: www.example.com is example.com.
         if (name.size() > 4 && name.compare(0, 4, L"www.") == 0 && IsName_(name.substr(4)))
            return String(name.substr(4));

         return String(name);
      }

      // A host part as a browser reads it before it looks the name up: percent-
      // decoded as UTF-8 (%C2%AD is a soft hyphen), the code points UTS 46
      // ignores deleted, and ASCII in lower case. Until 6.3.6 a
      // byte past ASCII was U+FFFD and nothing was deleted, so
      // http://ev%C2%ADil.example/ and a zero-width space in a name were no
      // name at all, where a browser goes to evil.example.
      std::wstring HostDecoded_(const std::wstring &hostPart)
      {
         std::wstring host;
         host.reserve(hostPart.size());

         for (unsigned int codePoint : PercentDecodedCodePoints_(hostPart))
         {
            if (IsIgnoredInAHost_(codePoint))
               continue;

            if (codePoint >= static_cast<unsigned int>('A') && codePoint <= static_cast<unsigned int>('Z'))
               codePoint += static_cast<unsigned int>('a' - 'A');

            AppendCodePoint_(codePoint, host);
         }

         return host;
      }

      // The hosts one link's host part hands over (SURBL::FindLinkHosts).
      void HostsOfHostPart_(const std::wstring &hostPart, std::vector<String> &hosts)
      {
         const std::wstring host = HostDecoded_(hostPart);

         // A host whose last label is a number is an address, or nothing a
         // browser can reach.
         if (EndsInANumber_(host))
         {
            String dotted;
            if (ParseIPv4_(host, dotted))
               hosts.push_back(dotted);
            return;
         }

         // The name up to the first character a name cannot hold: what a
         // link followed by punctuation ("(see http://example.com)") names,
         // and what the regular expression before 6.3.6 took.
         size_t run = 0;
         while (run < host.size() && (IsHostCharacter_(host[run]) || host[run] == L'.'))
            run++;

         const String leading = Canonical_(host.substr(0, run));
         if (!leading.empty())
            hosts.push_back(leading);

         if (run == host.size())
            return;

         // The labels after the last label that holds such a character: the
         // name a link that works is registered under, whatever comes before
         // it - "a!b.evil.example", or a label in Unicode.
         std::vector<std::wstring> labels = SplitOnDots_(WithoutTrailingDots_(host));
         size_t firstClean = 0;
         for (size_t i = 0; i < labels.size(); i++)
         {
            for (wchar_t c : labels[i])
            {
               if (!IsHostCharacter_(c))
               {
                  firstClean = i + 1;
                  break;
               }
            }
         }

         if (firstClean == 0 || firstClean >= labels.size())
            return;

         std::wstring suffix;
         for (size_t i = firstClean; i < labels.size(); i++)
         {
            if (!suffix.empty())
               suffix += L".";
            suffix += labels[i];
         }

         const String registered = Canonical_(suffix);
         if (!registered.empty() && registered != leading)
            hosts.push_back(registered);
      }

      // The WHATWG URL standard's IPv6 parser, on what is between a host's
      // brackets: eight pieces of up to four hexadecimal digits, "::" once for
      // a run of zero pieces, and the last two pieces written as a dotted IPv4
      // address if wished (four decimal parts of 0 to 255, none with a leading
      // zero). False where a browser fails the host; a zone ("%25eth0") is one
      // such.
      bool ParseIPv6_(const std::wstring &input, unsigned int (&address)[8])
      {
         for (unsigned int &piece : address)
            piece = 0;

         const size_t length = input.size();
         size_t pointer = 0;
         size_t pieceIndex = 0;
         bool compressed = false;
         size_t compress = 0;

         auto at = [&](size_t i) -> wchar_t { return i < length ? input[i] : 0; };

         if (at(pointer) == L':')
         {
            if (at(pointer + 1) != L':')
               return false;
            pointer += 2;
            pieceIndex++;
            compressed = true;
            compress = pieceIndex;
         }

         while (pointer < length)
         {
            if (pieceIndex == 8)
               return false;

            if (at(pointer) == L':')
            {
               if (compressed)
                  return false;
               pointer++;
               pieceIndex++;
               compressed = true;
               compress = pieceIndex;
               continue;
            }

            unsigned int value = 0;
            size_t digits = 0;
            while (digits < 4 && pointer < length && HexValue_(at(pointer)) >= 0)
            {
               value = value * 16 + static_cast<unsigned int>(HexValue_(at(pointer)));
               pointer++;
               digits++;
            }

            if (at(pointer) == L'.')
            {
               if (digits == 0)
                  return false;
               pointer -= digits;
               if (pieceIndex > 6)
                  return false;

               int numbersSeen = 0;
               while (pointer < length)
               {
                  if (numbersSeen > 0)
                  {
                     if (at(pointer) == L'.' && numbersSeen < 4)
                        pointer++;
                     else
                        return false;
                  }

                  if (at(pointer) < L'0' || at(pointer) > L'9')
                     return false;

                  int part = -1;
                  while (at(pointer) >= L'0' && at(pointer) <= L'9')
                  {
                     const int number = at(pointer) - L'0';
                     if (part == -1)
                        part = number;
                     else if (part == 0)
                        return false;
                     else
                        part = part * 10 + number;

                     if (part > 255)
                        return false;
                     pointer++;
                  }

                  address[pieceIndex] = address[pieceIndex] * 0x100 + static_cast<unsigned int>(part);
                  numbersSeen++;
                  if (numbersSeen == 2 || numbersSeen == 4)
                     pieceIndex++;
               }

               if (numbersSeen != 4)
                  return false;

               break;
            }
            else if (at(pointer) == L':')
            {
               pointer++;
               if (pointer >= length)
                  return false;
            }
            else if (pointer < length)
            {
               return false;
            }

            address[pieceIndex] = value;
            pieceIndex++;
         }

         if (compressed)
         {
            size_t swaps = pieceIndex - compress;
            pieceIndex = 7;
            while (pieceIndex != 0 && swaps > 0)
            {
               std::swap(address[pieceIndex], address[compress + swaps - 1]);
               pieceIndex--;
               swaps--;
            }
         }
         else if (pieceIndex != 8)
         {
            return false;
         }

         return true;
      }

      // The IPv4 address a browser reaches when a link names an IPv6 address
      // that carries one: an IPv4-mapped address (::ffff:0:0/96, RFC 4291
      // section 2.5.5.2), which a dual-stack system connects to over IPv4, and
      // one under the NAT64 well-known prefix (64:ff9b::/96, RFC 6052), which
      // exists only to be carried by a translator to the IPv4 address in its
      // last 32 bits. So http://[::ffff:192.0.2.1]/ and
      // http://[64:ff9b::c000:201]/ are 192.0.2.1, in dotted decimal. False
      // for any other IPv6 address, and for text no browser reads as one.
      bool EmbeddedIPv4_(const std::wstring &bracketed, std::wstring &dotted)
      {
         unsigned int address[8];
         if (!ParseIPv6_(bracketed, address))
            return false;

         const bool mapped = address[0] == 0 && address[1] == 0 && address[2] == 0 && address[3] == 0 &&
                             address[4] == 0 && address[5] == 0xFFFF;
         const bool nat64 = address[0] == 0x64 && address[1] == 0xFF9B && address[2] == 0 && address[3] == 0 &&
                            address[4] == 0 && address[5] == 0;
         if (!mapped && !nat64)
            return false;

         const unsigned int octets[4] = { address[6] >> 8, address[6] & 0xFF, address[7] >> 8, address[7] & 0xFF };
         dotted.clear();
         for (int i = 0; i < 4; i++)
         {
            if (i > 0)
               dotted += L'.';
            dotted += std::to_wstring(octets[i]);
         }

         return true;
      }

      // The host part of a link's authority. False when there is none to read.
      bool HostPartOfAuthority_(const std::wstring &authority, std::wstring &hostPart)
      {
         // The userinfo runs to the last '@' (RFC 3986 section 3.2.1, and the
         // WHATWG authority state, which takes every '@' before the host into
         // the userinfo). "%40" is an '@' in the userinfo's text, not its end.
         const size_t at = authority.rfind(L'@');
         const std::wstring hostAndPort = at == std::wstring::npos ? authority : authority.substr(at + 1);

         if (hostAndPort.empty())
            return false;

         // An IPv6 address, or RFC 3986's IPvFuture. No URI list publishes
         // either, so neither is asked about - but an IPv6 address that
         // carries an IPv4 one a browser reaches is that IPv4 address. Until
         // 6.3.6 no bracketed host was, so
         // http://[::ffff:192.0.2.1]/ escaped a listing of 192.0.2.1.
         if (hostAndPort[0] == L'[')
         {
            const size_t close = hostAndPort.find(L']');
            return close != std::wstring::npos && EmbeddedIPv4_(hostAndPort.substr(1, close - 1), hostPart);
         }

         const size_t colon = hostAndPort.find(L':');
         hostPart = colon == std::wstring::npos ? hostAndPort : hostAndPort.substr(0, colon);
         return true;
      }

      // The hosts one link's authority hands over.
      void HostsOfAuthority_(const std::wstring &authority, std::vector<String> &hosts)
      {
         std::wstring hostPart;
         if (HostPartOfAuthority_(authority, hostPart))
            HostsOfHostPart_(hostPart, hosts);
      }

      // In running text a link is made by the mail program that shows it, not
      // by a URL parser, and a mail program ends a link at characters a URL
      // parser takes into the userinfo: Thunderbird at '"', '`', '{', '}', an
      // unmatched ')' or ']', '[' and the bidirectional marks
      // (mozTXTToHTMLConv::FindURLEnd), so a click on
      // http://evil.example)@innocent.example/ there goes to evil.example.
      // Each part of the userinfo, between its '@'s, that holds a character
      // other than a name's, '.', ':' and '%' hands over the host a link ended
      // in it would have: the name it starts with, before any port. A userinfo
      // of names alone (http://innocent.example@evil.example/) hands over
      // nothing more: every program goes to the host after it.
      //
      // Whether a host was handed over already is looked up, not searched
      // for among those before it: searched, the time grew with the square of
      // the number of names in the user name, and a link of a megabyte took
      // seconds to read.
      void HostsInTheUserinfo_(const std::wstring &authority, std::vector<String> &hosts)
      {
         const size_t lastAt = authority.rfind(L'@');
         if (lastAt == std::wstring::npos)
            return;

         std::set<String> handedOver(hosts.begin(), hosts.end());

         size_t start = 0;
         while (start <= lastAt)
         {
            const size_t at = authority.find(L'@', start);
            const std::wstring part = authority.substr(start, at - start);
            start = at + 1;

            bool ended = false;
            for (wchar_t c : part)
            {
               if (!IsAsciiAlphanumeric_(c) && c != L'-' && c != L'_' && c != L'.' && c != L':' && c != L'%')
               {
                  ended = true;
                  break;
               }
            }

            if (!ended)
               continue;

            const size_t colon = part.find(L':');
            std::vector<String> found;
            HostsOfHostPart_(colon == std::wstring::npos ? part : part.substr(0, colon), found);
            for (const String &host : found)
               if (handedOver.insert(host).second)
                  hosts.push_back(host);
         }
      }

      // The code points UTS 46 ignores that a regular expression's \s matches
      // in some engine: U+FEFF, which JavaScript's \s matches, and U+180E and
      // U+200B, which were Unicode's spaces until 6.3 and 4.0.1 and still are
      // to an engine with older tables (Java 8's Character.isWhitespace takes
      // U+180E). Each is in the Basic Multilingual Plane, one wchar_t on every
      // platform.
      const wchar_t spacesToSomeEngine[] = { 0xFEFF, 0x180E, 0x200B };

      // A link in running text is made by the mail program that shows it, and
      // a webmail's script that ends a link at white space by its \s ends one
      // at such a code point, where a browser given the whole link deletes it
      // from the host: http://evil.example<U+FEFF>x/ goes to evil.examplex
      // from one program and to evil.example from another. So the hosts of
      // the authority before the first of each of them - where an engine that
      // takes it, or it among others, for white space ends the link - are
      // handed over too, after the others. The reading that deleted no code
      // point handed over the name before one, as the name before any
      // character a name cannot hold; without this, deleting them lost it.
      void HostsBeforeASpaceToSomeEngine_(const std::wstring &authority, std::vector<String> &hosts)
      {
         std::set<size_t> ends;
         for (wchar_t space : spacesToSomeEngine)
         {
            const size_t at = authority.find(space);
            if (at != std::wstring::npos)
               ends.insert(at);
         }

         if (ends.empty())
            return;

         std::set<String> handedOver(hosts.begin(), hosts.end());
         for (size_t end : ends)
         {
            const std::wstring before = authority.substr(0, end);
            std::vector<String> found;
            HostsOfAuthority_(before, found);
            HostsInTheUserinfo_(before, found);

            for (const String &host : found)
               if (handedOver.insert(host).second)
                  hosts.push_back(host);
         }
      }

      // Reads the authority of a link whose separator ends at start. quote is
      // the character the link was opened by, when one was; attribute says
      // the link is an HTML attribute's quoted value, which runs to that
      // quote. encoded says the link was written in another's query, with its
      // own separator as %3A%2F%2F. Returns where the reading stopped.
      size_t ReadAuthority_(const std::wstring &text, const std::vector<bool> &fromReference, size_t start,
                            wchar_t quote, bool attribute, bool encoded, std::wstring &authority)
      {
         const size_t length = text.size();
         size_t i = start;

         // In a value the URL parser reads, tabs and line breaks are dropped
         // before anything else, and every slash and backslash after "http:"
         // is skipped: <a href="http:&#10;//evil.example/"> goes to
         // evil.example, where the separator is "http:" alone and the reading
         // would have stopped at the first slash. Until 6.3.6
         // it did.
         if (attribute)
         {
            while (i < length && (text[i] == L'/' || text[i] == L'\\' || text[i] == L'\t' || text[i] == L'\r' || text[i] == L'\n'))
               i++;
         }

         for (; i < length; i++)
         {
            const wchar_t c = text[i];
            const bool reference = !fromReference.empty() && fromReference[i];

            // A quoted-printable soft line break, '=' at the end of a line,
            // joins the lines, as the regular expression before 6.3.6 did.
            if (c == L'=' && !reference && i + 1 < length && (text[i + 1] == L'\r' || text[i + 1] == L'\n'))
            {
               i++;
               if (text[i] == L'\r' && i + 1 < length && text[i + 1] == L'\n')
                  i++;
               continue;
            }

            if (encoded)
            {
               // A link in another's query ends where the parameter does.
               if (IsWhiteSpace_(c) || c == L'"' || c == L'\'' || c == L'<' || c == L'>' || c == L'&' || IsPathStart_(c))
                  break;
            }
            else if (attribute)
            {
               // The value runs to its quote; inside it, a space, a '<' and
               // the other quote are the link's own, and the URL parser drops
               // tabs and line breaks.
               if (c == quote && !reference)
                  break;
               if (c == L'\t' || c == L'\r' || c == L'\n')
                  continue;
               if (IsPathStart_(c))
                  break;
            }
            else
            {
               // In running text a link ends at white space and angle
               // brackets, and at the quote it was opened by.
               if (IsWhiteSpace_(c) || c == L'<' || c == L'>' || (quote != 0 && c == quote))
                  break;
               if (IsPathStart_(c))
                  break;
            }

            authority += c;
         }

         return i;
      }

      // What the userinfo, the port and the path are cut from when the link
      // was written percent-encoded in another's query: decoded once, it is a
      // link like any other, whose authority ends at its path.
      std::wstring DecodedEncodedAuthority_(const std::wstring &raw)
      {
         const std::wstring decoded = PercentDecoded_(raw);
         std::wstring authority;
         for (wchar_t c : decoded)
         {
            if (c == L'\t' || c == L'\r' || c == L'\n')
               continue;
            if (IsPathStart_(c))
               break;
            authority += c;
         }
         return authority;
      }

      // Where a run of "%2F" and "%5C", in either case, and of slashes and
      // backslashes, that starts at start ends: the slashes and backslashes of
      // a link written percent-encoded into another's query, which a
      // redirector decodes and a browser skips. Raw ones are part of the run:
      // an encoder that leaves '/' alone - Python's urllib.parse.quote,
      // JavaScript's escape - writes http://evil.example as
      // http%3A//evil.example, where the reading stopped at the first raw
      // slash and read nothing, until the raw ones were taken in.
      size_t SkipEncodedSlashes_(const std::wstring &text, size_t start)
      {
         size_t i = start;
         while (i < text.size())
         {
            if (text[i] == L'/' || text[i] == L'\\')
            {
               i++;
               continue;
            }

            if (i + 2 >= text.size() || text[i] != L'%')
               break;

            const wchar_t high = text[i + 1];
            const wchar_t low = static_cast<wchar_t>(text[i + 2] | 0x20);
            if (!((high == L'2' && low == L'f') || (high == L'5' && low == L'c')))
               break;
            i += 3;
         }
         return i;
      }

      bool IsDottedDecimal_(const std::wstring &host)
      {
         std::vector<std::wstring> parts = SplitOnDots_(host);
         if (parts.size() != 4)
            return false;

         for (const std::wstring &part : parts)
         {
            if (part.empty() || part.size() > 3)
               return false;
            for (wchar_t c : part)
               if (c < L'0' || c > L'9')
                  return false;
         }

         return true;
      }

      // The names an HTML document's type and fonts name in nearly every
      // message Outlook and its kind compose, which are not what the message
      // links to.
      bool IsDocumentTypeHost_(const String &host)
      {
         return host == _T("w3.org") || host == _T("w3c.org") || host == _T("schemas.microsoft.com") ||
                host == _T("fonts.googleapis.com") || host == _T("fonts.gstatic.com");
      }

      // Reads the links in source - a text part, or an HTML part with its
      // character references read, fromReference saying which characters a
      // reference wrote - and hands the hosts they go to to visit, as
      // SURBL::FindLinkHosts says, finding each by separator (FindLinkHosts
      // says what it is). cut: source is the first part of a longer text, and
      // a link whose authority runs on to its end is not handed over - the
      // text after the cut could have taken the host on - and ends the
      // reading, since every link after it would too. False when visit
      // stopped the reading.
      bool ReadLinks_(const boost::wregex &separator, const std::wstring &source, const std::vector<bool> &fromReference,
                      bool html, const std::function<bool(const String &host)> &visit, bool cut = false)
      {
         boost::wsmatch match;
         std::wstring::const_iterator from = source.begin();
         boost::match_flag_type flags = boost::match_default;

         while (boost::regex_search(from, source.end(), match, separator, flags))
         {
            const size_t matchStart = static_cast<size_t>(match[0].first - source.begin());
            const bool withScheme = match[1].matched;
            const bool encoded = withScheme ? match.str(1).find(L'%') != std::wstring::npos : match[3].matched;

            // The rest of the separator's run of slashes: "%2F", "%5C" and raw
            // slashes and backslashes after an encoded separator; raw, slashes
            // and backslashes after two with no scheme.
            size_t separatorEnd = static_cast<size_t>(match[0].second - source.begin());
            if (encoded)
               separatorEnd = SkipEncodedSlashes_(source, separatorEnd);
            else if (match[2].matched)
            {
               while (separatorEnd < source.size() && (source[separatorEnd] == L'/' || source[separatorEnd] == L'\\'))
                  separatorEnd++;
            }

            // The quote the link was opened by, when a quote came just before
            // it. In HTML it is an attribute's value when an '=' comes before
            // the quote: the value runs to the closing quote whatever is inside
            // it. The URL parser drops the spaces and control characters a
            // value begins with, so in HTML they may come between the quote and
            // the link, written as themselves or as references (&#32;, &#9;):
            // the tokenizer has made them the value's characters either way.
            wchar_t quote = 0;
            bool attribute = false;
            size_t opener = matchStart;
            if (html)
            {
               while (opener > 0 && source[opener - 1] <= 0x20)
                  opener--;
            }

            if (opener > 0)
            {
               const wchar_t before = source[opener - 1];
               const bool beforeIsReference = html && fromReference[opener - 1];
               if ((before == L'"' || before == L'\'') && !beforeIsReference)
               {
                  quote = before;

                  if (html)
                  {
                     size_t i = opener - 1;
                     while (i > 0 && source[i - 1] <= 0x20 && !fromReference[i - 1])
                        i--;
                     attribute = i > 0 && source[i - 1] == L'=' && !fromReference[i - 1];
                  }
               }
            }

            // A link with no scheme is one where it opens a value: two slashes
            // an attribute's quoted value begins with (after the spaces the URL
            // parser drops), or an unquoted one; and two slashes, as themselves
            // or percent-encoded, just after the '=' of a query parameter
            // (?u=//evil.example, ?u=%2F%2Fevil.example), in text as in HTML.
            // An attribute's value that begins with %2F%2F is a path, not a
            // link.
            bool isALink = withScheme;
            if (!withScheme)
            {
               const bool encodedSlashes = match[3].matched;
               if (attribute)
                  isALink = !encodedSlashes;
               else if (quote == 0 && opener > 0 && source[opener - 1] == L'=')
                  isALink = opener == matchStart || (html && !encodedSlashes);
            }

            size_t resume = separatorEnd;
            if (isALink)
            {
               std::wstring authority;
               const size_t stopped = ReadAuthority_(source, fromReference, separatorEnd, quote, attribute, encoded, authority);

               if (cut && stopped >= source.size())
                  return true;

               if (encoded)
                  authority = DecodedEncodedAuthority_(authority);

               std::vector<String> hosts;
               HostsOfAuthority_(authority, hosts);
               if (!attribute && !encoded)
               {
                  HostsInTheUserinfo_(authority, hosts);
                  HostsBeforeASpaceToSomeEngine_(authority, hosts);
               }

               for (const String &host : hosts)
                  if (!visit(host))
                     return false;

               // The next separator is looked for from the end of this one, so
               // that "http://a.example;http://b.example/" finds both. After a
               // long authority it is looked for from where the reading
               // stopped, so that the text is read in time proportional to its
               // length - less the six characters of "https:": a reading ends
               // at a slash or a backslash, and one that took in the scheme of
               // the link after it
               // ("http://<300 characters>!http://evil.example/") ended at
               // that link's own slashes, so the link starts just before the
               // stop.
               const size_t longestSchemeBeforeItsSlash = 6;
               if (stopped - separatorEnd > 256)
                  resume = stopped - longestSchemeBeforeItsSlash;
            }

            if (resume >= source.size())
               return true;

            from = source.begin() + static_cast<std::ptrdiff_t>(resume);
            flags = boost::match_default | boost::match_prev_avail;
         }
         return true;
      }

      // An HTML attribute's value is given to the URL parser, which deletes
      // every tab and line break in it before it reads anything - inside a
      // link written into the value's query too, where the reading of the
      // whole part stops at one: <a href="https://r.example/?u=http%3A%2F%2F
      // ev&#9;il.example"> sends its visitor to evil.example through the
      // redirector, and http%3&#10;A%2F%2F is http%3A%2F%2F to it. So each
      // quoted value of a tag that holds one is read alone twice, with them
      // and without, and the hosts only the second reading finds are handed
      // over too. A value is known as the tokenizer knows it: after an '='
      // inside a tag, to the matching quote, none of them written by a
      // reference; a tag the text ends inside is none, and a quote in the
      // text between tags opens no value. Up to 4,096 hosts of a value are
      // remembered to tell the two readings apart; past them a host may be
      // handed over twice, which every reader of the hosts allows. False when
      // visit stopped the reading.
      bool ReadValuesWithoutBreaks_(const boost::wregex &separator, const std::wstring &source,
                                    const std::vector<bool> &fromReference, const std::function<bool(const String &host)> &visit)
      {
         const size_t length = source.size();
         const size_t hostsRemembered = 4096;
         auto isBreak = [](wchar_t c) { return c == L'\t' || c == L'\r' || c == L'\n'; };
         auto isLetter = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };

         size_t i = 0;
         while (i < length)
         {
            // A tag opens at a '<' followed by a letter, and ends at a '>'
            // outside its values.
            if (source[i] != L'<' || fromReference[i] || i + 1 >= length || !isLetter(source[i + 1]))
            {
               i++;
               continue;
            }

            size_t j = i + 1;
            while (j < length && !(source[j] == L'>' && !fromReference[j]))
            {
               if (source[j] != L'=' || fromReference[j])
               {
                  j++;
                  continue;
               }

               size_t open = j + 1;
               while (open < length && source[open] <= 0x20 && !fromReference[open])
                  open++;
               if (open >= length || fromReference[open] || (source[open] != L'"' && source[open] != L'\''))
               {
                  j++;
                  continue;
               }

               const wchar_t quote = source[open];
               size_t close = open + 1;
               bool holdsABreak = false;
               while (close < length && (source[close] != quote || fromReference[close]))
               {
                  holdsABreak = holdsABreak || isBreak(source[close]);
                  close++;
               }

               if (close >= length)
                  return true;

               if (holdsABreak)
               {
                  // The value as an attribute's, ="...", as it is and with its
                  // breaks deleted.
                  std::wstring asItIs(1, L'=');
                  asItIs += quote;
                  std::wstring without = asItIs;
                  std::vector<bool> asItIsFromReference(2, false);
                  std::vector<bool> withoutFromReference(2, false);
                  for (size_t k = open + 1; k < close; k++)
                  {
                     asItIs += source[k];
                     asItIsFromReference.push_back(fromReference[k]);
                     if (isBreak(source[k]))
                        continue;
                     without += source[k];
                     withoutFromReference.push_back(fromReference[k]);
                  }
                  asItIs += quote;
                  asItIsFromReference.push_back(false);
                  without += quote;
                  withoutFromReference.push_back(false);

                  std::set<String> handedOver;
                  ReadLinks_(separator, asItIs, asItIsFromReference, true, [&](const String &host) -> bool
                  {
                     if (handedOver.size() < hostsRemembered)
                        handedOver.insert(host);
                     return true;
                  });

                  const bool goOn = ReadLinks_(separator, without, withoutFromReference, true, [&](const String &host) -> bool
                  {
                     if (handedOver.find(host) != handedOver.end())
                        return true;
                     if (handedOver.size() < hostsRemembered)
                        handedOver.insert(host);
                     return visit(host);
                  });

                  if (!goOn)
                     return false;
               }

               j = close + 1;
            }

            i = j + 1;
         }

         return true;
      }
   }

   SURBL::SURBL(void)
   {

   }

   SURBL::~SURBL(void)
   {
   }

   String
   SURBL::FirstPartForLinks(const String &text, bool html, size_t maxCharacters)
   {
      if (text.size() <= maxCharacters)
         return text;

      size_t end = maxCharacters;

      // A character reference the cut would split is read as another text by
      // itself - "&#10" for "&#109;", an m - so it is left out with what
      // follows it: the last '&' when nothing but the characters a reference
      // is written in comes after it before the cut. A reference written
      // with leading zeros is as long as its writer chose, so the search
      // goes back as far as those characters do.
      if (html)
      {
         const std::wstring &characters = text;
         size_t at = end;
         while (at > 0)
         {
            const wchar_t c = characters[at - 1];
            const bool referenceCharacter = c == L'#' || (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
            if (!referenceCharacter)
               break;
            at--;
         }
         if (at > 0 && characters[at - 1] == L'&')
            end = at - 1;
      }

      // By length, not by c_str alone: a part may hold a NUL.
      return String(text.c_str(), end);
   }

   void
   SURBL::FindLinkHosts(const String &text, bool html, const std::function<bool(const String &host)> &visit, size_t maxCharacters)
   {
      // A text longer than maxCharacters is read as far as its first part,
      // and a link that runs on to the end of that part is not handed over
      // (ReadLinks_'s cut).
      const bool cut = text.size() > maxCharacters;
      const String firstPart = cut ? FirstPartForLinks(text, html, maxCharacters) : String();
      const String &read = cut ? firstPart : text;

      // Until 6.3.6 a regular expression took the characters after "://" that
      // could be in a name, so http://anything@evil.example/ was looked up as
      // "anything", and evil.example never was: a spammer escaped every list by
      // writing a name and an '@' before the host. The separator is still
      // found by a regular expression; what follows it is read as a URL.
      std::wstring decoded;
      std::vector<bool> fromReference;
      if (html)
         DecodeCharacterReferences_(read, decoded, fromReference);

      const std::wstring &source = html ? decoded : static_cast<const std::wstring &>(read);

      // The first group is a link with a scheme. A link written into another's
      // query has its separator percent-encoded, and a redirector that decodes
      // it reads it as a browser reads any link: http%3A%5C%5C,
      // http%3A%2F%2F%2F and http%3A are http:// too.
      //
      // The second and third are a link with no scheme, which a browser
      // resolves against the page's own: two slashes or backslashes or more
      // (tabs and line breaks between them are dropped), as themselves or,
      // for a link in another's query, percent-encoded. <a href="//evil.example/">
      // goes to evil.example from a webmail page, as does a redirector's
      // ?u=//evil.example. Such a separator is a link only where it opens an
      // attribute's value or a query parameter's (below); elsewhere two
      // slashes are a path's or a comment's. Until 6.3.6 no
      // link without a scheme was read.
      //
      // No alternative repeats a group: boost keeps state for each repetition
      // of one, and throws when a long run passes its limit, which ends the
      // reading of the message (5701) with every link after the run unread -
      // as an expression of this tree that repeated "%2F" did, before any
      // release had it. A run of percent-encoded slashes and backslashes is
      // taken after the match instead (SkipEncodedSlashes_), as a run of raw
      // ones after two with no scheme is; a repeat of one character set, as
      // the slashes after "http:" are matched, keeps no such state.
      const boost::wregex separator(
         L"(https?:[\\\\/]*|https?%3a|%3a%2f%2f|://)|([/\\\\][\\t\\r\\n]*[/\\\\])|(%(?:2f|5c)%(?:2f|5c))",
         boost::wregex::icase);

      if (!ReadLinks_(separator, source, fromReference, html, visit, cut) || !html)
         return;

      // A value that the cut leaves open is not read again: its closing quote
      // is past the cut (ReadValuesWithoutBreaks_ stops there).
      ReadValuesWithoutBreaks_(separator, source, fromReference, visit);
   }

   std::vector<String>
   SURBL::NamesToAsk(const String &host)
   {
      std::vector<String> names;

      // An address is asked about with its octets reversed, as a DNS list is
      // asked about one (SURBL's and URIBL's guidelines). Until 6.3.6 it was
      // asked about in its own order too, which is the address with its
      // octets the other way round: a link to 192.0.2.1 was tagged if
      // 1.2.0.192 was listed.
      if (IsDottedDecimal_(host))
      {
         std::vector<std::wstring> octets = SplitOnDots_(host);
         names.push_back(String(octets[3] + L"." + octets[2] + L"." + octets[1] + L"." + octets[0]));
         return names;
      }

      // The host, and its registrable domain by the Public Suffix List.
      names.push_back(host);

      String registrable = host;
      bool isIPAddress = false;
      if (PublicSuffixList::GetRegistrableDomain(registrable, isIPAddress) && registrable != host)
         names.push_back(registrable);

      // And, for a host under a shared-hosting suffix of the list's PRIVATE
      // section, the domain registered under its ICANN section: github.io for
      // tenant.github.io, blogspot.com for x.blogspot.com. A wildcarded list
      // (SURBL's multi, the Spamhaus DBL) answers for that domain when it is
      // asked about the host, but one that is not wildcarded (URIBL) answers
      // only for the names it lists, and lists a whole hosting domain under
      // that domain alone: a link to evil.duckdns.org is caught by URIBL's
      // listing of duckdns.org only when duckdns.org is asked about. 6.3.5,
      // which cut at two labels, asked it; the cut by the whole list, which
      // replaced that cut in 6.3.6, did not, until this third name.
      String icannRegistrable = host;
      if (PublicSuffixList::GetRegistrableDomain(icannRegistrable, isIPAddress, PublicSuffixList::IcannSectionOnly) &&
          icannRegistrable != host && icannRegistrable != registrable)
         names.push_back(icannRegistrable);

      return names;
   }

   String
   SURBL::HostOfAuthority(const String &authority)
   {
      std::wstring hostPart;
      if (!HostPartOfAuthority_(authority, hostPart))
         return String();

      // The whole host part, as HostsOfHostPart_ hands over a host that is all
      // a name's characters or an address; and nothing for one that is not,
      // where a link hands over the name before the character and the labels
      // after it: an entry names one host, and is not widened to another.
      return Canonical_(HostDecoded_(hostPart));
   }

   SURBL::AnswerKind
   SURBL::ClassifyAnswer(const String &answer, String &meaning)
   {
      if (answer.GetLength() > 12 && answer.Left(12) == _T("127.255.255."))
      {
         const String code = answer.Mid(12);
         if (code == _T("252"))
            meaning = _T("Spamhaus's code for a mistyped zone name");
         else if (code == _T("254"))
            meaning = _T("Spamhaus's code for a query through a public or open resolver");
         else if (code == _T("255"))
            meaning = _T("Spamhaus's code for too many queries from this resolver");
         else
            meaning = _T("in 127.255.255.0/24, where Spamhaus answers an error");
         return AnswerRefused;
      }

      if (answer == _T("127.0.0.1"))
      {
         meaning = _T("SURBL's and URIBL's code for a blocked query");
         return AnswerBlocked;
      }

      if (answer == _T("127.0.1.255"))
      {
         meaning = _T("the Spamhaus DBL's code for an address, which it does not list (IP queries prohibited)");
         return AnswerNotForAddresses;
      }

      if (answer == _T("127.0.2.255"))
      {
         meaning = _T("the Spamhaus ZRD's code for an address, which it does not list (IP queries not supported)");
         return AnswerNotForAddresses;
      }

      meaning.clear();
      return AnswerVerdict;
   }

   void
   SURBL::ForEachLinkHost(std::shared_ptr<MessageData> pMessageData, const std::function<bool(const String &host)> &visit, size_t maxCharacters)
   {
      bool stopped = false;
      auto visitLink = [&](const String &host) -> bool
      {
         if (IsDocumentTypeHost_(host))
            return true;

         if (!visit(host))
         {
            stopped = true;
            return false;
         }

         return true;
      };

      try
      {
         // The text part as text, and the HTML part as HTML, in that order.
         FindLinkHosts(pMessageData->GetBody(), false, visitLink, maxCharacters);
         if (!stopped)
            FindLinkHosts(pMessageData->GetHTMLBody(), true, visitLink, maxCharacters);
      }
      catch (std::runtime_error &err) // regex_search throws runtime_error if the expression is too complex.
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5701, "SURBL::Run", "Parsing HTML body with regular expression threw a runtime_error", err);
      }
   }

   namespace
   {
      // One registrable domain a message links to, and what it is asked about
      // as: its own names (NamesToAsk's, less a host's own: the domain, and
      // under a suffix of the list's private section the ICANN section's
      // domain too), own of them, and then host names under it,
      // MaxHostNamesPerDomain of them at most - drawn from hosts, every host
      // name under it the message links to, when there are more than that.
      struct LinkedDomain_
      {
         std::vector<String> names;
         size_t own = 0;
         std::vector<String> hosts;
         std::set<String> hosts_seen;
      };

      void AddName_(LinkedDomain_ &domain, const String &name)
      {
         if (std::find(domain.names.begin(), domain.names.end(), name) == domain.names.end())
            domain.names.push_back(name);
      }

      // Fisher-Yates over mt19937's own output, which the standard specifies,
      // rather than std::shuffle, whose draws each standard library makes its
      // own way: one seed shuffles alike on every platform.
      void Shuffle_(std::vector<LinkedDomain_> &domains, unsigned int seed)
      {
         std::mt19937 engine(seed);
         for (size_t i = domains.size(); i > 1; i--)
         {
            const size_t j = static_cast<size_t>(engine() % i);
            std::swap(domains[i - 1], domains[j]);
         }
      }
   }

   SURBL::Choice
   SURBL::ChooseNamesToAsk(const std::vector<String> &html_hosts, const std::vector<String> &text_hosts, size_t max_domains,
                           size_t max_names, unsigned int seed)
   {
      // The domains linked, each once, in the order first linked: the HTML
      // part's, then those only the text part links to.
      std::vector<LinkedDomain_> tiers[2];
      std::map<String, std::pair<int, size_t> > where;

      const std::vector<String> *parts[2] = { &html_hosts, &text_hosts };
      for (int tier = 0; tier < 2; tier++)
      {
         for (const String &host : *parts[tier])
         {
            const std::vector<String> names = NamesToAsk(host);
            if (names.empty())
               continue;

            // The registrable domain by the whole Public Suffix List, which
            // NamesToAsk asks about too; an address is its own.
            String key = names[0];
            if (!IsDottedDecimal_(host))
            {
               String registrable = host;
               bool isIPAddress = false;
               if (PublicSuffixList::GetRegistrableDomain(registrable, isIPAddress))
                  key = registrable;
            }

            std::map<String, std::pair<int, size_t> >::const_iterator found = where.find(key);
            if (found == where.end())
            {
               tiers[tier].push_back(LinkedDomain_());
               where[key] = std::make_pair(tier, tiers[tier].size() - 1);

               // The domain's own names first: itself and, under a suffix of
               // the list's private section, its ICANN section's domain.
               LinkedDomain_ &added = tiers[tier].back();
               for (const String &name : names)
               {
                  if (name != host || name == key)
                     AddName_(added, name);
               }

               added.own = added.names.size();

               found = where.find(key);
            }

            LinkedDomain_ &domain = tiers[found->second.first][found->second.second];

            // Then the host itself, when it is not the domain - a list that is
            // not wildcarded may list a host and not its domain. Every one is
            // kept here, and a few of them asked about.
            if (host != key && names[0] == host &&
                std::find(domain.names.begin(), domain.names.end(), host) == domain.names.end() &&
                domain.hosts_seen.insert(host).second)
            {
               domain.hosts.push_back(host);
            }
         }
      }

      // The host names asked about under each domain: all of them when they
      // fit, and otherwise MaxHostNamesPerDomain drawn by the seed - never the
      // first ones linked, or two made-up hosts linked before a listed one
      // under the same domain would keep it from every list that lists the
      // host and not the domain.
      std::mt19937 hostDraw(seed ^ 0x9E3779B9u);
      const size_t hostCap = MaxHostNamesPerDomain;
      for (std::vector<LinkedDomain_> &domains : tiers)
      {
         for (LinkedDomain_ &domain : domains)
         {
            std::vector<String> &hosts = domain.hosts;
            const size_t take = std::min(hosts.size(), hostCap);

            if (hosts.size() > take)
            {
               // The first take places of a Fisher-Yates shuffle (Shuffle_ says
               // why not std::shuffle).
               for (size_t i = 0; i < take; i++)
               {
                  const size_t j = i + static_cast<size_t>(hostDraw() % (hosts.size() - i));
                  std::swap(hosts[i], hosts[j]);
               }
            }

            domain.names.insert(domain.names.end(), hosts.begin(), hosts.begin() + static_cast<std::ptrdiff_t>(take));
         }
      }

      // The domains, each counted once however many hosts of the message name
      // it: duckdns.org is one domain for every tenant of it the message links
      // to.
      std::set<String> ownNames[2];
      for (int tier = 0; tier < 2; tier++)
      {
         for (const LinkedDomain_ &domain : tiers[tier])
            ownNames[tier].insert(domain.names.begin(), domain.names.begin() + static_cast<std::ptrdiff_t>(domain.own));
      }

      std::set<String> everyOwnName = ownNames[0];
      everyOwnName.insert(ownNames[1].begin(), ownNames[1].end());

      Choice choice;
      choice.domains = everyOwnName.size();

      std::vector<String> &chosen = choice.names;
      std::set<String> asked;

      const size_t domainBudget = std::min(max_domains, max_names);

      // Every domain's own names, the HTML part's first. Not all of them can be
      // asked about when there are more than the budget: they are then taken
      // in an order the message does not choose, so that links put before
      // another cannot keep it from being asked - and the message is not asked
      // about whole, which the score for it says.
      for (int tier = 0; tier < 2 && choice.domains_asked < domainBudget; tier++)
      {
         std::vector<LinkedDomain_> &domains = tiers[tier];

         size_t fresh = 0;
         for (const String &name : ownNames[tier])
         {
            if (asked.find(name) == asked.end())
               fresh++;
         }

         if (choice.domains_asked + fresh > domainBudget)
            Shuffle_(domains, seed + static_cast<unsigned int>(tier));

         // Each domain's first own name, then each one's second: as many
         // domains as the budget allows are asked about before an ICANN
         // section's domain is.
         for (size_t round = 0; choice.domains_asked < domainBudget; round++)
         {
            bool more = false;
            for (const LinkedDomain_ &domain : domains)
            {
               if (round >= domain.own)
                  continue;

               more = true;
               const String &name = domain.names[round];
               if (asked.insert(name).second)
               {
                  chosen.push_back(name);
                  choice.domains_asked++;
                  if (choice.domains_asked >= domainBudget)
                     break;
               }
            }

            if (!more)
               break;
         }
      }

      // Then the host names under them, every domain's first before any
      // domain's second, as far as the names budget goes. A host name is asked
      // about beside its domain, never instead of it, so it does not count
      // towards the domains.
      for (int tier = 0; tier < 2 && chosen.size() < max_names; tier++)
      {
         std::vector<LinkedDomain_> &domains = tiers[tier];

         // Not all of them fit: the domains are taken in an order the message
         // does not choose, as their own names are above. In the order linked,
         // a message whose domains all fit - and so scores nothing - put a
         // host name of its own under each of the first hundred and more, and
         // a listed host name linked after them was never asked about, by any
         // list that lists the host and not its domain.
         size_t hostNames = 0;
         for (const LinkedDomain_ &domain : domains)
         {
            for (size_t at = domain.own; at < domain.names.size(); at++)
            {
               if (asked.find(domain.names[at]) == asked.end())
                  hostNames++;
            }
         }

         if (chosen.size() + hostNames > max_names)
            Shuffle_(domains, seed + 2 + static_cast<unsigned int>(tier));

         for (size_t round = 0; chosen.size() < max_names; round++)
         {
            bool more = false;
            for (const LinkedDomain_ &domain : domains)
            {
               const size_t at = domain.own + round;
               if (at >= domain.names.size())
                  continue;

               more = true;
               const String &name = domain.names[at];
               if (asked.insert(name).second)
               {
                  chosen.push_back(name);
                  if (chosen.size() >= max_names)
                     break;
               }
            }

            if (!more)
               break;
         }
      }

      return choice;
   }

   void
   SURBL::Collect_(std::shared_ptr<MessageData> pMessageData, Choice &out)
   {
      // Every link of both parts, each host once: the HTML part's first, then
      // the text part's. Until 6.3.6 the reading stopped at the sixteenth name,
      // in the order the links came and the text part first.
      std::vector<String> htmlHosts;
      std::vector<String> textHosts;
      std::set<String> seen;

      auto collectInto = [&seen](std::vector<String> &hosts)
      {
         return [&seen, &hosts](const String &host) -> bool
         {
            if (!IsDocumentTypeHost_(host) && seen.insert(host).second)
               hosts.push_back(host);
            return true;
         };
      };

      try
      {
         FindLinkHosts(pMessageData->GetHTMLBody(), true, collectInto(htmlHosts));
         FindLinkHosts(pMessageData->GetBody(), false, collectInto(textHosts));
      }
      catch (std::runtime_error &err) // regex_search throws runtime_error if the expression is too complex.
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5701, "SURBL::Run", "Parsing HTML body with regular expression threw a runtime_error", err);
      }

      // A seed the sender cannot know, for when not every name can be asked.
      const unsigned int seed = std::random_device()() ^
         static_cast<unsigned int>(std::chrono::steady_clock::now().time_since_epoch().count());

      out = ChooseNamesToAsk(htmlHosts, textHosts, MaxDomainsAsked, MaxNamesAsked, seed);

      // The cast names FormatArgument's unsigned __int64 constructor (ExtractUrls
      // says why).
      LOG_DEBUG(Formatter::Format(_T("SURBL: {0} host(s) linked from the HTML part and {1} more from the text part; {2} name(s) to ask about."),
         (unsigned __int64) htmlHosts.size(), (unsigned __int64) textHosts.size(), (unsigned __int64) out.names.size()));

      if (!out.EveryDomainAsked())
      {
         LOG_DEBUG(Formatter::Format(_T("SURBL: The links name {0} domain(s); {1} of them are asked about, and the message scores SurblTooManyDomainsScore for the rest."),
            (unsigned __int64) out.domains, (unsigned __int64) out.domains_asked));
      }

      // Each name with the link host that gave it, as 6.3.5 logged them: "asked
      // about as" when the name is not a host linked itself. Worked out only
      // when the debug log is on.
      if (Logger::Instance()->GetLogMask() & Logger::LSDebug)
      {
         std::map<String, String> origin;
         auto mapHosts = [&origin](const std::vector<String> &hosts, bool derived)
         {
            for (const String &host : hosts)
            {
               if (!derived)
               {
                  origin[host] = host;
                  continue;
               }

               for (const String &name : NamesToAsk(host))
                  origin.emplace(name, host);
            }
         };

         mapHosts(htmlHosts, false);
         mapHosts(textHosts, false);
         mapHosts(htmlHosts, true);
         mapHosts(textHosts, true);

         for (const String &name : out.names)
         {
            std::map<String, String>::const_iterator found = origin.find(name);
            if (found == origin.end() || found->second == name)
            {
               LOG_DEBUG(Formatter::Format(_T("SURBL: Found URL: {0}"), name));
            }
            else
            {
               LOG_DEBUG(Formatter::Format(_T("SURBL: Found URL: {0}, asked about as {1}"), found->second, name));
            }
         }
      }
   }

   bool
   SURBL::ExtractUrls(std::shared_ptr<MessageData> pMessageData, Choice &choice)
   {
      LOG_DEBUG("SURBL: Execute");

      Collect_(pMessageData, choice);

      if (choice.names.empty())
      {
         LOG_DEBUG("SURBL: No domain addresses found.");
         return false;
      }

      // The cast names FormatArgument's unsigned __int64 constructor, which is the
      // one a size_t binds to exactly on Windows. On a 64-bit POSIX build size_t is
      // unsigned long, which matches none of the constructors exactly and is no
      // closer to one than to the others. The cast is an identity on Windows.
      String logMessage = Formatter::Format("SURBL: {0} unique domain addresses found.", (unsigned __int64) choice.names.size());
      LOG_DEBUG(logMessage);

      return true;
   }

   // SURBL.h says what the pool is for and how it shares itself out.
   struct SURBLLookupPool::State
   {
      // One RunAlongside: the step its threads run, how many of the pool's
      // threads are inside it now and may be at most, and whether it has
      // answered false - then no thread of the pool takes it up again.
      struct Job
      {
         const std::function<bool()> *step = nullptr;
         size_t helpers = 0;
         size_t most = 0;
         bool done = false;
      };

      State(size_t threadsAtMost, size_t helpersAtMost, int idleMilliseconds) :
         threads(threadsAtMost),
         helpersPerRun(helpersAtMost),
         idle(idleMilliseconds)
      {
      }

      const size_t threads;
      const size_t helpersPerRun;
      const std::chrono::milliseconds idle;

      std::mutex lock;

      // A job came, or the pool is ending: a thread waiting for work looks.
      std::condition_variable work;

      // A thread left a job, or ended: RunAlongside and the destructor look.
      std::condition_variable left;

      std::vector<Job *> jobs;

      // Where a thread free for work looks first: after the job one was given
      // last, so that each job in turn has the next thread free.
      size_t next = 0;

      size_t running = 0;
      size_t idleNow = 0;
      size_t started = 0;
      size_t mostRunning = 0;
      bool ending = false;

      // With lock held: the job a thread free for work makes a lookup for -
      // the first from next on with lookups left and room for another
      // thread - and next moved past it when take is set; null when there is
      // none.
      Job *Find(bool take)
      {
         const size_t count = jobs.size();
         for (size_t i = 0; i < count; i++)
         {
            const size_t at = (next + i) % count;
            Job *job = jobs[at];
            if (!job->done && job->helpers < job->most)
            {
               if (take)
                  next = at + 1;
               return job;
            }
         }

         return nullptr;
      }

      // A thread of the pool: one lookup for one job at a time, each job in
      // turn, until it has had nothing to do for idle or the pool ends.
      static void Help(const std::shared_ptr<State> &shared)
      {
         State &s = *shared;
         std::unique_lock<std::mutex> guard(s.lock);

         for (;;)
         {
            Job *job = s.Find(true);
            if (!job)
            {
               if (s.ending)
                  break;

               s.idleNow++;
               const bool woken = s.work.wait_for(guard, s.idle, [&s]() { return s.ending || s.Find(false) != nullptr; });
               s.idleNow--;

               if (!woken)
                  break;

               continue;
            }

            job->helpers++;
            guard.unlock();

            bool more = false;
            try
            {
               more = (*job->step)();
            }
            catch (...)
            {
               more = false;
            }

            guard.lock();
            if (!more)
               job->done = true;
            job->helpers--;
            s.left.notify_all();
         }
      }

      // A thread of the pool, from its start to its end, which is counted
      // however Help left.
      static void RunThread(const std::shared_ptr<State> &shared)
      {
         // The top of a thread: nothing may leave it. Help catches what a
         // lookup throws; what is left is the pool's own lock failing.
         try
         {
            Help(shared);
         }
         catch (...)
         {
            LOG_DEBUG(_T("SURBL: a thread of the URI lists' lookup pool ended in an error."));
         }

         try
         {
            std::lock_guard<std::mutex> guard(shared->lock);
            shared->running--;
            shared->left.notify_all();
         }
         catch (...)
         {
            LOG_DEBUG(_T("SURBL: a thread of the URI lists' lookup pool could not count its end."));
         }
      }
   };

   SURBLLookupPool::SURBLLookupPool(size_t threads, size_t helpersPerRun, int idleMilliseconds) :
      state_(std::make_shared<State>(threads, helpersPerRun, idleMilliseconds))
   {
   }

   SURBLLookupPool::~SURBLLookupPool()
   {
      std::unique_lock<std::mutex> guard(state_->lock);
      state_->ending = true;
      state_->work.notify_all();

      State &s = *state_;
      s.left.wait(guard, [&s]() { return s.running == 0; });
   }

   SURBLLookupPool &
   SURBLLookupPool::Server()
   {
      // Never destroyed: a thread of it may still be waiting for work when
      // the process ends, and must not find its pool gone.
      static SURBLLookupPool *pool = new SURBLLookupPool(SURBL::PoolThreads, SURBL::LookupsAtOnce - 1, 60 * 1000);
      return *pool;
   }

   SURBLLookupPool::Counts
   SURBLLookupPool::GetCounts() const
   {
      std::lock_guard<std::mutex> guard(state_->lock);

      Counts counts;
      counts.started = state_->started;
      counts.running = state_->running;
      counts.most_running = state_->mostRunning;
      return counts;
   }

   void
   SURBLLookupPool::RunAlongside(const std::function<bool()> &step, size_t helpers)
   {
      State &s = *state_;

      State::Job job;
      job.step = &step;
      job.most = std::min(helpers, s.helpersPerRun);

      // Leaves the pool with the job: no thread takes it up again, and the
      // ones inside it are waited for, since it is this frame's.
      struct Leave
      {
         State &s;
         State::Job &job;

         ~Leave()
         {
            std::unique_lock<std::mutex> guard(s.lock);
            job.done = true;

            const std::vector<State::Job *>::iterator it = std::find(s.jobs.begin(), s.jobs.end(), &job);
            if (it != s.jobs.end())
            {
               const size_t at = static_cast<size_t>(it - s.jobs.begin());
               s.jobs.erase(it);
               if (at < s.next)
                  s.next--;
            }

            State::Job &mine = job;
            s.left.wait(guard, [&mine]() { return mine.helpers == 0; });
         }
      };

      const Leave leave{s, job};
      size_t start = 0;

      if (job.most > 0)
      {
         std::lock_guard<std::mutex> guard(s.lock);
         s.jobs.push_back(&job);

         // Threads for it: those waiting for work first, then new ones while
         // the pool has room.
         const size_t wanted = job.most > s.idleNow ? job.most - s.idleNow : 0;
         start = std::min(wanted, s.threads > s.running ? s.threads - s.running : 0);
         s.running += start;
         s.started += start;
         s.mostRunning = std::max(s.mostRunning, s.running);
         s.work.notify_all();
      }

      for (size_t i = 0; i < start; i++)
      {
         try
         {
            std::shared_ptr<State> shared = state_;
            std::thread([shared]() { State::RunThread(shared); }).detach();
         }
         catch (...)
         {
            // No more threads to be had - the system's refusal or no memory
            // for one: the ones there are, and this one, make the lookups.
            std::lock_guard<std::mutex> guard(s.lock);
            s.running -= start - i;
            s.started -= start - i;
            s.left.notify_all();
            break;
         }
      }

      // This thread makes lookups too, whatever the pool's threads are doing.
      while (step())
      {
      }
   }

   bool
   SURBL::Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls)
   {
      return Run(pSURBLServer, vecUrls, [](const String &name, std::vector<String> &answers) -> bool
      {
         DNSResolver resolver;
         return resolver.GetIpAddresses(name, answers, false);
      });
   }

   bool
   SURBL::Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls, const Lookup &lookup)
   {
      return Run(pSURBLServer, vecUrls, lookup, SURBLLookupPool::Server());
   }

   bool
   SURBL::Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls, const Lookup &lookup,
              SURBLLookupPool &pool)
   {
      const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
      const std::chrono::seconds deadline(10);

      // What an answer means is the server's to say, as it is for a DNSBL.
      // With an expected result set - the DNSBL syntax, 127.0.1.0-255 or
      // 127.0.0.2*, ranges and wildcards joined by | - only an answer it names
      // is a listing. With none set, any answer is.
      //
      // Except the codes the lists publish as errors, which are never a
      // listing, whatever the expected result (ClassifyAnswer). Until 12
      // September 2026 any answer at all was a listing, and Spamhaus's
      // refusals tagged every message with a link on a server that resolves
      // through 8.8.8.8 (discussion #167); until 6.3.6 the expected result
      // could still take them, and the DBL's 127.0.1.255, which it answers to
      // every address, tagged every message with a link to an address on a
      // server that asks the DBL with no expected result or with 127.0.1.*.
      const std::set<String> expected = BLCheck::ExpandAddresses(pSURBLServer->GetExpectedResult());
      const String zone = pSURBLServer->GetDNSHost();

      // What the lookups found, shared by the threads that make them. The
      // first that ends the list for the message - a listing, a refusal, a
      // lookup that failed, the deadline - stops any more being begun.
      struct Progress
      {
         std::mutex lock;
         size_t next = 0;
         bool stop = false;
         bool listed = false;
         bool timedOut = false;

         // A lookup that ended in an exception - set without the lock, from
         // the handler: no more are begun, and the list is told it was not
         // asked about whole, as for a lookup that failed.
         std::atomic<bool> broke{false};

         String failedName;
         String refusedName;
         String refusedAnswer;
         String refusedMeaning;

         // 127.0.0.1 is SURBL's and URIBL's answer to a query they block, and a
         // list that blocks this server answers it to every name. It does not
         // end the list for the message, as a refusal does: Hostkarma answers it
         // to a name on its whitelist, and a link to one would then switch the
         // list off for every link after it. The operator is told when every
         // name the list answered for a message was answered so.
         int answered = 0;
         int blocked = 0;
      };

      Progress progress;

      // One lookup, by whichever thread runs it: false when there was none
      // left to make - every name taken, or the list ended for the message.
      auto lookUpOne = [&]() -> bool
      {
         size_t index = 0;

         {
            std::lock_guard<std::mutex> guard(progress.lock);

            if (progress.stop || progress.broke.load() || progress.next >= vecUrls.size())
               return false;

            if (std::chrono::steady_clock::now() - start_time > deadline)
            {
               progress.stop = true;
               progress.timedOut = true;
               return false;
            }

            index = progress.next++;
         }

         const String &sURL = vecUrls[index];
         String sHostToLookup = sURL + "." + zone;

         // A name DNS cannot carry is not asked about. The resolver would
         // refuse it, and a lookup that fails abandons the list for the
         // message, so one long name in a link would have hidden the rest.
         // A zone written with the root's dot does not count it.
         size_t lookupLength = static_cast<size_t>(sHostToLookup.GetLength());
         if (sHostToLookup.Right(1) == _T("."))
            lookupLength--;

         if (lookupLength > maxNameLength)
         {
            LOG_DEBUG(Formatter::Format(_T("SURBL: Not asked: {0} is longer than DNS carries."), sHostToLookup));
            return true;
         }

         LOG_DEBUG(Formatter::Format(_T("SURBL: Lookup: {0}"), sHostToLookup));

         std::vector<String> saFoundNames;
         bool resolved = false;

         try
         {
            resolved = lookup(sHostToLookup, saFoundNames);
         }
         catch (...)
         {
            resolved = false;
         }

         if (!resolved)
         {
            LOG_DEBUG(Formatter::Format(_T("SURBL: DNS query failed: {0}"), sHostToLookup));

            std::lock_guard<std::mutex> guard(progress.lock);
            if (progress.failedName.IsEmpty())
               progress.failedName = sHostToLookup;
            progress.stop = true;
            return false;
         }

         bool listed = false;
         bool blocked = false;
         String errorAnswer;
         String errorMeaning;
         String refusedAnswer;
         String refusedMeaning;

         for (const String &found : saFoundNames)
         {
            String meaning;
            const AnswerKind kind = ClassifyAnswer(found, meaning);
            if (kind != AnswerVerdict)
            {
               if (errorAnswer.IsEmpty())
               {
                  errorAnswer = found;
                  errorMeaning = meaning;
               }

               if (kind == AnswerRefused && refusedAnswer.IsEmpty())
               {
                  refusedAnswer = found;
                  refusedMeaning = meaning;
               }

               if (kind == AnswerBlocked)
                  blocked = true;

               continue;
            }

            if (expected.empty())
            {
               listed = true;
            }
            else
            {
               for (const String &wanted : expected)
                  if (StringParser::WildcardMatch(wanted, found))
                     listed = true;
            }
         }

         LOG_DEBUG(Formatter::Format(_T("SURBL: {0} answered {1}: {2}"), sHostToLookup,
            StringParser::JoinVector(saFoundNames, ", "), listed ? _T("listed") : _T("not a listing")));

         if (!errorAnswer.IsEmpty())
         {
            LOG_DEBUG(Formatter::Format(_T("SURBL: {0} is {1}: an error, not a listing, whatever the expected result."), errorAnswer, errorMeaning));
         }

         std::lock_guard<std::mutex> guard(progress.lock);

         if (listed)
         {
            progress.listed = true;
            progress.stop = true;
            return false;
         }

         // A list that refuses this server's queries (Spamhaus's
         // 127.255.255.0/24) refuses the next one too. It is not asked again
         // for this message, and the operator is told, as for a lookup that
         // failed: its silence would look like clean mail.
         if (!refusedAnswer.IsEmpty())
         {
            if (progress.refusedName.IsEmpty())
            {
               progress.refusedName = sURL;
               progress.refusedAnswer = refusedAnswer;
               progress.refusedMeaning = refusedMeaning;
            }

            progress.stop = true;
            return false;
         }

         progress.answered++;
         if (blocked)
            progress.blocked++;

         return true;
      };

      // Nothing may leave a lookup: an exception - out of memory - ends the
      // list for the message, which is told it was not asked about whole.
      const std::function<bool()> step = [&lookUpOne, &progress]() -> bool
      {
         try
         {
            return lookUpOne();
         }
         catch (...)
         {
            progress.broke = true;
            return false;
         }
      };

      // The calling thread makes lookups itself, and up to LookupsAtOnce - 1
      // threads of the pool make them beside it, in turn with the other
      // messages being tested; every one of them is out of the lookups before
      // the answer is read. A lookup begun is bounded by the resolver's own
      // timeout. In the 6.3.6 tree before its release the threads were this
      // message's own, started here: fifteen more for each message being
      // tested at once, with no bound for the server.
      const size_t helpers = vecUrls.size() > 1 ? std::min(LookupsAtOnce, vecUrls.size()) - 1 : 0;
      pool.RunAlongside(step, helpers);

      if (progress.listed)
      {
         LOG_DEBUG("SURBL: Match found");
         return false;
      }

      if (!progress.refusedName.IsEmpty())
      {
         AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
            Formatter::Format("SURBL: {0} refused the query for {1}: it answered {2}, {3}. The list cannot be checked while it refuses this server's queries, and messages are accepted without its verdict.",
               zone, progress.refusedName, progress.refusedAnswer, progress.refusedMeaning));

         return true;
      }

      if (!progress.failedName.IsEmpty())
      {
         // Abandoning the whole list on the first failed lookup means the URLs
         // after it are never checked either, and the message is reported as
         // clean. That is the safe direction to fail, but it was completely
         // silent, so a resolver that had stopped answering presented as a
         // SURBL that never matches anything.
         AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
            Formatter::Format("SURBL: The lookup of {0} did not complete. URI blacklists cannot be checked while that continues, and affected messages are accepted without a SURBL verdict.", progress.failedName));

         return true;
      }

      if (progress.broke.load())
      {
         // An exception from a lookup - out of memory, most likely: the list
         // was not asked about every name, which is told as a failed lookup
         // is, and the message goes on without its verdict.
         AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
            Formatter::Format("SURBL: A lookup in {0} ended in an error. The list was not asked about every name of the message, which is accepted without its verdict.", zone));

         return true;
      }

      if (progress.timedOut)
      {
         LOG_DEBUG("SURBL: Aborting. Too long time elapsed.");
      }

      if (progress.blocked > 0 && progress.blocked == progress.answered)
      {
         AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
            Formatter::Format("SURBL: {0} answered 127.0.0.1 to every name of a message it was asked about ({1}). SURBL and URIBL answer that to a query they block: if the list is one of them, it cannot be checked while it blocks this server's queries, and messages are accepted without its verdict.",
               zone, progress.blocked));
      }

      LOG_DEBUG("SURBL: Match not found");
      return true;
   }

   namespace
   {
      struct LinkCase
      {
         const wchar_t *text;
         bool html;
         const wchar_t *expected;   // the hosts handed over, joined by '|'
         const char *why;
      };

      const LinkCase linkCases[] =
      {
         // The plain cases, as the regular expression before 6.3.6 read them.
         { L"see http://example.com/ now", false, L"example.com", "a link" },
         { L"HTTP://WWW.Example.COM/Path", false, L"example.com", "upper case and www." },
         { L"http://www.example", false, L"www.example", "www. kept when one label would be left" },
         { L"ftp://files.example/x", false, L"files.example", "another scheme" },
         { L"hxxp://defanged.example/", false, L"defanged.example", "a scheme written wrong" },
         { L"https://ca.secunia.com/?page=viewadvisory&vuln_id=50800", false, L"ca.secunia.com", "a query" },
         { L"http://a_b.evil.example/", false, L"a_b.evil.example", "an underscore" },

         // The userinfo: the host follows the last '@' before the path.
         { L"http://x@evil.example/", false, L"evil.example", "a user name" },
         { L"http://paypal.com@evil.example/", false, L"evil.example", "a user name that is a name" },
         { L"https://user:pass@evil.example:8443/x", false, L"evil.example", "a user name, a password and a port" },
         { L"http://a@b@evil.example/", false, L"evil.example", "two '@'" },
         { L"http://a%40b@evil.example/", false, L"evil.example", "%40 in the user name" },
         { L"http://evil.example:80@innocent.example/", false, L"innocent.example", "a name and a port in the user name" },
         { L"http://[2001:db8::1]@evil.example/", false, L"evil.example", "brackets in the user name" },
         { L"http://good.example\\@evil.example/", false, L"good.example", "a backslash ends the authority first" },
         { L"http://good.example/@evil.example/", false, L"good.example", "an '@' in the path" },
         { L"http://good.example?@evil.example/", false, L"good.example", "an '@' in the query" },
         { L"http://good.example#@evil.example/", false, L"good.example", "an '@' in the fragment" },
         { L"http://evil.example%40good.example/", false, L"evil.example", "%40 is not the userinfo's end: the name before it" },

         // Ports and IPv6.
         { L"http://evil.example:8080", false, L"evil.example", "a port" },
         { L"http://evil.example:/", false, L"evil.example", "an empty port" },
         { L"http://[2001:db8::1]/", false, L"", "an IPv6 address" },
         { L"http://x@[2001:db8::1]:80/", false, L"", "an IPv6 address with a user name and a port" },

         // Percent-encoding.
         { L"http://%65vil.example/", false, L"evil.example", "a percent-encoded letter" },
         { L"http://evil%2Eexample/", false, L"evil.example", "a percent-encoded dot" },
         { L"http://EVIL%2eEXAMPLE/", false, L"evil.example", "lower case after decoding" },
         { L"http://ev%il.example/", false, L"", "a '%' that encodes nothing, and one label either side of it" },

         // The separators a browser reads for http and https.
         { L"http:\\\\evil.example\\x", false, L"evil.example", "backslashes" },
         { L"http:/\\evil.example", false, L"evil.example", "a slash and a backslash" },
         { L"http:evil.example", false, L"evil.example", "no slash" },
         { L"https:///evil.example", false, L"evil.example", "three slashes" },
         { L"note http: that", false, L"", "a scheme and nothing after it" },

         // A link inside another's query.
         { L"https://r.example/?u=http%3A%2F%2Fx%40evil.example%2Fpath&v=1", false, L"r.example|evil.example", "a percent-encoded link with a user name" },
         { L"http%3a%2f%2fENCODED.example", false, L"encoded.example", "lower-case escapes" },
         { L"%3A%2F%2Fbare.example%3A81%2F", false, L"bare.example", "no scheme, and a port" },
         { L"https://r.example/?u=http%3A%5C%5Cevil.example%2F", false, L"r.example|evil.example", "an encoded link with backslashes" },
         { L"https://r.example/?u=http%3A%2F%2F%2Fevil.example", false, L"r.example|evil.example", "an encoded link with three slashes" },
         { L"https://r.example/?u=https%3Aevil.example&v=1", false, L"r.example|evil.example", "an encoded link with no slash" },
         { L"%3A%2F%2F%5Cbare.example", false, L"bare.example", "no scheme, and a backslash after the slashes" },
         { L"https://r.example/?u=http%3A//evil.example/x", false, L"r.example|evil.example", "an encoded link whose slashes were left alone, as Python's quote and JavaScript's escape write it" },
         { L"https://r.example/?u=https%3A%2F/%5C\\evil.example", false, L"r.example|evil.example", "encoded and raw slashes and backslashes mixed" },

         // IPv4 addresses, in the forms a browser reads.
         { L"http://192.0.2.1/", false, L"192.0.2.1", "an address" },
         { L"http://0xC0.0.2.1/", false, L"192.0.2.1", "a hexadecimal part" },
         { L"http://0300.0.2.1/", false, L"192.0.2.1", "an octal part" },
         { L"http://3221225985/", false, L"192.0.2.1", "one number" },
         { L"http://0xC0000201/", false, L"192.0.2.1", "one hexadecimal number" },
         { L"http://192.0.513/", false, L"192.0.2.1", "three parts" },
         { L"http://192.0.2.1./", false, L"192.0.2.1", "a trailing dot" },
         { L"http://user@192.0.2.1:8080/", false, L"192.0.2.1", "a user name and a port" },
         { L"http://192.0.2.256/", false, L"", "a part too large" },
         { L"http://08.0.0.1/", false, L"", "an octal part with an 8" },
         { L"http://1.2.3.4.5/", false, L"", "five parts" },
         { L"http://example.1/", false, L"", "a name whose last label is a number" },
         { L"(see http://192.0.2.1)", false, L"192.0.2.1", "an address followed by punctuation" },

         // Punctuation after a link, and names that are not names.
         { L"(see http://evil.example)", false, L"evil.example", "a parenthesis" },
         { L"http://evil.example, and", false, L"evil.example", "a comma" },
         { L"http://evil.example.)", false, L"evil.example", "a sentence's dot and a parenthesis" },
         { L"<http://evil.example>", false, L"evil.example", "angle brackets" },
         { L"'http://evil.example'", false, L"evil.example", "single quotes" },
         { L"\"http://evil.example\"@innocent.example", false, L"evil.example", "the quote the link was opened by ends it" },
         { L"http://evil.example\"@innocent.example", false, L"innocent.example|evil.example", "a quote nothing opened: a browser goes past it, a mail program stops at it" },

         // Where a mail program ends a link in text and a URL parser does not:
         // the host after the '@', and the name before the character.
         { L"http://evil.example)@innocent.example/", false, L"innocent.example|evil.example", "a ')' before the '@'" },
         { L"http://evil.example:80`@innocent.example/", false, L"innocent.example|evil.example", "a '`' after a port, before the '@'" },
         { L"http://a@evil.example}@innocent.example/", false, L"innocent.example|evil.example", "a '}' in the second part of the user name" },
         { L"http://evil.example\x202E@innocent.example/", false, L"innocent.example|evil.example", "a bidirectional mark before the '@'" },
         { L"http://innocent.example@evil.example/", false, L"evil.example", "a user name of names alone hands over nothing more" },
         { L"<p>http://evil.example)@innocent.example/</p>", true, L"innocent.example|evil.example", "in HTML's text as in a text part" },
         { L"<a href=\"http://evil.example)@innocent.example/\">", true, L"innocent.example", "a quoted value is read as a browser reads it, alone" },
         { L"http://evil.example;http://other.example/", false, L"evil.example|other.example", "two links with no space between them" },
         { L"http://localhost/", false, L"", "one label" },
         { L"http://../ http://two.example/", false, L"two.example", "no name, then a link" },
         { L"http://a..evil.example/", false, L"", "an empty label" },
         { L"http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.evil.example/", false, L"", "a label of 64 characters" },

         // A character a name cannot hold, before the registered name.
         { L"http://a!b.evil.example/", false, L"evil.example", "a '!' in a label" },
         { L"http://\x0430" L"pple.evil.example/", false, L"evil.example", "a label in Unicode" },
         { L"http://b\x00FC" L"cher.example/", false, L"", "an internationalised name, not turned into its xn-- form" },
         { L"http://evil.example),see.also.example", false, L"evil.example|also.example", "the name before the character and the labels after its label" },

         // Quoted-printable soft line breaks, and line breaks that are not.
         { L"http://surbl-org-perma=\r\nnent-test-point.com", false, L"surbl-org-permanent-test-point.com", "a soft line break" },
         { L"http://surbl-org-perma=\nnent-test-point.com", false, L"surbl-org-permanent-test-point.com", "a soft line break with a bare LF" },
         { L"http://surbl-org-perm\r\nanent-test-point.com", false, L"", "a line break ends the link" },
         { L"http://surbl-org-permanent-test-point.com\r\nHello", false, L"surbl-org-permanent-test-point.com", "a line break after the link" },
         { L"<a href=3D'http://surbl-org-permanent-test-point.com'>Test</a>", false, L"surbl-org-permanent-test-point.com", "HTML in a text part" },
         { L"http://x&#64;evil.example/", false, L"", "a character reference in a text part is text" },

         // HTML: character references, as the browser reads them.
         { L"<a href=\"http://x&#64;evil.example/\">", true, L"evil.example", "&#64;" },
         { L"<a href=\"http://x&#x40;evil.example/\">", true, L"evil.example", "&#x40;" },
         { L"<a href=\"http://x&commat;evil.example/\">", true, L"evil.example", "&commat;" },
         { L"<a href=\"http://x&#64evil.example/\">", true, L"evil.example", "a numeric reference without its semicolon" },
         { L"<a href=\"http&#58;//evil.example/\">", true, L"evil.example", "the separator's colon" },
         { L"<a href=\"http&colon;&sol;&sol;evil.example/\">", true, L"evil.example", "the separator by name" },
         { L"<a href=\"&#104;ttp://evil.example/\">", true, L"evil.example", "the scheme's first letter" },
         { L"<a href=\"http://&#101;vil.example/\">", true, L"evil.example", "the host's first letter" },
         { L"<a href=\"http://evil&period;example/\">", true, L"evil.example", "the host's dot" },
         { L"<a href=\"http://evil.example&#47;@innocent.example/\">", true, L"evil.example", "a slash written as a reference ends the authority" },
         { L"<p>&#x68;ttp&#x3A;//x&#x40;evil.example</p>", true, L"evil.example", "in text" },
         { L"&lt;http://evil.example&gt;", true, L"evil.example", "&lt; and &gt; around a link" },
         { L"<a href=\"https://r.example/?u=http%3A%2F%2Fevil.example%2F&amp;v=1\">", true, L"r.example|evil.example", "&amp; ends an encoded link" },

         // HTML: an attribute's quoted value is the link, to its quote.
         { L"<a href=\"http://a b@evil.example/\">", true, L"evil.example", "a space in the user name" },
         { L"<a href=\"http://a'b@evil.example/\">", true, L"evil.example", "the other quote in the user name" },
         { L"<a href='http://a\"b@evil.example/'>", true, L"evil.example", "the other quote, the other way round" },
         { L"<a href=\"http://a>b@evil.example/\">", true, L"evil.example", "a '>' in the user name" },
         { L"<a href = \"http://a<b@evil.example/\">", true, L"evil.example", "a '<' in the user name, spaces round the '='" },
         { L"<a href=\" http://a b@evil.example/\">", true, L"evil.example", "a space before the link in the value" },
         { L"<a href=\"\r\n\thttp://a b@evil.example/\">", true, L"evil.example", "a line break and a tab before the link in the value" },
         { L"<a href=\"&#32;&#9;http://a b@evil.example/\">", true, L"evil.example", "a space and a tab written as references before the link in the value" },
         { L"He wrote \" http://a b@evil.example/", false, L"", "in a text part a space ends the link, whatever came before it" },
         { L"<a href=\"http://x&quot;@evil.example/\">", true, L"evil.example", "&quot; is the value's, not its end" },
         { L"<a href=\"http://spam.example\"x=\"@innocent.example\">", true, L"spam.example", "the quote ends the value" },
         { L"<a href=\"http://ev&#10;il.example/\">", true, L"evil.example", "a line break the URL parser drops, as a reference" },
         { L"<a href=\"http://ev\r\nil.example/\">", true, L"evil.example", "a line break the URL parser drops" },
         { L"<p>\"http://good.example and write to x@innocent.example</p>", true, L"good.example", "a quote in text is not an attribute's" },
         { L"<a href=http://x@evil.example/>", true, L"evil.example", "an unquoted value" },

         // The code points UTS 46 ignores, which a browser deletes from a host,
         // and percent-encoding read as UTF-8.
         { L"http://ev\u00ADil.example/", false, L"evil.example", "a soft hyphen" },
         { L"http://ev%C2%ADil.example/", false, L"evil.example", "a soft hyphen, percent-encoded as UTF-8" },
         { L"http://evil\u200B.example/", false, L"evil.example", "a zero-width space" },
         { L"http://evil%E2%80%8B.example/", false, L"evil.example", "a zero-width space, percent-encoded" },
         { L"http://e\u2060vil\u2064.exam\uFEFFple/", false, L"evil.example|evil.exam", "a word joiner, an invisible plus and a zero-width no-break space; and in a text part the name before the last, where a script's \\s ends the link" },
         { L"http://evil\uFE0F.example/", false, L"evil.example", "a variation selector" },
         { L"http://evil\U000E0100.example/", false, L"evil.example", "VARIATION SELECTOR-17, outside the Basic Multilingual Plane" },
         { L"http://evil%F3%A0%84%80.example/", false, L"evil.example", "the same, percent-encoded in four bytes" },
         { L"http://192.0\u00AD.2.1/", false, L"192.0.2.1", "a soft hyphen in an address" },
         { L"http://evil%C2.example/", false, L"", "a byte that begins UTF-8 and is not followed as UTF-8 goes on is U+FFFD" },
         { L"http://evil%AD.example/", false, L"", "a byte that cannot begin UTF-8 is U+FFFD, not a soft hyphen" },
         { L"http://evil\xD800.example/", false, L"", "a surrogate on its own is U+FFFD" },
         { L"http://evil\u200D.example/", false, L"", "a zero-width joiner is not ignored: a browser refuses it here" },
         { L"<a href=\"http://ev&shy;il.example/\">", true, L"evil.example", "&shy;" },
         { L"<a href=\"http://ev&shy&#105;l.example/\">", true, L"evil.example", "&shy without its semicolon" },
         { L"<a href=\"http://evil&ZeroWidthSpace;.example/\">", true, L"evil.example", "&ZeroWidthSpace;" },
         { L"<a href=\"http://e&NoBreak;v&it;i&ic;l&af;.example/\">", true, L"evil.example", "&NoBreak;, &it;, &ic; and &af;" },
         { L"<a href=\"http://evil&#xE0100;.example/\">", true, L"evil.example", "a numeric reference outside the Basic Multilingual Plane" },
         { L"https://r.example/?u=http%3A%2F%2Fev%C2%ADil.example", false, L"r.example|evil.example", "a soft hyphen in a link in another's query" },

         // Where a mail program's script ends a link in running text at a
         // code point its \s matches and a browser given the whole link
         // deletes: both hosts.
         { L"see http://evil.example\uFEFFmore/ now", false, L"evil.examplemore|evil.example", "a zero-width no-break space, which JavaScript's \\s matches" },
         { L"http://ev\u00ADil.example\u200Bx.test/", false, L"evil.examplex.test|evil.example", "a zero-width space after a soft hyphen: the name before it, the soft hyphen deleted" },
         { L"http://x@evil.example\u180E.test@good.example/", false, L"good.example|evil.example.test|evil.example", "a Mongolian vowel separator in the user name" },
         { L"see https://u\u180Ev@evil.examp\uFEFFle/ now", false, L"evil.example|evil.examp", "a Mongolian vowel separator in the user name and a zero-width no-break space in the host: each is where some engine ends the link" },
         { L"<p>http://evil.example&#xFEFF;more/</p>", true, L"evil.examplemore|evil.example", "in HTML's text as in a text part" },
         { L"<a href=\"http://evil.example&#xFEFF;more/\">", true, L"evil.examplemore", "an attribute's value is the link a browser is given, whole" },
         { L"https://r.example/?u=http%3A%2F%2Fevil.example%EF%BB%BFmore", false, L"r.example|evil.examplemore", "a link in another's query is decoded by a redirector, not ended by a script" },
         { L"http://evil.example\u00ADmore/", false, L"evil.examplemore", "a soft hyphen, which no engine's \\s matches" },

         // An IPv6 address in brackets that carries an IPv4 one a browser
         // reaches, and one that does not.
         { L"http://[::ffff:192.0.2.1]/", false, L"192.0.2.1", "an IPv4-mapped address" },
         { L"http://[::FFFF:C000:0201]:8080/", false, L"192.0.2.1", "the same in hexadecimal, with a port" },
         { L"http://[0:0:0:0:0:ffff:192.0.2.1]/", false, L"192.0.2.1", "the same written out" },
         { L"http://x@[::ffff:c000:201]/", false, L"192.0.2.1", "the same after a user name" },
         { L"http://[64:ff9b::192.0.2.1]/", false, L"192.0.2.1", "the NAT64 well-known prefix" },
         { L"http://[64:ff9b::c000:201]/", false, L"192.0.2.1", "the NAT64 well-known prefix, in hexadecimal" },
         { L"http://[::192.0.2.1]/", false, L"", "an IPv4-compatible address, which nothing carries to IPv4" },
         { L"http://[::ffff:1:2:3]/", false, L"", "not mapped" },
         { L"http://[::ffff:192.0.2.01]/", false, L"", "a leading zero in the IPv4 part, which a browser refuses" },
         { L"http://[::ffff:192.0.2]/", false, L"", "three IPv4 parts" },
         { L"http://[::ffff:192.0.2.1%25eth0]/", false, L"", "a zone, which a browser refuses" },
         { L"http://[1::2::3]/", false, L"", "two compressions" },
         { L"http://[1:2:3:4:5:6:7:8:9]/", false, L"", "nine pieces" },
         { L"http://[::ffff:192.0.2.1/", false, L"", "no closing bracket" },
         { L"https://r.example/?u=http%3A%2F%2F%5B%3A%3Affff%3A192.0.2.1%5D%2F", false, L"r.example|192.0.2.1", "a mapped address in a link in another's query" },

         // Links with no scheme, which a browser resolves against the page's.
         { L"<a href=\"//evil.example/\">", true, L"evil.example", "two slashes at the start of an attribute's value" },
         { L"<img src='\\\\evil.example/x.png'>", true, L"evil.example", "two backslashes" },
         { L"<a href=\"/\\evil.example/\">", true, L"evil.example", "a slash and a backslash" },
         { L"<a href=\"///evil.example/\">", true, L"evil.example", "three slashes" },
         { L"<a href=\" //x@evil.example/\">", true, L"evil.example", "a space before it, and a user name" },
         { L"<a href=\"/\n/evil.example/\">", true, L"evil.example", "a line break between the slashes, which the URL parser drops" },
         { L"<a href=//evil.example/>", true, L"evil.example", "an unquoted value" },
         { L"<a href=\"/evil.example/\">", true, L"", "one slash is a path on the page's own host" },
         { L"<a href=\"%2F%2Fevil.example\">", true, L"", "percent-encoded slashes at the start of a value are a path" },
         { L"<p>see //evil.example/ here</p>", true, L"", "two slashes in text are no link" },
         { L"<p>x=\"//evil.example/\"</p>", true, L"evil.example", "an attribute's value is known by its '=' and quote, even in text" },
         { L"see //evil.example/ here", false, L"", "nor in a text part" },
         { L"https://r.example/?u=//evil.example&v=1", false, L"r.example|evil.example", "a query parameter's value" },
         { L"https://r.example/?u=%2F%2Fevil.example%2Fx", false, L"r.example|evil.example", "a query parameter's value, percent-encoded" },
         { L"<a href=\"https://r.example/?u=%2f%5cevil.example\">", true, L"r.example|evil.example", "a slash and a backslash, encoded, in HTML" },
         { L"http://a.example//evil.example/", false, L"a.example", "two slashes in a path" },

         // What the URL parser drops before it reads a value: a line break or
         // a tab after the scheme and between the slashes.
         { L"<a href=\"http:&#10;//evil.example/\">", true, L"evil.example", "a line break after the scheme" },
         { L"<a href=\"http:/\t/evil.example/\">", true, L"evil.example", "a tab between the slashes" },
         { L"http:\n//evil.example/", false, L"", "in a text part a line break ends a link" },

         // An attribute's value as the URL parser reads it, without its tabs
         // and line breaks: a link in its query too.
         { L"<a href=\"https://r.example/?u=http%3A%2F%2Fev&#9;il.example\">", true, L"r.example|evil.example", "a tab in a link in an attribute's query" },
         { L"<a href=\"https://r.example/?u=http%3&#10;A%2F%2Fevil.example\">", true, L"r.example|evil.example", "a line break in an encoded separator" },
         { L"<a href=\"https://r.example/?u=//ev\r\nil.example\">", true, L"r.example|evil.example", "a line break in a query's link with no scheme" },
         { L"<a title='x' href = 'https://r.example/?u=%2F&#9;%2Fevil.example'>", true, L"r.example|evil.example", "a tab between encoded slashes, in the second value" },
         { L"<a href=\"https://r.example/?u=http%3A%2F%2Fevil.example%2F\npath\">", true, L"r.example|evil.example", "a line break after the host: nothing more" },
         { L"<p>a=\"see http://evil.example\nmore\"</p>", true, L"evil.example", "a quote in the text between tags opens no value" },
         { L"<a href=\"https://r.example/?u=http%3A%2F%2Fev&#9;il.example", true, L"r.example", "a tag the text ends inside is no tag" },
         { L"<a title=\"&quot;=\" href=\"https://r.example/?u=//ev&#9;il.example\">", true, L"r.example|evil.example", "a quote written by a reference is the value's, not its end" },
      };

      String Joined_(const std::vector<String> &values)
      {
         String joined;
         for (size_t i = 0; i < values.size(); i++)
         {
            if (i > 0)
               joined += _T("|");
            joined += values[i];
         }
         return joined;
      }

      struct NamesCase
      {
         const wchar_t *host;
         const wchar_t *expected;
         const char *why;
      };

      const NamesCase namesCases[] =
      {
         { L"192.0.2.1", L"1.2.0.192", "an address, reversed and only reversed" },
         { L"example.com", L"example.com", "a registered name" },
         { L"mail.example.co.uk", L"mail.example.co.uk|example.co.uk", "a host and its registrable domain" },
         { L"tenant.github.io", L"tenant.github.io|github.io", "a name under a PRIVATE suffix, and the domain registered under the ICANN section's io" },
         { L"a.b.blogspot.com", L"a.b.blogspot.com|b.blogspot.com|blogspot.com", "a host under a PRIVATE suffix: the host, its registrable domain, and the ICANN section's" },
         { L"evil.duckdns.org", L"evil.duckdns.org|duckdns.org", "duckdns.org, which URIBL lists whole" },
         { L"github.io", L"github.io", "a PRIVATE suffix itself, registered under io" },
         { L"www.city.kawasaki.jp", L"www.city.kawasaki.jp|city.kawasaki.jp", "an ICANN exception: both sections cut alike" },
         { L"co.uk", L"co.uk", "a public suffix" },
      };

      struct AnswerCase
      {
         const wchar_t *answer;
         SURBL::AnswerKind expected;
      };

      const AnswerCase answerCases[] =
      {
         { L"127.0.0.2", SURBL::AnswerVerdict },
         { L"127.0.0.14", SURBL::AnswerVerdict },
         { L"127.0.0.254", SURBL::AnswerVerdict },
         { L"127.0.1.2", SURBL::AnswerVerdict },
         { L"127.0.1.254", SURBL::AnswerVerdict },
         { L"127.0.1.255", SURBL::AnswerNotForAddresses },
         { L"127.0.2.2", SURBL::AnswerVerdict },
         { L"127.0.2.24", SURBL::AnswerVerdict },
         { L"127.0.2.255", SURBL::AnswerNotForAddresses },
         { L"127.0.0.1", SURBL::AnswerBlocked },
         { L"127.255.255.252", SURBL::AnswerRefused },
         { L"127.255.255.254", SURBL::AnswerRefused },
         { L"127.255.255.255", SURBL::AnswerRefused },
         { L"127.255.255.250", SURBL::AnswerRefused },
         { L"127.255.25.1", SURBL::AnswerVerdict },
      };

      // The URL entries of the sender lists (SenderPattern), whose host is read
      // with HostOfAuthority: the entry as it is stored, and whether it is taken.
      struct UrlEntryCase
      {
         const wchar_t *entry;
         const wchar_t *stored;
         bool taken;
         const char *why;
      };

      const UrlEntryCase urlEntryCases[] =
      {
         { L"https://www.Bad.Example/x?y=1", L"bad.example/", true, "an entry with a scheme, www., a path and a query" },
         { L"bad.example/", L"bad.example/", true, "an entry as it is stored" },
         { L"http://a@b@host.example/", L"host.example/", true, "an entry with two '@': the host after the last" },
         { L"http://user@www.host.example/", L"host.example/", true, "an entry with a user name before www." },
         { L"http://user@host.example:8080/", L"host.example/", true, "an entry with a user name and a port" },
         { L"https://bad.example?x=1", L"bad.example/", true, "an entry with a query straight after the host" },
         { L"https://bad.example#top", L"bad.example/", true, "an entry with a fragment straight after the host" },
         { L"http://good.example\\@evil.example/", L"good.example/", true, "an entry whose backslash ends the authority first" },
         { L"http://%62ad.example/", L"bad.example/", true, "an entry with a percent-encoded letter" },
         { L"http://bad.example./", L"bad.example/", true, "an entry with a trailing dot" },
         { L"http://192.0.2.61/", L"192.0.2.61/", true, "an entry of an address" },
         { L"http://0xC0.0.2.61/", L"192.0.2.61/", true, "an entry of an address in another form a browser reads" },
         { L"http://www.example/", L"www.example/", true, "an entry whose www. is kept when one label would be left" },
         { L"http://www.www.example.com/", L"example.com/", true, "an entry with www. twice, read until reading changes nothing" },
         { L"http://[2001:db8::1]/", L"[2001/", false, "an entry of an IPv6 address" },
         { L"http://[::ffff:192.0.2.61]/", L"192.0.2.61/", true, "an entry of an IPv4-mapped IPv6 address: the IPv4 address a browser reaches" },
         { L"http://sh%C2%ADop.example/", L"shop.example/", true, "an entry with a soft hyphen, which a browser deletes" },
         { L"https://a!b.example/", L"a!b.example/", false, "an entry with a character a name cannot hold" },
         { L"https://nodot/x", L"nodot/", false, "an entry of one label" },
         { L"https://", L"/", false, "an entry with no host" },
      };

      // Whether a URL entry covers a link: the hosts FindLinkHosts hands over
      // for a text part holding it, each asked of SenderPattern::MatchesUrlHost.
      struct UrlMatchCase
      {
         const wchar_t *entry;
         const wchar_t *link;
         bool covered;
         const char *why;
      };

      const UrlMatchCase urlMatchCases[] =
      {
         { L"http://user@www.shop.example/", L"http://www.shop.example/sale", true, "an entry with a user name before www. covers its host's link" },
         { L"http://a@b@shop.example/", L"see http://x@shop.example/", true, "an entry with two '@' covers its host's link" },
         { L"https://shop.example/", L"http://cdn.shop.example/", true, "an entry covers a subdomain's link" },
         { L"https://shop.example/", L"http://notshop.example/", false, "an entry covers no other name that ends in it" },
         { L"http://www.example/", L"http://www.example/x", true, "an entry and a link whose www. is kept alike" },
         { L"http://www.www.shop.example/", L"http://www.www.shop.example/x", true, "an entry with www. twice covers its own link" },
         { L"http://0xC0.0.2.61/", L"http://192.0.2.61/", true, "an entry and a link of one address in two forms" },
         { L"https://shop.example/", L"http://sh\u00ADop.example/", true, "an entry covers a link with a soft hyphen in its host" },
         { L"http://192.0.2.61/", L"http://[::ffff:192.0.2.61]/", true, "an address's entry covers a link to it as an IPv4-mapped address" },
         { L"https://shop.example/", L"https://r.example/?u=//cdn.shop.example/", true, "an entry covers a link with no scheme in another's query" },
      };

      // The entry hosts that cover a link's host (SenderPattern::UrlEntryHostsCovering).
      struct CoveringCase
      {
         const wchar_t *host;
         const wchar_t *expected;
         const char *why;
      };

      const CoveringCase coveringCases[] =
      {
         { L"cdn.bad.example", L"cdn.bad.example|bad.example|example", "a host, and each name it ends in" },
         { L"www.shop.example", L"shop.example|example", "the host read again as an entry is: www. dropped" },
         { L"192.0.2.61", L"192.0.2.61|0.2.61|2.61|61", "an address" },
         { L"a!b.example", L"", "no host a link can have" },
      };

      void Fail_(const char *why, const String &input, const String &got, const String &expected)
      {
         throw std::logic_error("SURBLTester: " + std::string(why) + " (" + std::string(AnsiString(input)) + "): got \"" +
            std::string(AnsiString(got)) + "\", expected \"" + std::string(AnsiString(expected)) + "\"");
      }
   }

   void
   SURBLTester::Test()
   {
      for (const LinkCase &test : linkCases)
      {
         std::vector<String> hosts;
         SURBL::FindLinkHosts(test.text, test.html, [&hosts](const String &host) { hosts.push_back(host); return true; });

         const String got = Joined_(hosts);
         if (got != String(test.expected))
            Fail_(test.why, test.text, got, test.expected);
      }

      // visit stops the reading when it answers false.
      {
         std::vector<String> hosts;
         SURBL::FindLinkHosts(_T("http://one.example http://two.example http://three.example"), false,
            [&hosts](const String &host) { hosts.push_back(host); return hosts.size() < 2; });
         if (Joined_(hosts) != _T("one.example|two.example"))
            Fail_("the reading stops when visit answers false", _T("three links"), Joined_(hosts), _T("one.example|two.example"));
      }

      // A long text of separators and nothing else is read, and hands over
      // nothing, in time proportional to its length.
      {
         String text;
         for (int i = 0; i < 20000; i++)
            text += _T("%3A%2F%2F");
         int visits = 0;
         SURBL::FindLinkHosts(text, false, [&visits](const String &) { visits++; return true; });
         if (visits != 0)
            Fail_("a text of separators", _T("%3A%2F%2F x 20000"), StringParser::IntToString(visits), _T("0"));
      }

      // A long run of what a separator is made of - "%2F" after "http%3A" or
      // "%3A%2F%2F" or a query parameter's '=', slashes and line breaks - is
      // read without the regular expression keeping state for each piece of
      // it, and the link after it is still found. An expression of this tree
      // that repeated "%2F" made boost throw on such a run, before any release
      // had it, which ended the reading of the message.
      {
         String encodedRun;
         for (int i = 0; i < 60000; i++)
            encodedRun += _T("%2f");
         String brokenRun;
         for (int i = 0; i < 100000; i++)
            brokenRun += _T("/\n");
         String mixedRun;
         for (int i = 0; i < 60000; i++)
            mixedRun += _T("%2f/\\");

         const String texts[] =
         {
            _T("http%3a") + encodedRun + _T(" http://evil.example/"),
            _T("%3A%2F%2F") + encodedRun + _T(" http://evil.example/"),
            _T("?u=") + encodedRun + _T(" http://evil.example/"),
            _T("?u=") + String(200000, L'/') + _T(" http://evil.example/"),
            brokenRun + _T(" http://evil.example/"),
            _T("http%3A") + mixedRun + _T(" http://evil.example/"),
         };

         for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
         {
            std::vector<String> hosts;
            SURBL::FindLinkHosts(texts[i], false, [&hosts](const String &host) { hosts.push_back(host); return true; });
            if (Joined_(hosts) != _T("evil.example"))
               Fail_("a link after a long run of a separator's pieces", texts[i].Left(12) + _T("...") + texts[i].Right(30), Joined_(hosts), _T("evil.example"));
         }
      }

      // A reading of more than 256 characters that took in the scheme of the
      // link after it: the reading ends at that link's slashes, and the link
      // is still found.
      {
         const String filler(300, L'a');
         const String texts[] =
         {
            _T("http:") + filler + _T("!http://evil.example/"),
            _T("ftp://") + filler + _T("(https://evil.example/)"),
            _T("<p>http://") + filler + _T(",http:\\\\evil.example\\"),
         };

         for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
         {
            std::vector<String> hosts;
            SURBL::FindLinkHosts(texts[i], i == 2, [&hosts](const String &host) { hosts.push_back(host); return true; });
            if (Joined_(hosts) != _T("evil.example"))
               Fail_("a link after a reading of more than 256 characters", texts[i].Right(40), Joined_(hosts), _T("evil.example"));
         }
      }

      // A link whose user name holds 200,000 names, each where a mail program
      // may end the link, hands over each of them once and is read in time
      // proportional to its length: whether a name was handed over already is
      // looked up. Searched for among those before it, as it was, these
      // 3.3 MB took 84 s to read on the machine where they now take 0.4 s;
      // the bound leaves an emulated build room enough.
      {
         const int names = 200000;
         String text = _T("http://");
         for (int i = 0; i < names; i++)
            text += _T("a") + StringParser::IntToString(i) + _T(".example)@");
         text += _T("x.example/");

         size_t visits = 0;
         String last;
         const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
         SURBL::FindLinkHosts(text, false, [&](const String &host) { visits++; last = host; return true; });
         const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

         if (visits != static_cast<size_t>(names) + 1 || last != _T("a199999.example"))
            Fail_("each name in a long user name is handed over once", _T("http://a0.example)@...x.example/"),
               StringParser::IntToString(static_cast<int>(visits)) + _T(" hosts, the last ") + last, _T("200001 hosts, the last a199999.example"));

         const double bound = HM_TEST_TIME_BOUND(20000);
         if (seconds * 1000 > bound)
            Fail_("a long user name is read in time proportional to its length", _T("http://a0.example)@...x.example/"),
               StringParser::IntToString(static_cast<int>(seconds)) + _T(" s"),
               String(_T("well under ")) + StringParser::IntToString(static_cast<int>(bound / 1000)) + _T(" s"));
      }

      // A text read only as far as its first part (the sender lists' URL
      // entries, for a message over the anti-spam size limit): the links that
      // end before the cut, as the whole text has them, and no host that the
      // whole text does not hand over - a link the cut runs through is left
      // out, not read as the part of its host before the cut.
      {
         struct CutCase
         {
            const wchar_t *text;
            bool html;
            size_t maxCharacters;
            const wchar_t *expected;
            const char *why;
         };
         const CutCase cutCases[] =
         {
            { L"http://one.example/ http://two.example/", false, 20, L"one.example",
              "a link after the cut is not read" },
            { L"http://one.example/ http://evil.example.com/", false, 36, L"one.example",
              "a link the cut runs through hands over nothing - not evil.exam" },
            { L"http://one.example/ http://evil.example/x", false, 40, L"one.example|evil.example",
              "a link that ends before the cut is read whole" },
            { L"http://one.example/ http://two.example/", false, 1000, L"one.example|two.example",
              "a text no longer than the limit is read whole" },
            { L"http://one.example/ http://two.example/", false, 39, L"one.example|two.example",
              "a text exactly as long as the limit is read whole" },
            { L"<p>http://one.example/ http://evil.ex&#97;mple.com/x</p>", true, 40, L"one.example",
              "a character reference the cut would split is left out, not read as a tab (&#9) that ends the host - not evil.ex" },
            { L"<p>http://one.example/ http://evil.ex&#97;mple.com/x</p>", true, 1000, L"one.example|evil.example.com",
              "the same HTML read whole" },
            { L"<a href=\"http://one.example/\">x</a><a href=\"https://evil.exa\nmple.com/\">y</a>", true, 65, L"one.example",
              "a value the cut runs through hands over nothing - not evil.example" },
            { L"<a href=\"http://one.example/\">x</a><a href=\"https://evil.exa\nmple.com/\">y</a>", true, 1000, L"one.example|evil.example.com",
              "the same value, whole, its line break dropped" },
            { L"see http%3A%2F%2Fevil.example.com%2Fx and more", false, 30, L"",
              "an encoded link the cut runs through hands over nothing - not evil.example" },
            { L"see http%3A%2F%2Fevil.example.com%2Fx and more", false, 1000, L"evil.example.com",
              "the same encoded link read whole" },
            { L"http:", false, 3, L"",
              "a cut inside the scheme" },
         };

         for (const CutCase &test : cutCases)
         {
            std::vector<String> hosts;
            SURBL::FindLinkHosts(test.text, test.html, [&hosts](const String &host) { hosts.push_back(host); return true; }, test.maxCharacters);
            if (Joined_(hosts) != String(test.expected))
               Fail_(test.why, test.text, Joined_(hosts), test.expected);
         }

         // The first part itself: a reference the cut would split goes, with
         // its '&', however many leading zeros it was written with, and
         // nothing else does.
         struct PartCase
         {
            const wchar_t *text;
            bool html;
            size_t maxCharacters;
            const wchar_t *expected;
         };
         const PartCase partCases[] =
         {
            { L"abc&#0000000000000000000000000000000000000000000109;def", true, 40, L"abc" },
            { L"abc&amp;def", true, 8, L"abc&amp;" },
            { L"abc&amp;def", true, 7, L"abc" },
            { L"abc&amp;def", false, 7, L"abc&amp" },
            { L"a & b and more", true, 6, L"a & b " },
            { L"abcdefgh", true, 4, L"abcd" },
            { L"abcd", true, 4, L"abcd" },
         };
         for (const PartCase &test : partCases)
         {
            const String got = SURBL::FirstPartForLinks(test.text, test.html, test.maxCharacters);
            if (got != String(test.expected))
               Fail_("the first part of a text cut for its links", test.text, got, test.expected);
         }

         // A long text read only as far as its limit: the links after the cut
         // are never read, so the time is the limit's and not the text's.
         String many;
         for (int i = 0; i < 100000; i++)
            many += _T("http://a") + StringParser::IntToString(i % 10) + _T(".example/ ");
         size_t visits = 0;
         SURBL::FindLinkHosts(many, false, [&visits](const String &) { visits++; return true; }, 2000);
         // Each link is 19 characters with its space: 105 end before the cut.
         if (visits != 105)
            Fail_("a long text is read only as far as its limit", _T("http://a0.example/ x 100000, 2000 characters"),
               StringParser::IntToString(static_cast<int>(visits)) + _T(" hosts"), _T("105 hosts"));
      }

      for (const NamesCase &test : namesCases)
      {
         const String got = Joined_(SURBL::NamesToAsk(test.host));
         if (got != String(test.expected))
            Fail_(test.why, test.host, got, test.expected);
      }

      for (const AnswerCase &test : answerCases)
      {
         String meaning;
         const SURBL::AnswerKind got = SURBL::ClassifyAnswer(test.answer, meaning);
         if (got != test.expected)
            Fail_("the kind of answer", test.answer, StringParser::IntToString(got), StringParser::IntToString(test.expected));

         if ((got == SURBL::AnswerVerdict) != meaning.IsEmpty())
            Fail_("an error answer has a meaning, and a verdict none", test.answer, meaning, _T(""));
      }

      for (const UrlEntryCase &test : urlEntryCases)
      {
         const String stored = SenderPattern::Canonical(test.entry);
         if (stored != String(test.stored))
            Fail_(test.why, test.entry, stored, test.stored);

         // Read back from the database, the stored entry is itself again.
         if (SenderPattern::Canonical(stored) != stored)
            Fail_("an entry read back as it was stored", stored, SenderPattern::Canonical(stored), stored);

         String error;
         const bool taken = SenderPattern::Validate(test.entry, error);
         if (taken != test.taken)
            Fail_(test.why, test.entry, taken ? String(_T("taken")) : String(_T("refused: ")) + error,
               test.taken ? String(_T("taken")) : String(_T("refused")));
      }

      for (const UrlMatchCase &test : urlMatchCases)
      {
         bool covered = false;
         String hosts;
         SURBL::FindLinkHosts(test.link, false, [&](const String &host)
         {
            if (!hosts.IsEmpty())
               hosts += _T("|");
            hosts += host;
            covered = covered || SenderPattern::MatchesUrlHost(test.entry, host);
            return true;
         });

         if (covered != test.covered)
            Fail_(test.why, String(test.entry) + _T(" and ") + test.link, covered ? String(_T("covered ")) + hosts : String(_T("not covered ")) + hosts,
               test.covered ? String(_T("covered")) : String(_T("not covered")));
      }

      for (const CoveringCase &test : coveringCases)
      {
         std::vector<String> covering;
         SenderPattern::UrlEntryHostsCovering(test.host, covering);
         if (Joined_(covering) != String(test.expected))
            Fail_(test.why, test.host, Joined_(covering), test.expected);
      }

      // Every link of a message is matched against the URL entries, however
      // many come before it: until 6.3.6 the first 16 hosts
      // were, and an entry's link after sixteen others was never seen. The
      // first link an entry host covers is the one named.
      {
         String text;
         for (int i = 1; i <= 40; i++)
            text += _T("http://decoy") + StringParser::IntToString(i) + _T(".example/ ");
         text += _T("http://cdn.shop.example/sale http://192.0.2.61/ http://cdn2.shop.example/");

         std::set<String> entryHosts;
         entryHosts.insert(_T("shop.example"));
         entryHosts.insert(_T("192.0.2.61"));
         entryHosts.insert(_T("elsewhere.example"));

         std::map<String, String> covered;
         int links = 0;
         SURBL::FindLinkHosts(text, false, [&](const String &host)
         {
            links++;
            SenderListCache::CoverLink(entryHosts, host, covered);
            return true;
         });

         String got;
         for (const auto &entry : covered)
         {
            if (!got.IsEmpty())
               got += _T("|");
            got += entry.first + _T("=") + entry.second;
         }

         const String expected = _T("192.0.2.61=192.0.2.61|shop.example=cdn.shop.example");
         if (links != 43 || got != expected)
            Fail_("every link of a message is matched against the URL entries", _T("40 other links first"),
               StringParser::IntToString(links) + _T(" links, ") + got, _T("43 links, ") + expected);
      }

      // The names the URI lists are asked about (ChooseNamesToAsk). Until
      // 6.3.6 the first 16 names in the order of the links, the text part's
      // first: 16 harmless links in a text part an HTML mail program never
      // shows kept the HTML part's listed link from every list. In the 6.3.6
      // tree before its release, 48 drawn from all of them: N made-up domains
      // left a listed one asked about in 48 of N+1 messages. Every domain now,
      // up to MaxDomainsAsked, and a message with more is told so (the score
      // SpamTestSURBL gives it).
      {
         auto numbered = [](const wchar_t *prefix, int count)
         {
            std::vector<String> names;
            for (int i = 1; i <= count; i++)
               names.push_back(String(prefix) + StringParser::IntToString(i) + _T(".example"));
            return names;
         };

         auto has = [](const std::vector<String> &names, const wchar_t *name)
         {
            return std::find(names.begin(), names.end(), String(name)) != names.end();
         };

         std::vector<String> padding = numbered(L"pad", 16);

         // The HTML part's link is asked about first, whatever the text part holds.
         std::vector<String> html(1, String(_T("listed.example")));
         SURBL::Choice choice = SURBL::ChooseNamesToAsk(html, padding, SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 1);
         if (choice.names.size() != 17 || choice.names[0] != _T("listed.example") || !choice.EveryDomainAsked())
            Fail_("a link of the HTML part is asked about before the text part's", _T("16 text links, then one HTML link"),
               Joined_(choice.names), _T("listed.example first, 17 names, every domain"));

         // 16 links before it in one part: it is asked about too.
         std::vector<String> text = padding;
         text.push_back(_T("listed.example"));
         choice = SURBL::ChooseNamesToAsk(std::vector<String>(), text, SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 1);
         if (!has(choice.names, L"listed.example"))
            Fail_("a link after 16 others is asked about", _T("16 links, then the listed one"), Joined_(choice.names), _T("listed.example among them"));

         // A hundred links in the text part and one in the HTML part: all 101
         // are asked about, the HTML part's first. In batch42 48 were.
         std::vector<String> many = numbered(L"many", 100);
         choice = SURBL::ChooseNamesToAsk(html, many, SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 7);
         if (choice.names.size() != 101 || choice.names[0] != _T("listed.example") || choice.domains != 101 || !choice.EveryDomainAsked())
            Fail_("a hundred links in the text part are all asked about, after the HTML part's", _T("100 text links, one HTML link"),
               StringParser::IntToString(static_cast<int>(choice.names.size())) + _T(" names: ") + Joined_(choice.names).Left(80),
               _T("listed.example first, 101 names, every domain"));

         // ANTISPAM-1: 100 made-up domains in the HTML part and then the listed
         // one. The listed one is asked about whatever the seed - before 6.3.6
         // was released, in about 48 of 101 draws - and the choice says every
         // domain was.
         std::vector<String> crowded = numbered(L"pad", 100);
         crowded.push_back(_T("listed.example"));
         for (unsigned int seed = 1; seed <= 64; seed++)
         {
            choice = SURBL::ChooseNamesToAsk(crowded, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, seed);
            if (!has(choice.names, L"listed.example") || choice.names.size() != 101 || choice.domains != 101 || !choice.EveryDomainAsked())
               Fail_("a listed link after a hundred made-up domains is asked about, whatever the seed",
                  _T("100 HTML links, then the listed one, seed ") + StringParser::IntToString(static_cast<int>(seed)),
                  StringParser::IntToString(static_cast<int>(choice.names.size())) + _T(" names") +
                     (has(choice.names, L"listed.example") ? String(_T(", listed.example among them")) : String(_T(", listed.example not among them"))),
                  _T("101 names, listed.example among them, every domain"));
         }

         // MaxDomainsAsked domains are all asked; one more and the choice says
         // the message was not asked about whole. The draw is the seed's, and
         // never more than the budget.
         std::vector<String> full = numbered(L"full", static_cast<int>(SURBL::MaxDomainsAsked));
         choice = SURBL::ChooseNamesToAsk(full, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 3);
         if (choice.names.size() != SURBL::MaxDomainsAsked || !choice.EveryDomainAsked())
            Fail_("as many domains as the budget are all asked about", _T("200 HTML links"),
               StringParser::IntToString(static_cast<int>(choice.names.size())) + (choice.EveryDomainAsked() ? _T(", every domain") : _T(", not every domain")),
               _T("200, every domain"));

         std::vector<String> over = full;
         over.push_back(_T("listed.example"));
         std::set<String> leftOut;
         for (unsigned int seed = 1; seed <= 64; seed++)
         {
            choice = SURBL::ChooseNamesToAsk(over, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, seed);
            if (choice.names.size() != SURBL::MaxDomainsAsked || choice.domains != SURBL::MaxDomainsAsked + 1 ||
                choice.domains_asked != SURBL::MaxDomainsAsked || choice.EveryDomainAsked())
               Fail_("one domain more than the budget: the budget asked, and the message not asked about whole", _T("201 HTML links"),
                  StringParser::IntToString(static_cast<int>(choice.names.size())) + _T(" names of ") +
                     StringParser::IntToString(static_cast<int>(choice.domains)) + _T(" domains"),
                  _T("200 names of 201 domains, not every domain"));
            for (const String &name : over)
            {
               if (!has(choice.names, name.c_str()))
                  leftOut.insert(name);
            }
         }
         // Taken in order, the last would be the one left out every time.
         if (leftOut.size() < 2)
            Fail_("the domains of a message over the budget are drawn, not taken in order", _T("200 links, then the listed one, 64 seeds"),
               StringParser::IntToString(static_cast<int>(leftOut.size())) + _T(" different domains left out"), _T("many"));

         // Over the budget in the HTML part, a domain only the text part names
         // is not asked about - and the message is not asked about whole, which
         // is what scores it. Within the budget it is asked about.
         std::vector<String> textOnly(1, String(_T("listed.example")));
         choice = SURBL::ChooseNamesToAsk(numbered(L"html", static_cast<int>(SURBL::MaxDomainsAsked) - 1), textOnly,
            SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 5);
         if (!has(choice.names, L"listed.example") || !choice.EveryDomainAsked())
            Fail_("a domain only the text part names is asked about while the budget lasts", _T("199 HTML links, one text link"),
               Joined_(choice.names).Right(60), _T("listed.example among them, every domain"));
         choice = SURBL::ChooseNamesToAsk(full, textOnly, SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 5);
         if (has(choice.names, L"listed.example") || choice.EveryDomainAsked())
            Fail_("past the budget the text part's domain is left, and the message is not asked about whole", _T("200 HTML links, one text link"),
               has(choice.names, L"listed.example") ? String(_T("listed.example asked")) : String(_T("not every domain: wrong")), _T("not asked, not every domain"));

         // A domain is one domain however many hosts of a shared-hosting suffix
         // name it: duckdns.org, the ICANN section's domain of every tenant, is
         // asked about once and counted once; and it is a domain of its own,
         // asked about before any host name, however many others the message
         // gives.
         std::vector<String> tenants;
         for (int i = 1; i <= 150; i++)
         {
            tenants.push_back(_T("h.pad") + StringParser::IntToString(i) + _T(".example"));
            tenants.push_back(_T("t") + StringParser::IntToString(i) + _T(".duckdns.org"));
         }
         choice = SURBL::ChooseNamesToAsk(tenants, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 9);
         // 150 pad domains, 150 tenants and duckdns.org: 301 domains, over the
         // budget, so duckdns.org is drawn like any other ...
         if (choice.domains != 301 || choice.EveryDomainAsked() || choice.names.size() > SURBL::MaxNamesAsked)
            Fail_("the ICANN section's domain counts once", _T("150 hosts under made-up domains, 150 tenants of duckdns.org"),
               StringParser::IntToString(static_cast<int>(choice.domains)) + _T(" domains"), _T("301 domains, not every one"));
         // ... and within it, it is always asked, before any host name.
         std::vector<String> fewTenants;
         for (int i = 1; i <= 60; i++)
         {
            fewTenants.push_back(_T("h.pad") + StringParser::IntToString(i) + _T(".example"));
            fewTenants.push_back(_T("t") + StringParser::IntToString(i) + _T(".duckdns.org"));
         }
         for (unsigned int seed = 1; seed <= 16; seed++)
         {
            choice = SURBL::ChooseNamesToAsk(fewTenants, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, seed);
            const std::vector<String>::const_iterator icann = std::find(choice.names.begin(), choice.names.end(), String(_T("duckdns.org")));
            const std::vector<String>::const_iterator host = std::find(choice.names.begin(), choice.names.end(), String(_T("h.pad1.example")));
            if (choice.domains != 121 || !choice.EveryDomainAsked() || icann == choice.names.end() || host == choice.names.end() || icann > host)
               Fail_("the ICANN section's domain is asked about before any host name", _T("60 hosts under made-up domains, 60 tenants of duckdns.org"),
                  StringParser::IntToString(static_cast<int>(choice.domains)) + _T(" domains, ") + Joined_(choice.names).Left(80),
                  _T("121 domains, every one; duckdns.org before h.pad1.example"));
         }

         // The names a message makes the lists ask about are held to
         // MaxNamesAsked whatever it holds: 199 domains with two hosts each
         // are 199 domains asked and 57 host names.
         std::vector<String> hosted;
         for (int i = 1; i <= 199; i++)
         {
            hosted.push_back(_T("a.d") + StringParser::IntToString(i) + _T(".example"));
            hosted.push_back(_T("b.d") + StringParser::IntToString(i) + _T(".example"));
         }
         choice = SURBL::ChooseNamesToAsk(hosted, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 2);
         if (choice.names.size() != SURBL::MaxNamesAsked || choice.domains != 199 || !choice.EveryDomainAsked() ||
             choice.names[198].Find(_T("d")) != 0)
            Fail_("the names asked are held to the ceiling, the domains first", _T("199 domains with two hosts each"),
               StringParser::IntToString(static_cast<int>(choice.names.size())) + _T(" names, ") + choice.names[198],
               _T("256 names, the 199 domains first"));

         // The host names of a message whose domains all fit, when they do not
         // all fit themselves, are drawn too, not taken in the order linked:
         // 128 made-up domains, each linked by a host name, before a listed
         // host name under a domain of its own. Every domain is asked about,
         // so the message scores nothing; taken in order, the 127 places left
         // went to the first 127 made-up host names, and the listed one was
         // never asked about, by any list that lists a host and not its domain.
         std::vector<String> hostPadding;
         for (int i = 1; i <= 128; i++)
            hostPadding.push_back(_T("h.hp") + StringParser::IntToString(i) + _T(".example"));
         hostPadding.push_back(_T("bad.listed.example"));
         int listedHostAsked = 0;
         for (unsigned int seed = 1; seed <= 64; seed++)
         {
            choice = SURBL::ChooseNamesToAsk(hostPadding, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, seed);
            if (choice.names.size() != SURBL::MaxNamesAsked || choice.domains != 129 || !choice.EveryDomainAsked() ||
                !has(choice.names, L"listed.example"))
               Fail_("a message whose domains fit and whose host names do not: every domain, then host names to the ceiling",
                  _T("128 made-up domains with a host name each, then bad.listed.example, seed ") + StringParser::IntToString(static_cast<int>(seed)),
                  StringParser::IntToString(static_cast<int>(choice.names.size())) + _T(" names of ") +
                     StringParser::IntToString(static_cast<int>(choice.domains)) + _T(" domains"),
                  _T("256 names of 129 domains, every domain, listed.example among them"));
            if (has(choice.names, L"bad.listed.example"))
               listedHostAsked++;
         }
         if (listedHostAsked < 32)
            Fail_("the host names of a message whose domains fit are drawn when they do not all fit, not taken in the order linked",
               _T("128 made-up domains with a host name each, then bad.listed.example, 64 seeds"),
               StringParser::IntToString(listedHostAsked) + _T(" draws asked about bad.listed.example"), _T("nearly all of them"));

         // One registrable domain is one domain: its own name, then two of its
         // hosts, however many it has; a domain both parts link to, once.
         std::vector<String> twoHosts;
         twoHosts.push_back(_T("h1.shop.example"));
         twoHosts.push_back(_T("h2.shop.example"));
         choice = SURBL::ChooseNamesToAsk(twoHosts, std::vector<String>(1, String(_T("shop.example"))), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 1);
         if (Joined_(choice.names) != _T("shop.example|h1.shop.example|h2.shop.example") || choice.domains != 1)
            Fail_("one domain's hosts are one domain", _T("h1 and h2.shop.example, and shop.example in the text part"),
               Joined_(choice.names), _T("shop.example|h1.shop.example|h2.shop.example"));

         // More hosts under one domain than are asked about: two of them, drawn,
         // not the first two linked - two made-up hosts linked first kept a
         // listed one under the same domain from every list that lists hosts.
         std::vector<String> hosts;
         for (int i = 1; i <= 10; i++)
            hosts.push_back(_T("h") + StringParser::IntToString(i) + _T(".shop.example"));
         int lastDrawn = 0;
         for (unsigned int seed = 1; seed <= 64; seed++)
         {
            choice = SURBL::ChooseNamesToAsk(hosts, std::vector<String>(1, String(_T("shop.example"))), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, seed);
            const std::vector<String> &chosen = choice.names;
            if (chosen.size() != 3 || chosen[0] != _T("shop.example") || chosen[1] == chosen[2] ||
                std::find(hosts.begin(), hosts.end(), chosen[1]) == hosts.end() || std::find(hosts.begin(), hosts.end(), chosen[2]) == hosts.end())
               Fail_("one domain's hosts are one domain, and two of them are asked about", _T("h1..h10.shop.example, and shop.example in the text part"),
                  Joined_(chosen), _T("shop.example, then two of the ten hosts"));
            if (chosen[1] == _T("h10.shop.example") || chosen[2] == _T("h10.shop.example"))
               lastDrawn++;
         }
         if (lastDrawn < 2 || lastDrawn > 40)
            Fail_("the hosts of a domain are drawn, not taken in order", _T("h1..h10.shop.example, 64 seeds"),
               StringParser::IntToString(lastDrawn) + _T(" draws with h10"), _T("about one in five"));

         // Every domain's first name before any domain's second.
         std::vector<String> two;
         two.push_back(_T("www2.first.example"));
         two.push_back(_T("second.example"));
         choice = SURBL::ChooseNamesToAsk(two, std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 1);
         if (Joined_(choice.names) != _T("first.example|second.example|www2.first.example"))
            Fail_("each domain's own name before any host name", _T("www2.first.example, second.example"),
               Joined_(choice.names), _T("first.example|second.example|www2.first.example"));

         // An address is asked about reversed, as itself a domain.
         choice = SURBL::ChooseNamesToAsk(std::vector<String>(1, String(_T("192.0.2.7"))), std::vector<String>(), SURBL::MaxDomainsAsked, SURBL::MaxNamesAsked, 1);
         if (Joined_(choice.names) != _T("7.2.0.192") || choice.domains != 1)
            Fail_("an address is asked about reversed", _T("192.0.2.7"), Joined_(choice.names), _T("7.2.0.192"));
      }

      // One list asked about every name of a message, LookupsAtOnce at a time
      // (Run): a stand-in for the resolver counts the lookups and how many are
      // in flight together. Until 6.3.6 one at a time, and 48 at most.
      {
         std::shared_ptr<SURBLServer> list(new SURBLServer);
         list->SetDNSHost(_T("uri.list.test"));
         list->SetIsActive(true);

         std::vector<String> names;
         for (int i = 1; i <= static_cast<int>(SURBL::MaxNamesAsked); i++)
            names.push_back(_T("n") + StringParser::IntToString(i) + _T(".example"));

         std::mutex lock;
         std::set<String> asked;
         int inFlight = 0;
         int mostInFlight = 0;
         String listedName;

         SURBL::Lookup lookup = [&](const String &name, std::vector<String> &answers) -> bool
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               asked.insert(name);
               inFlight++;
               mostInFlight = std::max(mostInFlight, inFlight);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            std::lock_guard<std::mutex> guard(lock);
            inFlight--;
            if (!listedName.IsEmpty() && name == listedName + _T(".uri.list.test"))
               answers.push_back(_T("127.0.0.2"));
            return true;
         };

         SURBL surbl;
         const bool clean = surbl.Run(list, names, lookup);
         if (!clean || asked.size() != SURBL::MaxNamesAsked)
            Fail_("every name is asked about the list", _T("256 names, none listed"),
               StringParser::IntToString(static_cast<int>(asked.size())) + (clean ? _T(" asked, clean") : _T(" asked, listed")), _T("256 asked, clean"));
         if (mostInFlight < 2 || mostInFlight > static_cast<int>(SURBL::LookupsAtOnce))
            Fail_("the lookups of one list are made several at a time, and no more than LookupsAtOnce", _T("256 names"),
               StringParser::IntToString(mostInFlight) + _T(" in flight at most"), _T("2 to 16"));

         // The last name listed: found, whatever is asked before it.
         asked.clear();
         mostInFlight = 0;
         listedName = names.back();
         if (surbl.Run(list, names, lookup))
            Fail_("a listing among the last names is found", _T("256 names, the last one listed"),
               _T("clean"), _T("listed"));

         // A lookup that is not answered, or that throws, ends the list for
         // the message, as one lookup at a time did: no more are begun, the
         // message goes on without the list's verdict (fail-open, and told),
         // and the listing among the last names is not reached. The lookups
         // in flight beside it finish; far fewer than all are asked.
         for (int throws = 0; throws < 2; throws++)
         {
            asked.clear();
            const String broken = names[2] + _T(".uri.list.test");
            SURBL::Lookup failing = [&](const String &name, std::vector<String> &answers) -> bool
            {
               if (name == broken)
               {
                  if (throws == 1)
                     throw std::runtime_error("a lookup that throws");
                  return false;
               }

               return lookup(name, answers);
            };

            const bool clean = surbl.Run(list, names, failing);
            if (!clean || asked.size() >= SURBL::MaxNamesAsked / 2)
               Fail_(throws == 1 ? "a lookup that throws ends the list for the message" : "a lookup that fails ends the list for the message",
                  _T("256 names, the third not answered, the last one listed"),
                  StringParser::IntToString(static_cast<int>(asked.size())) + (clean ? _T(" asked, clean") : _T(" asked, listed")),
                  _T("fewer than 128 asked, clean"));
         }
      }

      // A flood: many messages tested at once, each with many names, share
      // one pool (SURBLLookupPool). Its threads stay within its size whatever
      // the number of messages, each message makes its lookups beside them,
      // no more than LookupsAtOnce at once, and every name of every message is
      // asked about - none is starved into its ten seconds. In the 6.3.6 tree
      // before its release each message started fifteen threads of its own:
      // here 12 messages, 180 threads, where the pool allows 4 and the 12
      // testing.
      {
         std::shared_ptr<SURBLServer> list(new SURBLServer);
         list->SetDNSHost(_T("uri.list.test"));
         list->SetIsActive(true);

         const size_t poolThreads = 4;
         const int messages = 12;
         const int namesEach = 128;
         SURBLLookupPool pool(poolThreads, SURBL::LookupsAtOnce - 1, 30 * 1000);

         std::mutex lock;
         std::set<std::thread::id> lookupThreads;
         std::vector<int> asked(messages, 0);
         std::vector<int> inFlight(messages, 0);
         std::vector<int> mostInFlight(messages, 0);
         std::vector<int> clean(messages, -1);
         int allInFlight = 0;
         int mostInFlightAll = 0;

         std::vector<std::thread> testing;
         for (int m = 0; m < messages; m++)
         {
            testing.push_back(std::thread([&, m]()
            {
               std::vector<String> names;
               for (int i = 1; i <= namesEach; i++)
                  names.push_back(_T("m") + StringParser::IntToString(m) + _T("n") + StringParser::IntToString(i) + _T(".example"));

               SURBL::Lookup lookup = [&, m](const String &, std::vector<String> &) -> bool
               {
                  {
                     std::lock_guard<std::mutex> guard(lock);
                     lookupThreads.insert(std::this_thread::get_id());
                     asked[m]++;
                     inFlight[m]++;
                     mostInFlight[m] = std::max(mostInFlight[m], inFlight[m]);
                     allInFlight++;
                     mostInFlightAll = std::max(mostInFlightAll, allInFlight);
                  }

                  std::this_thread::sleep_for(std::chrono::milliseconds(2));

                  std::lock_guard<std::mutex> guard(lock);
                  inFlight[m]--;
                  allInFlight--;
                  return true;
               };

               SURBL surbl;
               const bool result = surbl.Run(list, names, lookup, pool);

               std::lock_guard<std::mutex> guard(lock);
               clean[m] = result ? 1 : 0;
            }));
         }

         for (std::thread &thread : testing)
            thread.join();

         const SURBLLookupPool::Counts counts = pool.GetCounts();
         const String flood = _T("12 messages of 128 names at once, a pool of 4");

         for (int m = 0; m < messages; m++)
         {
            if (asked[m] != namesEach || clean[m] != 1)
               Fail_("every name of every message in a flood is asked about", flood,
                  _T("message ") + StringParser::IntToString(m) + _T(": ") + StringParser::IntToString(asked[m]) + _T(" asked"),
                  _T("128 asked, clean"));

            if (mostInFlight[m] > static_cast<int>(SURBL::LookupsAtOnce))
               Fail_("no message in a flood makes more than LookupsAtOnce lookups at once", flood,
                  StringParser::IntToString(mostInFlight[m]) + _T(" at once"), _T("16 at most"));
         }

         if (counts.most_running > poolThreads || lookupThreads.size() > static_cast<size_t>(messages) + poolThreads ||
             mostInFlightAll > messages + static_cast<int>(poolThreads))
            Fail_("the threads that make a flood's lookups are the messages' own and the pool's, held to its size", flood,
               StringParser::IntToString(static_cast<int>(lookupThreads.size())) + _T(" threads made lookups, ") +
                  StringParser::IntToString(static_cast<int>(counts.most_running)) + _T(" of the pool at once, ") +
                  StringParser::IntToString(mostInFlightAll) + _T(" lookups at once"),
               _T("16 threads, 4 of the pool, 16 lookups at most"));

         if (lookupThreads.size() <= static_cast<size_t>(messages))
            Fail_("the pool's threads make lookups beside the messages' own", flood,
               StringParser::IntToString(static_cast<int>(lookupThreads.size())) + _T(" threads made lookups"), _T("more than 12"));
      }

      // A message that came first with many names does not keep the pool: a
      // thread of it makes one lookup and turns to the next message in turn,
      // so a message tested after it has its share of the pool at once, not
      // once the first is done. Here the first has all eight of a pool's
      // threads; the second, while the first still has names not yet asked
      // about - more than fifty of its 256 - has at least three lookups at
      // once, its own and the pool's. Taken first come, first served, the
      // pool's threads stayed with the first until its last name was taken,
      // and the second had its own thread alone until then.
      {
         std::shared_ptr<SURBLServer> list(new SURBLServer);
         list->SetDNSHost(_T("uri.list.test"));
         list->SetIsActive(true);

         SURBLLookupPool pool(8, SURBL::LookupsAtOnce - 1, 30 * 1000);

         std::vector<String> firstNames;
         std::vector<String> secondNames;
         for (int i = 1; i <= 256; i++)
            firstNames.push_back(_T("first") + StringParser::IntToString(i) + _T(".example"));
         for (int i = 1; i <= 32; i++)
            secondNames.push_back(_T("second") + StringParser::IntToString(i) + _T(".example"));

         std::mutex lock;
         std::condition_variable changed;
         bool firstDone = false;
         int firstInFlight = 0;
         int firstMost = 0;
         int firstBegun = 0;
         int secondInFlight = 0;
         int secondMostWhileFirst = 0;

         SURBL::Lookup firstLookup = [&](const String &, std::vector<String> &) -> bool
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               firstInFlight++;
               firstBegun++;
               firstMost = std::max(firstMost, firstInFlight);
               changed.notify_all();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            std::lock_guard<std::mutex> guard(lock);
            firstInFlight--;
            return true;
         };

         SURBL::Lookup secondLookup = [&](const String &, std::vector<String> &) -> bool
         {
            {
               std::lock_guard<std::mutex> guard(lock);
               secondInFlight++;
               if (firstBegun < 200)
                  secondMostWhileFirst = std::max(secondMostWhileFirst, secondInFlight);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            std::lock_guard<std::mutex> guard(lock);
            secondInFlight--;
            return true;
         };

         bool firstClean = false;
         std::thread first([&]()
         {
            SURBL surbl;
            const bool result = surbl.Run(list, firstNames, firstLookup, pool);

            std::lock_guard<std::mutex> guard(lock);
            firstClean = result;
            firstDone = true;
         });

         bool hadPool = false;
         {
            std::unique_lock<std::mutex> guard(lock);
            hadPool = changed.wait_for(guard, std::chrono::seconds(10), [&]() { return firstMost >= 9 || firstDone; });
         }

         SURBL surbl;
         const bool secondClean = surbl.Run(list, secondNames, secondLookup, pool);
         first.join();

         if (!hadPool || firstMost != 9 || !firstClean || !secondClean)
            Fail_("a message tested alone has the pool's threads beside its own", _T("256 names, a pool of 8"),
               StringParser::IntToString(firstMost) + _T(" lookups at once"), _T("9"));

         if (secondMostWhileFirst < 3)
            Fail_("a message tested after one that has the whole pool has its share of it at once", _T("256 names, then 32, a pool of 8"),
               StringParser::IntToString(secondMostWhileFirst) + _T(" lookups at once while the first had names left"), _T("3 or more"));
      }

      // A pool with no thread to give - none can be had - leaves the message's
      // own thread to ask about every name, one at a time.
      {
         std::shared_ptr<SURBLServer> list(new SURBLServer);
         list->SetDNSHost(_T("uri.list.test"));
         list->SetIsActive(true);

         SURBLLookupPool pool(0, SURBL::LookupsAtOnce - 1, 1000);

         std::vector<String> names;
         for (int i = 1; i <= 40; i++)
            names.push_back(_T("alone") + StringParser::IntToString(i) + _T(".example"));

         std::mutex lock;
         int asked = 0;
         int inFlight = 0;
         int mostInFlight = 0;
         SURBL::Lookup lookup = [&](const String &, std::vector<String> &) -> bool
         {
            std::lock_guard<std::mutex> guard(lock);
            asked++;
            inFlight++;
            mostInFlight = std::max(mostInFlight, inFlight);
            inFlight--;
            return true;
         };

         SURBL surbl;
         const bool clean = surbl.Run(list, names, lookup, pool);
         if (!clean || asked != 40 || mostInFlight != 1 || pool.GetCounts().started != 0)
            Fail_("a message whose pool has no thread to give asks about every name on its own thread", _T("40 names, a pool of none"),
               StringParser::IntToString(asked) + _T(" asked, ") + StringParser::IntToString(mostInFlight) + _T(" at once"),
               _T("40 asked, 1 at once, clean"));
      }
   }
}
