// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "Unicode.h"
#include "Charset.h"

#include "Language.h"
#include "FileUtilities.h"
#include "GUIDCreator.h"

#include <boost/filesystem.hpp>

#pragma warning (disable: 4566)

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Language::Language(const String &sName, bool isDownloded, std::shared_ptr<const EnglishStrings> english, const String &directory) :
      name_(sName),
      is_downloaded_(isDownloded),
      english_(english),
      directory_(directory)
   {
   }

   Language::~Language(void)
   {
   }

   String
   Language::GetString(const String &sEnglishString)
   {
      // The translation is never changed once it is published, so it is read
      // with no lock held.
      std::shared_ptr<const Strings> strings = Strings_();

      auto iterString = strings->find(sEnglishString);
      if (iterString == strings->end() || iterString->second.IsEmpty())
         return sEnglishString;

      return iterString->second;
   }

   std::shared_ptr<const Language::Strings>
   Language::Strings_()
   {
      {
         boost::lock_guard<boost::mutex> guard(strings_mutex_);
         if (strings_)
            return strings_;
      }

      // Not read yet. Read with no lock held; two threads that get here at once
      // each read the file, and the first to finish publishes what it read, which
      // every later call is given.
      std::shared_ptr<const Strings> read = Read_();

      boost::lock_guard<boost::mutex> guard(strings_mutex_);
      if (!strings_)
         strings_ = read;

      return strings_;
   }

   std::shared_ptr<const Language::EnglishStrings>
   Language::LoadEnglish(const String &directory)
   {
      std::shared_ptr<EnglishStrings> english = std::make_shared<EnglishStrings>();

      String sEnglishLanguageFile = directory + FileUtilities::PathSeparator + "english.ini";
      String sEnglishContents = FileUtilities::ReadCompleteTextFile(sEnglishLanguageFile);

      std::vector<String> vecEnglishStrings = StringParser::SplitString(sEnglishContents, "\r\n");

      for (const String &sString : vecEnglishStrings)
      {
         if (sString.Left(7) != _T("String_"))
            continue;

         std::pair<int, String> pair = GetString_(sString);

         (*english)[pair.first] = pair.second;
      }

      return english;
   }

   std::shared_ptr<const Language::Strings>
   Language::Read_() const
   {
      std::shared_ptr<Strings> strings = std::make_shared<Strings>();

      // The directory English was read from, not the one the configuration names
      // now (Language.h, directory_).
      String sTranslatedLanguageFile = directory_ + FileUtilities::PathSeparator + name_ + ".ini";
      String sTranslatedContents = FileUtilities::ReadCompleteTextFile(sTranslatedLanguageFile);

      if (!english_)
         return strings;

      std::vector<String> vecTranslatedStrings = StringParser::SplitString(sTranslatedContents, "\r\n");

      for (const String &sString : vecTranslatedStrings)
      {
         std::pair<int, String> translatedPair = GetString_(sString);

         auto englishPair = english_->find(translatedPair.first);
         if (englishPair != english_->end())
            (*strings)[englishPair->second] = translatedPair.second;
      }

      return strings;
   }

   std::pair<int, String> 
   Language::GetString_(const String &sLine)
   {
      int iValueStart = 7;
      int iEqualsPos = sLine.Find(_T("="));
      int iValueLen = iEqualsPos - iValueStart;

      String sValue = sLine.Mid(iValueStart, iValueLen);
      String sText = sLine.Mid(iEqualsPos+1);

      int iValue = _ttoi(sValue);

      return std::make_pair(iValue, sText);
   }

   void 
   Language::CleanString_(String &sText) 
   {
      // Remove lading "&"
      if (sText.Left(1) == _T("&"))
         sText = sText.Mid(1);

      // Remove trailing "..."
      if (sText.Right(3) == _T("..."))
         sText = sText.Left(sText.GetLength() - 3);

   }

   void
   LanguageTester::WriteLanguages(const String &directory, bool swapped, const char *translated1, const char *translated2)
   {
      boost::system::error_code error;
      boost::filesystem::create_directories(boost::filesystem::path(std::wstring(directory.c_str())), error);
      if (error)
         throw std::logic_error("LanguageTester: a scratch Languages directory could not be made");

      const AnsiString english = swapped
         ? AnsiString("[Strings]\r\nString_1=Exit\r\nString_2=File\r\n")
         : AnsiString("[Strings]\r\nString_1=File\r\nString_2=Exit\r\n");
      const AnsiString translation = AnsiString("[Strings]\r\nString_1=") + translated1 + "\r\nString_2=" + translated2 + "\r\n";

      if (!FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + _T("english.ini"), english) ||
          !FileUtilities::WriteToFile(directory + FileUtilities::PathSeparator + _T("xx.ini"), translation))
         throw std::logic_error("LanguageTester: a scratch language file could not be written");
   }

   void
   LanguageTester::Test()
   {
      namespace fs = boost::filesystem;

      boost::system::error_code error;
      String unique = GUIDCreator::GetGUID();
      unique.Replace(_T("{"), _T(""));
      unique.Replace(_T("}"), _T(""));
      const fs::path scratch = fs::temp_directory_path(error) / fs::path(std::wstring(_T("hm-language-tests-") + unique));
      if (error)
         throw std::logic_error("LanguageTester: the system's temp directory could not be found");

      // Removed however the test ends.
      struct Scratch
      {
         explicit Scratch(const fs::path &path) : path_(path) {}
         ~Scratch() { boost::system::error_code ignored; fs::remove_all(path_, ignored); }
         fs::path path_;
      } removeAtTheEnd(scratch);

      const String first = String((scratch / "first").wstring());
      const String second = String((scratch / "second").wstring());
      WriteLanguages(first, false, "Arkiv-first", "Avsluta-first");
      WriteLanguages(second, true, "Avsluta-second", "Arkiv-second");

      // Each language made for its own directory and English, neither of them the
      // directory the configuration names, which has no xx.ini: a language that
      // read the configured directory would answer the English words.
      Language inFirst(_T("xx"), true, Language::LoadEnglish(first), first);
      Language inSecond(_T("xx"), true, Language::LoadEnglish(second), second);

      if (inFirst.GetString(_T("File")) != _T("Arkiv-first") || inFirst.GetString(_T("Exit")) != _T("Avsluta-first"))
         throw std::logic_error("LanguageTester: a language did not read its translation from the directory it was made for");
      if (inSecond.GetString(_T("File")) != _T("Arkiv-second") || inSecond.GetString(_T("Exit")) != _T("Avsluta-second"))
         throw std::logic_error("LanguageTester: a language did not map its translation through its own directory's English");

      // A word neither English has is answered as it was asked.
      if (inFirst.GetString(_T("Help")) != _T("Help"))
         throw std::logic_error("LanguageTester: a word without a translation was not answered as it was asked");
   }
}