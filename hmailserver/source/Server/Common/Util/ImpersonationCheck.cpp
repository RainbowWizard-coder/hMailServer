// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ImpersonationCheck.h"

#include "DirectoryStore.h"
#include "Parsing/StringParser.h"
#include "Unicode.h"

#include "../Application/Application.h"
#include "../Application/ErrorManager.h"
#include "../Application/EffectiveSetting.h"
#include "../Application/Logger.h"
#include "../Application/ObjectCache.h"
#include "../AntiSpam/DMARC/DMARC.h"
#include "../AntiSpam/QuarantineStore.h"
#include "../AntiSpam/SenderListCache.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/DomainAlias.h"
#include "../BO/DomainAliases.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

#ifndef HM_PLATFORM_POSIX
// NormalizeString (NFKC) lives in Normaliz.lib, as it does for SASLprep.
#pragma comment(lib, "Normaliz.lib")
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // What one domain's judgement reads: its people (accounts with their
   // aliases, and the protected persons), folded once, and the hosted and
   // partner names prepared once.
   struct ImpersonationCheck::Facts
   {
      time_t made;
      std::vector<Person> people;
      std::vector<Name> names;
   };

   namespace
   {
      // THE CONFUSABLE TABLE, single characters: what a reader's eye takes for a
      // Latin letter, mapped to that letter. Both cases of the Cyrillic letters,
      // because the lower-casing below is the C library's and need not know
      // Cyrillic; the Greek capitals the roadmap names, and the two Greek
      // lower-case letters that look like Latin ones. Latin capital I and
      // Greek capital iota go to l, not i, because in the sans-serif faces mail
      // is read in the two are one glyph - and the mapping runs before the
      // lower-casing, which is why "Ian" and "lan" fold alike.
      struct Confusable { wchar_t from; wchar_t to; };

      const Confusable Confusables[] =
      {
         { L'1', L'l' }, { L'|', L'l' }, { L'I', L'l' }, { L'0', L'o' },
         // Cyrillic, lower case: а е о р с х у і ј ѕ һ ԁ
         { 0x0430, L'a' }, { 0x0435, L'e' }, { 0x043E, L'o' }, { 0x0440, L'p' }, { 0x0441, L'c' },
         { 0x0445, L'x' }, { 0x0443, L'y' }, { 0x0456, L'i' }, { 0x0458, L'j' }, { 0x0455, L's' },
         { 0x04BB, L'h' }, { 0x0501, L'd' },
         // Cyrillic, upper case: А Е О Р С Х У І Ј Ѕ Һ Ԁ
         { 0x0410, L'a' }, { 0x0415, L'e' }, { 0x041E, L'o' }, { 0x0420, L'p' }, { 0x0421, L'c' },
         { 0x0425, L'x' }, { 0x0423, L'y' }, { 0x0406, L'i' }, { 0x0408, L'j' }, { 0x0405, L's' },
         { 0x04BA, L'h' }, { 0x0500, L'd' },
         // Cyrillic capitals whose lower case does not look Latin but which
         // do: Т Н В М К - and the palochka Ӏ ӏ, Ԛ ԛ, Ԝ ԝ, Ѵ ѵ in both cases.
         { 0x0422, L't' }, { 0x041D, L'h' }, { 0x0412, L'b' }, { 0x041C, L'm' }, { 0x041A, L'k' },
         { 0x04C0, L'l' }, { 0x04CF, L'l' }, { 0x051A, L'q' }, { 0x051B, L'q' }, { 0x051C, L'w' },
         { 0x051D, L'w' }, { 0x0474, L'v' }, { 0x0475, L'v' },
         // Latin small letter script g
         { 0x0261, L'g' },
         // Greek capitals: Α Β Ε Ζ Η Ι Κ Μ Ν Ο Ρ Τ Υ Χ
         { 0x0391, L'a' }, { 0x0392, L'b' }, { 0x0395, L'e' }, { 0x0396, L'z' }, { 0x0397, L'h' },
         { 0x0399, L'l' }, { 0x039A, L'k' }, { 0x039C, L'm' }, { 0x039D, L'n' }, { 0x039F, L'o' },
         { 0x03A1, L'p' }, { 0x03A4, L't' }, { 0x03A5, L'y' }, { 0x03A7, L'x' },
         // Greek lower case: ο ι
         { 0x03BF, L'o' }, { 0x03B9, L'i' }
      };

      wchar_t FoldOne(wchar_t c)
      {
         for (size_t i = 0; i < sizeof(Confusables) / sizeof(Confusables[0]); i++)
            if (Confusables[i].from == c)
               return Confusables[i].to;

         return c;
      }

      // The format characters no client draws: a zero-width space or joiner
      // inside a word, a soft hyphen, a bidi control, a byte order mark. They
      // are deleted before anything else looks at the text, because a name
      // with one of them in it is rendered exactly as the name without it.
      bool IsInvisible(wchar_t c)
      {
         return c == 0x00AD || c == 0x034F || (c >= 0x200B && c <= 0x200F) ||
                (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
      }

      String WithoutInvisibles(const String &text)
      {
         String result;

         for (int i = 0; i < text.GetLength(); i++)
            if (!IsInvisible(text.GetAt(i)))
               result += text.GetAt(i);

         return result;
      }

      // Unicode NFKC where the platform has it. On Windows this is the same
      // NormalizeString call SASLprep makes; the Linux build has no Unicode
      // tables, so there the compatibility step is skipped and the doc says so.
      // Text the normaliser refuses - an unpaired surrogate - is folded as it is.
      String NormalizeKC(const String &text)
      {
#ifdef HM_PLATFORM_POSIX
         return text;
#else
         if (text.IsEmpty())
            return text;

         bool ascii = true;
         for (int i = 0; i < text.GetLength(); i++)
         {
            if ((unsigned) text.GetAt(i) >= 0x80)
            {
               ascii = false;
               break;
            }
         }

         // Every ASCII code point is already in NFKC form.
         if (ascii)
            return text;

         int needed = NormalizeString(NormalizationKC, text.c_str(), text.GetLength(), NULL, 0);
         if (needed <= 0)
            return text;

         std::wstring buffer;
         buffer.resize((size_t) needed);
         int written = NormalizeString(NormalizationKC, text.c_str(), text.GetLength(), &buffer[0], needed);
         if (written < 0)
         {
            buffer.resize((size_t) (-written));
            written = NormalizeString(NormalizationKC, text.c_str(), text.GetLength(), &buffer[0], (int) buffer.size());
         }

         if (written <= 0)
            return text;

         buffer.resize((size_t) written);
         return String(buffer.c_str());
#endif
      }

      // White space and punctuation, for the collapse: the ASCII characters
      // that are neither letter nor digit, the Unicode spaces, and the general
      // punctuation block a name is decorated with.
      bool IsSeparatorOrPunctuation(wchar_t c)
      {
         if (c < 0x80)
            return !((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'));

         if (c == 0x00A0 || c == 0x00B7 || c == 0x00AB || c == 0x00BB || c == 0x3000)
            return true;

         if (c >= 0x2000 && c <= 0x206F)
            return true;

         return false;
      }

      // rn to m and vv to w, on text that is already lower case.
      String FoldPairs(const String &text)
      {
         String result;

         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text.GetAt(i);
            const wchar_t next = i + 1 < text.GetLength() ? text.GetAt(i + 1) : 0;

            if (c == L'r' && next == L'n')
            {
               result += L'm';
               i++;
            }
            else if (c == L'v' && next == L'v')
            {
               result += L'w';
               i++;
            }
            else
               result += c;
         }

         return result;
      }

      String Lower(const String &text)
      {
         String result = text;
         result.ToLower();
         return result;
      }

      String Trimmed(const String &text)
      {
         String result = text;
         result.Trim();
         return result;
      }

      // The display name of a From header value that GetUnicodeFieldValue has
      // already decoded: the text before <, without its quotes, or the comment
      // of the old addr (Name) form, or nothing.
      String DisplayNameOf(const String &fromHeader)
      {
         String value = Trimmed(fromHeader);

         const int bracket = value.Find(_T("<"));
         String name;

         if (bracket > 0)
            name = value.Mid(0, bracket);
         else if (bracket < 0)
         {
            const int open = value.Find(_T("("));
            const int close = open >= 0 ? value.Find(_T(")"), open) : -1;
            if (open >= 0 && close > open)
               name = value.Mid(open + 1, close - open - 1);
         }

         name.Trim();

         if (name.GetLength() >= 2 && name.GetAt(0) == L'"' && name.GetAt(name.GetLength() - 1) == L'"')
            name = name.Mid(1, name.GetLength() - 2);

         name.Replace(_T("\\\""), _T("\""));
         name.Replace(_T("\\\\"), _T("\\"));
         name.Trim();

         return name;
      }

      std::vector<String> Labels(const String &domain)
      {
         std::vector<String> labels;
         String current;

         for (int i = 0; i <= domain.GetLength(); i++)
         {
            const wchar_t c = i < domain.GetLength() ? domain.GetAt(i) : L'.';

            if (c == L'.')
            {
               labels.push_back(current);
               current = _T("");
            }
            else
               current += c;
         }

         return labels;
      }

      bool Contains(const std::vector<String> &names, const String &name)
      {
         return std::find(names.begin(), names.end(), name) != names.end();
      }

      // The trailing dot a fully qualified name may carry, and nothing else.
      String WithoutTrailingDot(const String &domain)
      {
         String result = domain;
         while (result.GetLength() > 1 && result.EndsWith(_T(".")))
            result = result.Left(result.GetLength() - 1);
         return result;
      }

      // Exactly one of the names, or beneath one, both sides IDNA decoded.
      bool IsOwnOrBeneath(const String &idna, const std::vector<ImpersonationCheck::Name> &names)
      {
         for (size_t i = 0; i < names.size(); i++)
         {
            if (idna == names[i].idna)
               return true;

            if (idna.GetLength() > names[i].idna.GetLength() + 1 && idna.EndsWith(_T(".") + names[i].idna))
               return true;
         }

         return false;
      }

      bool IsListSeparator(wchar_t c)
      {
         return c == L',' || c == L' ' || c == L';' || c == L'\t' || c == L'\r' || c == L'\n';
      }

      String StripQuotes(const String &text)
      {
         String name = Trimmed(text);

         if (name.GetLength() >= 2 && name.GetAt(0) == L'"' && name.GetAt(name.GetLength() - 1) == L'"')
            name = Trimmed(name.Mid(1, name.GetLength() - 2));

         return name;
      }

      // A name is written back in quotes when it holds a comma, so that the
      // stored text reads back as one entry.
      String Quoted(const String &name)
      {
         if (name.Find(_T(",")) >= 0)
            return _T("\"") + name + _T("\"");

         return name;
      }

      // One entry of the persons list, as typed or as stored: "Name <address>",
      // "Name" or "address". False with the refusal when the address is not an
      // address, a quote is inside the name, or the angle brackets are odd.
      bool ParseEntry(const String &entry, ImpersonationCheck::Person &person, String &canonical, String &refusal)
      {
         person = ImpersonationCheck::Person();

         const int open = entry.Find(_T("<"));

         if (open >= 0)
         {
            const int close = entry.Find(_T(">"), open);
            if (close < 0)
            {
               refusal = _T("protected_persons: ") + entry + _T(" opens an address with < and never closes it.");
               return false;
            }

            const String address = Lower(Trimmed(entry.Mid(open + 1, close - open - 1)));
            const String name = StripQuotes(entry.Mid(0, open));

            if (!StringParser::IsValidEmailAddress(address))
            {
               refusal = _T("protected_persons: ") + address + _T(" is not an address.");
               return false;
            }

            if (name.Find(_T("\"")) >= 0)
            {
               refusal = _T("protected_persons: ") + entry + _T(" has a quote inside the name.");
               return false;
            }

            person.name = name;
            person.addresses.push_back(address);
            person.owner = address;
            canonical = name.IsEmpty() ? address : Quoted(name) + _T(" <") + address + _T(">");
            return true;
         }

         if (entry.Find(_T(">")) >= 0)
         {
            refusal = _T("protected_persons: ") + entry + _T(" closes an address with > that was never opened.");
            return false;
         }

         if (entry.GetAt(0) != L'"' && entry.Find(_T("@")) >= 0)
         {
            const String address = Lower(entry);

            if (!StringParser::IsValidEmailAddress(address))
            {
               refusal = _T("protected_persons: ") + entry + _T(" is not an address. Write a name, an address, or Name <address>.");
               return false;
            }

            person.addresses.push_back(address);
            person.owner = address;
            canonical = address;
            return true;
         }

         const String name = StripQuotes(entry);

         if (name.IsEmpty())
         {
            refusal = _T("protected_persons: ") + entry + _T(" is an empty name.");
            return false;
         }

         if (name.Find(_T("\"")) >= 0)
         {
            refusal = _T("protected_persons: ") + entry + _T(" has a quote inside the name.");
            return false;
         }

         person.name = name;
         person.owner = _T("a protected person");
         canonical = Quoted(name);
         return true;
      }

      // The persons text cut into entries: at a line break, and at a comma
      // that is not inside quotes. False with the refusal for a quote that
      // is not closed on its line.
      bool SplitEntries(const String &text, std::vector<String> &entries, String &refusal)
      {
         String current;
         bool inQuotes = false;

         for (int i = 0; i <= text.GetLength(); i++)
         {
            const wchar_t c = i < text.GetLength() ? text.GetAt(i) : L'\n';

            if (c == L'"')
            {
               inQuotes = !inQuotes;
               current += c;
               continue;
            }

            if (c == L'\n' && inQuotes)
            {
               refusal = _T("protected_persons: ") + Trimmed(current) + _T(" has an unbalanced quote.");
               return false;
            }

            if (c == L'\n' || (c == L',' && !inQuotes))
            {
               current.Trim();
               if (!current.IsEmpty())
                  entries.push_back(current);
               current = _T("");
               continue;
            }

            current += c;
         }

         return true;
      }

      // The held facts, by domain id.
      boost::mutex facts_mutex_;
      std::map<__int64, std::shared_ptr<const ImpersonationCheck::Facts> > facts_;
   }

   // ------------------------------------------------------------- the folding

   String
   ImpersonationCheck::FoldConfusables(const String &text)
   {
      String result;

      for (int i = 0; i < text.GetLength(); i++)
         result += FoldOne(text.GetAt(i));

      return FoldPairs(result);
   }

   String
   ImpersonationCheck::FoldName(const String &name)
   {
      // NFKC first, so that a full-width or ligature spelling becomes the
      // letters it stands for; the invisible format characters deleted; then
      // the single confusables, before the lower-casing so that a capital I
      // folds to l; then the case; then every run of white space and
      // punctuation to one space; then the pairs.
      String text = WithoutInvisibles(NormalizeKC(name));

      String single;
      for (int i = 0; i < text.GetLength(); i++)
         single += FoldOne(text.GetAt(i));

      single.ToLower();

      String collapsed;
      bool inSeparator = false;

      for (int i = 0; i < single.GetLength(); i++)
      {
         const wchar_t c = single.GetAt(i);

         if (IsSeparatorOrPunctuation(c))
         {
            inSeparator = true;
            continue;
         }

         if (inSeparator && !collapsed.IsEmpty())
            collapsed += L' ';

         inSeparator = false;
         collapsed += c;
      }

      return FoldPairs(collapsed);
   }

   String
   ImpersonationCheck::IdnaOf(const String &domain)
   {
      String lower = Lower(WithoutTrailingDot(Trimmed(domain)));

      std::vector<String> labels = Labels(lower);
      String result;

      for (size_t i = 0; i < labels.size(); i++)
      {
         String label = labels[i];

         if (label.StartsWith(_T("xn--")))
         {
            String decoded;
            if (DecodePunycode(AnsiString(label.Mid(4)), decoded))
               label = Lower(decoded);
         }

         if (i > 0)
            result += L'.';

         result += label;
      }

      return result;
   }

   String
   ImpersonationCheck::FoldDomain(const String &domain)
   {
      std::vector<String> labels = Labels(IdnaOf(domain));
      String result;

      for (size_t i = 0; i < labels.size(); i++)
      {
         const String label = WithoutInvisibles(NormalizeKC(labels[i]));

         String single;
         for (int j = 0; j < label.GetLength(); j++)
            single += FoldOne(label.GetAt(j));

         single.ToLower();

         if (i > 0)
            result += L'.';

         result += FoldPairs(single);
      }

      return result;
   }

   int
   ImpersonationCheck::EditDistance(const String &a, const String &b)
   {
      const int n = a.GetLength();
      const int m = b.GetLength();

      // Lengths two apart are two edits apart at least, and that is all the
      // caller asks.
      if (n - m > 1 || m - n > 1)
         return n > m ? n - m : m - n;

      std::vector<std::vector<int> > d((size_t) n + 1, std::vector<int>((size_t) m + 1, 0));

      for (int i = 0; i <= n; i++)
         d[i][0] = i;
      for (int j = 0; j <= m; j++)
         d[0][j] = j;

      for (int i = 1; i <= n; i++)
      {
         for (int j = 1; j <= m; j++)
         {
            const int cost = a.GetAt(i - 1) == b.GetAt(j - 1) ? 0 : 1;

            int best = d[i - 1][j] + 1;
            best = std::min(best, d[i][j - 1] + 1);
            best = std::min(best, d[i - 1][j - 1] + cost);

            if (i > 1 && j > 1 && a.GetAt(i - 1) == b.GetAt(j - 2) && a.GetAt(i - 2) == b.GetAt(j - 1))
               best = std::min(best, d[i - 2][j - 2] + 1);

            d[i][j] = best;
         }
      }

      return d[n][m];
   }

   bool
   ImpersonationCheck::DecodePunycode(const AnsiString &input, String &decoded)
   {
      // RFC 3492 section 6.2, with the bootstring parameters of section 5.
      const unsigned int base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
      const unsigned int initialBias = 72, initialN = 128;
      const unsigned int maxint = 0x7FFFFFFF;

      std::vector<unsigned int> output;

      const int length = input.GetLength();

      int delimiter = -1;
      for (int i = 0; i < length; i++)
         if (input.GetAt(i) == '-')
            delimiter = i;

      for (int i = 0; i < delimiter; i++)
      {
         const unsigned char c = (unsigned char) input.GetAt(i);
         if (c >= 0x80)
            return false;
         output.push_back(c);
      }

      unsigned int n = initialN, i = 0, bias = initialBias;

      auto digitOf = [](char c) -> unsigned int
      {
         if (c >= '0' && c <= '9')
            return (unsigned int) (c - '0') + 26;
         if (c >= 'a' && c <= 'z')
            return (unsigned int) (c - 'a');
         if (c >= 'A' && c <= 'Z')
            return (unsigned int) (c - 'A');
         return 36;
      };

      auto adapt = [&](unsigned int delta, unsigned int numPoints, bool firstTime) -> unsigned int
      {
         delta = firstTime ? delta / damp : delta / 2;
         delta += delta / numPoints;

         unsigned int k = 0;
         while (delta > ((base - tmin) * tmax) / 2)
         {
            delta /= base - tmin;
            k += base;
         }

         return k + (base - tmin + 1) * delta / (delta + skew);
      };

      for (int in = delimiter >= 0 ? delimiter + 1 : 0; in < length; )
      {
         const unsigned int oldi = i;
         unsigned int w = 1;

         for (unsigned int k = base; ; k += base)
         {
            if (in >= length)
               return false;

            const unsigned int digit = digitOf(input.GetAt(in++));
            if (digit >= base)
               return false;

            if (digit > (maxint - i) / w)
               return false;

            i += digit * w;

            const unsigned int t = k <= bias ? tmin : (k >= bias + tmax ? tmax : k - bias);
            if (digit < t)
               break;

            if (w > maxint / (base - t))
               return false;

            w *= base - t;
         }

         const unsigned int points = (unsigned int) output.size() + 1;
         bias = adapt(i - oldi, points, oldi == 0);

         if (i / points > maxint - n)
            return false;

         n += i / points;
         i %= points;

         if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF))
            return false;

         output.insert(output.begin() + i, n);
         i++;
      }

      decoded = _T("");

      // A code point above the BMP is a surrogate pair where wchar_t is 16
      // bits (Windows) and one unit where it is 32 (Linux).
      const bool utf16 = sizeof(wchar_t) == 2;

      for (size_t j = 0; j < output.size(); j++)
      {
         const unsigned int point = output[j];

         if (utf16 && point > 0xFFFF)
         {
            const unsigned int v = point - 0x10000;
            decoded += (wchar_t) (0xD800 + (v >> 10));
            decoded += (wchar_t) (0xDC00 + (v & 0x3FF));
         }
         else
            decoded += (wchar_t) point;
      }

      return true;
   }

   // ------------------------------------------------------------- the checks

   bool
   ImpersonationCheck::JudgeName(const String &displayName, const String &fromAddress,
                                 const std::vector<Person> &people, String &reason)
   {
      const String shown = Trimmed(displayName);
      if (shown.IsEmpty())
         return false;

      const String from = Lower(Trimmed(fromAddress));

      // What the reason carries of the name: enough to recognise, never
      // enough to push the header line past what RFC 5322 allows.
      const String capped = shown.GetLength() > MaxShownLength ? shown.Left(MaxShownLength) + _T("...") : shown;

      auto isOwn = [&](const Person &person)
      {
         for (size_t i = 0; i < person.addresses.size(); i++)
            if (person.addresses[i] == from)
               return true;
         return false;
      };

      auto borrowed = [&](const Person &person)
      {
         String who = person.name.IsEmpty() ? String(_T("the address")) : person.name;
         if (who.GetLength() > MaxShownLength)
            who = who.Left(MaxShownLength) + _T("...");
         reason = _T("name \"") + capped + _T("\" matches ") + who + _T(" (") + person.owner + _T("); from ") + from;
         return true;
      };

      // A display name that is itself an address is compared as one: the
      // reader sees ceo@example.test and nothing else.
      if (shown.Find(_T("@")) >= 0)
      {
         const String shownAddress = Lower(shown);

         for (size_t i = 0; i < people.size(); i++)
         {
            if (isOwn(people[i]))
               continue;

            for (size_t j = 0; j < people[i].addresses.size(); j++)
               if (people[i].addresses[j] == shownAddress)
                  return borrowed(people[i]);
         }
      }

      const String folded = FoldName(shown);
      if (folded.IsEmpty())
         return false;

      for (size_t i = 0; i < people.size(); i++)
      {
         if (people[i].name.IsEmpty())
            continue;

         const String personFolded = people[i].folded.IsEmpty() ? FoldName(people[i].name) : people[i].folded;
         if (personFolded != folded)
            continue;

         // The person's own address, or one of their aliases: the name is theirs
         // to use.
         if (isOwn(people[i]))
            continue;

         return borrowed(people[i]);
      }

      return false;
   }

   std::vector<ImpersonationCheck::Name>
   ImpersonationCheck::PrepareNames(const std::vector<String> &names)
   {
      std::vector<Name> prepared;

      for (size_t i = 0; i < names.size(); i++)
      {
         Name name;
         name.raw = Lower(WithoutTrailingDot(Trimmed(names[i])));
         if (name.raw.IsEmpty())
            continue;
         name.idna = IdnaOf(name.raw);
         name.folded = FoldDomain(name.raw);
         prepared.push_back(name);
      }

      return prepared;
   }

   bool
   ImpersonationCheck::JudgeDomain(const String &domain, const std::vector<String> &hosted,
                                   const std::vector<String> &partners, const String &address,
                                   const String &label, String &reason)
   {
      std::vector<String> all = hosted;
      all.insert(all.end(), partners.begin(), partners.end());

      return JudgeDomain(domain, PrepareNames(all), address, label, reason);
   }

   bool
   ImpersonationCheck::JudgeDomain(const String &domain, const std::vector<Name> &names,
                                   const String &address, const String &label, String &reason)
   {
      const String raw = Lower(WithoutTrailingDot(Trimmed(domain)));
      if (raw.IsEmpty())
         return false;

      // Exactly one of ours or a partner's, or beneath one, the A-label and the
      // U-label of a name being one name: mail.example.test is example.test's
      // own, not a stranger one label away from it.
      if (IsOwnOrBeneath(IdnaOf(raw), names))
         return false;

      auto lookalike = [&](const String &candidate, const Name &name)
      {
         reason = _T("domain ") + candidate + _T(" looks like ") + name.raw + _T("; ") + label + _T(" ") + Lower(Trimmed(address));
         return true;
      };

      // The candidate, then each parent name of it down to two labels, so
      // that secure.examp1e.test is caught by the examp1e.test inside it -
      // stopping at the first parent that is one of ours, since everything
      // beneath that is ours too.
      String candidate = raw;

      for (;;)
      {
         const String candidateIdna = IdnaOf(candidate);
         const String candidateFolded = FoldDomain(candidate);
         const std::vector<String> candidateLabels = Labels(candidateFolded);

         // Equal after the confusable folding while not equal before it.
         for (size_t i = 0; i < names.size(); i++)
            if (candidateFolded == names[i].folded && candidateIdna != names[i].idna)
               return lookalike(candidate, names[i]);

         // One edit, inside one label: the label counts agree, every other
         // label is the same, and the one that differs is one insertion,
         // deletion, substitution or transposition away. An edit that adds,
         // removes or moves a dot is not counted, so a name is only ever
         // compared with a name of the same shape.
         for (size_t i = 0; i < names.size(); i++)
         {
            const std::vector<String> nameLabels = Labels(names[i].folded);

            if (nameLabels.size() != candidateLabels.size())
               continue;

            int differing = 0;
            int distance = 0;

            for (size_t j = 0; j < nameLabels.size(); j++)
            {
               if (nameLabels[j] == candidateLabels[j])
                  continue;

               differing++;
               distance = EditDistance(nameLabels[j], candidateLabels[j]);
            }

            if (differing == 1 && distance == 1)
               return lookalike(candidate, names[i]);
         }

         const int dot = candidate.Find(_T("."));
         if (dot < 0)
            break;

         const String parent = candidate.Mid(dot + 1);
         if (Labels(parent).size() < 2)
            break;

         if (IsOwnOrBeneath(IdnaOf(parent), names))
            break;

         candidate = parent;
      }

      return false;
   }

   // -------------------------------------------------------------- the lists

   std::vector<String>
   ImpersonationCheck::SplitList_(const String &stored)
   {
      std::vector<String> names;
      String current;

      for (int i = 0; i <= stored.GetLength(); i++)
      {
         const wchar_t c = i < stored.GetLength() ? stored.GetAt(i) : L',';

         if (!IsListSeparator(c))
         {
            current += c;
            continue;
         }

         if (!current.IsEmpty())
            names.push_back(Lower(current));

         current = _T("");
      }

      return names;
   }

   bool
   ImpersonationCheck::CanonicalisePartners(const String &text, String &canonical, String &refusal)
   {
      String value = Trimmed(text);

      if (value.GetLength() > MaxListLength)
      {
         refusal.Format(_T("protected_partners is at most %d characters: domain names separated by commas or spaces."), MaxListLength);
         return false;
      }

      for (int i = 0; i < value.GetLength(); i++)
      {
         if ((unsigned) value.GetAt(i) > 127)
         {
            refusal = _T("protected_partners is ASCII: write an internationalised domain name in its IDNA form (xn--...).");
            return false;
         }
      }

      std::vector<String> names;
      std::set<String> seen;
      String current;

      for (int i = 0; i <= value.GetLength(); i++)
      {
         const wchar_t c = i < value.GetLength() ? value.GetAt(i) : L',';

         if (!IsListSeparator(c))
         {
            current += c;
            continue;
         }

         if (current.IsEmpty())
            continue;

         current.ToLower();
         current = WithoutTrailingDot(current);

         if (current.Find(_T("@")) >= 0)
         {
            refusal = _T("protected_partners names domains, not addresses: ") + current + _T(" is an address. Write the domain (partner.example).");
            return false;
         }

         if (current.StartsWith(_T("[")) || current.StartsWith(_T(".")) || !StringParser::IsValidDomainName(current))
         {
            refusal = _T("protected_partners: ") + current + _T(" is not a domain name.");
            return false;
         }

         if (seen.insert(current).second)
            names.push_back(current);

         current = _T("");
      }

      canonical = _T("");

      for (size_t i = 0; i < names.size(); i++)
      {
         if (i > 0)
            canonical += _T(", ");

         canonical += names[i];
      }

      return true;
   }

   bool
   ImpersonationCheck::CanonicalisePersons(const String &text, String &canonical, String &refusal)
   {
      String value = text;
      value.Replace(_T("\r\n"), _T("\n"));
      value.Replace(_T("\r"), _T("\n"));
      value.Trim();

      if (value.GetLength() > MaxListLength)
      {
         refusal.Format(_T("protected_persons is at most %d characters: names and addresses, one per line or separated by commas."), MaxListLength);
         return false;
      }

      std::vector<String> raw;
      if (!SplitEntries(value, raw, refusal))
         return false;

      std::vector<String> entries;
      std::set<String> seen;

      for (size_t i = 0; i < raw.size(); i++)
      {
         Person person;
         String entry;

         if (!ParseEntry(raw[i], person, entry, refusal))
            return false;

         if (seen.insert(Lower(entry)).second)
            entries.push_back(entry);
      }

      canonical = _T("");

      for (size_t i = 0; i < entries.size(); i++)
      {
         if (i > 0)
            canonical += _T("\n");

         canonical += entries[i];
      }

      if (canonical.GetLength() > MaxListLength)
      {
         refusal.Format(_T("protected_persons is at most %d characters: names and addresses, one per line or separated by commas."), MaxListLength);
         return false;
      }

      return true;
   }

   std::vector<ImpersonationCheck::Person>
   ImpersonationCheck::ParsePersons(const String &stored)
   {
      std::vector<Person> people;

      String value = stored;
      value.Replace(_T("\r\n"), _T("\n"));
      value.Replace(_T("\r"), _T("\n"));

      String current;

      for (int i = 0; i <= value.GetLength(); i++)
      {
         const wchar_t c = i < value.GetLength() ? value.GetAt(i) : L'\n';

         if (c != L'\n')
         {
            current += c;
            continue;
         }

         current.Trim();

         if (!current.IsEmpty())
         {
            Person person;
            String canonical;
            String refusal;

            // A stored entry passed the canonicaliser once; one that does not
            // pass now is skipped rather than guessed at.
            if (ParseEntry(current, person, canonical, refusal))
            {
               // An address of an account here protects that account's name as
               // well as the address: the chairman at another hosted domain.
               if (person.name.IsEmpty() && !person.addresses.empty())
               {
                  std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(person.addresses[0]);
                  if (account)
                     person.name = DirectoryStore::NameOf(account->GetPersonFirstName(), account->GetPersonLastName());
               }

               person.folded = FoldName(person.name);
               people.push_back(person);
            }
         }

         current = _T("");
      }

      return people;
   }

   bool
   ImpersonationCheck::ReadAction(const String &word, int &action)
   {
      String lower = Lower(Trimmed(word));

      if (lower == _T("0") || lower == _T("off"))
         action = Off;
      else if (lower == _T("1") || lower == _T("flag"))
         action = Flag;
      else if (lower == _T("2") || lower == _T("quarantine"))
         action = Quarantine;
      else
         return false;

      return true;
   }

   String
   ImpersonationCheck::ActionWord(int action)
   {
      switch (action)
      {
      case Flag:
         return _T("flag");
      case Quarantine:
         return _T("quarantine");
      default:
         return _T("off");
      }
   }

   // ----------------------------------------------------------- the delivery

   std::vector<ImpersonationCheck::Person>
   ImpersonationCheck::AccountsOf_(std::shared_ptr<const Domain> domain)
   {
      std::vector<Person> people;

      SQLCommand command("select accountaddress, accountpersonfirstname, accountpersonlastname from hm_accounts where accountdomainid = @DOMAINID and accountactive = 1");
      command.AddParameter("@DOMAINID", domain->GetID());

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return people;

      std::map<String, size_t> byAddress;

      while (!recordset->IsEOF())
      {
         Person person;
         person.name = DirectoryStore::NameOf(recordset->GetStringValue("accountpersonfirstname"), recordset->GetStringValue("accountpersonlastname"));
         person.folded = FoldName(person.name);

         const String address = Lower(recordset->GetStringValue("accountaddress"));
         person.addresses.push_back(address);
         person.owner = address;

         // An account with no person name entered is protected by its address
         // alone: JudgeName compares nothing against an empty name, and the
         // address still answers for a display name that is an address.
         byAddress[address] = people.size();
         people.push_back(person);

         recordset->MoveNext();
      }

      // The domain's active aliases are the accounts' addresses too: an alias
      // of the account may carry the account's name, and a display name that
      // is an alias address is the account's own.
      SQLCommand aliases("select aliasname, aliasvalue from hm_aliases where aliasdomainid = @DOMAINID and aliasactive = 1");
      aliases.AddParameter("@DOMAINID", domain->GetID());

      std::shared_ptr<DALRecordset> aliasRows = Application::Instance()->GetDBManager()->OpenRecordset(aliases);
      if (aliasRows)
      {
         while (!aliasRows->IsEOF())
         {
            const String name = Lower(aliasRows->GetStringValue("aliasname"));
            const String value = Lower(aliasRows->GetStringValue("aliasvalue"));

            std::map<String, size_t>::const_iterator found = byAddress.find(value);
            if (found != byAddress.end() && !name.IsEmpty() && !Contains(people[found->second].addresses, name))
               people[found->second].addresses.push_back(name);

            aliasRows->MoveNext();
         }
      }

      return people;
   }

   std::vector<String>
   ImpersonationCheck::HostedDomains_()
   {
      std::vector<String> names;

      SQLCommand command("select domainname from hm_domains");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (recordset)
      {
         while (!recordset->IsEOF())
         {
            const String name = Lower(recordset->GetStringValue("domainname"));
            if (!name.IsEmpty())
               names.push_back(name);

            recordset->MoveNext();
         }
      }

      // A domain alias is a name this server answers to as well.
      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      if (aliases)
      {
         // A locked copy: this is ObjectCache's collection, the one every
         // session shares. Nothing refreshes that object in place today - a
         // reload replaces it - but a loop over a cached collection does not
         // lean on that staying true.
         for (auto alias : aliases->GetSnapshot())
         {
            const String name = Lower(alias->GetAlias());
            if (!name.IsEmpty() && !Contains(names, name))
               names.push_back(name);
         }
      }

      return names;
   }

   std::shared_ptr<const ImpersonationCheck::Facts>
   ImpersonationCheck::FactsFor_(std::shared_ptr<const Domain> domain)
   {
      const time_t now = time(0);

      {
         boost::lock_guard<boost::mutex> guard(facts_mutex_);

         std::map<__int64, std::shared_ptr<const Facts> >::const_iterator found = facts_.find(domain->GetID());
         if (found != facts_.end() && now - found->second->made <= CacheSeconds)
            return found->second;
      }

      // Built outside the lock: two queries and the folding of every name,
      // which another thread delivering to the same domain may do too - the
      // last one in wins, and both answers are equally current.
      std::shared_ptr<Facts> facts = std::shared_ptr<Facts>(new Facts());
      facts->made = now;
      facts->people = AccountsOf_(domain);

      const std::vector<Person> protectedPeople = ParsePersons(domain->GetProtectedPersons());
      facts->people.insert(facts->people.end(), protectedPeople.begin(), protectedPeople.end());

      std::vector<String> names = HostedDomains_();
      const std::vector<String> partners = SplitList_(domain->GetProtectedPartners());
      names.insert(names.end(), partners.begin(), partners.end());
      facts->names = PrepareNames(names);

      boost::lock_guard<boost::mutex> guard(facts_mutex_);
      facts_[domain->GetID()] = facts;
      return facts;
   }

   void
   ImpersonationCheck::Invalidate()
   {
      boost::lock_guard<boost::mutex> guard(facts_mutex_);
      facts_.clear();
   }

   String
   ImpersonationCheck::OneLine_(const String &text)
   {
      // The header is one line whatever a display name carried; the RFC 2047
      // writer folds it if it is long.
      String result;

      for (int i = 0; i < text.GetLength(); i++)
      {
         const wchar_t c = text.GetAt(i);
         result += (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
      }

      return result;
   }

   ImpersonationCheck::Outcome
   ImpersonationCheck::Apply(std::shared_ptr<const Account> account,
                             std::shared_ptr<const Domain> domain,
                             std::shared_ptr<Message> message,
                             const String &sendersIP,
                             MessageOrigin::Verdict &origin)
   {
      if (!account || !domain || !message)
         return Clean;

      // The action is a setting with levels: the recipient account's own where
      // an administrator gave it one, its domain's otherwise
      // (Application/EffectiveSetting.h). The two lists stay the domain's.
      const int action = EffectiveSetting::ResolveInt(_T("impersonation_action"), domain, account);
      if (action == Off)
         return Clean;

      const String fileName = PersistentMessage::GetFileName(account, message);

      // The header block and the outside/inside decision: the tagger's, when
      // the domain tags, and read here once otherwise.
      MessageOrigin::Judge(message, fileName, sendersIP, origin);

      // A stranger's forged warning is removed along with anything else this
      // finds nothing to say about; where there is something to say the
      // write below replaces it.
      const bool carriesForgedHeader = !MessageOrigin::FirstFieldValue(origin.header, Header()).IsEmpty();

      String reason;

      if (origin.external)
      {
         // The From header as the reader sees it, RFC 2047 decoded, and the
         // address in it as the DMARC check reads it. Parsed only for a copy
         // from outside, which is the only one judged.
         MimeHeader mimeHeader;
         mimeHeader.Load(origin.header.c_str(), origin.header.GetLength(), true);
         const String fromHeader = mimeHeader.GetUnicodeFieldValue("From");
         const String fromAddress = Lower(Trimmed(DMARC::ExtractAddressFromHeaderValue(fromHeader)));
         const String displayName = DisplayNameOf(fromHeader);
         const String envelope = Lower(Trimmed(origin.facts.envelope_sender));

         // The reader's own safe senders, and the domain's and the server's
         // allow list where nothing nearer decides: a sender somebody vouched
         // for is not then warned about.
         SenderListCache::Decision decision = SenderListCache::IsEmpty()
            ? SenderListCache::Decision()
            : SenderListCache::Decide(envelope, fromAddress, account->GetID(), account->GetDomainID());

         if (decision.verdict == SenderListCache::Allowed)
         {
            String logText;
            logText.Format(_T("ImpersonationCheck - Message %I64d for %s from %s was not checked: %s is on the %s allow list."),
               message->GetID(), account->GetAddress().c_str(), fromAddress.c_str(), decision.matched.c_str(), SenderListCache::ScopeName(decision.scope).c_str());
            LOG_DEBUG(logText);
         }
         else
         {
            std::shared_ptr<const Facts> facts = FactsFor_(domain);

            const String fromDomain = Lower(WithoutTrailingDot(StringParser::ExtractDomain(fromAddress)));
            const String envelopeDomain = Lower(WithoutTrailingDot(StringParser::ExtractDomain(envelope)));

            // A From domain that is itself hosted here or a protected partner,
            // exactly - its U-label and A-label being one name - is exempt from
            // both checks; the envelope is still judged below.
            bool trustedFrom = false;

            if (!fromDomain.IsEmpty())
            {
               const String fromIdna = IdnaOf(fromDomain);

               for (size_t i = 0; i < facts->names.size() && !trustedFrom; i++)
                  trustedFrom = facts->names[i].idna == fromIdna;
            }

            if (!trustedFrom)
            {
               if (!JudgeName(displayName, fromAddress, facts->people, reason) && !fromDomain.IsEmpty())
                  JudgeDomain(fromDomain, facts->names, fromAddress, _T("from"), reason);
            }

            if (reason.IsEmpty() && !envelopeDomain.IsEmpty() && envelopeDomain != fromDomain)
               JudgeDomain(envelopeDomain, facts->names, envelope, _T("envelope sender"), reason);
         }
      }

      if (reason.IsEmpty())
      {
         if (!carriesForgedHeader)
            return Clean;

         MessageData data;
         if (data.LoadFromMessage(fileName, message))
         {
            data.DeleteField(Header());
            data.WriteReported(fileName, "The impersonation warning");
         }

         return Clean;
      }

      const String headerValue = OneLine_(reason);

      if (action == Quarantine)
      {
         if (QuarantineStore::GetEnabled())
         {
            if (QuarantineStore::Quarantine(message, headerValue, 0, fileName))
            {
               String logText;
               logText.Format(_T("ImpersonationCheck - Message %I64d for %s quarantined: %s."),
                  message->GetID(), account->GetAddress().c_str(), headerValue.c_str());
               LOG_DEBUG(logText);

               return Quarantined;
            }

            // The store refused it. The trade the spam verdict makes: a copy
            // that was not stored anywhere is not discarded, it is flagged and
            // delivered.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7120, "ImpersonationCheck::Apply",
               Formatter::Format("Message {0} could not be quarantined for {1} ({2}). A flagged copy has been delivered instead.",
                  message->GetID(), account->GetAddress(), headerValue));
         }
         else
         {
            String logText;
            logText.Format(_T("ImpersonationCheck - Message %I64d for %s: the domain asks for quarantine but the quarantine store is off; the copy is flagged instead."),
               message->GetID(), account->GetAddress().c_str());
            LOG_DEBUG(logText);
         }
      }

      MessageData data;

      if (!data.LoadFromMessage(fileName, message))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7121, "ImpersonationCheck::Apply",
            Formatter::Format("Message {0} could not be loaded while flagging it for {1} ({2}). The copy is delivered unflagged.",
               message->GetID(), account->GetAddress(), headerValue));
         return Clean;
      }

      // Through the RFC 2047 writer in UTF-8, whatever charset the message
      // declares, so that the shown name reaches the reader as it was shown.
      data.GetMimeMessage()->SetUnicodeFieldValue(Header(), headerValue, "utf-8");

      if (!data.WriteReported(fileName, "The impersonation warning"))
      {
         String logText;
         logText.Format(_T("ImpersonationCheck - Message %I64d was delivered to %s without its impersonation warning: the rewrite of the message file failed."),
            message->GetID(), account->GetAddress().c_str());
         LOG_APPLICATION(logText);
         return Clean;
      }

      String logText;
      logText.Format(_T("ImpersonationCheck - Message %I64d for %s flagged: %s."),
         message->GetID(), account->GetAddress().c_str(), headerValue.c_str());
      LOG_DEBUG(logText);

      return Flagged;
   }

   // ---------------------------------------------------------------- the tests

   void
   ImpersonationCheckTester::Test()
   {
      TestFolding_();
      TestNames_();
      TestDomains_();
      TestLists_();
   }

   void
   ImpersonationCheckTester::TestFolding_()
   {
      if (ImpersonationCheck::FoldName(_T("Ada Lovelace")) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not lower-case a plain name.");

      if (ImpersonationCheck::FoldName(_T("  A.  Lovelace, ")) != _T("a lovelace"))
         throw std::logic_error("FoldName did not collapse punctuation and white space to one space.");

      if (ImpersonationCheck::FoldName(_T("Ada Love1ace")) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not fold the digit 1 to l.");

      if (ImpersonationCheck::FoldName(_T("Ada L0velace")) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not fold the digit 0 to o.");

      if (ImpersonationCheck::FoldName(_T("Ian")) != _T("lan"))
         throw std::logic_error("FoldName did not fold a capital I to l before lower-casing.");

      if (ImpersonationCheck::FoldName(_T("Arnold")) != _T("amold"))
         throw std::logic_error("FoldName did not fold rn to m.");

      if (ImpersonationCheck::FoldName(_T("Avva")) != _T("awa"))
         throw std::logic_error("FoldName did not fold vv to w.");

      // Cyrillic а е о and Greek capital Ο where Latin letters stand.
      String cyrillic = _T("Ad");
      cyrillic += (wchar_t) 0x0430;
      cyrillic += _T(" L");
      cyrillic += (wchar_t) 0x043E;
      cyrillic += _T("v");
      cyrillic += (wchar_t) 0x0435;
      cyrillic += _T("lace");
      if (ImpersonationCheck::FoldName(cyrillic) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not fold the Cyrillic look-alikes to Latin.");

      // A Cyrillic capital whose lower case looks nothing like Latin: Тom.
      String tom;
      tom += (wchar_t) 0x0422;
      tom += _T("om Smith");
      if (ImpersonationCheck::FoldName(tom) != _T("tom smith"))
         throw std::logic_error("FoldName did not fold Cyrillic capital Te to t.");

      String greek = _T("L");
      greek += (wchar_t) 0x039F;
      greek += _T("velace");
      if (ImpersonationCheck::FoldName(greek) != _T("lovelace"))
         throw std::logic_error("FoldName did not fold Greek capital omicron to o.");

      // The invisible format characters are deleted, not turned into spaces:
      // a zero-width space inside a word, a soft hyphen inside a word.
      String zeroWidth = _T("A");
      zeroWidth += (wchar_t) 0x200B;
      zeroWidth += _T("da Lovelace");
      if (ImpersonationCheck::FoldName(zeroWidth) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not delete a zero-width space inside a name.");

      String softHyphen = _T("Ada Love");
      softHyphen += (wchar_t) 0x00AD;
      softHyphen += _T("lace");
      if (ImpersonationCheck::FoldName(softHyphen) != _T("ada lovelace"))
         throw std::logic_error("FoldName did not delete a soft hyphen inside a name.");

      if (ImpersonationCheck::FoldName(_T("")) != _T(""))
         throw std::logic_error("FoldName of nothing is not nothing.");

      if (ImpersonationCheck::FoldDomain(_T("Exarnple.TEST.")) != _T("example.test"))
         throw std::logic_error("FoldDomain did not lower-case, drop the trailing dot and fold rn.");

      if (ImpersonationCheck::FoldDomain(_T("examp1e.test")) != _T("example.test"))
         throw std::logic_error("FoldDomain did not fold 1 to l.");

      if (ImpersonationCheck::FoldDomain(_T("xn--exmple-4nf.test")) != _T("example.test"))
         throw std::logic_error("FoldDomain did not decode the IDNA label and fold its Cyrillic a.");

      // IdnaOf decodes and lower-cases and folds nothing: the A-label and the
      // U-label are one name, and a look-alike letter is still itself.
      String buecherExample = _T("b");
      buecherExample += (wchar_t) 0x00FC;
      buecherExample += _T("cher.example");
      if (ImpersonationCheck::IdnaOf(_T("XN--BCHER-KVA.Example")) != buecherExample)
         throw std::logic_error("IdnaOf did not decode the A-label to the U-label.");
      if (ImpersonationCheck::IdnaOf(_T("examp1e.test")) != _T("examp1e.test"))
         throw std::logic_error("IdnaOf folded a confusable, which is not its job.");

      String decoded;
      String muenchen = _T("m");
      muenchen += (wchar_t) 0x00FC;
      muenchen += _T("nchen");
      if (!ImpersonationCheck::DecodePunycode("mnchen-3ya", decoded) || decoded != muenchen)
         throw std::logic_error("Punycode: mnchen-3ya did not decode to muenchen with a u-umlaut.");

      String buecher = _T("b");
      buecher += (wchar_t) 0x00FC;
      buecher += _T("cher");
      if (!ImpersonationCheck::DecodePunycode("bcher-kva", decoded) || decoded != buecher)
         throw std::logic_error("Punycode: bcher-kva did not decode.");

      // RFC 3492 section 7.1 (A), Arabic (Egyptian): the seventeen code
      // points the RFC lists for egbpdaj6bu4bxfgehfvwxn.
      const wchar_t arabicPoints[] = { 0x0644, 0x064A, 0x0647, 0x0645, 0x0627, 0x0628, 0x062A, 0x0643, 0x0644,
                                       0x0645, 0x0648, 0x0634, 0x0639, 0x0631, 0x0628, 0x064A, 0x061F };
      String arabic;
      for (size_t i = 0; i < sizeof(arabicPoints) / sizeof(arabicPoints[0]); i++)
         arabic += arabicPoints[i];
      if (!ImpersonationCheck::DecodePunycode("egbpdaj6bu4bxfgehfvwxn", decoded) || decoded != arabic)
         throw std::logic_error("Punycode: RFC 3492 sample (A) did not decode to the RFC's code points.");

      if (!ImpersonationCheck::DecodePunycode("abc-", decoded) || decoded != _T("abc"))
         throw std::logic_error("Punycode: an all-basic label did not decode to itself.");

      if (ImpersonationCheck::DecodePunycode("nonsense!", decoded))
         throw std::logic_error("Punycode: a label with a character outside the alphabet was accepted.");

#ifndef HM_PLATFORM_POSIX
      // NFKC: full-width Latin letters become the letters they stand for.
      String fullWidth;
      for (const wchar_t *p = L"example"; *p; p++)
         fullWidth += (wchar_t) (0xFF21 + (*p - L'a') + 0x20);
      if (ImpersonationCheck::FoldDomain(fullWidth + _T(".test")) != _T("example.test"))
         throw std::logic_error("FoldDomain did not NFKC-fold full-width letters.");
#endif

      if (ImpersonationCheck::EditDistance(_T("example"), _T("example")) != 0)
         throw std::logic_error("EditDistance of equal strings is not 0.");
      if (ImpersonationCheck::EditDistance(_T("exampel"), _T("example")) != 1)
         throw std::logic_error("EditDistance did not count a transposition as 1.");
      if (ImpersonationCheck::EditDistance(_T("examplee"), _T("example")) != 1)
         throw std::logic_error("EditDistance did not count an insertion as 1.");
      if (ImpersonationCheck::EditDistance(_T("exmple"), _T("example")) != 1)
         throw std::logic_error("EditDistance did not count a deletion as 1.");
      if (ImpersonationCheck::EditDistance(_T("exumple"), _T("example")) != 1)
         throw std::logic_error("EditDistance did not count a substitution as 1.");
      if (ImpersonationCheck::EditDistance(_T("abcd"), _T("dcba")) < 2)
         throw std::logic_error("EditDistance of a reversal is not more than 1.");
      if (ImpersonationCheck::EditDistance(_T("ab"), _T("abcde")) != 3)
         throw std::logic_error("EditDistance's length pre-check did not answer the difference.");
   }

   void
   ImpersonationCheckTester::TestNames_()
   {
      std::vector<ImpersonationCheck::Person> people;

      ImpersonationCheck::Person ada;
      ada.name = _T("Ada Lovelace");
      ada.addresses.push_back(_T("ada@example.test"));
      ada.addresses.push_back(_T("ada.l@example.test"));
      ada.owner = _T("ada@example.test");
      people.push_back(ada);

      ImpersonationCheck::Person chairman;
      chairman.name = _T("The Chairman");
      chairman.owner = _T("a protected person");
      people.push_back(chairman);

      ImpersonationCheck::Person chair;
      chair.name = _T("Board Chair");
      chair.addresses.push_back(_T("chair@board.example"));
      chair.owner = _T("chair@board.example");
      people.push_back(chair);

      ImpersonationCheck::Person nameless;
      nameless.addresses.push_back(_T("ceo@example.test"));
      nameless.owner = _T("ceo@example.test");
      people.push_back(nameless);

      String reason;

      if (!ImpersonationCheck::JudgeName(_T("Ada Lovelace"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A borrowed account name from a stranger was not judged.");
      if (reason != _T("name \"Ada Lovelace\" matches Ada Lovelace (ada@example.test); from x@evil.test"))
         throw std::logic_error("The borrowed-name reason is not in the header's shape.");

      if (ImpersonationCheck::JudgeName(_T("Ada Lovelace"), _T("ADA@example.test"), people, reason))
         throw std::logic_error("The account's own address was judged as borrowing its own name.");

      if (ImpersonationCheck::JudgeName(_T("ada lovelace"), _T("ada.l@example.test"), people, reason))
         throw std::logic_error("The account's alias was judged as borrowing the account's name.");

      if (ImpersonationCheck::JudgeName(_T("Ada Lovelac"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A name one letter off was judged as borrowed.");

      if (!ImpersonationCheck::JudgeName(_T("Ada L0velace"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A name spelled with a zero for the o was not judged as borrowed.");
      if (reason.Find(_T("name \"Ada L0velace\" matches Ada Lovelace (ada@example.test)")) != 0)
         throw std::logic_error("The reason does not carry the shown spelling and the person it matches.");

      if (!ImpersonationCheck::JudgeName(_T("The Chairman"), _T("anyone@anywhere.test"), people, reason))
         throw std::logic_error("A protected person's name with no address was not judged as borrowed.");
      if (reason != _T("name \"The Chairman\" matches The Chairman (a protected person); from anyone@anywhere.test"))
         throw std::logic_error("The protected-person reason is not in the header's shape.");

      if (ImpersonationCheck::JudgeName(_T("Board Chair"), _T("chair@board.example"), people, reason))
         throw std::logic_error("A protected person writing from the address given for them was judged.");

      if (!ImpersonationCheck::JudgeName(_T("Board Chair"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A protected person's name from another address was not judged.");

      if (!ImpersonationCheck::JudgeName(_T("ada@example.test"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A display name that is an account's address was not compared as an address.");

      if (!ImpersonationCheck::JudgeName(_T("ada.l@example.test"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A display name that is an account's alias address was not compared as an address.");

      if (!ImpersonationCheck::JudgeName(_T("ceo@example.test"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A display name that is a nameless account's address was not compared as an address.");
      if (reason != _T("name \"ceo@example.test\" matches the address (ceo@example.test); from x@evil.test"))
         throw std::logic_error("The nameless-account reason is not in the header's shape.");

      if (ImpersonationCheck::JudgeName(_T(""), _T("x@evil.test"), people, reason))
         throw std::logic_error("An empty display name was judged.");

      if (ImpersonationCheck::JudgeName(_T("Somebody Else"), _T("x@evil.test"), people, reason))
         throw std::logic_error("A name nobody here has was judged.");

      // A name longer than the reason carries is cut with an ellipsis.
      String longName;
      for (int i = 0; i < 200; i++)
         longName += L'x';
      longName += _T(" Ada Lovelace");
      ImpersonationCheck::Person longPerson;
      longPerson.name = longName;
      longPerson.owner = _T("a protected person");
      people.push_back(longPerson);
      if (!ImpersonationCheck::JudgeName(longName, _T("x@evil.test"), people, reason))
         throw std::logic_error("A long protected name was not judged.");
      if (reason.Find(_T("...\" matches ")) < 0 || reason.Find(_T("... (a protected person)")) < 0 || reason.GetLength() > 400)
         throw std::logic_error("A long shown name, or the long person it matched, was not capped in the reason.");
   }

   void
   ImpersonationCheckTester::TestDomains_()
   {
      std::vector<String> hosted;
      hosted.push_back(_T("example.test"));
      hosted.push_back(_T("xn--bcher-kva.example"));
      std::vector<String> partners;
      partners.push_back(_T("partner.example"));

      String reason;

      auto judge = [&](const wchar_t *domain)
      {
         reason = _T("");
         String address = _T("stranger@");
         address += domain;
         return ImpersonationCheck::JudgeDomain(String(domain), hosted, partners, address, String(_T("from")), reason);
      };

      if (!judge(L"examp1e.test"))
         throw std::logic_error("examp1e.test was not a lookalike of example.test.");
      if (reason != _T("domain examp1e.test looks like example.test; from stranger@examp1e.test"))
         throw std::logic_error("The lookalike reason is not in the header's shape.");
      if (!judge(L"exarnple.test"))
         throw std::logic_error("exarnple.test was not a lookalike.");
      if (!judge(L"exampel.test"))
         throw std::logic_error("A transposition was not a lookalike.");
      if (!judge(L"examplee.test"))
         throw std::logic_error("An insertion was not a lookalike.");
      if (!judge(L"exmple.test"))
         throw std::logic_error("A deletion was not a lookalike.");
      if (!judge(L"xn--exmple-4nf.test"))
         throw std::logic_error("The IDNA spelling with a Cyrillic a was not a lookalike.");
      if (!judge(L"example.tset"))
         throw std::logic_error("A transposition in the last label was not a lookalike.");

      // A lookalike used as a lookalike is used: one label in front of it.
      if (!judge(L"mail.examp1e.test"))
         throw std::logic_error("A name beneath a lookalike was not judged by the lookalike inside it.");
      if (reason != _T("domain examp1e.test looks like example.test; from stranger@mail.examp1e.test"))
         throw std::logic_error("The reason for a name beneath a lookalike does not name the lookalike.");
      if (!judge(L"secure-login.exarnple.test"))
         throw std::logic_error("A name two labels deep beneath a lookalike was not judged.");
      if (!judge(L"invoices.xn--exmple-4nf.test"))
         throw std::logic_error("A name beneath the IDNA lookalike was not judged.");

      if (judge(L"mail.example.test"))
         throw std::logic_error("A name beneath the hosted domain was judged a lookalike.");
      if (judge(L"a.b.mail.example.test"))
         throw std::logic_error("A name deep beneath the hosted domain was judged a lookalike.");
      if (judge(L"example.test"))
         throw std::logic_error("The hosted domain itself was judged a lookalike.");
      if (judge(L"EXAMPLE.test."))
         throw std::logic_error("The hosted domain in another case with a trailing dot was judged a lookalike.");
      if (judge(L"exampl.etest"))
         throw std::logic_error("An edit that moves a dot was counted.");
      if (judge(L"exampletest"))
         throw std::logic_error("A name with another label count was compared.");
      if (judge(L"elsewhere.test"))
         throw std::logic_error("An unrelated domain was judged a lookalike.");
      if (judge(L"exaample.tst"))
         throw std::logic_error("Two edits, one in each label, were counted as one.");
      if (!judge(L"partner.exarnple"))
         throw std::logic_error("A lookalike of a protected partner was not judged.");
      if (judge(L"partner.example"))
         throw std::logic_error("The partner itself was judged a lookalike.");
      if (judge(L"mail.partner.example"))
         throw std::logic_error("A name beneath the partner was judged a lookalike.");
      if (judge(L""))
         throw std::logic_error("An empty domain was judged.");

      // A hosted internationalised name in its U-label spelling is that name,
      // not a lookalike of its own A-label; the same letters without the
      // umlaut are one edit from it.
      String buecher = _T("b");
      buecher += (wchar_t) 0x00FC;
      buecher += _T("cher.example");
      if (judge(buecher.c_str()))
         throw std::logic_error("The U-label spelling of a hosted A-label was judged a lookalike of itself.");
      String beneathBuecher = _T("mail.");
      beneathBuecher += buecher;
      if (judge(beneathBuecher.c_str()))
         throw std::logic_error("A name beneath the U-label spelling of a hosted name was judged.");
      if (!judge(L"bucher.example"))
         throw std::logic_error("The umlaut-less spelling of a hosted IDNA name was not a lookalike.");
   }

   void
   ImpersonationCheckTester::TestLists_()
   {
      String canonical, refusal;

      if (!ImpersonationCheck::CanonicalisePartners(_T("Partner.Example  other.example, partner.example;third.example."), canonical, refusal))
         throw std::logic_error("A well-formed partner list was refused: " + std::string(AnsiString(refusal)));
      if (canonical != _T("partner.example, other.example, third.example"))
         throw std::logic_error("The partner list was not canonicalised: " + std::string(AnsiString(canonical)));

      if (ImpersonationCheck::CanonicalisePartners(_T("someone@partner.example"), canonical, refusal) || refusal.Find(_T("not addresses")) < 0)
         throw std::logic_error("An address in the partner list was not refused with the sentence.");
      if (ImpersonationCheck::CanonicalisePartners(_T("[192.0.2.1]"), canonical, refusal))
         throw std::logic_error("An address literal in the partner list was not refused.");
      if (ImpersonationCheck::CanonicalisePartners(_T(".example"), canonical, refusal))
         throw std::logic_error("A suffix in the partner list was not refused.");
      String nonAscii = _T("b");
      nonAscii += (wchar_t) 0x00FC;
      nonAscii += _T("cher.example");
      if (ImpersonationCheck::CanonicalisePartners(nonAscii, canonical, refusal) || refusal.Find(_T("ASCII")) < 0)
         throw std::logic_error("A non-ASCII partner was not refused with the sentence.");
      if (!ImpersonationCheck::CanonicalisePartners(_T("xn--bcher-kva.example"), canonical, refusal) || canonical != _T("xn--bcher-kva.example"))
         throw std::logic_error("An IDNA partner was refused.");
      if (!ImpersonationCheck::CanonicalisePartners(_T(""), canonical, refusal) || !canonical.IsEmpty())
         throw std::logic_error("An empty partner list is not empty.");

      String tooLong;
      for (int i = 0; i < 2001; i++)
         tooLong += L'a';
      if (ImpersonationCheck::CanonicalisePartners(tooLong, canonical, refusal) || refusal.Find(_T("2000")) < 0)
         throw std::logic_error("A partner list of 2,001 characters was not refused.");

      if (!ImpersonationCheck::CanonicalisePersons(_T("The Chairman, chair@Board.Example\r\n\"Ada Lovelace\" <ADA@Example.Test>\nthe chairman"), canonical, refusal))
         throw std::logic_error("A well-formed persons list was refused: " + std::string(AnsiString(refusal)));
      if (canonical != _T("The Chairman\nchair@board.example\nAda Lovelace <ada@example.test>"))
         throw std::logic_error("The persons list was not canonicalised: " + std::string(AnsiString(canonical)));

      // A comma inside quotes is part of the name, and the name is stored in
      // quotes so that it reads back as one entry.
      if (!ImpersonationCheck::CanonicalisePersons(_T("\"Holloway, Chris\" <chris@board.example>, \"Board, The\", Plain Name"), canonical, refusal))
         throw std::logic_error("A quoted name with a comma was refused: " + std::string(AnsiString(refusal)));
      if (canonical != _T("\"Holloway, Chris\" <chris@board.example>\n\"Board, The\"\nPlain Name"))
         throw std::logic_error("A quoted name with a comma was not kept whole: " + std::string(AnsiString(canonical)));

      if (ImpersonationCheck::CanonicalisePersons(_T("\"Holloway, Chris <chris@board.example>"), canonical, refusal) || refusal.Find(_T("unbalanced quote")) < 0)
         throw std::logic_error("An unbalanced quote was not refused with the sentence.");
      if (ImpersonationCheck::CanonicalisePersons(_T("Ada <not an address>"), canonical, refusal) || refusal.Find(_T("not an address")) < 0)
         throw std::logic_error("A person with a bad address was not refused with the sentence.");
      if (ImpersonationCheck::CanonicalisePersons(_T("Ada <ada@example.test"), canonical, refusal))
         throw std::logic_error("An unclosed address was not refused.");
      if (ImpersonationCheck::CanonicalisePersons(_T("no@t an address"), canonical, refusal))
         throw std::logic_error("A bare entry with an @ that is not an address was not refused.");
      if (ImpersonationCheck::CanonicalisePersons(tooLong, canonical, refusal) || refusal.Find(_T("2000")) < 0)
         throw std::logic_error("A persons list of 2,001 characters was not refused.");

      std::vector<ImpersonationCheck::Person> people = ImpersonationCheck::ParsePersons(_T("The Chairman\nchair@board.example\nAda Lovelace <ada@example.test>\n\"Holloway, Chris\" <chris@board.example>"));
      if (people.size() != 4)
         throw std::logic_error("ParsePersons did not read four entries.");
      if (people[0].name != _T("The Chairman") || !people[0].addresses.empty() || people[0].owner != _T("a protected person") || people[0].folded != _T("the chairman"))
         throw std::logic_error("ParsePersons misread a bare name.");
      if (people[1].addresses.size() != 1 || people[1].addresses[0] != _T("chair@board.example") || people[1].owner != _T("chair@board.example"))
         throw std::logic_error("ParsePersons misread a bare address.");
      if (people[2].name != _T("Ada Lovelace") || people[2].addresses.size() != 1 || people[2].addresses[0] != _T("ada@example.test"))
         throw std::logic_error("ParsePersons misread Name <address>.");
      if (people[3].name != _T("Holloway, Chris") || people[3].addresses.size() != 1 || people[3].addresses[0] != _T("chris@board.example"))
         throw std::logic_error("ParsePersons misread a quoted name with a comma.");

      int action = -1;
      if (!ImpersonationCheck::ReadAction(_T("Quarantine"), action) || action != ImpersonationCheck::Quarantine)
         throw std::logic_error("ReadAction did not read the word quarantine.");
      if (!ImpersonationCheck::ReadAction(_T("1"), action) || action != ImpersonationCheck::Flag)
         throw std::logic_error("ReadAction did not read the digit 1.");
      if (!ImpersonationCheck::ReadAction(_T("off"), action) || action != ImpersonationCheck::Off)
         throw std::logic_error("ReadAction did not read the word off.");
      if (ImpersonationCheck::ReadAction(_T("3"), action) || ImpersonationCheck::ReadAction(_T("sometimes"), action))
         throw std::logic_error("ReadAction accepted a value that is not an action.");
      if (ImpersonationCheck::ActionWord(ImpersonationCheck::Quarantine) != _T("quarantine") || ImpersonationCheck::ActionWord(0) != _T("off"))
         throw std::logic_error("ActionWord is not the word for the action.");
   }
}
