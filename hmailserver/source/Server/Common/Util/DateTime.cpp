// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// HM::DateTime and HM::DateTimeSpan, declared in VariantDateTime.h.
//
// Written in September 2026 from the Gregorian calendar and the definition of the
// OLE Automation date (COM's DATE type), to replace a date class of 1999 that
// stated no licence. It keeps that class's behaviour wherever the server could
// see it: a differential test held the two to the same answers, bit for bit, on
// every day of the years 0 to 9999, every second of two dozen days, and tens of
// millions of other values and calls. Some of that behaviour is odd, and is kept
// on purpose:
//
// - Fields are read back only from -657434 (1 January 100, 00:00:00) to 2958465
//   (31 December 9999, 00:00:00), both included: a later time on either of those
//   two days reads -1, like anything outside them.
// - Composing accepts any year from 0 to 9999, so a date in years 0 to 99 is valid
//   although none of its fields can be read back.
// - Each field given to SetDateTime, SetDate and the constructors is first reduced
//   modulo 65536.
// - Reading fields rounds to the second by adding half a second away from zero to
//   the stored number and truncating. For a negative date within half a second of
//   the next midnight that gives midnight at the START of the day before.
// - A DateTimeSpan is invalid until something sets it.
// - ToInt is the C library's UTC conversion, and the two platforms' libraries
//   differ at both ends. Windows' _mkgmtime64 counts from 1969-12-31 12:00:00 UTC
//   (a negative count, as unsigned) to 3001-01-19 21:59:59 UTC and answers 0
//   outside that; POSIX's timegm counts on in both directions. Both are truncated
//   to 32 bits, which matters from 2106. The last second of 1969 is -1, the
//   libraries' error value, and reads as 0 on both.
//
// One thing is not kept: a NaN passed the old range test and reached a conversion
// to an integer whose result is undefined. Here it reads as no date (-1).

#include "stdafx.h"
#include "VariantDateTime.h"
#include "Time.h"

#include <cmath>
#include <cstdint>

namespace HM
{
   namespace
   {
      const int Unreadable = -1;

      const double SecondsPerDay = 86400.0;
      const double HalfSecondInDays = 0.5 / SecondsPerDay;

      constexpr long long FloorDivide(long long numerator, long long denominator)
      {
         return numerator / denominator - ((numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) ? 1 : 0);
      }

      // Days are counted here from 1 March of year 0, so that a leap day, where
      // there is one, is the last day of its counting year: a counting year is
      // March to February, and months are numbered 0 (March) to 11 (February).
      // 153 days make five months from March (31+30+31+30+31), which is why the
      // month's first day is (153 * month + 2) / 5 into the year.
      constexpr long long DaysFromMarchOfYearZero(long long year, long long month, long long day)
      {
         const long long countingYear = month <= 2 ? year - 1 : year;
         const long long countingMonth = month <= 2 ? month + 9 : month - 3;

         return 365 * countingYear
            + FloorDivide(countingYear, 4) - FloorDivide(countingYear, 100) + FloorDivide(countingYear, 400)
            + (153 * countingMonth + 2) / 5
            + day - 1;
      }

      // Day 0 of an OLE Automation date.
      constexpr long long OleDayZero = DaysFromMarchOfYearZero(1899, 12, 30);

      // The first and the last values whose fields are read back.
      const double EarliestReadable = -657434.0;
      const double LatestReadable = 2958465.0;

      static_assert(DaysFromMarchOfYearZero(100, 1, 1) - OleDayZero == -657434, "1 January 100");
      static_assert(DaysFromMarchOfYearZero(9999, 12, 31) - OleDayZero == 2958465, "31 December 9999");
      static_assert(DaysFromMarchOfYearZero(1970, 1, 1) - OleDayZero == 25569, "1 January 1970");

      // Counted from March, as above: each block ends with its leap day, if it has one.
      const long long DaysPer400Years = 146097;
      const long long DaysPer100Years = 36524;   // but 36525 for the last century of the 400
      const long long DaysPer4Years = 1461;      // but 1460 for a century's last four, except in that last century

      bool IsLeapYear(long long year)
      {
         return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
      }

