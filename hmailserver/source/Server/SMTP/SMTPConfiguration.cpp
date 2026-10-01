// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "SMTPConfiguration.h"
#include "SMTPDeliveryManager.h"
#include "IpWarmup.h"

#include "../Common/Application/Property.h"

#include "../Common/BO/Routes.h"
#include "../Common/BO/RemoteDomainPolicies.h"

#include "../Common/TCPIP/SocketConstants.h"
#include "../Common/Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SMTPConfiguration::SMTPConfiguration()
   { 
   
   }

   SMTPConfiguration::~SMTPConfiguration()
   {
   
   }

   bool 
   SMTPConfiguration::Load(const std::shared_ptr<SMTPConfiguration> &previous)
   {
      // The previous load's lists where it has them, read again in place: a COM
      // Routes, IncomingRelays or RemoteDomainPolicies object made before the
      // reinitialise holds the list itself, and a change made through it has to
      // reach the list the server reads. Each Refresh reads the rows into a list of
      // its own and swaps them in under the list's lock (Collection::DBLoad_), so a
      // reader sees the rows before or after, never half; one that fails leaves the
      // rows as they were.
      incoming_relays_ = previous && previous->incoming_relays_ ? previous->incoming_relays_ : std::shared_ptr<IncomingRelays>(new IncomingRelays());
      routes_ = previous && previous->routes_ ? previous->routes_ : std::shared_ptr<Routes>(new Routes());
      remote_domain_policies_ = previous && previous->remote_domain_policies_ ? previous->remote_domain_policies_ :
         std::shared_ptr<RemoteDomainPolicies>(new RemoteDomainPolicies());

      if (!incoming_relays_->Refresh())
         return false;

      routes_->Refresh();
      remote_domain_policies_->Refresh();

      return true;
   }

   std::shared_ptr<PropertySet>
   SMTPConfiguration::GetSettings_() const
   {
      return Configuration::Instance()->GetSettings();
   }

   void
   SMTPConfiguration::SetMaxSMTPConnections(int newVal)
   {
      GetSettings_()->SetLong(PROPERTY_MAXSMTPCONNECTIONS, newVal);
   }

   int
   SMTPConfiguration::GetMaxSMTPConnections()
   {
      return GetSettings_()->GetLong(PROPERTY_MAXSMTPCONNECTIONS);
   }

   void
   SMTPConfiguration::SetAuthAllowPlainText(bool newVal)
   {
      GetSettings_()->SetBool(PROPERTY_AUTHALLOWPLAINTEXT, newVal);
   }

   bool 
   SMTPConfiguration::GetAuthAllowPlainText()
   {
      return GetSettings_()->GetBool(PROPERTY_AUTHALLOWPLAINTEXT);
   }

   void
   SMTPConfiguration::SetAllowMailFromNull(bool newVal)
   {
      GetSettings_()->SetBool(PROPERTY_ALLOWMAILFROMNULL, newVal);
   }

   
   bool 
   SMTPConfiguration::GetAllowMailFromNull()
   {
      return GetSettings_()->GetBool(PROPERTY_ALLOWMAILFROMNULL);
   }

   void
   SMTPConfiguration::SetNoOfRetries(long lNoOfRetries)
   {
      GetSettings_()->SetLong(PROPERTY_SMTPNOOFTRIES, lNoOfRetries);
   }

   long
   SMTPConfiguration::GetNoOfRetries()
   {
      return GetSettings_()->GetLong(PROPERTY_SMTPNOOFTRIES);
   }


   void 
   SMTPConfiguration::SetMinutesBetweenTry(long lMinutesBetween)
   {
      GetSettings_()->SetLong(PROPERTY_SMTPMINUTESBETWEEN, lMinutesBetween);
   }

   long
   SMTPConfiguration::GetMinutesBetweenTry()
   {
      return GetSettings_()->GetLong(PROPERTY_SMTPMINUTESBETWEEN);
   }


   String 
   SMTPConfiguration::GetWelcomeMessage() const
   {
      return GetSettings_()->GetString(PROPERTY_WELCOMESMTP);
   }

   void 
   SMTPConfiguration::SetWelcomeMessage(const String &sMessage)
   {
      GetSettings_()->SetString(PROPERTY_WELCOMESMTP, sMessage);
   }

   String 
   SMTPConfiguration::GetSMTPDeliveryBindToIP() const
   {
      return GetSettings_()->GetString(PROPERTY_SMTPDELIVERYBINDTOIP);
   }

   IPAddress
   SMTPConfiguration::OutboundBindAddress(const String &configured, const IPAddress &remote)
   {
      IPAddress none;

      String list = configured;
      list.Replace(_T(";"), _T(","));
      list.Replace(_T(" "), _T(","));

      std::vector<String> entries = StringParser::SplitString(list, _T(","));

      for (String entry : entries)
      {
         entry.Trim();

         if (entry.IsEmpty())
            continue;

         IPAddress candidate;
         if (!candidate.TryParse(AnsiString(entry), true))
            continue;

         if (candidate.GetType() == remote.GetType())
            return candidate;
      }

      return none;
   }

   void 
   SMTPConfiguration::SetSMTPDeliveryBindToIP(const String &sMessage)
   {
      GetSettings_()->SetString(PROPERTY_SMTPDELIVERYBINDTOIP, sMessage);
   }


   String 
   SMTPConfiguration::GetSMTPRelayer() const
   {
      return GetSettings_()->GetString(PROPERTY_SMTPRELAYER);
   }

   void 
   SMTPConfiguration::SetSMTPRelayer(const String &sRelayer)
   {
      GetSettings_()->SetString(PROPERTY_SMTPRELAYER, sRelayer);
   }

   void 
   SMTPConfiguration::SetSMTPRelayerPort(long lPortID)
   {
      GetSettings_()->SetLong(PROPERTY_SMTPRELAYER_PORT, lPortID);
   }

   long
   SMTPConfiguration::GetSMTPRelayerPort()
   {
      return GetSettings_()->GetLong(PROPERTY_SMTPRELAYER_PORT);
   }

   void 
   SMTPConfiguration::SetSMTPRelayerConnectionSecurity(ConnectionSecurity connection_security)
   {
      GetSettings_()->SetLong(PROPERTY_SMTPRELAYERCONNECTIONSECURITY, connection_security);
   }

   ConnectionSecurity
   SMTPConfiguration::GetSMTPRelayerConnectionSecurity()
   {
      return (ConnectionSecurity) GetSettings_()->GetLong(PROPERTY_SMTPRELAYERCONNECTIONSECURITY);
   }

   void 
   SMTPConfiguration::SetSMTPConnectionSecurity(ConnectionSecurity connection_security)
   {
      GetSettings_()->SetLong(PROPERTY_SMTPCONNECTIONSECURITY, connection_security);
   }

   ConnectionSecurity
   SMTPConfiguration::GetSMTPConnectionSecurity()
   {
      return (ConnectionSecurity) GetSettings_()->GetLong(PROPERTY_SMTPCONNECTIONSECURITY);
   }

   int
   SMTPConfiguration::GetMaxNoOfDeliveryThreads()
   {
      return GetSettings_()->GetLong(PROPERTY_MAXDELIVERYTHREADS);
   }

   void 
   SMTPConfiguration::SetMaxNoOfDeliveryThreads(int lNewValue)
   {
      GetSettings_()->SetLong(PROPERTY_MAXDELIVERYTHREADS, lNewValue);
   }
   
   int
   SMTPConfiguration::GetMaxMessageSize()
   {
      return GetSettings_()->GetLong(PROPERTY_MAXMESSAGESIZE);
   }

   void 
   SMTPConfiguration::SetMaxMessageSize(int lNewValue)
   {
      GetSettings_()->SetLong(PROPERTY_MAXMESSAGESIZE, lNewValue);
   }

   int
   SMTPConfiguration::GetMaxSMTPRecipientsInBatch()
   {
      return GetSettings_()->GetLong(PROPERTY_MAXSMTPRECIPIENTSINBATCH);
   }

   void 
   SMTPConfiguration::SetMaxSMTPRecipientsInBatch(int lNewValue)
   {
      GetSettings_()->SetLong(PROPERTY_MAXSMTPRECIPIENTSINBATCH, lNewValue);
   }


   int
   SMTPConfiguration::GetRuleLoopLimit()
   {
      return GetSettings_()->GetLong(PROPERTY_RULELOOPLIMIT);
   }

   void 
   SMTPConfiguration::SetRuleLoopLimit(int lNewValue)
   {
      GetSettings_()->SetLong(PROPERTY_RULELOOPLIMIT, lNewValue);
   }


   int
   SMTPConfiguration::GetMaxNumberOfMXHosts()
   {
      return GetSettings_()->GetLong(PROPERTY_MAX_NUMBER_OF_MXHOSTS);
   }

   void 
   SMTPConfiguration::SetMaxNumberOfMXHosts(int lNewValue)
   {
      GetSettings_()->SetLong(PROPERTY_MAX_NUMBER_OF_MXHOSTS, lNewValue);
   }

   bool
   SMTPConfiguration::GetSMTPRelayerRequiresAuthentication()
   {
      return GetSettings_()->GetBool(PROPERTY_SMTPRELAYER_USEAUTH);
   }

   void
   SMTPConfiguration::SetSMTPRelayerRequiresAuthentication(bool bNewVal)
   {
      GetSettings_()->SetBool(PROPERTY_SMTPRELAYER_USEAUTH, bNewVal);
   }

   void 
   SMTPConfiguration::SetSMTPRelayerUsername(const String & sValue)
   {
      GetSettings_()->SetString(PROPERTY_SMTPRELAYER_USERNAME, sValue);
   }

   String
   SMTPConfiguration::GetSMTPRelayerUsername() const
   {
      return GetSettings_()->GetString(PROPERTY_SMTPRELAYER_USERNAME);
   }

   void 
   SMTPConfiguration::SetSMTPRelayerPassword(const String & sValue)
   {
      GetSettings_()->SetString(PROPERTY_SMTPRELAYER_PASSWORD, sValue);
   }

   String
   SMTPConfiguration::GetSMTPRelayerPassword() const
   {
      return GetSettings_()->GetString(PROPERTY_SMTPRELAYER_PASSWORD);
   }




   void
   SMTPConfiguration::SetAllowIncorrectLineEndings(bool newVal)
   {
      GetSettings_()->SetBool(PROPERTY_ALLOWINCORRECTLINEENDINGS, newVal);
   }

   bool 
   SMTPConfiguration::GetAllowIncorrectLineEndings() 
   {
      return GetSettings_()->GetBool(PROPERTY_ALLOWINCORRECTLINEENDINGS);
   }

   void
   SMTPConfiguration::SetAddDeliveredToHeader(bool newVal)
   {
      GetSettings_()->SetBool(PROPERTY_ADDDELIVEREDTOHEADER, newVal);
   }

   bool 
   SMTPConfiguration::GetAddDeliveredToHeader() 
   {
      return GetSettings_()->GetBool(PROPERTY_ADDDELIVEREDTOHEADER);
   }

   void 
   SMTPConfiguration::OnPropertyChanged(std::shared_ptr<Property> pProperty)
   {
      String sPropertyName = pProperty->GetName();

      if (sPropertyName == PROPERTY_MAXDELIVERYTHREADS)
      {
         std::shared_ptr<SMTPDeliveryManager> pDeliveryManager = Application::Instance()->GetSMTPDeliveryManager();
         if (!pDeliveryManager)
            return;

         pDeliveryManager->OnPropertyChanged(pProperty);
      }
   }

   bool 
   SMTPConfiguration::XMLStore(XNode *pBackupNode, int iOptions)
   {
      if (!routes_->XMLStore(pBackupNode, iOptions))
         return false;

      if (!incoming_relays_->XMLStore(pBackupNode, iOptions))
         return false;

      // In the backup like the routes: a configuration backup that restored
      // every route and none of the policies would put mail back on the wire
      // to a domain the administrator had required TLS for.
      if (!remote_domain_policies_->XMLStore(pBackupNode, iOptions))
         return false;

      // The IP warm-up's schedules, for the same reason: a restore onto new
      // hardware, or a move to another database, that left them behind would
      // put a warming address back on the wire with no ceiling at all. The
      // day's counts are traffic and stay behind (SMTP/IpWarmup.h).
      if (!IpWarmup::Instance()->XMLStore(pBackupNode))
         return false;

      return true;
   }

   bool
   SMTPConfiguration::XMLLoad(XNode *pBackupNode, int iRestoreOptions)
   {
      // Checked, which they were not. Configuration::XMLLoad guards every other
      // collection it restores and then calls this one, which answered true whatever
      // happened inside it - so routes and incoming relays were the two the guard did
      // not reach. Both decide where mail is allowed to go and who is allowed to send
      // through this server, so silently restoring neither is the wrong pair to be
      // quiet about.
      routes_->Refresh();
      if (!routes_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      incoming_relays_->Refresh();
      if (!incoming_relays_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      remote_domain_policies_->Refresh();
      if (!remote_domain_policies_->XMLLoad(pBackupNode, iRestoreOptions))
         return false;

      if (!IpWarmup::Instance()->XMLLoad(pBackupNode))
         return false;

      return true;
   }


}
