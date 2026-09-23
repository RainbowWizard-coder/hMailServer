// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's first-hour checklist. See RestApiServer.h and
// Common/Util/SetupChecklist.h.
//
//    GET /api/v1/setup/checklist          the list, with DNS looked up
//    GET /api/v1/setup/checklist?dns=0    the list, with the record items
//                                         saying what to look up instead
//
// The route is the server's rather than a domain's - the submission port, the
// backup destination and the administrator's second factor are nobody's domain -
// so a credential confined to named domains is refused it, and reading needs
// view_configuration, as reading the listeners and the settings does. It is a
// read: nothing here changes the server, and the only traffic it causes is the
// DNS lookups a receiving server would make anyway.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "SetupChecklist.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   HttpResponse
   RestApiServer::HandleSetupChecklist_(const Route &route)
   {
      const AnsiString dns = QueryParameter_(route.query, "dns");
      const bool lookUpDns = !(dns == "0" || dns == "false" || dns == "no");
      return BuildResponse_(200, SetupChecklist::Json(lookUpDns));
   }

   AnsiString
   RestApiServer::OpenApiSetupChecklistPaths_()
   {
      static const char *paths =
         ",\"/api/v1/setup/checklist\":{\"get\":{\"summary\":\"The first-hour checklist, judged by the server\","
         "\"description\":\"Nine items every installation has to get past - a profile for the kind of server it is (profile_none on a young server, fewer than ten accounts outside the sample domain, that no profile has been applied to; profile_applied, profile_kept or profile_reverted once one has been, profile_failed, action needed, when one could not be written in full, profile_declined when the shipped settings were chosen instead, and profile_in_service on a server past its first accounts), a domain, an account, TLS on the submission port, SPF, DKIM, DMARC, a backup destination and a second factor on the administrator password - each done, action_needed, cannot_tell or not_needed, with findings that carry a code, its arguments and an English sentence. The three record items look SPF, DKIM and DMARC up in DNS for every active domain (the first twenty), which takes the resolver's timeout for each missing record; dns=0 skips the lookups and the findings say which host to look up instead.\","
         "\"parameters\":[{\"name\":\"dns\",\"in\":\"query\",\"required\":false,\"schema\":{\"type\":\"string\",\"enum\":[\"0\",\"1\"]},\"description\":\"0 to skip the DNS lookups\"}],"
         "\"responses\":{\"200\":{\"description\":\"The checklist\"}}}}";

      return AnsiString(paths);
   }
}
