// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   template <class T>
   class BusinessObject
   {
   public:
      virtual ~BusinessObject() {};

      __int64 GetID() const;
      void SetID(__int64 iNewVal);

   protected:

     __int64 id_;

   private:
      // Only T derives from BusinessObject<T>, as with Singleton<T>: a class that
      // named another type here now does not compile. Named without the class
      // qualification: repeating it inside the class is an MSVC extension that a
      // conforming compiler rejects outright.
      BusinessObject();
      friend T;
   };

   template <class T>
   BusinessObject<T>::BusinessObject()
   {
      id_ = 0;
   }

   template <class T>
   __int64 BusinessObject<T>::GetID() const
   {
      return id_;
   }

   template <class T> 
   void BusinessObject<T>::SetID(__int64 iNewID)
   {
      id_ = iNewID;
   }  

}  