      int DaysInMonth(long long year, int month)
      {
         static const int lengths[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

         if (month == 2 && IsLeapYear(year))
            return 29;

         return lengths[month - 1];
      }

      // The civil date of a day counted from 1 March of year 0 (a count that is
      // not negative).
      void CivilDate(long long days, long long &year, int &month, int &day)
      {
         const long long cycle = days / DaysPer400Years;
         const long long dayOfCycle = days % DaysPer400Years;

         // The fourth century of a cycle ends with the cycle's extra leap day.
         long long century = dayOfCycle / DaysPer100Years;
         if (century > 3)
            century = 3;
         const long long dayOfCentury = dayOfCycle - century * DaysPer100Years;

         // A century's last four years are one day short, except in the fourth
         // century, and in either case the division below stays within 0 to 24.
         const long long quad = dayOfCentury / DaysPer4Years;
         const long long dayOfQuad = dayOfCentury - quad * DaysPer4Years;

         // The fourth year of four is the one that may have 366 days.
         long long yearOfQuad = dayOfQuad / 365;
         if (yearOfQuad > 3)
            yearOfQuad = 3;
         const long long dayOfYear = dayOfQuad - yearOfQuad * 365;

         const long long countingMonth = (5 * dayOfYear + 2) / 153;

         day = static_cast<int>(dayOfYear - (153 * countingMonth + 2) / 5 + 1);
         month = static_cast<int>(countingMonth < 10 ? countingMonth + 3 : countingMonth - 9);
         year = cycle * 400 + century * 100 + quad * 4 + yearOfQuad + (month <= 2 ? 1 : 0);
      }

      // Fields to a DATE. False, with value untouched, unless they are a date and a
      // time of day. Each field arrives reduced to 16 bits.
      bool ComposeDate(std::uint16_t year, std::uint16_t month, std::uint16_t day,
                       std::uint16_t hour, std::uint16_t minute, std::uint16_t second, DATE &value)
      {
         if (year > 9999 || month < 1 || month > 12)
            return false;

         if (day < 1 || day > DaysInMonth(year, month) || hour > 23 || minute > 59 || second > 59)
            return false;

         const long long dayNumber = DaysFromMarchOfYearZero(year, month, day) - OleDayZero;
         const long secondOfDay = static_cast<long>(hour) * 3600L + static_cast<long>(minute) * 60L + static_cast<long>(second);
         const double timeOfDay = static_cast<double>(secondOfDay) / SecondsPerDay;

         // Before day 0 the time is carried by the size of the fraction, so it is
         // taken away from the (negative) day rather than added to it.
         value = static_cast<double>(dayNumber) + (dayNumber >= 0 ? timeOfDay : -timeOfDay);

         return true;
      }

      bool ComposeDate(int year, int month, int day, int hour, int minute, int second, DATE &value)
      {
         return ComposeDate(static_cast<std::uint16_t>(year), static_cast<std::uint16_t>(month),
            static_cast<std::uint16_t>(day), static_cast<std::uint16_t>(hour),
            static_cast<std::uint16_t>(minute), static_cast<std::uint16_t>(second), value);
      }

      struct CalendarFields
      {
         int year;
         int month;
         int day;
         int hour;
         int minute;
         int second;
         int dayOfWeek;
      };

      // A DATE to fields, to the nearest second. False if the value is outside the
      // range read back, or not a number.
      bool DecomposeDate(DATE value, CalendarFields &fields)
      {
         if (!(value >= EarliestReadable && value <= LatestReadable))
            return false;

         const double rounded = value + (value > 0.0 ? HalfSecondInDays : -HalfSecondInDays);

         // The day is the whole part toward zero; the time is the fraction's size.
         const long long dayNumber = static_cast<long long>(rounded);
         const double size = std::fabs(rounded);
         const long secondOfDay = static_cast<long>((size - std::floor(size)) * SecondsPerDay);

         long long year = 0;
         int month = 0;
         int day = 0;
         CivilDate(dayNumber + OleDayZero, year, month, day);

         fields.year = static_cast<int>(year);
         fields.month = month;
         fields.day = day;
         fields.hour = static_cast<int>(secondOfDay / 3600);
         fields.minute = static_cast<int>((secondOfDay / 60) % 60);
         fields.second = static_cast<int>(secondOfDay % 60);

         // Day 0, 30 December 1899, was a Saturday; 1 is Sunday and 7 Saturday.
         fields.dayOfWeek = static_cast<int>(((dayNumber % 7) + 7 + 6) % 7) + 1;

         return true;
      }

      bool ReadFields(const DateTime &value, CalendarFields &fields)
      {
         return value.GetStatus() == DateTime::valid && DecomposeDate(value.dt_, fields);
      }

      // A negative DATE is not continuous with the positive ones: -1.25 is between
      // -1 and -2 as a number, and six hours after the start of day -1 as a moment.
      // These two turn a DATE into a plain count of days and back, for arithmetic
      // and comparison.
      double ToDayCount(DATE value)
      {
         if (value >= 0)
            return value;

         const double day = std::ceil(value);
         return day + (day - value);
      }

      DATE FromDayCount(double days)
      {
         if (days >= 0)
            return days;

         const double day = std::floor(days);
         return day - (days - day);
      }

      void FromLocalFields(const struct tm &local, DATE &value, DateTime::DateTimeStatus &status)
      {
         // tm_year counts from 1900 and comes near INT_MAX for a moment billions of
         // years away, which a 64-bit time_t can name, so the year is formed in 64
         // bits. Like every field, it then keeps its low 16 bits.
         const long long year = static_cast<long long>(local.tm_year) + 1900;

         status = ComposeDate(static_cast<std::uint16_t>(year), static_cast<std::uint16_t>(local.tm_mon + 1),
            static_cast<std::uint16_t>(local.tm_mday), static_cast<std::uint16_t>(local.tm_hour),
            static_cast<std::uint16_t>(local.tm_min), static_cast<std::uint16_t>(local.tm_sec), value) ? DateTime::valid : DateTime::invalid;
      }
   }

