// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// String::Format, and the Logger's default format made with it, over text outside
// ASCII, as a program of its own for the POSIX build - the counterpart of
// live-log-tests.
//
// On POSIX every wide format reaches HMPlatform::FormatWideForGlibc
// (platform/hm_platform.h), which makes the text glibc's vswprintf makes without
// the C library's multibyte form. Up to 6.3.7 the formats went to vswprintf
// itself, and musl's (Alpine) writes each wide character through the C locale's
// multibyte form, which carries nothing above U+007F but 0xDF80 to 0xDFFF: a
// format whose text held any other such character failed, and String::Format
// gave nothing - on Alpine no log line held a character outside ASCII, and a line
// that would have was not written at all. This program asks for every value a
// wchar_t can hold there - U+00E0, U+0130, U+20AC and U+1F600, musl's
// byte-transparent 0xDF80 to 0xDFFF, the other surrogates, U+FFFE, U+10FFFF and
// the values above it - through %s, %c and the format's own text, with flags,
// widths and precisions, and holds the text to what glibc gives; it holds the
// formatter to the C library's own vswprintf over a table of formats, every case
// with glibc, the cases that library can format elsewhere; and it logs lines
// holding those characters through the Logger - the default format's two
// shapes, the event log and the backup log - and reads them back, from the live
// log and from the files.
//
// A configuration file of its own in a directory of its own under the system's
// temp directory, which it names for every directory, so that the log files land
// there; the directory is removed again. No database and no network. A line for
// each check that fails, then one for the whole, and a non-zero exit on a failure.
// Well under a second.

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "tester_scratch.h"

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/Logger.h"

namespace
{
   int failures = 0;
   int checks = 0;

   void Check(bool condition, const std::string &what)
   {
      checks++;
      if (!condition)
      {
         printf("FAIL  %s\n", what.c_str());
         failures++;
      }
   }

   // A wide character's value, its code unit as an unsigned number.
   unsigned int CodeUnit(const std::wstring &text, size_t i)
   {
      return (unsigned int) text[i];
   }

   // U+ and a value in hexadecimal, of four digits at least.
   std::string CodePointName(unsigned int value)
   {
      char name[16];
      const int written = snprintf(name, sizeof(name), "U+%04X", value);
      if (written <= 0 || (size_t) written >= sizeof(name))
         return "U+?";
      return std::string(name, (size_t) written);
   }

   // The text as ASCII, a character outside it as its code point; a long one cut
   // after its first 40 characters, with its length.
   std::string Show(const std::wstring &text)
   {
      std::string shown;
      for (size_t i = 0; i < text.size(); i++)
      {
         if (i == 40)
         {
            shown += "... (" + std::to_string(text.size()) + " characters)";
            break;
         }
         const unsigned int unit = CodeUnit(text, i);
         if (unit >= 0x20 && unit < 0x7F)
            shown.push_back((char) unit);
         else
            shown += "<" + CodePointName(unit) + ">";
      }
      return shown;
   }

   std::wstring Wide(const HM::String &text)
   {
      return std::wstring(text.c_str(), (size_t) text.GetLength());
   }

   // What String::Format calls - _vscwprintf for the length, then _vstprintf into a
   // buffer of that length, here through vswprintf_s, the layer's name for the same
   // call - and the C library's own vswprintf, over the same arguments and the same
   // rewritten format.
   int Ours(std::wstring &out, const wchar_t *format, ...)
   {
      va_list arguments;
      va_list copy;
      va_start(arguments, format);
      va_copy(copy, arguments);
      out.clear();
      const int needed = _vscwprintf(format, arguments);
      int written = -1;
      if (needed >= 0)
      {
         std::vector<wchar_t> buffer((size_t) needed + 1, L'\0');
         written = vswprintf_s(buffer.data(), buffer.size(), format, copy);
         if (written >= 0)
            out.assign(buffer.data(), (size_t) written);
         if (written != needed)
            written = -2;              // measured one length and wrote another
      }
      va_end(copy);
      va_end(arguments);
      return written;
   }

