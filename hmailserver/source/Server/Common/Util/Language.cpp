// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "Unicode.h"
#include "Charset.h"

#include "Language.h"
#include "FileUtilities.h"

#pragma warning (disable: 4566)

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Language::Language(const String &sName, bool isDownloded, std::shared_ptr<const EnglishStrings> english) :
      name_(sName),
      is_downloaded_(isDownloded),
      english_(english)
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
   Language::LoadEnglish()
   {
      std::shared_ptr<EnglishStrings> english = std::make_shared<EnglishStrings>();

      String sEnglishLanguageFile = IniFileSettings::Instance()->GetLanguageDirectory() + FileUtilities::PathSeparator + "english.ini";
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

      String sTranslatedLanguageFile = IniFileSettings::Instance()->GetLanguageDirectory() + FileUtilities::PathSeparator + name_ + ".ini";
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
}