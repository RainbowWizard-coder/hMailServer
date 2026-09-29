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
#include <boost/regex.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int maxURLsToProcess = 15;

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

      struct NamedReference
      {
         const wchar_t *name;
         wchar_t character;
      };

      // Every named character reference HTML defines for an ASCII character -
      // the 45 written with their semicolon and the 8 legacy ones without it,
      // the 53 of html.spec.whatwg.org/entities.json - and the two for the
      // no-break space, which ends a link in text. No named reference stands
      // for an ASCII letter, digit or hyphen; a numeric one can stand for any.
      const NamedReference namedReferences[] =
      {
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
      };

      // Reads the character reference at text[position] ('&'), as the HTML
      // tokenizer does: numeric, decimal or hexadecimal, with or without its
      // semicolon, and the named ones above. True, with the character and the
      // reference's length, when there is one.
      bool ReadCharacterReference_(const std::wstring &text, size_t position, wchar_t &character, size_t &length)
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
            // are U+FFFD here. So is a code point outside the Basic
            // Multilingual Plane, which is never ASCII, never a space, and
            // would be a surrogate pair on Windows.
            if (value == 0 || value > 0xFFFF || (value >= 0xD800 && value <= 0xDFFF) || (value >= 0x80 && value <= 0x9F))
               character = 0xFFFD;
            else
               character = static_cast<wchar_t>(value);

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
            wchar_t character = 0;
            size_t length = 0;
            if (html[i] == L'&' && ReadCharacterReference_(html, i, character, length))
            {
               text += character;
               fromReference.push_back(true);
               i += length - 1;
               continue;
            }

            text += html[i];
            fromReference.push_back(false);
         }
      }

      // %XX read as its byte. A byte outside ASCII is U+FFFD: this server asks
      // the lists about ASCII names only, and an internationalised name is not
      // turned into its xn-- form.
      std::wstring PercentDecoded_(const std::wstring &value)
      {
         std::wstring decoded;
         decoded.reserve(value.size());

         for (size_t i = 0; i < value.size(); i++)
         {
            if (value[i] == L'%' && i + 2 < value.size())
            {
               const int high = HexValue_(value[i + 1]);
               const int low = HexValue_(value[i + 2]);
               if (high >= 0 && low >= 0)
               {
                  const int byte = high * 16 + low;
                  decoded += byte < 0x80 ? static_cast<wchar_t>(byte) : static_cast<wchar_t>(0xFFFD);
                  i += 2;
                  continue;
               }
            }

            decoded += value[i];
         }

         return decoded;
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

      // The hosts one link's host part hands over (SURBL::FindLinkHosts).
      void HostsOfHostPart_(const std::wstring &hostPart, std::vector<String> &hosts)
      {
         std::wstring host = PercentDecoded_(hostPart);
         for (size_t i = 0; i < host.size(); i++)
            if (host[i] >= L'A' && host[i] <= L'Z')
               host[i] = static_cast<wchar_t>(host[i] + (L'a' - L'A'));

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

      // The hosts one link's authority hands over.
      void HostsOfAuthority_(const std::wstring &authority, std::vector<String> &hosts)
      {
         // The userinfo runs to the last '@' (RFC 3986 section 3.2.1, and the
         // WHATWG authority state, which takes every '@' before the host into
         // the userinfo). "%40" is an '@' in the userinfo's text, not its end.
         const size_t at = authority.rfind(L'@');
         const std::wstring hostAndPort = at == std::wstring::npos ? authority : authority.substr(at + 1);

         // An IPv6 address, or RFC 3986's IPvFuture. No URI list publishes
         // either, so neither is asked about.
         if (hostAndPort.empty() || hostAndPort[0] == L'[')
            return;

         const size_t colon = hostAndPort.find(L':');
         HostsOfHostPart_(colon == std::wstring::npos ? hostAndPort : hostAndPort.substr(0, colon), hosts);
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
      void HostsInTheUserinfo_(const std::wstring &authority, std::vector<String> &hosts)
      {
         const size_t lastAt = authority.rfind(L'@');
         if (lastAt == std::wstring::npos)
            return;

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
               if (std::find(hosts.begin(), hosts.end(), host) == hosts.end())
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
   }

   SURBL::SURBL(void)
   {

   }

   SURBL::~SURBL(void)
   {
   }

   void
   SURBL::FindLinkHosts(const String &text, bool html, const std::function<bool(const String &host)> &visit)
   {
      // Until 6.3.6 a regular expression took the characters after "://" that
      // could be in a name, so http://anything@evil.example/ was looked up as
      // "anything", and evil.example never was: a spammer escaped every list by
      // writing a name and an '@' before the host. The separator is still
      // found by a regular expression; what follows it is read as a URL.
      std::wstring decoded;
      std::vector<bool> fromReference;
      if (html)
         DecodeCharacterReferences_(text, decoded, fromReference);

      const std::wstring &source = html ? decoded : static_cast<const std::wstring &>(text);

      // A link written into another's query has its separator percent-encoded,
      // and a redirector that decodes it reads it as a browser reads any
      // link: http%3A%5C%5C, http%3A%2F%2F%2F and http%3A are http:// too.
      const boost::wregex separator(L"https?:[\\\\/]*|https?%3a(?:%2f|%5c)*|%3a%2f%2f(?:%2f|%5c)*|://", boost::wregex::icase);

      boost::wsmatch match;
      std::wstring::const_iterator from = source.begin();
      boost::match_flag_type flags = boost::match_default;

      while (boost::regex_search(from, source.end(), match, separator, flags))
      {
         const size_t matchStart = static_cast<size_t>(match[0].first - source.begin());
         const size_t separatorEnd = static_cast<size_t>(match[0].second - source.begin());
         const bool encoded = match.str(0).find(L'%') != std::wstring::npos;

         // The quote the link was opened by, when a quote came just before it.
         // In HTML it is an attribute's value when an '=' comes before the
         // quote: the value runs to the closing quote whatever is inside it.
         // The URL parser drops the spaces and control characters a value
         // begins with, so in HTML they may come between the quote and the link,
         // written as themselves or as references (&#32;, &#9;): the tokenizer
         // has made them the value's characters either way.
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

         std::wstring authority;
         const size_t stopped = ReadAuthority_(source, fromReference, separatorEnd, quote, attribute, encoded, authority);

         if (encoded)
            authority = DecodedEncodedAuthority_(authority);

         std::vector<String> hosts;
         HostsOfAuthority_(authority, hosts);
         if (!attribute && !encoded)
            HostsInTheUserinfo_(authority, hosts);

         for (const String &host : hosts)
            if (!visit(host))
               return;

         // The next separator is looked for from the end of this one, so that
         // "http://a.example;http://b.example/" finds both. After a long
         // authority it is looked for from where the reading stopped, so that
         // the text is read in time proportional to its length - less the six
         // characters of "https:": a reading ends at a slash or a backslash,
         // and one that took in the scheme of the link after it
         // ("http://<300 characters>!http://evil.example/") ended at that
         // link's own slashes, so the link starts just before the stop.
         const size_t longestSchemeBeforeItsSlash = 6;
         const size_t resume = stopped - separatorEnd > 256 ? stopped - longestSchemeBeforeItsSlash : separatorEnd;
         if (resume >= source.size())
            return;

         from = source.begin() + static_cast<std::ptrdiff_t>(resume);
         flags = boost::match_default | boost::match_prev_avail;
      }
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

      return names;
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

   bool
   SURBL::Collect_(std::shared_ptr<MessageData> pMessageData, bool namesToAsk, std::vector<String> &out)
   {
      std::set<String> collected;
      bool full = false;

      auto visit = [&](const String &host) -> bool
      {
         if (IsDocumentTypeHost_(host))
            return true;

         const std::vector<String> names = namesToAsk ? NamesToAsk(host) : std::vector<String>(1, host);
         for (const String &name : names)
         {
            if (collected.find(name) != collected.end())
               continue;

            // LOG_DEBUG is an if of its own, so each is in braces.
            if (namesToAsk && name == host)
            {
               LOG_DEBUG(Formatter::Format(_T("SURBL: Found URL: {0}"), name));
            }
            else if (namesToAsk)
            {
               LOG_DEBUG(Formatter::Format(_T("SURBL: Found URL: {0}, asked about as {1}"), host, name));
            }

            collected.insert(name);
            out.push_back(name);

            if (collected.size() > maxURLsToProcess)
            {
               full = true;
               return false;
            }
         }

         return true;
      };

      try
      {
         // The text part as text, and the HTML part as HTML, in that order.
         FindLinkHosts(pMessageData->GetBody(), false, visit);
         if (!full)
            FindLinkHosts(pMessageData->GetHTMLBody(), true, visit);
      }
      catch (std::runtime_error &err) // regex_search throws runtime_error if the expression is too complex.
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5701, "SURBL::Run", "Parsing HTML body with regular expression threw a runtime_error", err);
      }

      return !out.empty();
   }

   bool
   SURBL::ExtractUrls(std::shared_ptr<MessageData> pMessageData, std::vector<String> &vecUrls)
   {
      LOG_DEBUG("SURBL: Execute");

      Collect_(pMessageData, true, vecUrls);

      if (vecUrls.empty())
      {
         LOG_DEBUG("SURBL: No domain addresses found.");
         return false;
      }

      // The cast names FormatArgument's unsigned __int64 constructor, which is the
      // one a size_t binds to exactly on Windows. On a 64-bit POSIX build size_t is
      // unsigned long, which matches none of the constructors exactly and is no
      // closer to one than to the others. The cast is an identity on Windows.
      String logMessage = Formatter::Format("SURBL: {0} unique domain addresses found.", (unsigned __int64) vecUrls.size());
      LOG_DEBUG(logMessage);

      return true;
   }

   bool
   SURBL::ExtractHosts(std::shared_ptr<MessageData> pMessageData, std::vector<String> &hosts)
   {
      return Collect_(pMessageData, false, hosts);
   }

   bool
   SURBL::Run(std::shared_ptr<SURBLServer> pSURBLServer, std::vector<String> &vecUrls)
   {
      boost::chrono::system_clock::time_point start_time = boost::chrono::system_clock::now();

      // 127.0.0.1 is SURBL's and URIBL's answer to a query they block, and a
      // list that blocks this server answers it to every name. It does not end
      // the list for the message, as a refusal does: Hostkarma answers it to a
      // name on its whitelist, and a link to one would then switch the list off
      // for every link after it. The operator is told when every name the list
      // answered for a message was answered so.
      int lookupsAnswered = 0;
      int lookupsBlocked = 0;
      auto reportIfEveryLookupWasBlocked = [&]()
      {
         if (lookupsBlocked == 0 || lookupsBlocked != lookupsAnswered)
            return;

         AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
            Formatter::Format("SURBL: {0} answered 127.0.0.1 to every name of a message it was asked about ({1}). SURBL and URIBL answer that to a query they block: if the list is one of them, it cannot be checked while it blocks this server's queries, and messages are accepted without its verdict.",
               pSURBLServer->GetDNSHost(), lookupsBlocked));
      };

      int processedAddresses = 0;
      for (auto sURL : vecUrls)
      {
         boost::chrono::duration<double> elapsed_seconds = boost::chrono::system_clock::now() - start_time;

         if (elapsed_seconds.count() > 10)
         {
            LOG_DEBUG("SURBL: Aborting. Too long time elapsed.");
            reportIfEveryLookupWasBlocked();
            return true;
         }

         if (processedAddresses > maxURLsToProcess)
         {
            LOG_DEBUG("SURBL: Aborting. Too many urls.");
            reportIfEveryLookupWasBlocked();
            return true;
         }

         String sHostToLookup = sURL + "." + pSURBLServer->GetDNSHost();

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
            processedAddresses++;
            continue;
         }

         LOG_DEBUG(Formatter::Format(_T("SURBL: Lookup: {0}"), sHostToLookup));

         std::vector<String> saFoundNames;
         DNSResolver resolver;
         if (!resolver.GetIpAddresses(sHostToLookup, saFoundNames, false))
         {
            LOG_DEBUG("SURBL: DNS query failed.");

            // Abandoning the whole list on the first failed lookup means the URLs
            // after it are never checked either, and the message is reported as
            // clean. That is the safe direction to fail, but it was completely
            // silent, so a resolver that had stopped answering presented as a
            // SURBL that never matches anything.
            AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
               Formatter::Format("SURBL: The lookup of {0} did not complete. URI blacklists cannot be checked while that continues, and affected messages are accepted without a SURBL verdict.", sHostToLookup));

            return true;
         }

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
         bool listed = false;
         bool blocked = false;
         String errorAnswer;
         String errorMeaning;
         String refusedAnswer;
         String refusedMeaning;
         const std::set<String> expected = BLCheck::ExpandAddresses(pSURBLServer->GetExpectedResult());
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

         if (listed)
         {
            LOG_DEBUG("SURBL: Match found");
            return false;
         }

         // A list that refuses this server's queries (Spamhaus's
         // 127.255.255.0/24) refuses the next one too. It is not asked again
         // for this message, and the operator is told, as for a lookup that
         // failed: its silence would look like clean mail.
         if (!refusedAnswer.IsEmpty())
         {
            AntiSpamDiagnostics::ReportCheckIncomplete(AntiSpamDiagnostics::CheckSurbl,
               Formatter::Format("SURBL: {0} refused the query for {1}: it answered {2}, {3}. The list cannot be checked while it refuses this server's queries, and messages are accepted without its verdict.",
                  pSURBLServer->GetDNSHost(), sURL, refusedAnswer, refusedMeaning));

            return true;
         }

         lookupsAnswered++;
         if (blocked)
            lookupsBlocked++;

         processedAddresses++;
      }

      reportIfEveryLookupWasBlocked();

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
         { L"tenant.github.io", L"tenant.github.io", "a name under a PRIVATE suffix" },
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
   }
}
