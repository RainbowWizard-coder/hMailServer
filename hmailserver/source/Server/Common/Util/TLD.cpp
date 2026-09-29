// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "TLD.h"

#include "Utilities.h"
#include "FileUtilities.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   TLD::TLD(void)
   {
      
   }

   TLD::~TLD(void)
   {
   }

   void 
   TLD::Initialize()
   {
      // Read into a set of its own and swapped in whole, so that a lookup on another
      // thread never sees the list half read.
      std::set<String> tlds;

      String sTLDSFile = Utilities::GetBinDirectory();
      if (sTLDSFile.Right(1) != FileUtilities::PathSeparator)
         sTLDSFile += FileUtilities::PathSeparator;
      sTLDSFile += "tlds.txt";

#ifdef _DEBUG
      if (!FileUtilities::Exists(sTLDSFile))
      {
         // We're running in debug. Since the tlds.txt
         // is not copied to the Source\hMailServer\Debug directory, 
         // we try to find it on a hard coded place.

         sTLDSFile = "C:\\Program Files (x86)\\hMailServer\\Bin\\tlds.txt";
      }
#endif

      String sContents = FileUtilities::ReadCompleteTextFile(sTLDSFile);

      if (sContents.IsEmpty())
      {
         // Failed to load file containing TLD's. Report an error.
         String sErrorMessage;
         sErrorMessage.Format(_T("Failed to load file with TLD's %s."), sTLDSFile.c_str());
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4335, "TLD::_LoadCCTLD", sErrorMessage);
      }
      else
      {
         std::vector<String> vecTLDs = StringParser::SplitString(sContents, "\r\n");
         for (auto iter = vecTLDs.begin(); iter != vecTLDs.end(); iter++)
            tlds.insert(*iter);
      }

      // Empty when the file could not be read, as the list always was then; the
      // list replaced is let go of after the lock.
      {
         boost::lock_guard<boost::mutex> guard(tld_mutex_);
         tld_.swap(tlds);
      }
   }

   bool 
   TLD::IsTLD(const String &sName)
   {
      String sTmp = sName;

      sTmp.ToLower();

      boost::lock_guard<boost::mutex> guard(tld_mutex_);

      if (tld_.find(sName) != tld_.end())
         return true;
      else
         return false;
   }

   bool 
   TLD::GetDomainNameFromHost(String &sHost, bool &bIsIPAddress)
   {
      bIsIPAddress = false;

      // Start at the end.
      std::vector<String> vecParts = StringParser::SplitString(sHost, ".");

      size_t iParts = vecParts.size();

      if (iParts < 2)
         return false;

      // Check if it's an IP address
      if (iParts == 4 && StringParser::IsValidIPAddress(sHost))
      {
         sHost = vecParts[3] + "." + vecParts[2] + "." + vecParts[1] + "." + vecParts[0];
         bIsIPAddress = true;
         return true;
      }

      // It's not an IP address.
      String sTwoLast = vecParts[iParts-2] + "." + vecParts[iParts-1];
      sTwoLast.ToLower();

      if (!IsTLD(sTwoLast))
         sHost = sTwoLast;
      else
      {
         if (iParts == 2)
            return false;

         sHost = vecParts[iParts-3] + "." + vecParts[iParts-2] + "." + vecParts[iParts-1];
      }

      return true;      
   }

}