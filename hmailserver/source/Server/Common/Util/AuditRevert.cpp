// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See AuditRevert.h.
//
// WHAT THE TRAIL HOLDS, AND SO WHAT CAN BE PUT BACK
//
// A setting's row says "name: old -> new" in its detail (AuditTrail::
// RecordSettingChange), each value on one line and cut at 120 characters,
// with "(changed)" in place of both for a secret and "(default)" for the side
// of a [Settings] key that has no stored value. That is the whole record, so
// a put-back is honest about its limits: a secret cannot be put back, because
// the trail never held it; a value the trail cut cannot be, because what it
// holds is not the value; and a row that is not a setting's - an account
// created, a rule deleted - is not a change with an old value to write.
//
// Everything else is: the old value goes through PropertySet::SetString,
// SetLong or SetBool for an hm_settings row, or through IniFileSettings::
// WriteSettingsValue or RemoveSettingsValue for a [Settings] key - which record
// the change through IniSettingStore::WriteSetting and RemoveSetting and
// publish it as any save does - so the put-back is one more row in the trail,
// made by whoever asked for it, and never a quiet edit of history.
//
// WHICH COLUMN THE VALUE GOES BACK INTO
//
// An hm_settings row has two columns, settingstring and settinginteger, and a
// property is read from one of them: GetString reads the text, GetLong and
// GetBool the number. The row in the trail does not say which, and a value
// written into the column nobody reads changes nothing while the put-back
// answers that the setting is back. Until 29 September 2026 that is what
// became of every switch: PropertySet::SetBool records its two sides as "true"
// and "false", which are neither the switch's text (it has none) nor its number
// (0 or 1), so the guess this made from the values took the row for a text's,
// SetString wrote the old side's word into settingstring, and the switch stayed
// as it had been changed - answered as put back, over COM and REST alike.
//
// The column is now the property's Kind in SettingsDefaults.h, which
// build/generate-settings-defaults.py reads from the PropertySet calls that
// read each property: a KindString property goes back through SetString, and a
// KindNumber one through SetBool when its row has a switch's two words and
// through SetLong when it has a number - the setter that writes each form -
// and anything else in a KindNumber row is refused. Only a property no read
// names one column for (KindEither: backupencryptionpassphrase, whose rows are
// a secret's, and rows nothing reads), or a row this build ships no default
// for, is still told by its values.
//
// WHERE THE OLD VALUE ENDS
//
// A value may itself hold " -> ", and then "old -> new" can be split in more
// than one place. The first arrow was taken, so a text "a -> b" changed to "c"
// was put back as "a" and answered as put back. Now a row with one arrow is
// split there, and of several the split is the one whose new side is the
// setting's value now, as the row would record it; a row where that is not
// exactly one split is refused rather than guessed. The value now tells the
// split only while it is still the value the row gave the setting - "a"
// changed to "b -> c" and then to "c" leaves a first row "a -> b -> c" whose
// second split matches "c", and "a -> b" would go back - so such a row is
// also refused once the trail records a later change of the setting. A
// [Settings] key has no such value to compare with here, so its row is
// refused when it has more than one arrow.
#include "StdAfx.h"
#include "AuditRevert.h"

#include "../Application/Configuration.h"
#include "../Application/IniSettingStore.h"
#include "../Application/PropertySet.h"
#include "../Application/SettingsDefaults.h"
#include "Parsing/StringParser.h"

