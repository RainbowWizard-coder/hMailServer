// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DeliveryTask.h"
#include "SMTPDeliverer.h"
#include "SMTPDeliveryManager.h"

#include "../Common/BO/Message.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Util/ServerStatus.h"



#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DeliveryTask::DeliveryTask(std::shared_ptr<Message> pMessage) :
      Task("DeliveryTask"),
      message_(pMessage)
   {
      
   }

   DeliveryTask::~DeliveryTask(void)
   {
   }

   void
   DeliveryTask::DoWork()
   {
      // A drain pauses the delivery manager, but a task it had already handed
      // to the queue can still reach the front of it once a thread frees up -
      // which is exactly when the drain is waiting for that thread's delivery
      // to finish. "A delivery already on a thread finishes; one not yet
      // started is not started" is decided here: the message goes back
      // unlocked, exactly as it was, for the restart or the resume to pick up.
      if (SMTPDeliveryManager::IsDeliveryPaused())
      {
         PersistentMessage::UnlockObject(message_);
         return;
      }

      // Counted here, once the delivery is really starting, and not where the
      // manager handed the task out: the un-started task above was never
      // processed, and the status counters should not say it was.
      ServerStatus::Instance()->OnMessageProcessed();

      // Do our delivery work.
      SMTPDeliverer::DeliverMessage(message_);
   }

}