   DateTime::DateTime() :
      dt_(0),
      status_(invalid)
   {
   }

   DateTime::DateTime(DATE value) :
      dt_(value),
      status_(valid)
   {
   }

   DateTime::DateTime(time_t &moment) :
      dt_(0),
      status_(invalid)
   {
      struct tm local = {};
      if (localtime_s(&local, &moment) != 0)
         return;

      FromLocalFields(local, dt_, status_);
   }

   DateTime::DateTime(const FILETIME &utc) :
      dt_(0),
      status_(invalid)
   {
#ifdef HM_PLATFORM_POSIX
      // A FILETIME counts 100-nanosecond ticks from 1 January 1601 UTC, which is
      // 11644473600 seconds before 1970. What the C library cannot place in local
      // time, and anything before 1970, is no date.
      const std::uint64_t ticks = (static_cast<std::uint64_t>(utc.dwHighDateTime) << 32) | static_cast<std::uint64_t>(utc.dwLowDateTime);
      const std::uint64_t secondsSince1601 = ticks / 10000000ULL;
      const std::uint64_t secondsFrom1601To1970 = 11644473600ULL;

      if (secondsSince1601 < secondsFrom1601To1970)
         return;

      const time_t moment = static_cast<time_t>(secondsSince1601 - secondsFrom1601To1970);

      struct tm local = {};
      if (::localtime_r(&moment, &local) == nullptr)
         return;

      FromLocalFields(local, dt_, status_);
#else
      // Windows' own conversions, UTC to local and then to fields; the
      // milliseconds are dropped. FileTimeToLocalFileTime applies the offset in
      // force now, not the one of the moment converted, as the class this replaced
      // did.
      FILETIME local = {};
      SYSTEMTIME fields = {};

      if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &fields))
         return;

      status_ = ComposeDate(fields.wYear, fields.wMonth, fields.wDay,
         fields.wHour, fields.wMinute, fields.wSecond, dt_) ? valid : invalid;
#endif
   }

#ifndef HM_PLATFORM_POSIX
   // A VARIANT reaches this class only from the COM API, which the POSIX build
   // does not have. The header declares these two everywhere; they are defined
   // only here, so a POSIX caller would fail to link rather than run a stub.
   DateTime::DateTime(const VARIANT &value) :
      dt_(0),
      status_(invalid)
   {
      if (value.vt == VT_BSTR)
      {
         *this = Time::GetDateFromSystemDate(value.bstrVal);
         return;
      }

      dt_ = value.date;
      status_ = valid;
   }

   VARIANT
   DateTime::GetVariant() const
   {
      VARIANT result;
      ::VariantInit(&result);
      result.vt = VT_DATE;
      result.date = dt_;

      return result;
   }
