// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class DALRecordset;
   class Property;
      
   class PropertySet
   {
   public:
      PropertySet(void);
      ~PropertySet(void);

      void Refresh();


      long GetLong(const String &sPropertyName);
      bool GetBool(const String &sPropertyName);
      String GetString(const String &sPropertyName);

      void SetLong(const String &sPropertyName, long lValue);
      void SetBool(const String &sPropertyName, bool lValue);
      void SetString(const String &sPropertyName, const String &lValue);

      // Creates the property, with the given value, when the database has no row
      // for it; does nothing at all when it has one.
      //
      // This is how a setting added after a database was created gets a row
      // without a schema step of its own. It matters because of what GetProperty_
      // does when a property is missing: it reports HM5015 on EVERY read, so a
      // feature whose settings have no rows fills the error log of an installation
      // that has never used it. Called from Configuration::Load, on the one thread
      // that is running at that point, before any other thread can read the set -
      // which is why this may write to items_ and the getters may not.
      void EnsureLong(const String &sPropertyName, long lValue);

      // Whether the set holds a row of this name - without the HM5015 a read
      // of a missing name reports, for a caller that is asking rather than
      // assuming.
      bool Exists(const String &sPropertyName) const;
      void EnsureString(const String &sPropertyName, const String &sValue);

      bool Contains(const String &sPropertyName) const;

      /// <summary>
      /// Every property the set holds, as name -> (settinginteger, settingstring).
      ///
      /// Both columns, because the table does not say which of them a property
      /// uses and this set does not know either: a long property leaves the string
      /// empty and a string property leaves the number 0, so a caller comparing a
      /// property with its shipped default compares both and reports whichever has
      /// moved. Reading the whole set rather than asking for names one at a time
      /// also avoids GetProperty_'s HM5015 for a name that is not there, which is
      /// the right report for a read of a setting and the wrong one for a walk.
      ///
      /// The string of a crypted property (the relayer password) is the DECRYPTED
      /// value, as GetString gives it - Refresh decrypts on load. A caller putting
      /// these in front of anybody masks by name, which is what
      /// IniSettingStore::IsSecretName is for.
      /// </summary>
      void GetAll(std::map<String, std::pair<long, String> > &values) const;

      bool XMLStore(XNode *pBackupNode);
      bool XMLLoad(XNode *pBackupNode);

   private:

      void OnPropertyChanged_(std::shared_ptr<Property> pProperty);

      std::shared_ptr<Property> GetProperty_(const String &sPropertyName);

      bool IsCryptedProperty_(const String &sPropertyName);
      std::map<String, std::shared_ptr<Property> > items_;
   };
}
