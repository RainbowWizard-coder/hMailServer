// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "SpamAssassinLearner.h"
#include "SpamAssassinClient.h"

#include "../AntiSpamConfiguration.h"
#include "../../Application/Application.h"
#include "../../Application/Configuration.h"
#include "../../Application/ErrorManager.h"
#include "../../Application/IniFileSettings.h"
#include "../../BO/Account.h"
#include "../../BO/IMAPFolder.h"
#include "../../BO/Message.h"
#include "../../Persistence/PersistentMessage.h"
#include "../../Persistence/MessageStore.h"
#include "../../TCPIP/DNSResolver.h"
#include "../../TCPIP/IOService.h"
#include "../../Threading/WorkQueue.h"
#include "../../Util/Event.h"
#include "../../Util/FileUtilities.h"
#include "../../Util/Unicode.h"
#include "../../Util/Strings/Formatter.h"
#include "../../../IMAP/IMAPSpecialUse.h"
#include "../../../IMAP/IMAPFolderContainer.h"
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   void
   SpamAssassinLearner::LearnFromMove(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                                      std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner)
   {
      bool spam = false;
      if (!copy || !IsLesson(source, destination, destinationOwner, spam))
         return;

      Teach(copy, destinationOwner, spam);
   }

   bool
   SpamAssassinLearner::IsLesson(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                                 std::shared_ptr<const Account> destinationOwner, bool &spam)
   {
      spam = false;

      if (!IniFileSettings::Instance()->GetSpamAssassinLearnOnMove())
         return false;

      if (!Configuration::Instance()->GetAntiSpamConfiguration().GetSpamAssassinEnabled())
         return false;

      if (!source || !destination || !destinationOwner)
         return false;

      // A lesson is a move between a user's own folders: into their Junk folder
      // (spam) or out of it (ham). Public folders and other people's mailboxes are
      // not the user's to teach with, and Junk to Junk says nothing.
      if (destination->IsPublicFolder() || source->IsPublicFolder())
         return false;

      if (source->GetAccountID() != destination->GetAccountID())
         return false;

      // Which folder is the Junk folder is what LIST tells the client: the explicit
      // designation when one was stored (CREATE ... USE, or the Control Panel), else
      // the one the folder's name earns it - so a folder simply named Junk counts,
      // the way it does in every client that files spam by name.
      std::map<__int64, int> designations;
      IMAPSpecialUse::Resolve(IMAPFolderContainer::Instance()->GetFoldersForAccount(destination->GetAccountID()), designations);
      const int sourceDesignation = designations.count(source->GetID()) ? designations[source->GetID()] : 0;
      const int destinationDesignation = designations.count(destination->GetID()) ? designations[destination->GetID()] : 0;
      const bool fromJunk = (sourceDesignation & IMAPSpecialUse::DesignationJunk) != 0;
      const bool toJunk = (destinationDesignation & IMAPSpecialUse::DesignationJunk) != 0;
      if (fromJunk == toJunk)
         return false;

      spam = toJunk;
      return true;
   }

   void
   SpamAssassinLearner::Teach(std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam)
   {
      if (!copy || !destinationOwner)
         return;

      // The user spamd learns as: the mailbox owner when preferences follow the
      // recipient, else the fixed profile, else nobody - the same rule the scan
      // applies, so the lesson lands in the store the verdicts come from.
      String user = IniFileSettings::Instance()->GetSpamAssassinUser();
      if (IniFileSettings::Instance()->GetSpamAssassinUserFromRecipient())
         user = destinationOwner->GetAddress();

      // None while the servers are stopped (the error log says it is missing):
      // the lesson is not given, and the application log says so, as it says
      // each lesson's outcome.
      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();
      if (!queue)
      {
         LOG_APPLICATION(Formatter::Format("SpamAssassin was not told message {0} of {1} is {2}: the servers are stopped.",
            copy->GetID(), destinationOwner->GetAddress(), spam ? String("spam") : String("ham")));
         return;
      }

      // The task is given the message's file as the store holds it, and makes the
      // plain file spamd is sent when it runs (SpamAssassinLearnTask::DoWork): on
      // a store that compresses or encrypts, or is an object store, that is a read
      // of the whole message and a temporary copy written, which is no work for
      // the IMAP command that queued the lesson to wait on.
      std::shared_ptr<SpamAssassinLearnTask> task = std::shared_ptr<SpamAssassinLearnTask>(
         new SpamAssassinLearnTask(PersistentMessage::GetFileName(destinationOwner, copy), spam, user,
                                   SpamAssassinLearnTask::StoredFile, destinationOwner->GetAddress()));
      queue->AddTask(task);
   }

   bool
   SpamAssassinLearner::Tell(const String &messageFile, bool spam, const String &user)
   {
      // Gone while the servers are stopped, and a lesson queued before a stop
      // can be told after it (Application::GetIOService): there is no
      // connection to make then, and the lesson is not delivered.
      std::shared_ptr<IOService> ioService = Application::Instance()->GetIOService();
      if (!ioService)
      {
         LOG_APPLICATION(Formatter::Format("The lesson for {0} was not delivered to SpamAssassin: the servers are stopped.", messageFile));
         return false;
      }

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();

      DNSResolver resolver;
      std::vector<String> ipAddresses;
      resolver.GetIpAddresses(config.GetSpamAssassinHost(), ipAddresses, true);
      if (ipAddresses.empty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5508, "SpamAssassinLearner::Tell",
            Formatter::Format("The IP address for SpamAssassin ({0}) could not be resolved; the lesson for {1} was not delivered.",
               config.GetSpamAssassinHost(), messageFile));
         return false;
      }

      std::shared_ptr<bool> completed = std::make_shared<bool>(false);
      std::shared_ptr<Event> disconnected = std::shared_ptr<Event>(new Event());

      std::shared_ptr<SpamAssassinClient> client = std::shared_ptr<SpamAssassinClient>(
         new SpamAssassinClient(messageFile, ioService->GetIOContext(), ioService->GetClientContext(), disconnected, completed));
      client->SetUser(SpamAssassinClient::UserHeaderValue(user));
      client->SetLearning(spam);

      if (!client->Connect(*ipAddresses.begin(), config.GetSpamAssassinPort(), IPAddress()))
         return false;

      // Ownership went to the connection layer; the connection's own session
      // ceiling guarantees the event fires.
      client.reset();
      disconnected->Wait();

      return *completed;
   }

   SpamAssassinLearnTask::SpamAssassinLearnTask(const String &messageFile, bool spam, const String &user, bool removeFileAfterwards) :
      Task("SpamAssassinLearnTask"),
      message_file_(messageFile),
      spam_(spam),
      user_(user),
      remove_file_afterwards_(removeFileAfterwards),
      stored_file_(false)
   {
   }

   SpamAssassinLearnTask::SpamAssassinLearnTask(const String &storedFile, bool spam, const String &user, StoredFileTag, const String &owner) :
      Task("SpamAssassinLearnTask"),
      message_file_(storedFile),
      spam_(spam),
      user_(user),
      remove_file_afterwards_(false),
      stored_file_(true),
      owner_(owner)
   {
   }

   void
   SpamAssassinLearnTask::DoWork()
   {
      String told = message_file_;
      bool removeAfterwards = remove_file_afterwards_;

      if (stored_file_)
      {
         // spamd is sent a plain file: the stored one when it is plain, else a
         // temporary copy of its original bytes, removed below once spamd has
         // had it (Persistence/MessageStore.h) - and none at all for a message
         // of a mailbox encrypted at rest, which is never written out in the
         // clear for another process (docs/EncryptionAtRest.md): such a move
         // teaches spamd nothing, and the application log says so, as it says
         // each lesson's outcome. So it does of a message no plain file could
         // be had of; the store reports why where it is an error (HM7351, or an
         // unreadable message's own).
         std::string publicKey;
         if (MessageStore::IsEncrypted(message_file_, publicKey))
         {
            LOG_APPLICATION(Formatter::Format("SpamAssassin was not told {0} is {1}{2}: the mailbox of {3} is encrypted at rest, and its messages are not written out for another program.",
               message_file_, spam_ ? String("spam") : String("ham"),
               user_.IsEmpty() ? String("") : String(" for ") + user_, owner_));
            return;
         }

         bool temporary = false;
         told = MessageStore::PlainFileFor(message_file_, temporary);
         if (told.IsEmpty())
         {
            LOG_APPLICATION(Formatter::Format("SpamAssassin was not told {0} is {1}{2}: no plain copy of it could be had.",
               message_file_, spam_ ? String("spam") : String("ham"),
               user_.IsEmpty() ? String("") : String(" for ") + user_));
            return;
         }

         removeAfterwards = temporary;
      }

      const bool acknowledged = SpamAssassinLearner::Tell(told, spam_, user_);

      // A copy made for this lesson is removed once spamd has had it, whatever
      // it answered: nothing else will ever read it, and leaving it would grow
      // the temporary directory by one message per lesson.
      if (removeAfterwards)
         FileUtilities::DeleteFile(told);

      LOG_APPLICATION(Formatter::Format("SpamAssassin was told {0} is {1}{2}: {3}.",
         message_file_, spam_ ? String("spam") : String("ham"),
         user_.IsEmpty() ? String("") : String(" for ") + user_,
         acknowledged ? String("acknowledged") : String("not acknowledged")));
   }
}