#endif

   DateTime::DateTime(int year, int month, int day, int hour, int minute, int second) :
      dt_(0),
      status_(invalid)
   {
      SetDateTime(year, month, day, hour, minute, second);
   }

   DateTime
   DateTime::GetCurrentTime()
   {
      time_t now = ::time(nullptr);
      return DateTime(now);
   }

   int
   DateTime::SetDateTime(int year, int month, int day, int hour, int minute, int second)
   {
      status_ = ComposeDate(year, month, day, hour, minute, second, dt_) ? valid : invalid;
      return status_;
   }

   int
   DateTime::SetDate(int year, int month, int day)
   {
      return SetDateTime(year, month, day, 0, 0, 0);
   }

   int
   DateTime::GetYear() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.year : Unreadable;
   }

   int
   DateTime::GetMonth() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.month : Unreadable;
   }

   int
   DateTime::GetDay() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.day : Unreadable;
   }

   int
   DateTime::GetHour() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.hour : Unreadable;
   }

   int
   DateTime::GetMinute() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.minute : Unreadable;
   }

   int
   DateTime::GetSecond() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.second : Unreadable;
   }

   int
   DateTime::GetDayOfWeek() const
   {
      CalendarFields fields = {};
      return ReadFields(*this, fields) ? fields.dayOfWeek : Unreadable;
   }

   unsigned int
   DateTime::ToInt() const
   {
      CalendarFields fields = {};
      if (!ReadFields(*this, fields))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5160, "DateTime::ToInt()", "Tried to convert non-date to integral representation.");
         return 0;
      }

      struct tm utc = {};
      utc.tm_year = fields.year - 1900;
      utc.tm_mon = fields.month - 1;
      utc.tm_mday = fields.day;
      utc.tm_hour = fields.hour;
      utc.tm_min = fields.minute;
      utc.tm_sec = fields.second;

#ifdef HM_PLATFORM_POSIX
      const time_t seconds = ::timegm(&utc);
#else
      const time_t seconds = ::_mkgmtime64(&utc);
