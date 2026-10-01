// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "Languages.h"
#include "Language.h"
#include "FileInfo.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Languages::Languages(void) :
      languages_(std::make_shared<LanguageMap>())
   {

   }

   Languages::~Languages(void)
   {
   }

   void 
   Languages::Load()
   {
      // Built whole with no lock held - English and the directory are read here -
      // and published below. See Languages.h. The directory is asked for once, and
      // English, the list of files and every language made are of that one: each
      // language reads its translation from it on its first use, whatever
      // ProgramFolder says by then (Language.h, directory_).
      const String directory = IniFileSettings::Instance()->GetLanguageDirectory();
      std::shared_ptr<const Language::EnglishStrings> english = Language::LoadEnglish(directory);

      std::shared_ptr<LanguageMap> languages = std::make_shared<LanguageMap>();

      std::vector<FileInfo> languageFiles =
         FileUtilities::GetFilesInDirectory(directory, "^.*[.]ini$");

      for (FileInfo &fileInfo : languageFiles)
      {
         String sFileName = fileInfo.GetName();

         int dotPos = sFileName.Find(_T("."));
         if (dotPos <= 0)
            continue;

         String sLanguage = sFileName.Mid(0, dotPos);
         String sFormattedLanguage = sLanguage;
         sFormattedLanguage.ToLower();

         if (!IsValidLangauge_(sFormattedLanguage))
            continue;

         (*languages)[sFormattedLanguage] = std::make_shared<Language>(sFormattedLanguage, true, english, directory);
      }

      // The set this replaces is released when `previous` goes, after the lock.
      std::shared_ptr<const LanguageMap> previous = languages;
      {
         boost::lock_guard<boost::mutex> guard(languages_mutex_);
         languages_.swap(previous);
      }
   }

   std::shared_ptr<const Languages::LanguageMap>
   Languages::Published_()
   {
      boost::lock_guard<boost::mutex> guard(languages_mutex_);
      return languages_;
   }

   bool 
   Languages::IsValidLangauge_(const String &sLanguage) const
   {
      std::vector<String> validLanguages = IniFileSettings::Instance()->GetValidLanguages();
      auto iter = validLanguages.begin();
      auto iterEnd = validLanguages.end();

      for (; iter != iterEnd; iter++)
      {
         String validLanguage = (*iter);

         if (validLanguage.CompareNoCase(sLanguage) == 0)
            return true;
      }

      return false;

   }

   size_t
   Languages::GetCount()
   {
      return Published_()->size();
   }

   std::shared_ptr<Language>
   Languages::GetLanguage(const String &sLanguage)
   {
      String sFormattedLanguage = sLanguage;
      sFormattedLanguage.ToLower();

      std::shared_ptr<const LanguageMap> languages = Published_();

      auto iterLanguage = languages->find(sFormattedLanguage);
      if (iterLanguage != languages->end())
         return (*iterLanguage).second;

      std::shared_ptr<Language> pEmpty;
      return pEmpty;
   }

   std::shared_ptr<Language>
   Languages::GetLanguage(int index)
   {
      std::shared_ptr<const LanguageMap> languages = Published_();

      int current = 0;
      for (const auto &language : *languages)
      {
         if (current == index)
            return language.second;

         current++;
      }

      std::shared_ptr<Language> empty;
      return empty;
   }
}