#include <algorithm>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // AuditTrail's own limit for one value in a detail, and the mark it
      // leaves: a value of exactly that length plus the mark was cut.
      const int MaxValueLength = 120;

      const TCHAR *const Arrow = _T(" -> ");
      const int ArrowLength = 4;

      bool IsWholeNumber(const String &value)
      {
         if (value.IsEmpty())
            return false;

         for (int i = 0; i < value.GetLength(); i++)
         {
            const TCHAR c = value.GetAt(i);
            if (i == 0 && c == _T('-') && value.GetLength() > 1)
               continue;
            if (c < _T('0') || c > _T('9'))
               return false;
         }

         return true;
      }

      // The two words PropertySet::SetBool records a switch's sides with.
      bool IsSwitchWord(const String &value)
      {
         return value == _T("true") || value == _T("false");
      }

      // The column the server reads the property from, as SettingsDefaults.h
      // says, by name without case: the property names are spelled both ways
      // (SettingsDiff::DatabaseDefaultFor says so too). KindEither for a name
      // this build ships no default for.
      SettingsDefaults::SettingKind KindOf(const String &name)
      {
         for (size_t i = 0; i < SettingsDefaults::SettingsCount; i++)
         {
            if (name.CompareNoCase(SettingsDefaults::Settings[i].Name) == 0)
               return SettingsDefaults::Settings[i].Kind;
         }

         return SettingsDefaults::KindEither;
      }

      // Every place "old -> new" can be split: each " -> " in it, overlapping
      // ones included (a value ending " ->" before the arrow makes two).
      std::vector<int> ArrowsIn(const String &rest)
      {
         std::vector<int> positions;

         for (int position = rest.Find(Arrow); position >= 0; position = rest.Find(Arrow, static_cast<size_t>(position) + 1))
            positions.push_back(position);

         return positions;
      }

      // The row's two sides. One arrow is the split; of several, the one whose
      // new side is among `now` - the setting's value now, in each form a row
      // could have recorded it in - when exactly one is. False otherwise.
      bool Split(const String &rest, const std::vector<int> &arrows, const std::vector<String> &now, String &oldValue, String &newValue)
      {
         if (arrows.size() == 1)
         {
            oldValue = rest.Left(arrows[0]);
            newValue = rest.Mid(arrows[0] + ArrowLength);
            return true;
         }

         int matches = 0;

         for (const int position : arrows)
         {
            const String candidate = rest.Mid(position + ArrowLength);
            if (std::find(now.begin(), now.end(), candidate) == now.end())
               continue;

            matches++;
            oldValue = rest.Left(position);
            newValue = candidate;
         }

         return matches == 1;
      }

      const TCHAR *const NoValue = _T("The row does not carry the value the setting had, so it cannot be put back from the trail.");
      const TCHAR *const NoSplit = _T("The values in the row hold \" -> \" themselves, so the row does not say where the value the setting had ends. Set it again by hand.");
      const TCHAR *const NoSplitChangedSince = _T("The values in the row hold \" -> \" themselves and the setting has been changed since, so the row does not say where the value the setting had ends. Set it again by hand.");
   }

   bool
   AuditRevert::PutBack(const AuditTrail::Entry &entry, String &restored, String &error)
   {
      restored.Empty();
      error.Empty();

      if (entry.object_type.CompareNoCase(_T("setting")) != 0)
      {
         error = _T("Only a setting's change can be put back; this row records a change to a ") + entry.object_type + _T(".");
         return false;
      }

      // The detail: "name: old -> new", with the role the change was made under
      // after it when there was one - AuditTrail::Write_ appends " [role: <name>]"
      // last, so it is the last such mark and the detail ends with it.
      String detail = entry.detail;
      const int role = detail.ReverseFind(_T(" [role: "));
      if (role >= 0 && detail.Right(1) == _T("]"))
         detail = detail.Left(role);

      const String prefix = entry.object_name + _T(": ");
      if (!detail.StartsWith(prefix))
      {
         error = NoValue;
         return false;
      }

      const String rest = detail.Mid(prefix.GetLength());
      if (rest == _T("(changed)"))
      {
         error = _T("A secret's values are never kept in the trail, so it cannot be put back from it. Set it again by hand.");
         return false;
      }

      const std::vector<int> arrows = ArrowsIn(rest);
      if (arrows.empty())
      {
         error = NoValue;
         return false;
      }

      String oldValue;
      String newValue;

      // A [Settings] key: the store that records "(default)" for a side with no row.
      String spelling;
      if (IniSettingStore::IsSettingTheServerReads(entry.object_name, spelling))
      {
         if (!Split(rest, arrows, std::vector<String>(), oldValue, newValue))
         {
            error = NoSplit;
            return false;
         }

         if (oldValue.GetLength() >= MaxValueLength + 3 && oldValue.Right(3) == _T("..."))
         {
            error.Format(_T("The value it had was longer than the trail keeps (%d characters), so what the trail holds is not the value. Set it again by hand."), MaxValueLength);
            return false;
         }

         if (oldValue == _T("(default)"))
         {
            // Through IniFileSettings, as every other door to the store goes: the
            // removal and its reload under the one hold of the reload lock that keeps
            // another save's reload from writing the line back between them, and
            // published - a key marked now is back at its default when the revert
            // returns.
            if (!IniFileSettings::Instance()->RemoveSettingsValue(spelling))
            {
               error = _T("The setting could not be returned to its default: the row or the line could not be removed. The error log says which.");
               return false;
            }

            restored = _T("(default)");
            return true;
         }

         if (!IniSettingStore::IsStorableValue(oldValue))
         {
            error = _T("The value it had cannot be stored as a [Settings] value.");
            return false;
         }

         // As above: the store write and its reload under one hold, and published.
         if (!IniFileSettings::Instance()->WriteSettingsValue(spelling, oldValue))
         {
            error = _T("The value could not be written. The error log says why.");
            return false;
         }

         restored = oldValue;
         return true;
      }

      // An hm_settings property.
      std::shared_ptr<PropertySet> settings = Configuration::Instance()->GetSettings();
      if (!settings || !settings->Exists(entry.object_name))
      {
         error = _T("This version of the server has no setting of that name, so there is nothing to put back.");
         return false;
      }

      const SettingsDefaults::SettingKind kind = KindOf(entry.object_name);

      // The setting now, in each form a row records it in: its text as the trail
      // writes a value, its number, and its number as a switch's word.
      const String currentText = AuditTrail::RecordedValue(settings->GetString(entry.object_name));
      const long currentLong = settings->GetLong(entry.object_name);
      const String currentNumber = StringParser::IntToString((__int64) currentLong);
      const String currentSwitch = currentLong != 0 ? _T("true") : _T("false");

      std::vector<String> now;
      if (kind != SettingsDefaults::KindNumber)
         now.push_back(currentText);
      if (kind != SettingsDefaults::KindString)
      {
         now.push_back(currentNumber);
         now.push_back(currentSwitch);
      }

      // Of several arrows the split is told by the value the setting has now,
      // which says where this row divides only while it is still the value the
      // row gave it: once the setting has changed again it can equal another of
      // the row's possible new sides by chance. So such a row is divided only
      // while the trail records no later change of the setting.
      if (arrows.size() > 1)
      {
         bool changed = true;
         if (entry.id <= 0 || !AuditTrail::Instance()->ChangedSince(entry.object_name, entry.id, changed))
         {
            error = NoSplit;
            return false;
         }

         if (changed)
         {
            error = NoSplitChangedSince;
            return false;
         }
      }

      if (!Split(rest, arrows, now, oldValue, newValue))
      {
         error = NoSplit;
         return false;
      }

      if (oldValue.GetLength() >= MaxValueLength + 3 && oldValue.Right(3) == _T("..."))
      {
         error.Format(_T("The value it had was longer than the trail keeps (%d characters), so what the trail holds is not the value. Set it again by hand."), MaxValueLength);
         return false;
      }

      enum Column
      {
         ColumnText,
         ColumnNumber,
         ColumnSwitch
      };

      // By the kind; for KindEither, by the values: the text column when the row's
      // new side is the text the setting holds now, a switch when both sides are
      // a switch's words beside an empty text, the number column when the new side
      // is the number now, and failing all three a whole number beside an empty
      // text is a number.
      Column column;
      if (kind == SettingsDefaults::KindNumber)
         column = IsSwitchWord(oldValue) ? ColumnSwitch : ColumnNumber;
      else if (kind == SettingsDefaults::KindString || (!newValue.IsEmpty() && newValue == currentText))
         column = ColumnText;
      else if (IsSwitchWord(oldValue) && IsSwitchWord(newValue) && currentText.IsEmpty())
         column = ColumnSwitch;
      else if (newValue == currentNumber && newValue != currentText)
         column = ColumnNumber;
      else
         column = currentText.IsEmpty() && IsWholeNumber(oldValue) ? ColumnNumber : ColumnText;

      switch (column)
      {
      case ColumnSwitch:
         settings->SetBool(entry.object_name, oldValue == _T("true"));
         break;

      case ColumnNumber:
         if (!IsWholeNumber(oldValue))
         {
            error = _T("The setting is a number and the value it had is not one, so it cannot be put back.");
            return false;
         }

         settings->SetLong(entry.object_name, _ttol(oldValue));
         break;

      case ColumnText:
         settings->SetString(entry.object_name, oldValue);
         break;
      }

      restored = oldValue;
      return true;
   }
}
