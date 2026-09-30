// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "PasswordGenerator.h"
#include "../Util/GUIDCreator.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   int PasswordGenerator::Seed = 0;

   PasswordGenerator::PasswordGenerator()
   {
      
   }

   PasswordGenerator::~PasswordGenerator()
   {

   }

   String
   PasswordGenerator::Generate()
   {
      String sGUID = GUIDCreator::GetGUID();

      String sOutString;

      for (int i = 0; i < sGUID.GetLength(); i++)
      {
         wchar_t c = sGUID[i];

         switch (c)
         {
         case '{':
         case '}':
         case '-':
            break;
         default:
            sOutString += c;
         }

      }

      String sRetVal = sOutString.Mid(0,12);
      return sRetVal;
   }

   bool 
   PasswordGenerator::IsStrongPassword(const String &sUsername, const String &sPassword)
   {

      // The password should not be a part of a user name.
      if (sUsername.FindNoCase(sPassword) >= 0)
         return false;

      // The password should be more than 4 characters long.
      if (sPassword.GetLength() <= 4)
         return false;
     
      // If the password is 6 characters or less, it should contain a number or special char.
      //
      // The special characters are a Swedish keyboard's shifted digit row and the keys
      // beside it, as upstream's first commit (c71a6de74, April 2014) has them in
      // Windows-1252: U+00A4 CURRENCY SIGN after '#', and U+00A8 DIAERESIS and U+00B4
      // ACUTE ACCENT before '+' (bytes A4, A8 and B4). The commit after it re-encoded
      // the file as UTF-8 and wrote U+FFFD for each. MSVC reads a source with no byte
      // order mark in the ANSI code page, so on Windows each became the three
      // characters its UTF-8 bytes are in Windows-1252 (U+00EF, U+00BF, U+00BD): a short
      // password with one of the three right characters was refused, and one with those
      // was taken. Written as \u escapes, they are the same characters on every compiler.
      if (sPassword.GetLength() <= 6)
      {
         if (sPassword.FindOneOf(_T("01234567890!\"#\u00A4%&/()=?^*_:;><,.-'\u00A8\u00B4+")) < 0)
            return false;
      }

      if (sPassword.CompareNoCase(_T("secret")) == 0) return false;
      if (sPassword.CompareNoCase(_T("password")) == 0) return false;
      if (sPassword.CompareNoCase(_T("info")) == 0) return false;
      if (sPassword.CompareNoCase(_T("webmaster")) == 0) return false;
      if (sPassword.CompareNoCase(_T("admin")) == 0) return false;
      if (sPassword.CompareNoCase(_T("12345")) == 0) return false;
      if (sPassword.CompareNoCase(_T("qwerty")) == 0) return false;

      return true;
   }

}
