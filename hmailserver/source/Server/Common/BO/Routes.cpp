// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "Routes.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Routes::Routes()
   {
      
   }

   Routes::~Routes()
   {

   }

   void 
   Routes::Refresh()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Reloads all routes from the database.
   //---------------------------------------------------------------------------()
   {
      String sSQL = "select * from hm_routes order by routedomainname asc";
      DBLoad_(sSQL);
   }

   std::shared_ptr<Route> 
   Routes::GetItemByNameWithWildcardMatch(const String &domainName)
   {
      // Under the collection's lock, as GetItemByName and GetItemByDBID are.
      // This is SMTPConfiguration's collection, asked by every session and
      // delivery, and changed in place by the Control Panel (InterfaceRoutes
      // holds this very object) and by REST.
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      auto iter = vecObjects.begin();
      auto iterEnd = vecObjects.end();

      for (; iter != iterEnd; iter++)
      {
         std::shared_ptr<Route> pRoute = (*iter);

         if (StringParser::WildcardMatchNoCase(pRoute->DomainName(), domainName))
         {
            return pRoute;
         }
      }

      std::shared_ptr<Route> empty;
      return empty;
   }

}
