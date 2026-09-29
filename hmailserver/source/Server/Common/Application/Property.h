// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Property
   {
   public:
      Property(void);
      ~Property(void);

      Property(const String &sName, long lValue, const String &sValue, bool SaveCrypted = false);
   

      AnsiString GetName() const {return name_;}
      void SetName(const AnsiString &NewVal) {name_ = NewVal;}

      int GetLongValue() const;
      void SetLongValue(long NewVal);

      String GetStringValue() const;
      void SetStringValue(const String &NewVal);

      // The row written first, and the value taken only when the database took it:
      // false, with the value as it was, when the write failed.
      bool TrySetStringValue(const String &NewVal);

      bool GetBoolValue() const { return GetLongValue() ? true : false; }
      void SetBoolValue(bool NewVal);

      void SetIsCrypted() {save_crypted_ = true; }
   private:

  
      bool WriteBoolSetting_(bool bValue);
      bool WriteLongSetting_(long lValue);
      bool WriteStringSetting_(const String &sValue);
      bool SettingRowExists_() const;

      long long_value_;
      String string_value_;
      AnsiString name_;

      bool save_crypted_;

      // Guards long_value_ and string_value_. A setting is read on every thread
      // that consults it and written on the COM or REST thread that changes it, and
      // a String assigned while another thread copies it can be read empty or out
      // of a freed buffer. Held only to copy a value in or out - never across the
      // database write - so it is a leaf: nothing is locked, called or written
      // while it is held. (PropertySet never calls a Property under its own lock.)
      mutable boost::mutex value_mutex_;

   };
}
