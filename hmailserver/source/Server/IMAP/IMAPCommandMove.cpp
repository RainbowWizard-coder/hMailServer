// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPCommandMove.h"
#include "IMAPMove.h"
#include "IMAPConnection.h"

#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/ACLPermission.h"

#include "IMAPSimpleCommandParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPCommandMOVE::IMAPCommandMOVE()
   {

   }

   IMAPCommandMOVE::~IMAPCommandMOVE()
   {

   }


   IMAPResult
   IMAPCommandMOVE::ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pConnection->IsAuthenticated())
         return IMAPResult(IMAPResult::ResultNo, "Authenticate first");
      
      if (!pConnection->GetCurrentFolder())
         return IMAPResult(IMAPResult::ResultNo, "No folder selected.");

      if (pConnection->GetCurrentFolderReadOnly())
         return IMAPResult(IMAPResult::ResultNo, "MOVE command on read-only folder.");

      // NO: a missing right refuses a well-formed command (RFC 4314 section 4).
      if (!pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionExpunge))
         return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Expunge permission denied (Required for MOVE command).");

      std::shared_ptr<IMAPMove> pMove = std::shared_ptr<IMAPMove>(new IMAPMove());
      pMove->SetIsUID(false);

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

      std::shared_ptr<IMAPFolder> pFolder = pConnection->GetFolderByFullPath(sFolderName);

      if (!pFolder)
         return IMAPResult(IMAPResult::ResultNo, "[TRYCREATE] Can't find mailbox with that name.");

      IMAPResult result = pMove->DoForMails(pConnection, sMailNo, pArgument);

      if (result.GetResult() == IMAPResult::ResultOK)
      {
         // The copies are there to stay: spamd may be taught of them now.
         pMove->GiveLessons();

         // RFC 6851 section 4.3: COPYUID in an untagged OK BEFORE the EXPUNGE
         // responses, not in the tagged OK after them - a client would otherwise
         // be told a message is gone before it learns where it went.
         pMove->SendUIDPlusResponse(pConnection);
         pMove->ExpungeMovedMessages(pConnection);

         // After MOVE's own EXPUNGE responses, which number what it moved as the
         // client numbers it: what other sessions expunged meanwhile, held back
         // while the set was read (RFC 2180 4.4, as for COPY).
         pConnection->ReportExpungesHeldForCommand();

         pConnection->SubmitDelayedChangeNotification();
         pConnection->SendAsciiData(pArgument->Tag() + " OK MOVE completed\r\n");
      }
      else
      {
         // RFC 6851 3.3: a MOVE that fails leaves no message in both mailboxes. The
         // copies made before the message that could not be moved are taken out of
         // the destination again, and every original stays where it was.
         pMove->RollBack(pConnection);

         // A MOVE refused after its set was read reports them before its NO.
         pConnection->ReportExpungesHeldForCommand();
      }

      return result;
   }
}
