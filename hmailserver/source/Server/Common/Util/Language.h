// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // One translation of the strings COM's Language.String answers - to a
   // script or another COM client; the Control Panel has translations of its
   // own. See Languages.h for who reads one and when a new one is made.
   class Language
   {
   public:
      // english.ini's strings by their number: what a translation's numbered
      // lines are mapped to English through.
      typedef std::map<int, String> EnglishStrings;

      Language(const String &sName, bool isDownloded, std::shared_ptr<const EnglishStrings> english);
      ~Language(void);

      String GetString(const String &sEnglishString);
      String GetName() const {return name_; }

      bool GetIsDownloded() const {return is_downloaded_; }

      // english.ini, read whole into a map of its own. Languages::Load reads it
      // once per load and hands it to every language it makes.
      static std::shared_ptr<const EnglishStrings> LoadEnglish();

   private:
      typedef std::map<String, String> Strings;

      // The translation, read on the first GetString and then kept.
      std::shared_ptr<const Strings> Strings_();

      // The language's file read into a map of its own. Holds no lock.
      std::shared_ptr<const Strings> Read_() const;

      static std::pair<int, String> GetString_(const String &sLine);
      static void CleanString_(String &sText);

      const String name_;
      const bool is_downloaded_;
      const std::shared_ptr<const EnglishStrings> english_;

      // Null until the first GetString has read the file, then the translation,
      // never changed after. The pointer is copied and published under
      // strings_mutex_, a leaf held for that and nothing else: the file is read
      // with no lock held. COM answers Language.String on several threads at
      // once, and until 29 September 2026 the first calls each read the file
      // into one map while the others looked strings up in it, with a plain
      // bool saying whether it had been read.
      std::shared_ptr<const Strings> strings_;
      boost::mutex strings_mutex_;
   };
}