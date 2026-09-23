// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"


// InterfaceSettings

class ATL_NO_VTABLE InterfaceSettings : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceSettings, &CLSID_Settings>,
   public IDispatchImpl<IInterfaceSettings, &IID_IInterfaceSettings, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator,
   public ISupportErrorInfo
{
public:

   InterfaceSettings();
   

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACESETTINGS)


BEGIN_COM_MAP(InterfaceSettings)
   COM_INTERFACE_ENTRY(IInterfaceSettings)
   COM_INTERFACE_ENTRY(IDispatch)
   COM_INTERFACE_ENTRY(ISupportErrorInfo)
END_COM_MAP()


   DECLARE_PROTECT_FINAL_CONSTRUCT()

   HRESULT FinalConstruct()
   {
      return S_OK;
   }
   
   void FinalRelease() 
   {
   }

   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

   STDMETHOD(get_Scripting)(/*[out, retval]*/ IInterfaceScripting **pVal);
   STDMETHOD(get_Backup)(/*[out, retval]*/ IInterfaceBackupSettings **pVal);
   STDMETHOD(get_Directories)(/*[out, retval]*/ IInterfaceDirectories **pVal);
   STDMETHOD(get_AntiSpam)(/*[out, retval]*/ IInterfaceAntiSpam **pVal);
   STDMETHOD(get_ServerMessages)(/*[out, retval]*/ IInterfaceServerMessages **pVal);
   STDMETHOD(get_TCPIPPorts)(/*[out, retval]*/ IInterfaceTCPIPPorts **pVal);
   STDMETHOD(get_SSLCertificates)(/*[out, retval]*/ IInterfaceSSLCertificates **pVal);
   STDMETHOD(get_Groups)(/*[out, retval]*/ IInterfaceGroups * *pVal);
   STDMETHOD(get_IncomingRelays)(/*[out, retval]*/ IInterfaceIncomingRelays * *pVal);
   STDMETHOD(get_MessageIndexing)(/*[out, retval]*/ IInterfaceMessageIndexing * *pVal);

   STDMETHOD(get_ServiceIMAP)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ServiceIMAP)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ServicePOP3)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ServicePOP3)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ServiceSMTP)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ServiceSMTP)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_WelcomeIMAP)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_WelcomeIMAP)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_WelcomePOP3)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_WelcomePOP3)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_WelcomeSMTP)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_WelcomeSMTP)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_SMTPRelayer)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SMTPRelayer)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_SMTPRelayerPort)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SMTPRelayerPort)(/*[in]*/ long newVal);

   STDMETHOD(get_MaxMessageSize)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxMessageSize)(/*[in]*/ long newVal);

   STDMETHOD(get_SMTPRelayerRequiresAuthentication)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SMTPRelayerRequiresAuthentication)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_SMTPRelayerUsername)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SMTPRelayerUsername)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_UserInterfaceLanguage)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_UserInterfaceLanguage)(/*[in]*/ BSTR newVal);


   STDMETHOD(SetSMTPRelayerPassword)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_HostName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_HostName)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_DenyMailFromNull)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_DenyMailFromNull)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_AllowSMTPAuthPlain)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_AllowSMTPAuthPlain)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_MirrorEMailAddress)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_MirrorEMailAddress)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_MaxPOP3Connections)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxPOP3Connections)(/*[in]*/ long newVal);
   STDMETHOD(get_MaxSMTPConnections)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxSMTPConnections)(/*[in]*/ long newVal);
   STDMETHOD(get_MaxIMAPConnections)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxIMAPConnections)(/*[in]*/ long newVal);

   STDMETHOD(get_Logging)(/*[out, retval]*/ IInterfaceLogging** pVal);
   STDMETHOD(get_AntiVirus)(/*[out, retval]*/ IInterfaceAntiVirus** pVal);
   STDMETHOD(get_Cache)(/*[out, retval]*/ IInterfaceCache** pVal);

   STDMETHOD(get_SecurityRanges)(IInterfaceSecurityRanges **pVal);

   STDMETHOD(get_SMTPNoOfTries)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SMTPNoOfTries)(/*[in]*/ long newVal);
   STDMETHOD(get_SMTPMinutesBetweenTry)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SMTPMinutesBetweenTry)(/*[in]*/ long newVal);


   STDMETHOD(get_MaxDeliveryThreads)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxDeliveryThreads)(/*[in]*/ long newVal);

   STDMETHOD(get_Routes)(IInterfaceRoutes **pVal);
   STDMETHOD(get_RemoteDomainPolicies)(IInterfaceRemoteDomainPolicies **pVal);

   STDMETHOD(get_RuleLoopLimit)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_RuleLoopLimit)(/*[in]*/ long newVal);

   STDMETHOD(get_DefaultDomain)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_DefaultDomain)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_IMAPPublicFolderName)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_IMAPPublicFolderName)(/*[in]*/ BSTR newVal);


   STDMETHOD(get_SMTPDeliveryBindToIP)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SMTPDeliveryBindToIP)(/*[in]*/ BSTR newVal);

   
   STDMETHOD(get_IMAPSortEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPSortEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_IMAPQuotaEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPQuotaEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_IMAPIdleEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPIdleEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_IMAPACLEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPACLEnabled)(/*[in]*/ VARIANT_BOOL newVal);


   STDMETHOD(get_WorkerThreadPriority)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_WorkerThreadPriority)(/*[in]*/ long newVal);

   STDMETHOD(get_TCPIPThreads)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_TCPIPThreads)(/*[in]*/ long newVal);

   STDMETHOD(get_AllowIncorrectLineEndings)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_AllowIncorrectLineEndings)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_MaxSMTPRecipientsInBatch)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxSMTPRecipientsInBatch)(/*[in]*/ long newVal);

   STDMETHOD(get_DisconnectInvalidClients)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_DisconnectInvalidClients)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_MaxNumberOfInvalidCommands)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxNumberOfInvalidCommands)(/*[in]*/ long newVal);

   STDMETHOD(get_SMTPRelayerUseSSL)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SMTPRelayerUseSSL)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_SMTPRelayerConnectionSecurity)(/*[out, retval]*/ eConnectionSecurity *pVal);
   STDMETHOD(put_SMTPRelayerConnectionSecurity)(/*[in]*/ eConnectionSecurity newVal);

   STDMETHOD(get_SMTPConnectionSecurity)(/*[out, retval]*/ eConnectionSecurity *pVal);
   STDMETHOD(put_SMTPConnectionSecurity)(/*[in]*/ eConnectionSecurity newVal);


   STDMETHOD(get_AddDeliveredToHeader)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_AddDeliveredToHeader)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(SetAdministratorPassword)(/*[in]*/ BSTR newVal);
   STDMETHOD(EnrolAdministratorTOTP)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(DisableAdministratorTOTP)();

   STDMETHOD(get_PublicFolders)(/*[out, retval]*/ IInterfaceIMAPFolders** pVal);

   STDMETHOD(get_PublicFolderDiskName)(/*[out, retval]*/ BSTR *pVal);

   STDMETHOD(get_AutoBanOnLogonFailure)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_AutoBanOnLogonFailure)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_MaxInvalidLogonAttempts)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxInvalidLogonAttempts)(/*[in]*/ long newVal);

   STDMETHOD(get_MaxInvalidLogonAttemptsWithin)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxInvalidLogonAttemptsWithin)(/*[in]*/ long newVal);

   STDMETHOD(get_AutoBanMinutes)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_AutoBanMinutes)(/*[in]*/ long newVal);

   STDMETHOD(ClearLogonFailureList)();

   STDMETHOD(get_IMAPHierarchyDelimiter)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_IMAPHierarchyDelimiter)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_MaxAsynchronousThreads)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxAsynchronousThreads)(/*[in]*/ long newVal);

   STDMETHOD(get_MaxNumberOfMXHosts)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxNumberOfMXHosts)(/*[in]*/ long newVal);
   
   STDMETHOD(get_VerifyRemoteSslCertificate)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_VerifyRemoteSslCertificate)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_SslCipherList)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_SslCipherList)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_TlsVersion10Enabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsVersion10Enabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_TlsVersion11Enabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsVersion11Enabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_TlsVersion12Enabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsVersion12Enabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_TlsVersion13Enabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsVersion13Enabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_TlsOptionPreferServerCiphersEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsOptionPreferServerCiphersEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_QuotaGracePercent)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_QuotaGracePercent)(/*[in]*/ long newVal);
   STDMETHOD(get_QuotaSendStopPercent)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_QuotaSendStopPercent)(/*[in]*/ long newVal);

   // Outbound shaping (SMTP/DestinationBackoff.h). See the IDL.
   STDMETHOD(get_OutboundBackoffMinutes)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_OutboundBackoffMinutes)(/*[in]*/ long newVal);
   STDMETHOD(get_OutboundBackoffMaxMinutes)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_OutboundBackoffMaxMinutes)(/*[in]*/ long newVal);
   STDMETHOD(get_MaxOutboundConnectionsPerDestination)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxOutboundConnectionsPerDestination)(/*[in]*/ long newVal);
   STDMETHOD(get_MaxOutboundConnectionsPerTenant)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_MaxOutboundConnectionsPerTenant)(/*[in]*/ long newVal);
   STDMETHOD(get_TlsOptionPrioritizeChaChaEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_TlsOptionPrioritizeChaChaEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_CrashSimulationMode)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_CrashSimulationMode)(/*[in]*/ long newVal);

   STDMETHOD(get_IMAPMasterUser)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_IMAPMasterUser)(/*[in]*/ BSTR newVal);

   STDMETHOD(get_IMAPSASLPlainEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPSASLPlainEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_IMAPSASLInitialResponseEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IMAPSASLInitialResponseEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_IPv6PreferredEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_IPv6PreferredEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_RewriteEnvelopeFromWhenForwarding)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_RewriteEnvelopeFromWhenForwarding)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_CreateDefaultSpecialUseFoldersEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_CreateDefaultSpecialUseFoldersEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   // The password breach check (PasswordBreachCheck), in hm_settings.
   STDMETHOD(get_BreachCheckEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_BreachCheckEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_BreachCheckUrl)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_BreachCheckUrl)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_BreachCheckMinimumCount)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BreachCheckMinimumCount)(/*[in]*/ long newVal);
   STDMETHOD(get_BreachCheckUnreachableAction)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BreachCheckUnreachableAction)(/*[in]*/ long newVal);
   STDMETHOD(TestBreachCheckEndpoint)(/*[in]*/ BSTR Url, /*[out, retval]*/ long *Entries);

   // Bounces read, and the suppression list. See the IDL.
   STDMETHOD(get_BounceProcessingEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_BounceProcessingEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_BounceHardBounceThreshold)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BounceHardBounceThreshold)(/*[in]*/ long newVal);
   STDMETHOD(get_BounceMailboxFullIsHard)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_BounceMailboxFullIsHard)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_BounceSuppressionAction)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BounceSuppressionAction)(/*[in]*/ long newVal);
   STDMETHOD(get_BounceSuppressionExpiryDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BounceSuppressionExpiryDays)(/*[in]*/ long newVal);
   STDMETHOD(get_BounceReadDeliveryReports)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_BounceReadDeliveryReports)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_BounceCountRetentionDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BounceCountRetentionDays)(/*[in]*/ long newVal);
   STDMETHOD(get_BounceReportWindowDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_BounceReportWindowDays)(/*[in]*/ long newVal);

   STDMETHOD(get_OpenPGPWebKeyDirectoryEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_OpenPGPWebKeyDirectoryEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_OpenPGPKeyMaximumBytes)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_OpenPGPKeyMaximumBytes)(/*[in]*/ long newVal);
   STDMETHOD(get_OpenPGPKeysPerAccount)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_OpenPGPKeysPerAccount)(/*[in]*/ long newVal);
   STDMETHOD(get_OpenPGPRequireAddressInUserId)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_OpenPGPRequireAddressInUserId)(/*[in]*/ VARIANT_BOOL newVal);

   // Outbound webhooks. Four rows in hm_settings; the subscriptions themselves are
   // reached through Utilities.GetWebhookSubscriptions and over REST.
   STDMETHOD(get_WebhooksEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_WebhooksEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_WebhookMaxAttempts)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_WebhookMaxAttempts)(/*[in]*/ long newVal);
   STDMETHOD(get_WebhookMaxPerMinute)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_WebhookMaxPerMinute)(/*[in]*/ long newVal);
   STDMETHOD(get_WebhookRetentionDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_WebhookRetentionDays)(/*[in]*/ long newVal);

   // The session policies, server-wide. See SessionPolicies: 0 in either of the
   // numbers means the policy is not set, not that there is no limit.
   STDMETHOD(get_SessionIdleSeconds)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SessionIdleSeconds)(/*[in]*/ long newVal);
   STDMETHOD(get_SessionLifetimeSeconds)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SessionLifetimeSeconds)(/*[in]*/ long newVal);
   STDMETHOD(get_SessionOnePerDevice)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SessionOnePerDevice)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_SessionBindToAddress)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SessionBindToAddress)(/*[in]*/ VARIANT_BOOL newVal);

   // Complaints, read: the feedback loops. See the IDL.
   STDMETHOD(get_ComplaintProcessingEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ComplaintProcessingEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ComplaintMailbox)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ComplaintMailbox)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_ComplaintSuppressionThreshold)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_ComplaintSuppressionThreshold)(/*[in]*/ long newVal);
   STDMETHOD(get_ComplaintRetentionDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_ComplaintRetentionDays)(/*[in]*/ long newVal);

   // What this server has changed from stock, across both settings stores. Reads
   // only; see SettingsDiff for what "default" means and why a secret is named
   // and not shown.
   STDMETHOD(GetChangedFromDefault)(/*[out, retval]*/ BSTR *Json);
   STDMETHOD(GetSettingDefault)(/*[in]*/ BSTR Name, /*[out, retval]*/ BSTR *Value);

   // The [Settings] section of hMailServer.INI. See the IDL for why these exist and
   // for the two things they deliberately do not do.
   STDMETHOD(GetIniSetting)(/*[in]*/ BSTR Name, /*[out, retval]*/ BSTR *pVal);
   STDMETHOD(SetIniSetting)(/*[in]*/ BSTR Name, /*[in]*/ BSTR Value);
   STDMETHOD(DeleteIniSetting)(/*[in]*/ BSTR Name);
   STDMETHOD(get_IniSettingNames)(/*[out, retval]*/ BSTR *pVal);

   // When a saved key applies, and which saved keys wait (docs/SettingsApply.md).
   STDMETHOD(IniSettingApplies)(/*[in]*/ BSTR Name, /*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_PendingIniSettings)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(get_IniSettingAppliesAll)(/*[out, retval]*/ BSTR *pVal);

   // The quarantine digest, which lives in hm_settings rather than the ini.
   STDMETHOD(get_QuarantineDigestBaseUrl)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_QuarantineDigestBaseUrl)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_GeoIpDatabaseFile)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_GeoIpDatabaseFile)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_GeoIpDatabaseMaxAgeDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_GeoIpDatabaseMaxAgeDays)(/*[in]*/ long newVal);
   STDMETHOD(get_CountryBlockingExemptRanges)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_CountryBlockingExemptRanges)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_CountryPolicy)(/*[in]*/ BSTR Protocol, /*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_CountryPolicy)(/*[in]*/ BSTR Protocol, /*[in]*/ BSTR newVal);
   STDMETHOD(get_QuarantineDigestLinkHours)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_QuarantineDigestLinkHours)(/*[in]*/ long newVal);

   // Web Push. No getter for the private key, here or anywhere.
   STDMETHOD(get_WebPushEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_WebPushEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_WebPushSubject)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_WebPushSubject)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_WebPushVapidPublicKey)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(GenerateWebPushKeys)(/*[out, retval]*/ BSTR *PublicKey);

   // The reputation watch (ReputationWatch), in hm_settings.
   STDMETHOD(get_ReputationWatchEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ReputationWatchEnabled)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ReputationWatchLists)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ReputationWatchLists)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_ReputationWatchAddresses)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_ReputationWatchAddresses)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_HelpWikiBaseUrl)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_HelpWikiBaseUrl)(/*[in]*/ BSTR newVal);

   // Spam traps, which live in hm_settings as the digest does.
   STDMETHOD(get_SpamTrapBanMinutes)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SpamTrapBanMinutes)(/*[in]*/ long newVal);
   STDMETHOD(get_SpamTrapBlockDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SpamTrapBlockDays)(/*[in]*/ long newVal);
   STDMETHOD(get_SpamTrapHitRetentionDays)(/*[out, retval]*/ long *pVal);
   STDMETHOD(put_SpamTrapHitRetentionDays)(/*[in]*/ long newVal);
   STDMETHOD(get_SpamTrapTeachSpamAssassin)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_SpamTrapTeachSpamAssassin)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(TestLdapDirectory)(/*[in]*/ long MaxUsers, /*[out]*/ BSTR *ResultText, /*[out, retval]*/ VARIANT_BOOL *pResult);

   STDMETHOD(PreviewDirectorySync)(/*[in]*/ BSTR DomainName, /*[in]*/ VARIANT_BOOL DisableMissing, /*[out]*/ BSTR *ResultText, /*[out, retval]*/ VARIANT_BOOL *pResult);
   STDMETHOD(ApplyDirectorySync)(/*[in]*/ BSTR DomainName, /*[in]*/ VARIANT_BOOL DisableMissing, /*[out]*/ BSTR *ResultText, /*[out, retval]*/ VARIANT_BOOL *pResult);

private:

   // Shared by both, so the two cannot report the same run differently. The only thing
   // the callers decide is the mode and whether missing accounts are disabled.
   HRESULT RunDirectorySync_(BSTR DomainName, bool apply, bool disableMissing, BSTR *ResultText, VARIANT_BOOL *pResult);

public:

   bool LoadSettings();

private:
   

   
   // The refusal every getter of the configuration answers on a database the
   // server refused: the guard's sentence, with the reason.
   HRESULT NotLoaded_() const;

   HM::Configuration *config_;
   HM::IniFileSettings *ini_file_settings_;
   
};

OBJECT_ENTRY_AUTO(__uuidof(Settings), InterfaceSettings)
