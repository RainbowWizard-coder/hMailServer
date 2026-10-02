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
         "\"description\":\"Thirteen items every installation has to get past - a profile for the kind of server it is (profile_none on a young server, fewer than ten accounts outside the sample domain, that no profile has been applied to; profile_applied, profile_kept or profile_reverted once one has been, profile_failed, action needed, when one could not be written in full, profile_declined when the shipped settings were chosen instead, and profile_in_service on a server past its first accounts), a domain (domain_name_reserved, action needed, for one whose name is a directory the server keeps its own files in under the data directory), an account, TLS on the submission port (submission_self_signed, action needed, while that is the self-signed certificate a new installation made), passwords only over TLS (sign_in_tls: sign_in_plain, action needed, for each IP range outside the loopback that takes a password before TLS on listeners that all offer STARTTLS with a certificate, naming them; sign_in_no_tls, action needed and with no fix, when one of them has no TLS to start, so that requiring it would lock every client there out), no password on port 25, where other servers deliver (delivery_port_auth: delivery_port_auth_on, action needed and with no fix, for an SMTP listener on 25 whose authentication is On - an upgraded server's, since only a new installation makes it a honeypot; delivery_port_auth_honeypot, delivery_port_auth_off, delivery_port_auth_no_ban while minutes_to_ban is 0, and delivery_port_auth_disabled when DisableAUTHList names the port; delivery_port_none, not needed, without such a listener), SPF, DKIM, DMARC, a backup destination, a second factor on the administrator password, the stored-secrets key made, the database's and exported (stored_secrets_key_none, stored_secrets_key_not_held, stored_secrets_key_not_exported, stored_secrets_key_exported; stored_secrets_off, not needed, with ProtectStoredSecretsWithDPAPI at 0), and service_account, the account the server runs as (service_account_local_system or service_account_root, action needed, while it is the most privileged one on the machine; service_account_own otherwise; on Windows, beside it, autoban_firewall_not_permitted and update_apply_by_administrator for what an account that is not an administrator cannot do) - each done, action_needed, cannot_tell or not_needed, with findings that carry a code, its arguments and an English sentence. A finding the server can put right in one step carries fix, the REST request that takes it - {method, path, body}, sign_in_plain's being PUT /api/v1/ipranges/{id} with require_tls_for_auth true, offered only when every listener it names can start TLS - which the route answers as it answers any caller. The three record items look SPF, DKIM and DMARC up in DNS for every active domain (the first twenty), which takes the resolver's timeout for each missing record; dns=0 skips the lookups and the findings say which host to look up instead.\","
         "\"parameters\":[{\"name\":\"dns\",\"in\":\"query\",\"required\":false,\"schema\":{\"type\":\"string\",\"enum\":[\"0\",\"1\"]},\"description\":\"0 to skip the DNS lookups\"}],"
         "\"responses\":{\"200\":{\"description\":\"The checklist\"}}}}";

      return AnsiString(paths);
   }
}
