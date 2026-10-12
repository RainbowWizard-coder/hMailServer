// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPStore.h"
#include "IMAPConnection.h"
#include "IMAPFolderView.h"

#include "../Common/Application/FolderManager.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/Messages.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/ACLPermission.h"
#include "../Common/Sieve/SieveParser.h"
#include "../Common/Util/Parsing/StringParser.h"

#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Tracking/NotificationServer.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   IMAPStore::IMAPStore()
   {

   }

   IMAPStore::~IMAPStore()
   {
      
   }


   IMAPResult
   IMAPStore::DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex,  std::shared_ptr<Message> pMessage, const std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      if (!pMessage || !pArgument)
         return IMAPResult(IMAPResult::ResultBad, "Invalid parameters");

      if (pConnection->GetCurrentFolderReadOnly())
      {
         return IMAPResult(IMAPResult::ResultNo, "Store command on read-only folder.");
      }

      // RFC 7162 (CONDSTORE): parse the optional "(UNCHANGEDSINCE <modseq>)" modifier once.
      if (!modifier_parsed_)
      {
         ParseModifier_(pArgument->Command());

         // RFC 7162 section 3.1: UNCHANGEDSINCE is a CONDSTORE-enabling command.
         if (has_unchangedsince_)
            pConnection->OnCondstoreEnablingCommand();
      }

      // MODSEQ is reported in STORE responses when CONDSTORE is enabled or a conditional
      // STORE is in progress.
      bool includeModSeq = pConnection->GetCondstoreEnabled() || has_unchangedsince_;

      // Conditional STORE: a message changed since the client's mod-sequence is left
      // untouched and added to the MODIFIED set returned in the tagged response.
      if (has_unchangedsince_ && pMessage->GetModSeq() > unchangedsince_)
      {
         modified_.push_back(GetIsUID() ? pMessage->GetUID() : (unsigned int) messageIndex);
         return IMAPResult();
      }

      // The flags and keywords are read from the command once, for the first
      // message that reaches here, and kept for the rest: the command is the
      // same for each. They were read for every message - seven searches of the
      // whole command, its keyword list split and checked - so a STORE of a
      // long keyword list over a large range took a time that grew with the
      // product of the two.
      if (!flags_parsed_)
      {
         flags_result_ = ParseFlags_(pArgument->Command());
         flags_parsed_ = true;
      }

      if (flags_result_.GetResult() != IMAPResult::ResultOK)
         return flags_result_;

      const bool bSilent = silent_;
      const bool bSeen = seen_;
      const bool bDeleted = deleted_;
      const bool bDraft = draft_;
      const bool bAnswered = answered_;
      const bool bFlagged = flagged_;
      const std::vector<String> &keywordsGiven = keywords_given_;

      // Keywords are "the other flags" of RFC 4314.
      if (!keywordsGiven.empty() && !pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteOthers))
         return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: WriteOthers permission denied (Required for STORE command).");
   
      if (bSeen)
      {
         // ACL: If user tries to change the Seen flag, check that he has permission to do so.
         if (!pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteSeen))
            return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: WriteSeen permission denied (Required for STORE command).");
      }

      if (bDeleted)
      {
         if (!pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteDeleted))
            return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: DeleteMessages permission denied (Required for STORE command).");
      }

      if (bDraft || bAnswered || bFlagged)
      {
         if (!pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteOthers))
            return IMAPResult(IMAPResult::ResultNo, "[NOPERM] ACL: WriteOthers permission denied (Required for STORE command).");
      }


      // What the command does, applied to the folder's live message under its
      // flag-change lock (FolderManager::ChangeMessageFlags): the read, the
      // change and the write are one step. They used to be three - the flags
      // changed here on the shared object, then read back and written over it
      // again with the database row - so two STOREs on one message from two
      // sessions at once could each write back what it had read, and one of
      // the two changes was lost, in memory and in the database.
      const bool removing = removing_;
      const bool adding = adding_;
      const bool replacing = replacing_;
      const bool replaceKeywords = replacing && pConnection->CheckPermission(pConnection->GetCurrentFolder(), ACLPermission::PermissionWriteOthers);

      bool changedSince = false;
      bool tooManyKeywords = false;

      auto change = [&](Message &message) -> bool
      {
         // RFC 7162 3.1.3: the UNCHANGEDSINCE test and the store it guards are
         // one operation - asked again here, under the lock, since another
         // session may have changed the message after the test above.
         if (has_unchangedsince_ && message.GetModSeq() > unchangedsince_)
         {
            changedSince = true;
            return false;
         }

         if (removing)
         {
            if (bSeen)
               message.SetFlagSeen(false);
            if (bDeleted)
               message.SetFlagDeleted(false);
            if (bDraft)
               message.SetFlagDraft(false);
            if (bAnswered)
               message.SetFlagAnswered(false);
            if (bFlagged)
               message.SetFlagFlagged(false);
            for (size_t i = 0; i < keywordsGiven.size(); i++)
               message.RemoveKeyword(keywordsGiven[i]);
         }
         else if (adding)
         {
            if (bSeen)
               message.SetFlagSeen(true);
            if (bDeleted)
               message.SetFlagDeleted(true);
            if (bDraft)
               message.SetFlagDraft(true);
            if (bAnswered)
               message.SetFlagAnswered(true);
            if (bFlagged)
               message.SetFlagFlagged(true);
            for (size_t i = 0; i < keywordsGiven.size(); i++)
            {
               // The message is put back as it was, by ChangeMessageFlags:
               // this used to return NO with the flags and the keywords before
               // this one already changed in memory and never written.
               if (!message.AddKeyword(keywordsGiven[i]))
               {
                  tooManyKeywords = true;
                  return false;
               }
            }
         }
         else if (replacing)
         {
            message.SetFlagSeen(bSeen);
            message.SetFlagDeleted(bDeleted);
            message.SetFlagDraft(bDraft);
            message.SetFlagAnswered(bAnswered);
            message.SetFlagFlagged(bFlagged);
            if (replaceKeywords)
               message.SetKeywordList(keywordsGiven);
         }

         return true;
      };

      FolderManager::FlagChangeResult result = Application::Instance()->GetFolderManager()->ChangeMessageFlags(
         pConnection->GetCurrentFolder()->GetAccountID(),
         pConnection->GetCurrentFolder()->GetID(),
         pMessage->GetID(), change, pMessage);

      if (result == FolderManager::FlagChangeDeclined)
      {
         if (changedSince)
         {
            modified_.push_back(GetIsUID() ? pMessage->GetUID() : (unsigned int) messageIndex);
            return IMAPResult();
         }

         if (tooManyKeywords)
            return IMAPResult(IMAPResult::ResultNo, "Too many keywords on the message.");

         return IMAPResult(IMAPResult::ResultNo, "Unable to store message flags.");
      }

      if (result != FolderManager::FlagChangeStored)
      {
         // A message another session expunged after this command resolved it -
         // the folder's collection no longer holds it, so there are no flags to
         // store. That is the race imaptest's checking load found, four clients
         // on one mailbox answered "NO Unable to store message flags". It is
         // treated as a message expunged before the command began (RFC 2180
         // 4.2.1 lets the server ignore it): left out, and reported to this
         // session at the next opportunity. Any other failure is still NO.
         std::shared_ptr<IMAPFolder> pFolder = pConnection->GetCurrentFolder();

         if (pFolder && !pFolder->GetMessages()->GetItemByDBID((unsigned __int64) pMessage->GetID()))
         {
            std::shared_ptr<IMAPFolderView> view = pConnection->GetCurrentFolderView();
            if (view)
               view->MarkVanished(pMessage->GetID());

            return IMAPResult();
         }

         return IMAPResult(IMAPResult::ResultNo, "Unable to store message flags.");
      }

      // RFC 3501 7.2.6: a keyword this STORE created is announced in an untagged
      // FLAGS before a FETCH response carries it - and even when none does
      // (FLAGS.SILENT), since the mailbox's flags have changed either way.
      pConnection->AnnounceKeywords(pMessage->GetKeywordList());

      if (!bSilent)
      {
         pConnection->SendAsciiData(GetMessageFlags(pMessage, messageIndex, includeModSeq));
      }
      else if (includeModSeq)
      {
         // RFC 7162: even a silent conditional/CONDSTORE STORE must report the new MODSEQ.
         String sModSeqOnly;
         sModSeqOnly.Format(_T("* %d FETCH (UID %u MODSEQ (%I64d))\r\n"), messageIndex, pMessage->GetUID(), pMessage->GetModSeq());
         pConnection->SendAsciiData(sModSeqOnly);
      }

      // BEGIN IMAP IDLE

      // Notify the mailbox notifier that the mailbox contents have changed.
      std::vector<__int64> effectedMessages;
      effectedMessages.push_back(pMessage->GetID());

      std::shared_ptr<ChangeNotification> pNotification = 
         std::shared_ptr<ChangeNotification>(new ChangeNotification(pConnection->GetCurrentFolder()->GetAccountID(), pConnection->GetCurrentFolder()->GetID(),  ChangeNotification::NotificationMessageFlagsChanged, effectedMessages));

      Application::Instance()->GetNotificationServer()->SendNotification(pConnection->GetNotificationClient(), pNotification);
      // END IMAP IDLE

      return IMAPResult();
   }

   String 
   IMAPStore::GetMessageFlags(std::shared_ptr<Message> pMessage, int messageIndex, bool includeModSeq)
   {
      // Build a flags string.
      String sFlags;

      if (pMessage->GetFlagDeleted())
      {
         if (!sFlags.IsEmpty()) 
            sFlags += " ";

         sFlags += "\\Deleted";
      }

      if (pMessage->GetFlagAnswered())
      {
         if (!sFlags.IsEmpty()) 
            sFlags += " ";

         sFlags += "\\Answered";
      }

      if (pMessage->GetFlagFlagged())
      {
         if (!sFlags.IsEmpty()) 
            sFlags += " ";

         sFlags += "\\Flagged";
      }

      if (pMessage->GetFlagDraft())
      {
         if (!sFlags.IsEmpty()) 
            sFlags += " ";

         sFlags += "\\Draft";
      }

      if (pMessage->GetFlagSeen())
      {
         if (!sFlags.IsEmpty()) 
            sFlags += " ";

         sFlags += "\\Seen";
      }


      std::vector<String> keywords = pMessage->GetKeywordList();
      for (size_t i = 0; i < keywords.size(); i++)
      {
         if (!sFlags.IsEmpty())
            sFlags += " ";
         sFlags += keywords[i];
      }

      // It really should be FETCH below...
      String sRet;
      if (includeModSeq)
         sRet.Format(_T("* %d FETCH (FLAGS (%s) UID %u MODSEQ (%I64d))\r\n"), messageIndex, sFlags.c_str(), pMessage->GetUID(), pMessage->GetModSeq());
      else
         sRet.Format(_T("* %d FETCH (FLAGS (%s) UID %u)\r\n"), messageIndex, sFlags.c_str(), pMessage->GetUID());
      return sRet;
   }

   IMAPResult
   IMAPStore::ParseFlags_(const String &sCommand)
   //---------------------------------------------------------------------------
   // DESCRIPTION:
   // Reads what the command stores - the system flags, the keywords and whether
   // they are removed, added or replace those there - into the members, once
   // for the command. An error is answered for each message as it was.
   //---------------------------------------------------------------------------
   {
      silent_ = sCommand.FindNoCase(_T("FLAGS.SILENT")) >= 0;

      // Read flags from command.
      seen_ = (sCommand.FindNoCase(_T("\\Seen")) >= 0);
      deleted_ = (sCommand.FindNoCase(_T("\\Deleted")) >= 0);
      draft_ = (sCommand.FindNoCase(_T("\\Draft")) >= 0);
      answered_ = (sCommand.FindNoCase(_T("\\Answered")) >= 0);
      flagged_ = (sCommand.FindNoCase(_T("\\Flagged")) >= 0);

      // The keywords: every atom of the flag list that is not a system flag.
      // The list is what follows FLAGS or FLAGS.SILENT, in parentheses or
      // not; an atom that is not a keyword is a syntax error.
      keywords_given_.clear();
      {
         int at = sCommand.FindNoCase(_T("FLAGS"));
         if (at >= 0)
         {
            String rest = sCommand.Mid(at + 5);
            if (rest.FindNoCase(_T(".SILENT")) == 0)
               rest = rest.Mid(7);
            rest.TrimLeft();
            rest.TrimRight();
            // RFC 3501: the flag list is one parenthesised list, or bare atoms.
            // A parenthesis anywhere else - "(Not(Valid))" - is a syntax error,
            // not two keywords; it used to be read as exactly that.
            if (!rest.IsEmpty() && rest[0] == '(')
            {
               if (rest[rest.GetLength() - 1] != ')')
                  return IMAPResult(IMAPResult::ResultBad, "Unbalanced flag list");
               rest = rest.Mid(1, rest.GetLength() - 2);
            }
            if (rest.Find(_T("(")) >= 0 || rest.Find(_T(")")) >= 0)
               return IMAPResult(IMAPResult::ResultBad, "Invalid keyword: a flag is an atom");
            std::vector<String> atoms = StringParser::SplitString(rest, " ");
            for (size_t i = 0; i < atoms.size(); i++)
            {
               String atom = atoms[i];
               atom.TrimLeft();
               atom.TrimRight();
               if (atom.IsEmpty() || atom[0] == '\\')
                  continue;
               if (!SieveParser::IsValidFlagName(atom))
                  return IMAPResult(IMAPResult::ResultBad, String("Invalid keyword: ") + atom);
               keywords_given_.push_back(atom);
            }
         }
      }

      removing_ = sCommand.FindNoCase(_T("-FLAGS")) >= 0;
      adding_ = !removing_ && sCommand.FindNoCase(_T("+FLAGS")) >= 0;
      replacing_ = !removing_ && !adding_ && sCommand.FindNoCase(_T("FLAGS")) >= 0;

      return IMAPResult();
   }

   void
   IMAPStore::ParseModifier_(const String &sCommand)
   {
      modifier_parsed_ = true;

      long lPos = sCommand.FindNoCase(_T("UNCHANGEDSINCE"));
      if (lPos < 0)
         return;

      String sRest = sCommand.Mid(lPos + 14); // length of "UNCHANGEDSINCE"
      sRest.TrimLeft();
      unchangedsince_ = _ttoi64(sRest);
      has_unchangedsince_ = true;
   }

   String
   IMAPStore::GetConditionalStoreResponseCode()
   {
      if (modified_.empty())
         return _T("");

      String sSet;
      for (size_t i = 0; i < modified_.size(); i++)
      {
         if (i > 0)
            sSet += _T(",");

         String sTemp;
         sTemp.Format(_T("%u"), modified_[i]);
         sSet += sTemp;
      }

      return _T("[MODIFIED ") + sSet + _T("] ");
   }
      

   namespace
   {
      // Message::JoinKeywords_ as 6.3.5 had it, word for word but for the name
      // and KeywordsMaximumLength's value: what the one that compares with the
      // keywords it kept is held to.
      String OldJoinKeywords_(const std::vector<String> &keywords)
      {
         String joined;
         for (size_t i = 0; i < keywords.size(); i++)
         {
            if (keywords[i].IsEmpty())
               continue;
            bool seen = false;
            for (size_t j = 0; j < i && !seen; j++)
               seen = keywords[j].CompareNoCase(keywords[i]) == 0;
            if (seen)
               continue;
            String next = joined.IsEmpty() ? keywords[i] : joined + _T(" ") + keywords[i];
            if (next.GetLength() > 500)
               break;
            joined = next;
         }
         return joined;
      }

      // xorshift64*: the same cases on every run and every platform.
      unsigned long long NextKeywordRandom_(unsigned long long &state)
      {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return state * 2685821657736338717ULL;
      }
   }

   void
   IMAPStoreKeywordsTester::Test(int cases)
   {
      // The time first: 30,000 keywords and then 30,000 of another, which a
      // STORE's flag list can be. 6.3.5 compared each with every one before it.
      {
         std::vector<String> keywords;
         for (int i = 0; i < 30000; i++)
            keywords.push_back(_T("first"));
         for (int i = 0; i < 30000; i++)
            keywords.push_back(_T("Second"));

         Message message(false);

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         message.SetKeywordList(keywords);
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("60,000 repeated keywords took " + std::to_string((long long) milliseconds) + " ms to store");

         if (message.GetKeywords() != _T("first Second"))
            HM_TEST_FAILED();
      }

      // Then generated keyword lists: what 6.3.5 kept of them.
      String longOne;
      for (int i = 0; i < 120; i++)
         longOne += _T("k");

      const String pieces[] =
      {
         _T("a"), _T("A"), _T("b"), _T("B"), _T("flag"), _T("FLAG"), _T("Flag"), _T("x"), _T(""), longOne,
         _T("$Forwarded"), _T("$forwarded")
      };
      const int pieceCount = (int) (sizeof(pieces) / sizeof(pieces[0]));

      unsigned long long state = 0x3C6EF372FE94F82BULL;

      for (int number = 0; number < cases; number++)
      {
         std::vector<String> keywords;
         const int count = (int) (NextKeywordRandom_(state) % 31);
         for (int i = 0; i < count; i++)
            keywords.push_back(pieces[NextKeywordRandom_(state) % pieceCount]);

         Message message(false);
         message.SetKeywordList(keywords);

         if (message.GetKeywords() != OldJoinKeywords_(keywords))
            throw std::logic_error("The keywords kept were other than 6.3.5's in case " + std::to_string(number));
      }
   }
}
