// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPCommandStatus.h"

#include "IMAPCommandAppend.h"
#include "IMAPConnection.h"
#include "IMAPSimpleCommandParser.h"
#include "../Common/BO/IMAPFolders.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/BO/IMAPFolder.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   
   IMAPResult
   IMAPCommandSTATUS::ExecuteCommand(std::shared_ptr<HM::IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument)
   {

      if (!pConnection->IsAuthenticated())
         return IMAPResult(IMAPResult::ResultNo, "Authenticate first");

      std::shared_ptr<IMAPSimpleCommandParser> pParser = std::shared_ptr<IMAPSimpleCommandParser>(new IMAPSimpleCommandParser());

      pParser->Parse(pArgument);

      if (pParser->ParamCount() != 1 && pParser->ParamCount() != 2)
         return IMAPResult(IMAPResult::ResultBad, "STATUS Command requires 1 or 2 parameters.");

      String sFolderName = pParser->GetParamValue(pArgument, 0);
      String sFlags = pParser->GetParamValue(pArgument, 1);

      // NO, not BAD, for both refusals below: BAD says the command was malformed
      // (RFC 3501 7.1.3), and a mailbox that does not exist or may not be read
      // is a well-formed command the server declines (7.1.2; RFC 4314 section 4).
      std::shared_ptr<IMAPFolder> pTheFolder = pConnection->GetFolderByFullPath(sFolderName);
      if (!pTheFolder)
         return IMAPResult(IMAPResult::ResultNo, "Folder could not be found.");

      // Check if ther user has access to read this folder.
      if (!pConnection->CheckPermission(pTheFolder, ACLPermission::PermissionRead))
         return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: Read permission denied (Required for STATUS command).");

      // RFC 7162 section 3.1: STATUS (HIGHESTMODSEQ) is a CONDSTORE-enabling
      // command; with a mailbox selected, that mailbox's HIGHESTMODSEQ is due.
      if (sFlags.FindNoCase(_T("HIGHESTMODSEQ")) >= 0)
         pConnection->OnCondstoreEnablingCommand();

      String sResponse = CreateStatusLine(pConnection, pTheFolder, sFolderName, sFlags);

      sResponse += pArgument->Tag() + " OK Status completed\r\n";

      pConnection->SendAsciiData(sResponse);

      return IMAPResult();
   }

   // static
   String
   IMAPCommandSTATUS::CreateStatusLine(std::shared_ptr<HM::IMAPConnection> pConnection, std::shared_ptr<IMAPFolder> pTheFolder, String sFolderName, const String &sFlags)
   {
      std::shared_ptr<Messages> pMessages = pTheFolder->GetMessages();

      String sResponse = "";

      bool bAddSpace = false;

      IMAPFolder::EscapeFolderString(sFolderName);
      sResponse = "* STATUS \"" + sFolderName + "\" ";

      sResponse+= "(";
      if (sFlags.FindNoCase(_T("MESSAGES")) >= 0)
      {
         String sTemp;
         sTemp.Format(_T("MESSAGES %d"), pMessages->GetCount());
         
         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse +=sTemp;
        
      }

      if (sFlags.FindNoCase(_T("UNSEEN")) >= 0)
      {
         String sTemp;
         long lMsgCount = pMessages->GetCount();
         long lSeen = pMessages->GetNoOfSeen();
         long lUnseen = lMsgCount - lSeen;
         sTemp.Format(_T("UNSEEN %d"), lUnseen);


         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse +=sTemp;
      }

      if (sFlags.FindNoCase(_T("RECENT")) >= 0 && !pConnection->GetImap4Rev2Enabled())
      {
         // RFC 9051 (IMAP4rev2): RECENT was removed as a STATUS data item.
         String sTemp;

         // Count \Recent in the folder being asked about. Using the connection's
         // set reported the SELECTED folder's count for every folder queried, so
         // clients polling all folders lit up new-mail indicators on folders that
         // had received nothing. For the selected folder the session's set is
         // still the right answer, because its recent flags have already been
         // consumed from the database by SELECT.
         std::shared_ptr<IMAPFolder> pCurrentFolder = pConnection->GetCurrentFolder();
         bool queryingSelectedFolder = pCurrentFolder && pCurrentFolder->GetID() == pTheFolder->GetID();

         long lNoOfRecent = queryingSelectedFolder
            ? (long) pConnection->GetRecentMessageCount()
            : (long) pMessages->GetRecentMessageIDs().size();

         sTemp.Format(_T("RECENT %d"), lNoOfRecent);

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse +=sTemp;
      }

      if (sFlags.FindNoCase(_T("UIDNEXT")) >= 0)
      {
         String sTemp;
         sTemp.Format(_T("UIDNEXT %u"),  pTheFolder->GetCurrentUID() + 1);

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse += sTemp;
      }

      if (sFlags.FindNoCase(_T("UIDVALIDITY")) >= 0)
      {

         String sTemp;
         sTemp.Format(_T("UIDVALIDITY %d"), pTheFolder->GetCreationTime().ToInt());

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse +=sTemp;
      }

      if (sFlags.FindNoCase(_T("SIZE")) >= 0)
      {
         // RFC 8438: total size of the mailbox in octets (sum of RFC822.SIZE).
         __int64 iTotalSize = 0;
         for (std::shared_ptr<Message> oMessage : pMessages->GetCopy())
            iTotalSize += oMessage->GetSize();

         String sTemp;
         sTemp.Format(_T("SIZE %I64d"), iTotalSize);

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse += sTemp;
      }

      if (sFlags.FindNoCase(_T("HIGHESTMODSEQ")) >= 0)
      {
         // RFC 7162 (CONDSTORE/QRESYNC): the mailbox highest mod-sequence.
         String sTemp;
         sTemp.Format(_T("HIGHESTMODSEQ %I64d"), pTheFolder->GetCurrentModSeq());

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse += sTemp;
      }

      if (sFlags.FindNoCase(_T("MAILBOXID")) >= 0)
      {
         // RFC 8474: the folder's database id - stable across RENAME.
         String sTemp;
         sTemp.Format(_T("MAILBOXID (F%I64d)"), pTheFolder->GetID());

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse += sTemp;
      }

      if (sFlags.FindNoCase(_T("APPENDLIMIT")) >= 0)
      {
         // RFC 7889: the pre-authentication capability is the bare APPENDLIMIT
         // form, and a server advertising that form must answer this item. The
         // number is per-account rather than per-mailbox here, so every mailbox
         // reports the same limit APPEND will actually enforce.
         String sTemp;
         sTemp.Format(_T("APPENDLIMIT %I64d"),
            IMAPCommandAppend::GetEffectiveAppendLimitBytes(pConnection->GetAccount()));

         if (bAddSpace)
            sResponse += " ";
         else
            bAddSpace = true;

         sResponse += sTemp;
      }


      sResponse+= ")\r\n";

      return sResponse;
   }
}