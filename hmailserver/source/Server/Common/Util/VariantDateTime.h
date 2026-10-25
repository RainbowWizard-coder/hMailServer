// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

// A moment as the server keeps it in memory, and a length of time.
//
// DateTime holds an OLE Automation date, the DATE type of COM and of the ADO
// recordsets: a double whose whole part counts days from 30 December 1899 and
// whose fraction is the time of day. A date before that day is negative, and there
// the whole part (toward zero) is still the day and the fraction's size still the
// time, so -1.25 is 29 December 1899 06:00 and -0.5 is the same moment as 0.5.
// Arithmetic and comparison therefore go through a continuous day count, and
// splitting a value into calendar fields rounds it to the nearest second first.
//
// The calendar is the proleptic Gregorian one. Fields are read back only for
// dates from 1 January 100 to 31 December 9999 (the first moment of each; see
// DateTime.cpp for the rest of the edges), and a field that cannot be read is -1.
//
// The class is the project's own, written in 2026 to the behaviour of the one it
// replaced, which stated no licence; DateTime.cpp says what that behaviour
// is, and where the old class's quirks are kept on purpose. Only what the server
// uses is here.

namespace HM
{
   class DateTimeSpan;

   class DateTime
   {
   public:
      enum DateTimeStatus
      {
         valid = 0,
         invalid = 1,    // not a date: never set, out of range, or made from one that was not
         null = 2,       // no value; only arithmetic on a null operand produces it
      };

      // An invalid DateTime whose value is 0. Copies copy both members.
      DateTime();

      // Takes the value as it is, valid, without looking at its range.
      DateTime(DATE value);

      // The moment, in the server's local time, to the second. Invalid if the C
      // library cannot convert it. A non-const reference, as it always was: with a
      // const one a call with an int would become ambiguous with DATE.
      DateTime(time_t &moment);

      // A UTC file time, as the server's local time: on POSIX at the offset of
      // that moment, on Windows at the offset in force now (FileTimeToLocalFileTime),
      // so there a time on the other side of a daylight saving change is an hour out.
      DateTime(const FILETIME &utc);

      // Windows only (the COM API): a VT_BSTR is read as "yyyy-mm-dd hh:mm:ss" by
      // Time::GetDateFromSystemDate; anything else is taken as the VARIANT's date.
      DateTime(const VARIANT &value);

      // As SetDateTime.
      DateTime(int year, int month, int day, int hour, int minute, int second);

      // Now, in the server's local time, to the second. <windows.h> defines
      // GetCurrentTime as a macro for GetTickCount, so on Windows this member is
      // called GetTickCount everywhere it is declared, defined and called alike.
      static DateTime GetCurrentTime();

      // Sets the date and time if the fields make one, and returns the new status.
      // Each field is taken modulo 65536 first. A field out of range leaves the
      // value as it was and makes the status invalid.
      int SetDateTime(int year, int month, int day, int hour, int minute, int second);
      int SetDate(int year, int month, int day);

      void SetStatus(DateTimeStatus status) { status_ = status; }
      DateTimeStatus GetStatus() const { return status_; }

      // The calendar fields, or -1 when the status is not valid or the value is
      // outside the range that is read back.
      int GetYear() const;
      int GetMonth() const;       // 1 = January
      int GetDay() const;         // 1-31
      int GetHour() const;        // 0-23
      int GetMinute() const;      // 0-59
      int GetSecond() const;      // 0-59
      int GetDayOfWeek() const;   // 1 = Sunday, 7 = Saturday

      // The fields read as UTC, in seconds since 1970, truncated to 32 bits; 0 for
      // what the C library will not convert. Reports an error and returns 0 for a
      // value that is not a date.
      unsigned int ToInt() const;

      // Windows only (the COM API): a VT_DATE holding the value, whatever the status.
      VARIANT GetVariant() const;

      // Compare the moments; the status is not looked at.
      BOOL operator<(const DateTime &other) const;
      BOOL operator>(const DateTime &other) const;
      BOOL operator<=(const DateTime &other) const;
      BOOL operator>=(const DateTime &other) const;

      // A null operand gives null, an invalid one invalid; a result outside the
      // decomposable range is invalid.
      DateTime operator+(const DateTimeSpan &span) const;
      DateTime operator-(const DateTimeSpan &span) const;

      // The difference in days. Null or invalid as above, never range-checked.
      DateTimeSpan operator-(const DateTime &other) const;

      DATE dt_;
      DateTimeStatus status_;

   private:
      DateTime Shifted_(const DateTimeSpan &span, bool later) const;
   };

   class DateTimeSpan
   {
   public:
      enum DateTimeSpanStatus
      {
         valid = 0,
         invalid = 1,
         null = 2,
      };

      // A zero span that is INVALID until something sets it - what the class this
      // replaced did, and what every caller relies on by calling SetDateTimeSpan.
      DateTimeSpan();

      // A valid span of that many days.
      DateTimeSpan(double days);

      // Days, hours, minutes and seconds, each of either sign, taken as they are.
      void SetDateTimeSpan(long days, int hours, int minutes, int seconds);

      // The length, or 0 if the span is not valid.
      double GetNumberOfDays() const;
      double GetNumberOfHours() const;
      double GetNumberOfSeconds() const;

      DateTimeSpanStatus GetStatus() const { return status_; }

      double span_;
      DateTimeSpanStatus status_;
   };

   class DateTimeTests
   {
   public:
      void Test();
   };
}