   int Libc(std::wstring &out, const wchar_t *format, ...)
   {
      std::vector<wchar_t> buffer(1 << 20, L'\0');
      va_list arguments;
      va_start(arguments, format);
      const int written = ::vswprintf(buffer.data(), buffer.size(), HMPlatform::WideFormatForGlibc(format).c_str(), arguments);
      va_end(arguments);
      out.assign(buffer.data(), written > 0 ? (size_t) written : 0);
      return written;
   }

#if defined(__GLIBC__)
   const bool kGlibc = true;
#else
   const bool kGlibc = false;
#endif

   int compared = 0;
   int libraryRefused = 0;

   // With glibc the two must agree in every case, a refusal (-1) included; with
   // another C library, in every case that library formats at all.
   template <typename... Arguments>
   void Same(const char *what, const wchar_t *format, Arguments... arguments)
   {
      std::wstring ours, libc;
      const int oursWritten = Ours(ours, format, arguments...);
      const int libcWritten = Libc(libc, format, arguments...);
      if (!kGlibc && libcWritten < 0)
      {
         printf("      the C library does not format it: %s\n", what);
         libraryRefused++;
         return;
      }
      compared++;
      Check(oursWritten == libcWritten && ours == libc,
         std::string("as the C library formats it: ") + what + " gives " + std::to_string(oursWritten) + " '" + Show(ours) +
         "', the C library " + std::to_string(libcWritten) + " '" + Show(libc) + "'");
   }

   HM::String Formatted(const wchar_t *format, ...)
   {
      HM::String text;
      va_list arguments;
      va_start(arguments, format);
      text.FormatV(format, arguments);
      va_end(arguments);
      return text;
   }

