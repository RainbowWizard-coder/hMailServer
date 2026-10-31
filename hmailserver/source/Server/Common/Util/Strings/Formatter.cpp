// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"
#include "Formatter.h"
#include "FormatArgument.h"

#include "../Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String 
   Formatter::Format(const AnsiString &fmt, const FormatArgument &argument1)
   {
      std::list<FormatArgument> arguments;
      arguments.push_back(argument1);

      return Format(fmt, arguments);
   }

   String 
   Formatter::Format(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2)
   {
      std::list<FormatArgument> arguments;
      arguments.push_back(argument1);
      arguments.push_back(argument2);

      return Format(fmt, arguments);
   }


   String 
   Formatter::Format(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2, const FormatArgument &argument3)
   {
      std::list<FormatArgument> arguments;
      arguments.push_back(argument1);
      arguments.push_back(argument2);
      arguments.push_back(argument3);

      return Format(fmt, arguments);
   }



   String 
   Formatter::Format(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2,const FormatArgument &argument3, const FormatArgument &argument4)
   {
      std::list<FormatArgument> arguments;
      arguments.push_back(argument1);
      arguments.push_back(argument2);
      arguments.push_back(argument3);
      arguments.push_back(argument4);

      return Format(fmt, arguments);
   }

   String 
   Formatter::Format(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2,const FormatArgument &argument3, const FormatArgument &argument4, const FormatArgument &argument5)
   {
      std::list<FormatArgument> arguments;
      arguments.push_back(argument1);
      arguments.push_back(argument2);
      arguments.push_back(argument3);
      arguments.push_back(argument4);
      arguments.push_back(argument5);

      return Format(fmt, arguments);
   }

   AnsiString 
   Formatter::FormatAsAnsi(const AnsiString &fmt, const FormatArgument &argument1)
   {
       String value = Format(fmt, argument1);

       return Unicode::ToANSI(value);
   }

   AnsiString 
   Formatter::FormatAsAnsi(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2)
   {
      String value = Format(fmt, argument1, argument2);

      return Unicode::ToANSI(value);
   }


   AnsiString 
   Formatter::FormatAsAnsi(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2, const FormatArgument &argument3)
   {
      String value = Format(fmt, argument1, argument2, argument3);

      return Unicode::ToANSI(value);
   }



   AnsiString 
   Formatter::FormatAsAnsi(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2,const FormatArgument &argument3, const FormatArgument &argument4)
   {
      String value = Format(fmt, argument1, argument2, argument3, argument4);

      return Unicode::ToANSI(value);
   }

   AnsiString 
   Formatter::FormatAsAnsi(const AnsiString &fmt, const FormatArgument &argument1, const FormatArgument &argument2,const FormatArgument &argument3, const FormatArgument &argument4, const FormatArgument &argument5)
   {
      // All five. Up to 6.3.7 this passed the first, the second and the fifth to
      // the three-argument Format, so that {2} was the fifth argument and {3} and
      // {4} were left in the text.
      String value = Format(fmt, argument1, argument2, argument3, argument4, argument5);

      return Unicode::ToANSI(value);
   }


   String 
   Formatter::Format(const String &fmt, const std::list<FormatArgument> &arguments)
   {
      String result = fmt;

#ifdef _DEBUG
         
         /*
            Validate that the number of placeholders in the format matches
            the number of arguments.
         */

         std::set<int> placeholders = GetPlaceholders(fmt);

         if (arguments.size() != placeholders.size())
         {
            return "";
         }
#endif

     int placeholderIndex = 0;
     for(FormatArgument argument : arguments)
     {
        String s;
        s.Format(_T("{%d}"), placeholderIndex);
        
        result.Replace(s, argument.GetValue());

        placeholderIndex++;
     }

     return result;
   }
   

   std::set<int>
   Formatter::GetPlaceholders(const String &format)
   {
      std::set<int> result;

      int length = format.GetLength();
      int placeholderStartPos = -1;

      for (int pos = 0; pos < length; pos++)
      {
         wchar_t c = format[pos];

         if (c == '{')
            placeholderStartPos = pos;
         else if (c == '}')
         {
            if (placeholderStartPos == -1)
            {
               std::set<int> emptyResult;
               return emptyResult;
            }

            // extract the placeholder position.
            int placeholderLength = pos - placeholderStartPos-1;
            String val = format.Mid(placeholderStartPos+1, placeholderLength);

            int idx = _ttoi(val);

            if (result.find(idx) == result.end())
               result.insert(idx);

            placeholderStartPos = -1;
         }
      }

      if (placeholderStartPos != -1)
      {
         std::set<int> emptyResult;
         return emptyResult;
      }

      return result;
   }

   void
   FormatterTester::Test()
   {
      // Single argument
      if (Formatter::Format(_T("{0}"), 25) != _T("25"))
         HM_TEST_FAILED();
      
      // Argument sorting.
      if (Formatter::Format(_T("{0} {1}"), 25, 100) != _T("25 100"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{1} {0}"), 25, 100) != _T("100 25"))
         HM_TEST_FAILED();

      // Ansi String parameters.
      if (Formatter::Format(_T("{0} {1}"), 25, "Test") != _T("25 Test"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}, {1}"), "String 1", "String 2") != _T("String 1, String 2"))
         HM_TEST_FAILED();

      // Explicit ansi strings.
      if (Formatter::Format(_T("{0} {1}"), 25, AnsiString("Test")) != _T("25 Test"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}, {1}"), AnsiString("String 1"), AnsiString("String 2")) != _T("String 1, String 2"))
         HM_TEST_FAILED();

      // Unicode strings
      if (Formatter::Format(_T("{0} {1}"), 25, _T("Test")) != _T("25 Test"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}, {1}"), _T("String 1"), _T("String 2")) != _T("String 1, String 2"))
         HM_TEST_FAILED();

      // Explicit unicode strings
      if (Formatter::Format(_T("{0} {1}"), 25, String("Test")) != _T("25 Test"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}, {1}"), String("String 1"), String("String 2")) != _T("String 1, String 2"))
         HM_TEST_FAILED();

      // Invalid format strings. These are only tested in debug mode since
      // format string validation isn't made in release for performance reasons.
#ifdef DEBUG

      if (Formatter::Format(_T("{0"), "Test") != _T(""))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("}"), "Test") != _T(""))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}, {1}"), "Test") != _T(""))
         HM_TEST_FAILED();
#endif

      // Large numeric values.
      if (Formatter::Format(_T("{0}"), INT_MAX) != _T("2147483647"))
         HM_TEST_FAILED();

      if (Formatter::Format(_T("{0}"), UINT_MAX) != _T("4294967295"))
         HM_TEST_FAILED();

      // UINT64 max. Suffixed, because a literal too large for a long long is an
      // unsigned long long on MSVC and an __int128 on GCC, and the second matches
      // no FormatArgument constructor better than any other.
      if (Formatter::Format(_T("{0}"), 18446744073709551614ULL) != _T("18446744073709551614"))
         HM_TEST_FAILED();

      // INT64 max. The LL suffix is what keeps this an __int64 everywhere: on Windows
      // a long is 32 bits, so the unsuffixed literal was already a long long, but on
      // an LP64 build it fits in a long - and long is not one of FormatArgument's
      // constructor types and is no closer to one of them than to the others.
      if (Formatter::Format(_T("{0}"), 9223372036854775807LL) != _T("9223372036854775807"))
         HM_TEST_FAILED();

      // Max parameter count.
      if (Formatter::Format(_T("{0}, {1}, {2}, {3}"), "Test", 1, _T("Test"), 0xff) != _T("Test, 1, Test, 255"))
         HM_TEST_FAILED();

      // Boolean
      if (Formatter::Format(_T("{0} {1}"), true, false) != _T("True False"))
         HM_TEST_FAILED();

      // FormatAsAnsi with each count of arguments, every one in its place: up to
      // 6.3.7 the five-argument form gave "one|two|five|{3}|{4}".
      if (Formatter::FormatAsAnsi("{0}", "one") != AnsiString("one"))
         HM_TEST_FAILED();
      if (Formatter::FormatAsAnsi("{1}|{0}", "one", "two") != AnsiString("two|one"))
         HM_TEST_FAILED();
      if (Formatter::FormatAsAnsi("{0}|{1}|{2}", "one", "two", "three") != AnsiString("one|two|three"))
         HM_TEST_FAILED();
      if (Formatter::FormatAsAnsi("{0}|{1}|{2}|{3}", "one", "two", "three", "four") != AnsiString("one|two|three|four"))
         HM_TEST_FAILED();
      if (Formatter::FormatAsAnsi("{0}|{1}|{2}|{3}|{4}", "one", "two", "three", "four", "five") != AnsiString("one|two|three|four|five"))
         HM_TEST_FAILED();
      if (Formatter::FormatAsAnsi("{4}{3}{2}{1}{0}", 1, 2, 3, 4, 5) != AnsiString("54321"))
         HM_TEST_FAILED();

      // Unicode::ToANSI, which FormatAsAnsi ends in, gives the same bytes on both
      // platforms: a character up to U+00FF as its byte, and nothing at all for a
      // text with any character above it. Up to 6.3.7 Linux gave nothing for any
      // character outside ASCII.
      struct AnsiCase
      {
         const wchar_t *text;
         const char *bytes;
      };

      const wchar_t agrave[] = { 0x00E0, 0 };
      const wchar_t inLatin1[] = { L'a', 0x00E0, L'b', 0x00FF, 0x0080, 0x009F, 0x00A0, 0 };
      const wchar_t idot[] = { 0x0130, 0 };
      const wchar_t euro[] = { L'a', 0x00E0, 0x20AC, L'b', 0 };
      const wchar_t cjk[] = { L'x', 0x65E5, 0 };
      const wchar_t stopped[] = { L'a', L'b', 0, 0x00E0, 0 };
      const AnsiCase ansiCases[] =
      {
         { L"plain ASCII", "plain ASCII" },
         { L"", "" },
         { agrave, "\xE0" },
         { inLatin1, "a\xE0" "b\xFF\x80\x9F\xA0" },
         { idot, "" },
         { euro, "" },
         { cjk, "" },
         { stopped, "ab" },
      };

      for (const AnsiCase &ansiCase : ansiCases)
      {
         const AnsiString bytes = Unicode::ToANSI(String(ansiCase.text));
         if (bytes != AnsiString(ansiCase.bytes))
            throw std::logic_error(std::string("FormatterTester: Unicode::ToANSI gave ") + std::to_string(bytes.GetLength()) +
               " byte(s) where " + std::to_string(strlen(ansiCase.bytes)) + " were expected");
      }

      // A character U+FFFF cannot hold, where wchar_t can hold it (four bytes on
      // Linux; on Windows it is a surrogate pair, each half above U+00FF): nothing.
      String beyondBmp(_T("a"));
#ifdef HM_PLATFORM_POSIX
      beyondBmp += (wchar_t) 0x1F600;
#else
      beyondBmp += (wchar_t) 0xD83D;
      beyondBmp += (wchar_t) 0xDE00;
#endif
      if (!Unicode::ToANSI(beyondBmp).IsEmpty())
         HM_TEST_FAILED();

      // And through FormatAsAnsi, a name above U+00FF empties the whole text as
      // ToANSI does, where one within Latin-1 is its bytes.
      if (Formatter::FormatAsAnsi("file {0}", String(agrave)) != AnsiString("file \xE0"))
         HM_TEST_FAILED();
      if (!Formatter::FormatAsAnsi("file {0}", String(idot)).IsEmpty())
         HM_TEST_FAILED();
   }
}