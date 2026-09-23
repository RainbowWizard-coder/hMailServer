// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>

#include "../Common/Threading/Task.h"

namespace HM
{
   class Message;
   class ClientInfo;

   class SMTPDeliveryManager : public Task
   {
   public:
      SMTPDeliveryManager();
      virtual ~SMTPDeliveryManager();

      void SetDeliverMessage();
      void DoWork();

      const String &GetQueueName() const;

      void UncachePendingMessages();

      // The queue's half of a drain. Paused, DoWork hands no more messages to
      // the delivery threads, and a DeliveryTask that reaches the front of the
      // queue after the pause puts its message back unlocked and returns rather
      // than opening a session; a delivery already talking to a remote finishes.
      // Resumed, the pending list is re-read and the loop woken, so a message
      // that became due during the pause goes at once. The flag is per process:
      // a restart starts unpaused, which is what a restart is for.
      void Pause();
      void Resume();
      bool IsPaused() const { return paused_; }

      // The same flag, for a DeliveryTask on a delivery thread: static, so that
      // the task does not read the Application's manager pointer, which
      // StopServers resets on another thread while delivery threads can still
      // be running. One manager exists at a time, and a new one starts unpaused.
      static bool IsDeliveryPaused() { return paused_; }

      // Deliveries in flight: the tasks running on the delivery queue's threads
      // plus the ones accepted and waiting for a thread. What a drain waits on,
      // beside the session count.
      int GetDeliveriesInFlight() const;

      void OnPropertyChanged(std::shared_ptr<Property> pProperty);

   private:
      
      void LoadPendingMessageList_();
      std::shared_ptr<Message> GetNextMessage_();

      std::shared_ptr<DALRecordset> pending_messages_;
      
      const String queue_name_;

      bool uncache_pending_messages_;

      size_t queue_id_;

      static std::atomic<bool> paused_;

      Event deliver_messages_;
   };

}
