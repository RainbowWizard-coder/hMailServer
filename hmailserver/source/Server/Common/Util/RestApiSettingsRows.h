// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// What the REST settings rows (RestApiSettings.cpp) say about a setting, for a
   /// caller that has the setting's name and not a row: hmailserver
   /// --apply-environment, which checks HM_SETTING_&lt;key&gt; and
   /// HM_PROPERTY_&lt;name&gt; the way PUT /api/v1/settings/... checks the row that
   /// holds the setting, and WRITES them through that row's setter - the setter
   /// COM's property runs - so that a value is stored as REST would store it: the
   /// script language canonicalised, a URL trimmed, a VAPID private key stored with
   /// the public half derived from it, a backup passphrase with its is-set flag
   /// (docs/EnvironmentConfiguration.md).
   ///
   /// Which row holds which property is not written down for a row that can be
   /// read: every row's value is read with PropertySet::ReadRecorder installed,
   /// because a row's getter is the code COM's getter runs, and the name it reads is
   /// the name the row holds. Such a row lends itself only when it is the one row
   /// that reads the property, reads nothing else, and answers the property as
   /// stored - so a row that inverts a switch (deny_mail_from_null) or shows a size
   /// in other units, whose check and setter are in the row's terms and not the
   /// property's, lends nothing and the property is written as it stands. A
   /// write-only row, which cannot be read, names its property (Row::property); a
   /// row kept in the settings store names its key (Row::ini_key). Needs the
   /// configuration loaded, except DeclaresProperty.
   /// </summary>
   namespace RestApiSettingsRows
   {
      /// <summary>How the row takes a value.</summary>
      enum ValueKind
      {
         KindNone,         // no row lends itself
         KindText,
         KindWholeNumber,
         KindSwitch,       // 1 or 0; true and false and their like as words
         KindWords         // one of the row's words, or the number it stands for
      };

      struct Lent
      {
         Lent() : kind(KindNone), writeOnly(false), readOnly(false) {}

         ValueKind kind;
         /// <summary>The row's REST key and its group's path, for a sentence.</summary>
         AnsiString key;
         AnsiString path;
         bool writeOnly;
         /// <summary>
         /// The only rows that read the property are read-only: REST refuses to set
         /// it, because the server derives it or keeps it itself.
         /// </summary>
         bool readOnly;
         /// <summary>The row's words, comma separated, for KindWords.</summary>
         String words;
      };

      /// <summary>
      /// The row a setting is written through: a [Settings] key when iniKey, else
      /// an hm_settings property. False, with lent.kind KindNone, when none lends
      /// itself (lent.readOnly says whether REST holds the property read-only).
      /// </summary>
      bool Find(bool iniKey, const String &name, Lent &lent);

      /// <summary>
      /// The row's own check of a value in the row's terms - the number for a whole
      /// number, a switch or a word, the text otherwise - with the row's type rules
      /// first (a switch takes 1 or 0, a row of words one of its values). Empty when
      /// acceptable or no row lends itself; otherwise the row's sentence, which may
      /// quote the value.
      /// </summary>
      String Check(bool iniKey, const String &name, long number, const String &text);

      /// <summary>
      /// Writes the value through the row's setter, as PUT /api/v1/settings/...
      /// does. False with the setter's sentence when the setter refused or failed;
      /// the caller writes the setting itself when no row lends one.
      /// </summary>
      bool Apply(bool iniKey, const String &name, long number, const String &text, String &refusal);

      /// <summary>
      /// For a setting whose row is one of words (a connection security, a log
      /// device), the number a word of that row stands for. False when it is not one
      /// of them; words is the row's words whenever the row has words.
      /// </summary>
      bool WordValue(bool iniKey, const String &name, const String &word, long &number, String &words);

      /// <summary>
      /// Whether a write-only row names this property as the one its setter writes.
      /// Read from the tables alone, with no configuration: what lets the
      /// environment set backupencryptionpassphrase, a row read both as text and as
      /// a number, through the one setter that keeps the two together.
      /// </summary>
      bool DeclaresProperty(const String &property);
   }
}
