// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// What the REST settings rows (RestApiSettings.cpp) say about an hm_settings
   /// property, for a caller that has the property's name and not a row:
   /// hmailserver --apply-environment, which checks HM_PROPERTY_&lt;name&gt; the way
   /// PUT /api/v1/settings/... checks the row that holds it
   /// (docs/EnvironmentConfiguration.md).
   ///
   /// Which row holds which property is not written down anywhere: it is found by
   /// reading every row's value with PropertySet::ReadRecorder installed, because a
   /// row's getter is the code COM's getter runs, and the name it reads is the name
   /// the row holds. A property lends a row's check only when exactly one row reads
   /// it, that row reads nothing else, and the row's value IS the property's value
   /// as stored - so a row that inverts a switch (deny_mail_from_null) or shows a
   /// size in other units, whose check would be in the row's terms and not the
   /// property's, lends none. Needs the configuration loaded.
   /// </summary>
   namespace RestApiSettingsRows
   {
      /// <summary>
      /// The value, in the property's own terms - a number, or the text - checked
      /// as the property's REST row checks it: a switch row takes 0 or 1, a row of
      /// words one of its words' values, and the row's own check its own values.
      /// Empty when it is acceptable or no row holds the property; otherwise the
      /// row's sentence, which names the row's key.
      /// </summary>
      String CheckPropertyValue(const String &property, bool isNumber, long number, const String &text);

      /// <summary>
      /// For a property whose REST row is one of words (a connection security, a
      /// log device), the number a word of that row stands for - so that
      /// HM_PROPERTY_&lt;name&gt; may say starttls_required as the REST route does,
      /// as well as the number the property holds. False when the word is not one
      /// of them; words is the row's words, comma separated, whenever the row has
      /// words, for the sentence that refuses one.
      /// </summary>
      bool WordValue(const String &property, const String &word, long &number, String &words);
   }
}
