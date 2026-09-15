// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class COMAuthenticator
   {
   public:

      COMAuthenticator()
      {
         // Create a dummy object so that it always exists.
         authentication_ = std::shared_ptr<HM::COMAuthentication>(new COMAuthentication);

         authentication_->AttempAnonymousAuthentication();
      }
      
      void SetAuthentication(std::shared_ptr<HM::COMAuthentication> pAuthentication)
      {
         authentication_ = pAuthentication; 
      }

   protected:

      bool GetIsServerAdmin()
      {
         if (!authentication_)
            return false;

         if (!authentication_->GetIsServerAdmin())
            return false;

         return true;
      }

      // The narrow roles' entry points. A COM object asks for the permission
      // it needs rather than for a level, which is how a help desk can reset a
      // password without being able to change a setting. Only the handful of
      // places named in build/check-role-choke-point.py may call these; every
      // other gate stays GetIsServerAdmin or GetIsDomainAdmin, which are the
      // same question asked of the same class.
      bool Allows(RolePermission permission)
      {
         if (!authentication_)
            return false;

         return authentication_->Allows(permission);
      }

      bool AllowsForDomain(RolePermission permission, const String &domainName)
      {
         if (!authentication_)
            return false;

         return authentication_->AllowsForDomain(permission, domainName);
      }

      int GetAccessDenied()
      {
         if (!authentication_)
            return -1;

         return authentication_->GetAccessDenied();
      }

      std::shared_ptr<COMAuthentication> authentication_;

   private:

   };

}