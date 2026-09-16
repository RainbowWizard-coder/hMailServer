// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class NotificationClient;
   class ChangeNotification;
   class NotificationClientSubscription;

   class NotificationServer
   {
   public:
      NotificationServer();

      void SendNotification(std::shared_ptr<ChangeNotification> pChangeNotification);
      void SendNotification(std::shared_ptr<NotificationClient> source, std::shared_ptr<ChangeNotification> pChangeNotification);

      __int64 SubscribeMessageChanges(__int64 accountID, __int64 folderID, std::shared_ptr<NotificationClient> pChangeClient);
      void UnsubscribeMessageChanges(__int64 accountID, __int64 folderID, __int64 subscriptionKey);

      __int64 SubscribeFolderListChanges(__int64 accountID, std::shared_ptr<NotificationClient> pChangeClient);

      // Whether a live client is watching a mailbox of this account: an IMAP
      // session subscribes the folder it SELECTs and unsubscribes it when the
      // selection ends or the connection goes, so this is the running server's
      // own answer to "is anybody in there". MailboxMove asks it, because a
      // session already open holds an Account object carrying the address the
      // move is about to change and would go on building message paths from it.
      //
      // A subscription whose client has already gone does not count: the map
      // holds weak pointers and an entry is removed when its folder is
      // unselected, so an expired one is a client that has been destroyed
      // without its last unsubscribe being seen, and refusing a move for it
      // would refuse every move on that account for ever.
      bool HasMessageSubscribers(__int64 accountID);
      void UnsubscribeFolderListChanges(__int64 accountID, __int64 subscriptionKey);

   private:

      std::set<std::shared_ptr<NotificationClient> > GetClientsToNotify_(std::shared_ptr<NotificationClient> source, std::shared_ptr<ChangeNotification> pChangeNotification);

      std::multimap<std::pair<__int64, __int64>, std::shared_ptr<NotificationClientSubscription> > message_change_subscribers_;
      std::multimap<__int64, std::shared_ptr<NotificationClientSubscription> > folder_list_change_subscribers_;

      boost::recursive_mutex mutex_;

      __int64 subscription_counter_;
   };
}
