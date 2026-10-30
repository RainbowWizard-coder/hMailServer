// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "MoveLearning.h"

#include "AntiSpamConfiguration.h"
#include "RspamdLearner.h"
#include "SpamAssassin/SpamAssassinLearner.h"

#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/Message.h"
#include "../../IMAP/IMAPConnection.h"
#include "../../IMAP/IMAPFolderContainer.h"
#include "../../IMAP/IMAPSpecialUse.h"

#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   MoveLearning::Facts::Facts() :
      backend(BackendSpamAssassin),
      spamassassin_learn_on_move(false),
      spamassassin_enabled(false),
      is_copy(false),
      learn_from_copy(true),
      has_owner(false),
      session_account_id(0),
      source_account_id(0),
      destination_account_id(0),
      source_public(false),
      destination_public(false),
      source_junk(false),
      destination_junk(false)
   {
   }

   bool
   MoveLearning::MayTeach(const Facts &facts)
   {
      if (facts.backend == BackendSpamAssassin)
      {
         if (!facts.spamassassin_learn_on_move || !facts.spamassassin_enabled)
            return false;
      }
      else if (facts.backend != BackendRspamd)
         return false;

      // A COPY leaves the message where it was as well: a lesson only while the
      // administrator has not said that a COPY is no judgement.
      if (facts.is_copy && !facts.learn_from_copy)
         return false;

      if (!facts.has_owner)
         return false;

      // A public folder is nobody's, and a move between two accounts is a lesson
      // for neither.
      if (facts.source_public || facts.destination_public)
         return false;

      if (facts.source_account_id <= 0 || facts.source_account_id != facts.destination_account_id)
         return false;

      // Rspamd's statistics are per user, and the user is the owner: a move made
      // in another account's mailbox, shared or delegated to the session's, would
      // put the delegate's judgement into the owner's statistics.
      if (facts.backend == BackendRspamd && facts.session_account_id != facts.source_account_id)
         return false;

      return true;
   }

   bool
   MoveLearning::IsLesson(const Facts &facts, bool &spam)
   {
      spam = false;

      if (!MayTeach(facts))
         return false;

      // Junk to Junk says nothing, and neither does a move between two folders
      // neither of which is Junk.
      if (facts.source_junk == facts.destination_junk)
         return false;

      spam = facts.destination_junk;
      return true;
   }

   bool
   MoveLearning::IsJunk_(const std::map<__int64, int> &designations, __int64 folderId)
   {
      auto found = designations.find(folderId);
      return found != designations.end() && (found->second & IMAPSpecialUse::DesignationJunk) != 0;
   }

   bool
   MoveLearning::IsLesson(std::shared_ptr<IMAPConnection> connection, std::shared_ptr<IMAPFolder> source,
                          std::shared_ptr<IMAPFolder> destination, std::shared_ptr<const Account> destinationOwner,
                          bool isCopy, bool &spam, int &backend)
   {
      spam = false;

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();

      Facts facts;
      facts.backend = config.GetMoveLearningBackend();
      backend = facts.backend;

      if (facts.backend == BackendSpamAssassin)
      {
         // Read as they always were, the [Settings] key first: a server that has
         // not switched it on reads it and nothing else.
         facts.spamassassin_learn_on_move = IniFileSettings::Instance()->GetSpamAssassinLearnOnMove();
         if (!facts.spamassassin_learn_on_move)
            return false;

         facts.spamassassin_enabled = config.GetSpamAssassinEnabled();
      }

      facts.is_copy = isCopy;
      facts.learn_from_copy = isCopy ? config.GetMoveLearningFromCopy() : true;
      facts.has_owner = destinationOwner != nullptr;

      std::shared_ptr<const Account> sessionAccount = connection ? connection->GetAccount() : nullptr;
      facts.session_account_id = sessionAccount ? sessionAccount->GetID() : 0;

      if (!source || !destination)
         return false;

      facts.source_account_id = source->GetAccountID();
      facts.destination_account_id = destination->GetAccountID();
      facts.source_public = source->IsPublicFolder();
      facts.destination_public = destination->IsPublicFolder();

      if (!MayTeach(facts))
         return false;

      // Which folder is the Junk folder is what LIST tells the client: the explicit
      // designation when one was stored (CREATE ... USE, or the Control Panel), else
      // the one the folder's name earns it - so a folder simply named Junk counts,
      // the way it does in every client that files spam by name.
      std::map<__int64, int> designations;
      IMAPSpecialUse::Resolve(IMAPFolderContainer::Instance()->GetFoldersForAccount(destination->GetAccountID()), designations);
      facts.source_junk = IsJunk_(designations, source->GetID());
      facts.destination_junk = IsJunk_(designations, destination->GetID());

      return IsLesson(facts, spam);
   }

   void
   MoveLearning::LearnFromMove(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                               std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner,
                               std::shared_ptr<const Account> sessionAccount)
   {
      if (!copy)
         return;

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();
      const int backend = config.GetMoveLearningBackend();

      // SpamAssassin's as it was, call for call.
      if (backend == BackendSpamAssassin)
      {
         SpamAssassinLearner::LearnFromMove(source, destination, copy, destinationOwner);
         return;
      }

      if (!source || !destination)
         return;

      Facts facts;
      facts.backend = backend;
      facts.is_copy = false;
      facts.has_owner = destinationOwner != nullptr;
      facts.session_account_id = sessionAccount ? sessionAccount->GetID() : 0;
      facts.source_account_id = source->GetAccountID();
      facts.destination_account_id = destination->GetAccountID();
      facts.source_public = source->IsPublicFolder();
      facts.destination_public = destination->IsPublicFolder();

      if (!MayTeach(facts))
         return;

      std::map<__int64, int> designations;
      IMAPSpecialUse::Resolve(IMAPFolderContainer::Instance()->GetFoldersForAccount(destination->GetAccountID()), designations);
      facts.source_junk = IsJunk_(designations, source->GetID());
      facts.destination_junk = IsJunk_(designations, destination->GetID());

      bool spam = false;
      if (!IsLesson(facts, spam))
         return;

      Teach(backend, copy, destinationOwner, spam);
   }

   void
   MoveLearning::Teach(int backend, std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam)
   {
      if (!copy || !destinationOwner)
         return;

      if (backend == BackendRspamd)
      {
         RspamdLearner::Lesson lesson;
         lesson.copy = copy;
         lesson.owner = destinationOwner;
         lesson.spam = spam;

         std::vector<RspamdLearner::Lesson> lessons;
         lessons.push_back(lesson);
         RspamdLearner::Queue(lessons);
         return;
      }

      SpamAssassinLearner::Teach(copy, destinationOwner, spam);
   }
}
