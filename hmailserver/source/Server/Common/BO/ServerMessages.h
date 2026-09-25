// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

namespace HM
{
   class ServerMessage;
   class PersistentServerMessage;

   class ServerMessages : public Collection<ServerMessage, PersistentServerMessage>
   {
   public:
      ServerMessages();
      ~ServerMessages(void);

      // Refreshes this collection from the database. False when the read failed,
      // and the collection is then left as it was (Collection::DBLoad_).
      bool Refresh();

      String GetMessage(const String &sName) const;
   
   protected:
      virtual String GetCollectionName() const {return "ServerMessages"; }
   private:
     
   };
}