// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPCommandCopy.h"
#include "IMAPCopy.h"
#include "IMAPConnection.h"

#include "../Common/BO/IMAPFolder.h"

#include "IMAPSimpleCommandParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPCommandCOPY::IMAPCommandCOPY()
   {

   }

   IMAPCommandCOPY::~IMAPCommandCOPY()
   {

   }


   IMAPResult
   IMAPCommandCOPY::ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pConnection->IsAuthenticated())
         return IMAPResult(IMAPResult::ResultNo, "Authenticate first");
      
      if (!pConnection->GetCurrentFolder())
         return IMAPResult(IMAPResult::ResultNo, "No folder selected.");

      std::shared_ptr<IMAPCopy> pCopy = std::shared_ptr<IMAPCopy>(new IMAPCopy());
      pCopy->SetIsUID(false);

      String sResponse; 

      std::shared_ptr<IMAPSimpleCommandParser> pParser = std::shared_ptr<IMAPSimpleCommandParser>(new IMAPSimpleCommandParser());
      pParser->Parse(pArgument);
      // No "\r\n" in a result's text: the connection ends the tagged line
      // itself, and the text's own line break put an empty line after it,
      // which a client reads as a malformed response.
      if (pParser->ParamCount() != 2)
         return IMAPResult(IMAPResult::ResultBad, "Command requires 2 parameters.");

      String sMailNo = pParser->GetParamValue(pArgument, 0);
      String sFolderName = pParser->GetParamValue(pArgument, 1);

      pArgument->Command("\"" + sFolderName + "\"");

      // We should check if the folder exists. If not, notify user with trycreate
      std::shared_ptr<IMAPFolder> pFolder = pConnection->GetFolderByFullPath(sFolderName);

      if (!pFolder)
      {
         // Nope, doesn't exist. RFC 3501 6.4.7: "[TRYCREATE]" says so, so a
         // client may create the mailbox and try again - the comment above said
         // it was sent, and it was not.
         return IMAPResult(IMAPResult::ResultNo, "[TRYCREATE] Can't find mailbox with that name.");
      }

      IMAPResult result = pCopy->DoForMails(pConnection, sMailNo, pArgument);

      // RFC 3501 6.4.7: a COPY that fails leaves the destination as it was.
      if (result.GetResult() != IMAPResult::ResultOK)
         pCopy->RollBack(pConnection);

      if (result.GetResult() == IMAPResult::ResultOK)
      {
         // The other sessions are told before the OK, so that one asked next
         // already knows of the copies (SubmitDelayedChangeNotification).
         pConnection->SubmitDelayedChangeNotification();
         pConnection->SendAsciiData(pArgument->Tag() + " OK " + pCopy->GetUIDPlusResponseCode() + "COPY completed\r\n");
      }

      return result;
   }
}
