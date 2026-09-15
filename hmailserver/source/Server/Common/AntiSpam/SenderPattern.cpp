// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SenderPattern.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   String
   SenderPattern::Canonical(const String &pattern)
   {
      String entry = pattern;
      entry.Trim();
      entry.MakeLower();

      // "@example.com" is accepted as an alternative spelling of the
      // documented "example.com". An address keeps its '@', obviously: the
      // leading one is only stripped when nothing precedes it.
      if (entry.Left(1) == _T("@"))
         entry = entry.Mid(1);

      return entry;
   }

   bool
   SenderPattern::Matches(const String &pattern, const String &address)
   {
      // The null sender (<>) carries bounces and delivery status
      // notifications. It can never be listed.
      if (address.IsEmpty())
         return false;

      const String entry = Canonical(pattern);

      if (entry.IsEmpty())
         return false;

      String from = address;
      from.Trim();
      from.MakeLower();

      if (entry.Find(_T("@")) >= 0)
         return from == entry;

      const int atPos = from.ReverseFind('@');
      if (atPos < 0)
         return false;

      const String fromDomain = from.Mid(atPos + 1);

      if (fromDomain == entry)
         return true;

      String dottedSuffix = _T(".");
      dottedSuffix += entry;

      if (fromDomain.GetLength() > dottedSuffix.GetLength() &&
          fromDomain.Right(dottedSuffix.GetLength()) == dottedSuffix)
         return true;

      return false;
   }

   int
   SenderPattern::Specificity(const String &pattern)
   {
      const String entry = Canonical(pattern);

      if (entry.IsEmpty())
         return 0;

      if (entry.Find(_T("@")) >= 0)
      {
         // Every address is more specific than every domain, however deep the
         // domain: an entry naming one person is the most exact statement
         // anybody can make about a sender.
         return 1000;
      }

      int labels = 1;
      for (int i = 0; i < entry.GetLength(); i++)
      {
         if (entry.GetAt(i) == _T('.'))
            labels++;
      }

      return 1 + labels;
   }

   bool
   SenderPattern::Validate(const String &pattern, String &out_error)
   {
      const String entry = Canonical(pattern);

      if (entry.IsEmpty())
      {
         out_error = _T("an address or a domain is required");
         return false;
      }

      if (entry.GetLength() > 255)
      {
         out_error = _T("an entry is at most 255 characters");
         return false;
      }

      if (entry.Find(_T(" ")) >= 0 || entry.Find(_T("\t")) >= 0)
      {
         out_error = _T("an entry is one address or one domain, with no spaces in it");
         return false;
      }

      const int atPos = entry.Find(_T("@"));

      if (atPos >= 0)
      {
         // Exactly one '@', with something either side of it. Two of them is
         // not an address, and neither is "@example.com" once the leading one
         // has already been taken as the domain spelling above.
         if (entry.ReverseFind('@') != atPos)
         {
            out_error = _T("an address has one @ in it");
            return false;
         }

         if (atPos == 0 || atPos == entry.GetLength() - 1)
         {
            out_error = _T("an address needs a local part and a domain");
            return false;
         }

         if (entry.Mid(atPos + 1).Find(_T(".")) < 0)
         {
            out_error = _T("the domain of an address needs a dot in it");
            return false;
         }

         return true;
      }

      if (entry.Find(_T(".")) < 0)
      {
         out_error = _T("a domain needs a dot in it; an address needs an @");
         return false;
      }

      if (entry.Left(1) == _T(".") || entry.Right(1) == _T("."))
      {
         out_error = _T("a domain does not begin or end with a dot");
         return false;
      }

      return true;
   }
}