   std::string ReadBytes(const HM::String &path)
   {
      std::ifstream in(HM::AnsiString(path).c_str(), std::ios::in | std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   // Each line as it is made: a C library whose vswprintf ends the process on a
   // null %s (musl's) must not take the lines before it with it.
   if (setvbuf(stdout, nullptr, _IOLBF, 0) != 0)
      printf("      stdout could not be made line-buffered: the lines before an end of the process may be lost\n");
   printf("      the C library: %s\n", kGlibc ? "glibc" : "not glibc");

   // Every value a wchar_t holds that is not ASCII, in each of the places a wide
   // format carries one: an argument of %s, of %c, and the format's own text.
   const unsigned int values[] = { 0x80, 0xE0, 0xFF, 0x130, 0x20AC, 0xFFFD, 0x1F600, 0xD800, 0xDBFF, 0xDC00, 0xDF7F,
                                   0xDF80, 0xDFE0, 0xDFFF, 0xFFFE, 0x10FFFF, 0x110000, 0x7FFFFFFF, 0xFFFFFFFF };
   for (unsigned int value : values)
   {
      const wchar_t c = (wchar_t) value;
      const std::string name = CodePointName(value);

      const std::wstring argument = std::wstring(L"x") + c + L"y";
      Check(Wide(Formatted(_T("[%s] %d"), argument.c_str(), 7)) == L"[" + argument + L"] 7",
         std::string("%s of a string holding ") + name + " gives it");
      Check(Wide(Formatted(_T("[%c]"), c)) == std::wstring(L"[") + c + L"]", std::string("%c of ") + name + " gives it");

      const std::wstring format = std::wstring(L"<") + c + L"%d>";
      Check(Wide(Formatted(format.c_str(), 5)) == std::wstring(L"<") + c + L"5>", std::string(name) + " in the format's own text is kept");

      HM::String appended(_T("a"));
      appended.AppendFormat(_T("%s"), argument.c_str());
      Check(Wide(appended) == L"a" + argument, std::string("AppendFormat of a string holding ") + name + " gives it");
   }

   // Flags, widths and precisions over such a string: glibc's text.
   const std::wstring word = L"\u00e0\u0130\u20ac\U0001F600e";
   Check(Wide(Formatted(_T("[%7s]"), word.c_str())) == L"[  " + word + L"]", "a width pads on the left");
   Check(Wide(Formatted(_T("[%-7s]"), word.c_str())) == L"[" + word + L"  ]", "the - flag pads on the right");
   Check(Wide(Formatted(_T("[%07s]"), word.c_str())) == L"[  " + word + L"]", "the 0 flag pads a string with spaces");
   Check(Wide(Formatted(_T("[%.2s]"), word.c_str())) == L"[\u00e0\u0130]", "a precision cuts a string to as many characters");
   Check(Wide(Formatted(_T("[%*.*s]"), 6, 3, word.c_str())) == L"[   \u00e0\u0130\u20ac]", "a * width and precision are read");
   Check(Wide(Formatted(_T("[%*s]"), -7, word.c_str())) == L"[" + word + L"  ]", "a negative * width pads on the right");
   Check(Wide(Formatted(_T("[%3c]"), (wchar_t) 0x20AC)) == L"[  \u20ac]", "a width pads a character");
   Check(Wide(Formatted(_T("[%s]"), (const wchar_t *) nullptr)) == L"[(null)]", "a null string is (null)");
   Check(Wide(Formatted(_T("[%.3s]"), (const wchar_t *) nullptr)) == L"[]", "  ... and nothing under a precision below six");
   Check(Wide(Formatted(_T("%s %d %Iu %I64d %.2f %x"), word.c_str(), -12, (size_t) 5000000000ULL, -5000000000LL, 2.5, 255U)) ==
      word + L" -12 5000000000 -5000000000 2.50 ff", "numbers beside such a string");
   Check(Wide(Formatted(_T("%hs|%S"), "narrow", "too")) == L"narrow|too", "a narrow string in a wide format");

   // A format whose text is longer than the 64 KB the formats were once measured in.
   const std::wstring longText(70000, L'\u00e0');
   Check(Wide(Formatted(_T("[%s]"), longText.c_str())).size() == 70002, "a text of 70,002 characters is made whole");

   HM::String kept(_T("before"));
   kept.Format(_T("%ls"), (const wchar_t *) L"after");
   Check(Wide(kept) == L"after", "Format replaces what the string held");

   // A format that ends inside a conversion is rewritten to its end and no further:
   // up to 6.3.7 both rewriters read on past the terminating NUL, which
   // AddressSanitizer reported of the case "a format ending in %" below.
   Check(HMPlatform::WideFormatForGlibc(L"[%") == L"[%" && HMPlatform::WideFormatForGlibc(L"%-5") == L"%-5" &&
         HMPlatform::WideFormatForGlibc(L"%I64") == L"%ll" && HMPlatform::WideFormatForGlibc(L"%l") == L"%l" &&
         HMPlatform::WideFormatForGlibc(L"%ll") == L"%ll" && HMPlatform::WideFormatForGlibc(L"%I") == L"%z",
      "the wide rewriter stops at a format's end inside a conversion");
   Check(HMPlatform::NarrowFormatForGlibc("[%") == "[%" && HMPlatform::NarrowFormatForGlibc("%-5") == "%-5" &&
         HMPlatform::NarrowFormatForGlibc("%I64") == "%ll" && HMPlatform::NarrowFormatForGlibc("%I") == "%z",
      "  ... and so does the narrow one");

   // The formatter against the C library's vswprintf, conversion by conversion.
   const wchar_t ascii[] = L"plain text";
   const wchar_t outside[] = L"caf\u00e9 \u0130 \u20ac \U0001F600";
   const char narrowAscii[] = "narrow";
   const char narrowHigh[] = { 'a', (char) 0xE0, 'b', 0 };
   int number = 42;
   Same("%d", L"%d|%i|%5d|%-5d|%05d|%+d|% d|%.3d", -7, 7, 42, 42, 42, 42, 42, 42);
   Same("%u %x %X %o and their flags", L"%u|%x|%X|%o|%#x|%#o|%08X", 4000000000U, 0xBEEFU, 0xBEEFU, 8U, 255U, 8U, 0xABCU);
   Same("the lengths", L"%hd|%hhd|%ld|%lu|%lld|%llu|%I64d|%I64u|%Iu|%zu|%jd|%td", (int) 70000, (int) 300, -5000000000L, 5000000000UL,
      -9000000000000000000LL, 18000000000000000000ULL, -5000000000LL, 5000000000ULL, (size_t) 5000000000ULL, (size_t) 77,
      (intmax_t) -1, (ptrdiff_t) -2);
   // Every integer is read as the type its length names and written as a long long:
   // h and hh cut to a short or a char, as printf cuts them, signed and unsigned.
   Same("h and hh cut a value", L"%hd|%hhd|%hu|%hhu|%hx|%hhX|%hi|%hhi|%ho", (int) -70000, (int) -129, 4000000000U, 511U,
      0xFFFF8000U, 0x1FFU, (int) 32768, (int) 128, 0x12345U);
   Same("the extremes of each length", L"%ld|%lu|%lld|%llu|%jd|%ju|%zd|%zu|%td|%tu", LONG_MIN, ULONG_MAX, LLONG_MIN, ULLONG_MAX,
      INTMAX_MIN, UINTMAX_MAX, (ssize_t) -5, SIZE_MAX, PTRDIFF_MIN, (size_t) 7);
   Same("a flag given twice", L"[%--5d|%++d|%  d|%00#8x|%##o]", 1, 2, 3, 255U, 8U);
   // Longer than the 128 characters the formatter writes a number into first.
   Same("a number wider than a short buffer", L"[%200d|%-150.140x|%130.3f|%.135d]", 42, 0xABCU, 2.5, -7);
   Same("the reals", L"%f|%.3f|%10.2f|%-10.2f|%e|%E|%g|%G|%a|%Lf|%lf", 3.14159, 2.0 / 3, -1.5, 1.5, 12345.678, 0.000123, 1e20,
      1e-5, 1.0, (long double) 2.5, 0.1);
   Same("a * width and precision", L"[%*d|%-*d|%.*f|%*.*s|%*s]", 6, 42, -6, 42, 2, 3.14159, 8, 3, ascii, -12, ascii);
   Same("%p", L"%p", (void *) &number);
   Same("%% and text", L"100%% of %s", ascii);
   Same("a string outside ASCII", L"[%s]", outside);
   Same("a string outside ASCII with a width and precision", L"[%20s|%-20s|%.4s|%*.*s]", outside, outside, outside, 12, 2, outside);
   Same("a character outside ASCII", L"[%c|%3c|%-3c]", (wchar_t) 0xE9, (wchar_t) 0x130, (wchar_t) 0x1F600);
   Same("the format's own text outside ASCII", L"\u00e0 \u0130 \u20ac \U0001F600 %d", 1);
   // musl's vswprintf reads a null %ls as a string, and the process ends: asked
   // of glibc alone, which writes (null).
   if (kGlibc)
      Same("a null wide string", L"[%s|%.3s|%.6s|%8s]", (const wchar_t *) nullptr, (const wchar_t *) nullptr, (const wchar_t *) nullptr,
         (const wchar_t *) nullptr);
   Same("a narrow string", L"[%hs|%S|%10hs|%-10hs|%.3hs]", narrowAscii, narrowAscii, narrowAscii, narrowAscii, narrowAscii);
   Same("a null narrow string", L"[%hs]", (const char *) nullptr);
   // musl cuts its (null) to a precision; glibc writes nothing under one below six.
   if (kGlibc)
      Same("  ... under a precision", L"[%.2hs|%.6hs]", (const char *) nullptr, (const char *) nullptr);
   Same("a narrow string with a byte above 0x7F", L"[%hs]", narrowHigh);
   Same("  ... cut before it", L"[%.1hs]", narrowHigh);
   Same("a NUL character", L"[%c]", (wchar_t) 0);
   Same("a long text", L"[%s]", longText.c_str());
   Same("an unknown conversion", L"[%y]%d", 5);
   Same("a % with a width", L"[%5%]");
   Same("a format ending in %", L"[%");
   printf("      %d of the C library's cases compared, %d it could not format itself\n", compared, libraryRefused);
   Check(compared >= 10, "the C library formats the ASCII cases itself, and they were compared");

   // The Logger, with a configuration of its own.
   boost::system::error_code error;
   const boost::filesystem::path scratch = HMTesterScratch::Make("hm-string-format-tests", error);
   if (error)
   {
      printf("FAIL  String::Format: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   const boost::filesystem::path file = scratch / "hMailServer.ini";
   const std::string directory = scratch.string();
   {
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << directory << "\n"
          << "DataFolder=" << directory << "\n"
          << "LogFolder=" << directory << "\n"
          << "TempFolder=" << directory << "\n"
          << "EventFolder=" << directory << "\n"
          << "DatabaseFolder=" << directory << "\n";
      if (!ini)
      {
         printf("FAIL  String::Format: %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 1;
      }
   }

   HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
   HM::IniFileSettings::Instance()->LoadSettings();

   HM::Logger *logger = HM::Logger::Instance();
   logger->SetLogMask(HM::Logger::LSEnabled | HM::Logger::LSApplication | HM::Logger::LSIMAP);
   logger->EnableLiveLogging(true);

   const std::wstring probe = L"format-probe \u00e0 \u0130 \u20ac \U0001F600";
   const std::wstring login = L"RECEIVED: A1 LOGIN \"geov\u00e0riant@example.test\" ***";
   logger->LogApplication(HM::String(probe.c_str()));
   logger->LogIMAPConversation(7, _T("127.0.0.1"), HM::String(login.c_str()));
   logger->LogEvent(HM::String((L"event-probe \u00e0 \u0130 \u20ac \U0001F600")));
   logger->LogBackup(HM::String((L"backup-probe \u00e0 \u0130 \u20ac \U0001F600")));

   const std::wstring live = Wide(logger->GetLiveLog());
   logger->EnableLiveLogging(false);
   const std::wstring applicationEnd = L"\t\"" + probe + L"\"\r\n";
   Check(live.find(L"\"APPLICATION\"\t") == 0 && live.find(applicationEnd) != std::wstring::npos,
      "the default format's line of an entry with no session holds the message whole: '" + Show(live) + "'");
   Check(live.find(L"\"IMAPD\"\t") != std::wstring::npos && live.find(L"\t7\t") != std::wstring::npos &&
         live.find(L"\t\"127.0.0.1\"\t\"" + login + L"\"\r\n") != std::wstring::npos,
      "  ... and of an IMAP line, with its session and host");

   // The files: the application log in the server's narrow code page - Latin-1, a
   // character above U+00FF written as '?' - and the event and backup logs in UTF-8.
   const std::string normal = ReadBytes(logger->GetCurrentLogFileName(HM::Logger::Normal));
   Check(normal.find("\t\"format-probe \xE0 ? ? ?\"\r\n") != std::string::npos,
      "the application log holds the line, in Latin-1 with '?' above U+00FF");
   Check(normal.find("\t7\t") != std::string::npos && normal.find("\"RECEIVED: A1 LOGIN \"geov\xE0riant@example.test\" ***\"\r\n") != std::string::npos,
      "  ... and the IMAP line, the name's \xC3\xA0 its byte");
   const std::string events = ReadBytes(logger->GetCurrentLogFileName(HM::Logger::Events));
   Check(events.find("\"event-probe \xC3\xA0 \xC4\xB0 \xE2\x82\xAC \xF0\x9F\x98\x80\"\r\n") != std::string::npos,
      "the event log holds its line, in UTF-8");
   const std::string backup = ReadBytes(logger->GetCurrentLogFileName(HM::Logger::Backup));
   Check(backup.find("\tbackup-probe \xC3\xA0 \xC4\xB0 \xE2\x82\xAC \xF0\x9F\x98\x80\r\n") != std::string::npos,
      "the backup log holds its line, in UTF-8");

   boost::filesystem::remove_all(scratch, error);

   if (failures > 0)
   {
      printf("FAIL  String::Format: %d of %d check(s) failed\n", failures, checks);
      return 1;
   }

   printf("ok    String::Format: %d checks - text outside ASCII through %%s, %%c and the format's own text, every value a wchar_t holds, "
          "as the C library formats it, and the Logger's lines read back\n", checks);
   return 0;
}
