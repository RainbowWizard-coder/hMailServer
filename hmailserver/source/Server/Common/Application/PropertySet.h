// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class DALRecordset;
   class Property;

   class PropertySet
   {
   public:

      /// <summary>
      /// Records, for as long as it lives, the name of every property read on this
      /// thread through GetLong, GetBool or GetString. It is how the REST settings
      /// rows are known by the hm_settings names they hold without a hand-kept
      /// table beside them (docs/EnvironmentConfiguration.md): a row's getter is the
      /// same code COM's getter runs, so the name it reads is the name the row is.
      /// Nests; the innermost recorder is the one written to.
      /// </summary>
      class ReadRecorder
      {
      public:
         ReadRecorder();
         ~ReadRecorder();

         /// <summary>Each name read, once, in the order first read.</summary>
         const std::vector<String> &Names() const { return names_; }

      private:
         ReadRecorder(const ReadRecorder &);
         ReadRecorder &operator=(const ReadRecorder &);

         friend class PropertySet;

         std::vector<String> names_;
         ReadRecorder *previous_;
      };

      PropertySet(void);
      ~PropertySet(void);

      // Reads every row of hm_settings into the set, in place of what it held, and
      // returns the properties read. It raises no change event: Configuration::Load
      // fills a set no other thread can see yet, and a handler reads settings of its
      // own through the set Configuration publishes, so the events are raised once
      // this one is published (RaiseChangeEvents).
      std::vector<std::shared_ptr<Property> > Refresh();

      // The change event of each property, as a setting changed by a write raises
      // it: how the logger's mask, device and format and the caches' settings take
      // the values a load read.
      void RaiseChangeEvents(const std::vector<std::shared_ptr<Property> > &properties);


      long GetLong(const String &sPropertyName);
      bool GetBool(const String &sPropertyName);
      String GetString(const String &sPropertyName);

      void SetLong(const String &sPropertyName, long lValue);
      void SetBool(const String &sPropertyName, bool lValue);
      void SetString(const String &sPropertyName, const String &lValue);

      // The server's own bookkeeping row - the records --apply-environment keeps
      // (EnvironmentSettings) - written as it is: not a setting, so no refusal, no
      // audit row and no change event. The row must exist.
      void SetBookkeeping(const String &sPropertyName, const String &sValue);

      // The same, for a record whose loss matters - the way back a setting profile
      // keeps (Profiles): the database written first, and the copy here changed
      // only when it took the value. False when the write failed, which the caller
      // must act on; nothing has changed then. A row that is not in the set yet is
      // inserted, and added only once it is stored.
      bool WriteBookkeeping(const String &sPropertyName, const String &sValue);

      // Creates the property, with the given value, when the database has no row
      // for it; does nothing at all when it has one.
      //
      // This is how a setting added after a database was created gets a row
      // without a schema step of its own. It matters because of what GetProperty_
      // does when a property is missing: it reports HM5015 on EVERY read, so a
      // feature whose settings have no rows fills the error log of an installation
      // that has never used it. Called from Configuration::Load on the set it is
      // building, before the set is published and any other thread can read it -
      // at a start and at a reinitialise alike - and by --apply-environment
      // (EnvironmentSettings) on the published set, in a process of its own that
      // opens no listener; the insert is made under items_mutex_ either way.
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

      // network-deadline-tests fills a set in memory, with no database and so no
      // rows behind it, for the TLS settings the server's HTTPS clients read
      // (Configuration.h says more).
      friend class NetworkDeadlineTests;

      void OnPropertyChanged_(std::shared_ptr<Property> pProperty);

      std::shared_ptr<Property> GetProperty_(const String &sPropertyName);

      bool IsCryptedProperty_(const String &sPropertyName);

      // An administrator's change to a property the environment manages is refused
      // here, the chokepoint every interface writes through: see
      // EnvironmentSettings::RefuseAdministratorChange. Called only for a change
      // that moves the value.
      static void RefuseIfEnvironmentManaged_(const String &sPropertyName);

      // Whether the value being replaced is one a file of the environment supplied,
      // which the audit trail records as changed and never as a value; and, after
      // an administrator's change, the new value is theirs and is shown again.
      static bool IsWithheld_(const String &sPropertyName);
      static void ForgetIfWithheld_(const String &sPropertyName, bool withheld);

      static thread_local ReadRecorder *recorder_;

      // Every property the set holds, by name.
      std::map<String, std::shared_ptr<Property> > items_;

      // Guards items_ - the map itself; each Property guards its own value. A
      // profile's records are written into the published set on a request thread
      // (WriteBookkeeping) while every other thread looks settings up in it, and a
      // lookup in a std::map that another thread is inserting into can follow a
      // pointer the insert has just moved. (Configuration::Load fills a set and
      // ensures its defaults before it publishes it.) Held only to find, copy or
      // insert an entry: nothing is called under
      // it - not the database, not a change event, not the error report of a
      // missing name - so no other lock is ever taken under it: a leaf.
      mutable boost::mutex items_mutex_;

      // The entries, in the map's order, copied under items_mutex_ for a walk that
      // must not hold it.
      std::vector<std::pair<String, std::shared_ptr<Property> > > Snapshot_() const;
   };
}
