// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Language;

   // The translations in the Languages directory that [GUILanguages]
   // ValidLanguages names, for COM's Languages and Language objects - a
   // script's or another COM client's. The Control Panel does not ask for them
   // (its translations are its own, Services/Loc.cs), nothing else in the server
   // reads them, and on Linux, where COM is not built, nothing reads them at all.
   //
   // Load runs in Application::InitInstance: at the start, and again at every
   // reinitialise - COM's Application.Reinitialize, the REST route's thread of
   // its own, the Reinitializator's thread after a restore or a certificate
   // renewal, SIGHUP on Linux - while COM goes on answering other callers on
   // other threads. So Load builds the whole set, each language new and
   // English read afresh, in a local, and publishes it with a swap under
   // languages_mutex_; the getters copy the pointer under it and read the set,
   // which is never changed once published, with no lock held. The mutex is a
   // leaf held for the copy or the swap only: no file is read, and no language
   // released, under it. A Language a caller already holds is the previous
   // load's and stays whole: it holds the English it was made with, and the
   // directory that English was read from, where it reads its translation on its
   // first use even when ProgramFolder has changed since (Language.h).
   class Languages : public Singleton<Languages>
   {
   public:
      Languages(void);
      ~Languages(void);

      void Load();

      std::shared_ptr<Language> GetLanguage(const String &sLanguage);
      std::shared_ptr<Language> GetLanguage(int index);
      size_t GetCount();

   private:

      typedef std::map<String, std::shared_ptr<Language> > LanguageMap;

      std::shared_ptr<const LanguageMap> Published_();

      bool IsValidLangauge_(const String &sLanguage) const;

      std::shared_ptr<const LanguageMap> languages_;
      boost::mutex languages_mutex_;
   };
}