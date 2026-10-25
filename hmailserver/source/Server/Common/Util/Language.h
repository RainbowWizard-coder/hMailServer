// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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

      // directory is the Languages directory english was read from: the
      // translation is read from the same one, on the first GetString, however
      // long after. Languages::Load reads the directory once and hands it to
      // English and to every language it makes.
      Language(const String &sName, bool isDownloded, std::shared_ptr<const EnglishStrings> english, const String &directory);
      ~Language(void);

      String GetString(const String &sEnglishString);
      String GetName() const {return name_; }

      bool GetIsDownloded() const {return is_downloaded_; }

      // english.ini in directory, read whole into a map of its own. Languages::Load
      // reads it once per load and hands it to every language it makes.
      static std::shared_ptr<const EnglishStrings> LoadEnglish(const String &directory);

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

      // The Languages directory as it was when this language was made, which is
      // the one english_ came from. Until 6.3.6 the first GetString asked
      // IniFileSettings for the directory then: a ProgramFolder changed in
      // hMailServer.INI and read again (a reinitialise) between the load and a
      // language's first use - a script or a COM client holding a Language across
      // the reinitialise - had it read another directory's translation and map it
      // through the first one's English, whose numbered strings need not agree.
      const String directory_;

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

   // A Language reads its translation from the directory it was made for, English's,
   // whatever the configuration names by its first use: two scratch Languages
   // directories whose English numbers the same words the other way round, and a
   // language made for each, neither of them the directory the configuration names.
   // Throws std::logic_error with a sentence. ClassTester runs it inside the
   // service (Utilities.RunTestSuite), language-tests on Linux with a ProgramFolder
   // changed under a loaded set as well.
   class LanguageTester
   {
   public:
      void Test();

      // Writes english.ini and xx.ini into directory (made if missing): English
      // numbering "File" and "Exit" 1 and 2, or 2 and 1 when swapped, and the
      // translation giving each number the text translated[0] and translated[1].
      static void WriteLanguages(const String &directory, bool swapped, const char *translated1, const char *translated2);
   };
}