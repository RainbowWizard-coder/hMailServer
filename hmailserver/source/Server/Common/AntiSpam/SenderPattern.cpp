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

      // A URL: the host, and a '/' to say so. "https://www.Bad.Example/x?y",
      // "http://bad.example" and "bad.example/" are all "bad.example/".
      bool url = false;
      if (entry.StartsWith(_T("http://")))
      {
         entry = entry.Mid(7);
         url = true;
      }
      else if (entry.StartsWith(_T("https://")))
      {
         entry = entry.Mid(8);
         url = true;
      }

      const int slash = entry.Find(_T("/"));
      if (slash >= 0)
      {
         entry = entry.Left(slash);
         url = true;
      }

      if (url)
      {
         if (entry.StartsWith(_T("www.")))
            entry = entry.Mid(4);

         // The port and the credentials a link may carry are not part of the host.
         const int at = entry.Find(_T("@"));
         if (at >= 0)
            entry = entry.Mid(at + 1);
         const int colon = entry.Find(_T(":"));
         if (colon >= 0)
            entry = entry.Left(colon);

         entry.Trim();
         entry += _T("/");
      }

      return entry;
   }

   bool
   SenderPattern::IsUrl(const String &pattern)
   {
      const String entry = Canonical(pattern);
      return entry.GetLength() > 1 && entry.Right(1) == _T("/");
   }

   bool
   SenderPattern::MatchesUrlHost(const String &pattern, const String &host)
   {
      if (!IsUrl(pattern) || host.IsEmpty())
         return false;

      const String entry = Canonical(pattern);
      const String entryHost = entry.Left(entry.GetLength() - 1);

      String linkHost = host;
      linkHost.Trim();
      linkHost.MakeLower();
      if (linkHost.StartsWith(_T("www.")))
         linkHost = linkHost.Mid(4);

      if (linkHost == entryHost)
         return true;

      // A subdomain, anchored at a label boundary: "bad.example/" covers
      // "cdn.bad.example" and not "notbad.example".
      if (linkHost.GetLength() > entryHost.GetLength() &&
          linkHost.Right(entryHost.GetLength()) == entryHost &&
          linkHost.GetAt(linkHost.GetLength() - entryHost.GetLength() - 1) == _T('.'))
         return true;

      return false;
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

      // A URL entry is about the links in a message, never about its sender.
      if (entry.Right(1) == _T("/"))
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

      // A URL host counts its labels as a domain does; the '/' is not one.
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
      // A URL entry is judged on its host, which has to be a domain: a scheme
      // or a path alone names nothing a link could have.
      if (IsUrl(pattern))
      {
         const String canonical = Canonical(pattern);
         const String host = canonical.Left(canonical.GetLength() - 1);
         if (host.IsEmpty() || host.Find(_T(".")) < 0 || host.Find(_T(" ")) >= 0 || host.Left(1) == _T(".") || host.Right(1) == _T("."))
         {
            out_error = _T("A URL entry needs a host name after its scheme, such as https://example.com/, and the host has to be a domain name.");
            return false;
         }
         return true;
      }

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
