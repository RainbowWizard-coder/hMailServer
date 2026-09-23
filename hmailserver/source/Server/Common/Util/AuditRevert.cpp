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
// Everything else is: the old value goes through PropertySet::SetString or
// SetLong for an hm_settings row, or through IniSettingStore::WriteSetting or
// RemoveSetting for a [Settings] key - the two chokepoints that record a
// change and, for a property, tell the running server - so the put-back is
// one more row in the trail, made by whoever asked for it, and never a quiet
// edit of history.
#include "StdAfx.h"
#include "AuditRevert.h"

#include "../Application/Configuration.h"
#include "../Application/IniSettingStore.h"
#include "../Application/PropertySet.h"
#include "Parsing/StringParser.h"

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
      // after it, in brackets, when there was one.
      String detail = entry.detail;
      const int role = detail.Find(_T(" [role: "));
      if (role >= 0)
         detail = detail.Left(role);

      const String prefix = entry.object_name + _T(": ");
      if (!detail.StartsWith(prefix))
      {
         error = _T("The row does not carry the value the setting had, so it cannot be put back from the trail.");
         return false;
      }

      const String rest = detail.Mid(prefix.GetLength());
      if (rest == _T("(changed)"))
      {
         error = _T("A secret's values are never kept in the trail, so it cannot be put back from it. Set it again by hand.");
         return false;
      }

      const int arrow = rest.Find(_T(" -> "));
      if (arrow < 0)
      {
         error = _T("The row does not carry the value the setting had, so it cannot be put back from the trail.");
         return false;
      }

      const String oldValue = rest.Left(arrow);
      const String newValue = rest.Mid(arrow + 4);

      if (oldValue.GetLength() >= MaxValueLength + 3 && oldValue.Right(3) == _T("..."))
      {
         error.Format(_T("The value it had was longer than the trail keeps (%d characters), so what the trail holds is not the value. Set it again by hand."), MaxValueLength);
         return false;
      }

      // A [Settings] key: the store that records "(default)" for a side with no row.
      String spelling;
      if (IniSettingStore::IsSettingTheServerReads(entry.object_name, spelling))
      {
         if (oldValue == _T("(default)"))
         {
            if (!IniSettingStore::RemoveSetting(spelling))
            {
               error = _T("The setting could not be returned to its default: the row or the line could not be removed. The error log says which.");
               return false;
            }

            // Published, as a save through IniFileSettings is: a key marked now is
            // back in effect when the revert returns.
            IniFileSettings::Instance()->Reload();

            restored = _T("(default)");
            return true;
         }

         if (!IniSettingStore::IsStorableValue(oldValue))
         {
            error = _T("The value it had cannot be stored as a [Settings] value.");
            return false;
         }

         if (!IniSettingStore::WriteSetting(spelling, oldValue))
         {
            error = _T("The value could not be written. The error log says why.");
            return false;
         }

         IniFileSettings::Instance()->Reload();

         restored = oldValue;
         return true;
      }

      // An hm_settings property. Whether it is a number or a text is not in the
      // row, so it is read from the set: the new value the row records is what
      // the setting holds now, unless it has changed again since, and it is
      // either the text column or the number column that holds it. Failing
      // that, a value that is a whole number beside an empty text is a number.
      std::shared_ptr<PropertySet> settings = Configuration::Instance()->GetSettings();
      if (!settings || !settings->Exists(entry.object_name))
      {
         error = _T("This version of the server has no setting of that name, so there is nothing to put back.");
         return false;
      }

      const String currentText = settings->GetString(entry.object_name);
      const String currentNumber = StringParser::IntToString((__int64) settings->GetLong(entry.object_name));

      bool number;
      if (!newValue.IsEmpty() && newValue == currentText)
         number = false;
      else if (newValue == currentNumber && newValue != currentText)
         number = true;
      else
         number = currentText.IsEmpty() && IsWholeNumber(oldValue);

      if (number)
      {
         if (!IsWholeNumber(oldValue))
         {
            error = _T("The setting is a number and the value it had is not one, so it cannot be put back.");
            return false;
         }

         settings->SetLong(entry.object_name, _ttol(oldValue));
      }
      else
      {
         settings->SetString(entry.object_name, oldValue);
      }

      restored = oldValue;
      return true;
   }
}