#endif

      if (seconds == -1)
         return 0;

      return static_cast<unsigned int>(seconds);
   }

   BOOL
   DateTime::operator<(const DateTime &other) const
   {
      return ToDayCount(dt_) < ToDayCount(other.dt_);
   }

   BOOL
   DateTime::operator>(const DateTime &other) const
   {
      return ToDayCount(dt_) > ToDayCount(other.dt_);
   }

   BOOL
   DateTime::operator<=(const DateTime &other) const
   {
      return ToDayCount(dt_) <= ToDayCount(other.dt_);
   }

   BOOL
   DateTime::operator>=(const DateTime &other) const
   {
      return ToDayCount(dt_) >= ToDayCount(other.dt_);
   }

   DateTime
   DateTime::Shifted_(const DateTimeSpan &span, bool later) const
   {
      DateTime result;

      if (status_ == null || span.GetStatus() == DateTimeSpan::null)
      {
         result.status_ = null;
         return result;
      }

      if (status_ == invalid || span.GetStatus() == DateTimeSpan::invalid)
         return result;

      const double days = later ? ToDayCount(dt_) + span.span_ : ToDayCount(dt_) - span.span_;

      result.dt_ = FromDayCount(days);
      result.status_ = (result.dt_ > LatestReadable || result.dt_ < EarliestReadable) ? invalid : valid;

      return result;
   }

   DateTime
   DateTime::operator+(const DateTimeSpan &span) const
   {
      return Shifted_(span, true);
   }

   DateTime
   DateTime::operator-(const DateTimeSpan &span) const
   {
      return Shifted_(span, false);
   }

   DateTimeSpan
   DateTime::operator-(const DateTime &other) const
   {
      DateTimeSpan result;

      if (status_ == null || other.status_ == null)
      {
         result.status_ = DateTimeSpan::null;
         return result;
      }

      if (status_ == invalid || other.status_ == invalid)
         return result;

      return DateTimeSpan(ToDayCount(dt_) - ToDayCount(other.dt_));
   }

   DateTimeSpan::DateTimeSpan() :
      span_(0),
      status_(invalid)
   {
   }

   DateTimeSpan::DateTimeSpan(double days) :
      span_(days),
      status_(valid)
   {
   }

   void
   DateTimeSpan::SetDateTimeSpan(long days, int hours, int minutes, int seconds)
   {
      // Added in this order, which settles the last bit of the sum.
      span_ = static_cast<double>(days)
         + static_cast<double>(hours) / 24.0
         + static_cast<double>(minutes) / 1440.0
         + static_cast<double>(seconds) / 86400.0;
      status_ = valid;
   }

   double
   DateTimeSpan::GetNumberOfDays() const
   {
      return status_ == valid ? span_ : 0;
   }

   double
   DateTimeSpan::GetNumberOfHours() const
   {
      return status_ == valid ? span_ * 24.0 : 0;
   }

   double
   DateTimeSpan::GetNumberOfSeconds() const
   {
      return status_ == valid ? span_ * 24.0 * 3600.0 : 0;
   }

   void
   DateTimeTests::Test()
   {
      // Known OLE Automation dates.
      struct KnownDate { int year; int month; int day; int hour; int minute; int second; double value; };
      const KnownDate known[] =
      {
         { 1899, 12, 30,  0, 0, 0,        0.0 },
         { 1899, 12, 31,  0, 0, 0,        1.0 },
         { 1900,  1,  1,  0, 0, 0,        2.0 },
         { 1899, 12, 29,  6, 0, 0,       -1.25 },
         { 1899, 12, 30, 12, 0, 0,        0.5 },
         { 1970,  1,  1,  0, 0, 0,    25569.0 },
         { 2000,  1,  1,  0, 0, 0,    36526.0 },
         { 2000,  2, 29, 18, 0, 0,    36585.75 },
         {  100,  1,  1,  0, 0, 0,  -657434.0 },
         { 9999, 12, 31,  0, 0, 0,  2958465.0 },
      };

      for (const KnownDate &k : known)
      {
         DateTime composed;
         if (composed.SetDateTime(k.year, k.month, k.day, k.hour, k.minute, k.second) != DateTime::valid || composed.dt_ != k.value)
            HM_TEST_FAILED();

         DateTime read(k.value);
         if (read.GetYear() != k.year || read.GetMonth() != k.month || read.GetDay() != k.day ||
             read.GetHour() != k.hour || read.GetMinute() != k.minute || read.GetSecond() != k.second)
            HM_TEST_FAILED();
      }

      // -0.5 and 0.5 are the same moment.
      if (DateTime(-0.5).GetDay() != 30 || DateTime(-0.5).GetHour() != 12)
         HM_TEST_FAILED();

      // Rounded to the nearest second, carrying into the next day.
      if (DateTime(36526.0 - 0.4 / 86400.0).GetDay() != 1 || DateTime(36526.0 - 0.4 / 86400.0).GetYear() != 2000)
         HM_TEST_FAILED();
      if (DateTime(36526.0 + 1.6 / 86400.0).GetSecond() != 2)
         HM_TEST_FAILED();

      // The range read back: the first moment of 1 January 100 and of 31 December 9999.
      if (DateTime(2958465.5).GetYear() != -1 || DateTime(-657434.5).GetYear() != -1 || DateTime(-657435.0).GetYear() != -1)
         HM_TEST_FAILED();
      if (DateTime().GetYear() != -1)
         HM_TEST_FAILED();
      if (DateTime(std::nan("")).GetYear() != -1)
         HM_TEST_FAILED();

      // Leap years, and a refused field leaving the value alone.
      DateTime leap;
      if (leap.SetDate(2000, 2, 29) != DateTime::valid || leap.SetDate(2400, 2, 29) != DateTime::valid || leap.SetDate(2024, 2, 29) != DateTime::valid)
         HM_TEST_FAILED();
      const DATE before = leap.dt_;
      if (leap.SetDate(1900, 2, 29) != DateTime::invalid || leap.SetDate(2100, 2, 29) != DateTime::invalid || leap.SetDate(2023, 2, 29) != DateTime::invalid)
         HM_TEST_FAILED();
      if (leap.dt_ != before)
         HM_TEST_FAILED();

      DateTime fields;
      if (fields.SetDateTime(2026, 9, 29, 24, 0, 0) != DateTime::invalid || fields.SetDateTime(2026, 9, 29, 23, 60, 0) != DateTime::invalid ||
          fields.SetDateTime(2026, 9, 29, 23, 59, 60) != DateTime::invalid || fields.SetDateTime(2026, 0, 1, 0, 0, 0) != DateTime::invalid ||
          fields.SetDateTime(2026, 13, 1, 0, 0, 0) != DateTime::invalid || fields.SetDateTime(2026, 9, 31, 0, 0, 0) != DateTime::invalid ||
          fields.SetDateTime(10000, 1, 1, 0, 0, 0) != DateTime::invalid || fields.SetDateTime(-1, 1, 1, 0, 0, 0) != DateTime::invalid)
         HM_TEST_FAILED();

      // Each field modulo 65536, as the class this replaced took them.
      if (fields.SetDate(65536 + 2020, 1, 1) != DateTime::valid || fields.GetYear() != 2020)
         HM_TEST_FAILED();

      // Days of the week, 1 = Sunday.
      if (DateTime(0.0).GetDayOfWeek() != 7 || DateTime(1970, 1, 1, 0, 0, 0).GetDayOfWeek() != 5 || DateTime(2026, 9, 29, 12, 0, 0).GetDayOfWeek() != 3)
         HM_TEST_FAILED();

      // Arithmetic and comparison across day 0, where the encoding turns over.
      if ((DateTime(-1.5) + DateTimeSpan(1.0)).dt_ != 0.5 || (DateTime(0.25) - DateTimeSpan(1.0)).dt_ != -1.25)
         HM_TEST_FAILED();
      if ((DateTime(-1.25) - DateTime(0.5)).GetNumberOfDays() != -1.25)
         HM_TEST_FAILED();
      if (!(DateTime(-1.25) < DateTime(-1.5)) || DateTime(-1.25) >= DateTime(-1.5) || !(DateTime(-2.0) < DateTime(-1.75)))
         HM_TEST_FAILED();

      // Out of range is invalid; an invalid or null operand passes through.
      if ((DateTime(2958464.0) + DateTimeSpan(2.0)).GetStatus() != DateTime::invalid)
         HM_TEST_FAILED();
      if ((DateTime() + DateTimeSpan(1.0)).GetStatus() != DateTime::invalid || (DateTime(1.0) + DateTimeSpan()).GetStatus() != DateTime::invalid)
         HM_TEST_FAILED();
      DateTime nothing(1.0);
      nothing.SetStatus(DateTime::null);
      if ((nothing - DateTimeSpan(1.0)).GetStatus() != DateTime::null || (nothing - DateTime(1.0)).GetStatus() != DateTimeSpan::null)
         HM_TEST_FAILED();

      // Spans.
      DateTimeSpan span;
      if (span.GetStatus() != DateTimeSpan::invalid || span.GetNumberOfDays() != 0)
         HM_TEST_FAILED();
      span.SetDateTimeSpan(1, 12, 0, 0);
      if (span.GetNumberOfDays() != 1.5 || span.GetNumberOfHours() != 36 || span.GetNumberOfSeconds() != 129600)
         HM_TEST_FAILED();

      // Seconds since 1970, read as UTC.
      if (DateTime(1970, 1, 1, 0, 0, 0).ToInt() != 0 || DateTime(1970, 1, 1, 0, 0, 5).ToInt() != 5 ||
          DateTime(1970, 1, 2, 0, 0, 0).ToInt() != 86400 || DateTime(1971, 1, 1, 0, 0, 0).ToInt() != 365 * 24 * 3600)
         HM_TEST_FAILED();
      if (DateTime(2009, 5, 14, 22, 2, 0).ToInt() != 1242338520 || DateTime(2020, 12, 31, 23, 59, 0).ToInt() != 1609459140)
         HM_TEST_FAILED();
      if (DateTime(2030, 1, 1, 0, 0, 0).ToInt() == 0)
         HM_TEST_FAILED();
      // -1 is the C library's answer for what it cannot convert, and also the last
      // second of 1969: 0 on both platforms.
      if (DateTime(1969, 12, 31, 23, 59, 59).ToInt() != 0)
         HM_TEST_FAILED();
#ifndef HM_PLATFORM_POSIX
      // Windows' _mkgmtime64 refuses a moment more than 12 hours before 1970;
      // POSIX's timegm counts back.
      if (DateTime(1960, 1, 1, 0, 0, 0).ToInt() != 0)
         HM_TEST_FAILED();

      const VARIANT asVariant = DateTime(36526.25).GetVariant();
      if (asVariant.vt != VT_DATE || asVariant.date != 36526.25)
         HM_TEST_FAILED();
#endif
   }
}
