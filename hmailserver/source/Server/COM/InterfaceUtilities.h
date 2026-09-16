// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "../hMailServer/resource.h"       // main symbols

#include "../hMailServer/hMailServer.h"


// InterfaceUtilities

class ATL_NO_VTABLE InterfaceUtilities : 
   public CComObjectRootEx<CComSingleThreadModel>,
   public CComCoClass<InterfaceUtilities, &CLSID_Utilities>,
   public IDispatchImpl<IInterfaceUtilities, &IID_IInterfaceUtilities, &LIBID_hMailServer, /*wMajor =*/ 1, /*wMinor =*/ 0>,
   public HM::COMAuthenticator,
   public ISupportErrorInfo

{
public:
   InterfaceUtilities()
   {
   }

DECLARE_REGISTRY_RESOURCEID(IDR_INTERFACEUTILITIES)


BEGIN_COM_MAP(InterfaceUtilities)
   COM_INTERFACE_ENTRY(IInterfaceUtilities)
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

public:
   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

   STDMETHOD(IsValidDomainName)(BSTR sDomainName, VARIANT_BOOL *bIsValid);
   STDMETHOD(GetMailServer)(/*[in]*/ BSTR EMailAddress, /*[out, retval]*/ BSTR *MailServer);
   STDMETHOD(MD5)(BSTR Input, BSTR *Output);
   STDMETHOD(SHA256)(BSTR Input, BSTR *Output);
   STDMETHOD(IsValidEmailAddress)(/*[in]*/ BSTR EMailAddress, /*[out, retval]*/ VARIANT_BOOL *bIsValid);
   STDMETHOD(BlowfishEncrypt)(BSTR Input, BSTR *Output);
   STDMETHOD(BlowfishDecrypt)(BSTR Input, BSTR *Output);
   STDMETHOD(GenerateGUID)(BSTR *Output);
   STDMETHOD(MakeDependent)(BSTR sOtherService);
   STDMETHOD(ImportMessageFromFile)(/*[in]*/ BSTR sFilename, long lAccountID, /*[out, retval]*/VARIANT_BOOL *bIsSuccessful);
   STDMETHOD(RetrieveMessageID)(/*[in]*/ BSTR sFilename, /*[out, retval]*/hyper *messageID);
   STDMETHOD(EmailAllAccounts)(BSTR sRecipientWildcard, BSTR sFromAddress, BSTR sFromName, BSTR sSubject, BSTR sBody, VARIANT_BOOL *bIsSuccessful);
   STDMETHOD(RunTestSuite)(BSTR sTestPassword);
   STDMETHOD(IsLocalHost)(BSTR sDomainName, VARIANT_BOOL *bIsValid);
   STDMETHOD(ImportMessageFromFileToIMAPFolder)(/*[in]*/ BSTR sFilename, long lAccountID, BSTR sIMAPFolder, /*[out, retval]*/VARIANT_BOOL *bIsSuccessful);
   STDMETHOD(IsStrongPassword)(BSTR Username, BSTR Password, VARIANT_BOOL *bIsValid);
   STDMETHOD(CriteriaMatch)(BSTR MatchValue, eRuleMatchType, BSTR TestValue,  /*[out, retval]*/ VARIANT_BOOL *bIsValid);
   STDMETHOD(IsValidIPAddress)(/*[in]*/ BSTR IPAddress, /*[out, retval]*/ VARIANT_BOOL *bIsValid);
   STDMETHOD(PerformMaintenance)(/*[in]*/ eMaintenanceOperation operation);
   STDMETHOD(CheckSieveSyntax)(/*[in]*/ BSTR Script, /*[out, retval]*/ BSTR *Result);
   STDMETHOD(EvaluateSieveScript)(/*[in]*/ BSTR Script, /*[in]*/ BSTR RawMessage, /*[out, retval]*/ BSTR *Result);
   STDMETHOD(ResolveMXRecords)(/*[in]*/ BSTR DomainName, /*[out, retval]*/ BSTR *Result);
   STDMETHOD(SendTlsRptReports)(/*[in]*/ VARIANT_BOOL IncludeCurrentDay, /*[out, retval]*/ long *ReportCount);
   STDMETHOD(SendDmarcReports)(/*[in]*/ VARIANT_BOOL IncludeCurrentDay, /*[out, retval]*/ long *ReportCount);
   STDMETHOD(EraseAddressTraces)(/*[in]*/ BSTR Address, /*[in]*/ VARIANT_BOOL IncludeArchive, /*[out, retval]*/ long *RemovedCount);
   STDMETHOD(RunMessageRetention)(/*[out, retval]*/ long *DeletedCount);
   STDMETHOD(SampleMetricsNow)(/*[out, retval]*/ long *RowsWritten);
   STDMETHOD(GetMetricHistory)(/*[in]*/ BSTR Metric, /*[in]*/ long MinutesBack, /*[in]*/ long BucketMinutes, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(SearchArchive)(/*[in]*/ BSTR Domain, /*[in]*/ BSTR Mailbox, /*[in]*/ BSTR Sender, /*[in]*/ BSTR Recipient, /*[in]*/ BSTR Subject, /*[in]*/ BSTR Since, /*[in]*/ BSTR Until, /*[in]*/ VARIANT_BOOL HoldOnly, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(SetArchiveHold)(/*[in]*/ long ArchiveID, /*[in]*/ VARIANT_BOOL Hold, /*[out, retval]*/ VARIANT_BOOL *Found);
   STDMETHOD(RunDkimRotation)(/*[in]*/ long SecondsAhead, /*[out, retval]*/ BSTR *Summary);
   STDMETHOD(BackupArchiveIsEncrypted)(/*[in]*/ BSTR File, /*[out, retval]*/ VARIANT_BOOL *Encrypted);
   // Spam traps: what has addressed one, and the clearing of that record.
   STDMETHOD(GetSpamTrapHits)(/*[in]*/ BSTR Domain, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(ClearSpamTrapHits)(/*[in]*/ BSTR Domain, /*[out, retval]*/ long *Removed);
   STDMETHOD(RunReputationCheck)(/*[out, retval]*/ BSTR *Report);
   STDMETHOD(GetReputationReport)(/*[out, retval]*/ BSTR *Report);
   STDMETHOD(GetSuppressionList)(/*[in]*/ BSTR DomainName, /*[in]*/ BSTR Search, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(AddSuppression)(/*[in]*/ BSTR DomainName, /*[in]*/ BSTR Address, /*[in]*/ BSTR Note, /*[in]*/ long ExpiresDays, /*[out, retval]*/ VARIANT_BOOL *Added);
   STDMETHOD(RemoveSuppression)(/*[in]*/ BSTR DomainName, /*[in]*/ BSTR Address, /*[out, retval]*/ VARIANT_BOOL *Removed);
   STDMETHOD(GetBounceCounts)(/*[in]*/ BSTR DomainName, /*[in]*/ BSTR Search, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(ClassifyBounce)(/*[in]*/ BSTR Status, /*[in]*/ BSTR Diagnostic, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(RunBounceMaintenance)(/*[out, retval]*/ long *Removed);
   STDMETHOD(MoveMailbox)(/*[in]*/ BSTR Address, /*[in]*/ BSTR NewAddress, /*[in]*/ VARIANT_BOOL KeepOldAddressAsAlias, /*[out, retval]*/ BSTR *Summary);
   STDMETHOD(GetMailboxMoves)(/*[in]*/ BSTR Address, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(GetComplaints)(/*[in]*/ BSTR DomainName, /*[in]*/ BSTR Search, /*[in]*/ long MaxRows, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(GetComplaintRates)(/*[out, retval]*/ BSTR *Json);
   STDMETHOD(LookupCountry)(/*[in]*/ BSTR IPAddress, /*[out, retval]*/ BSTR *Country);
   STDMETHOD(GeoIpDatabaseStatus)(/*[out, retval]*/ BSTR *Json);
   STDMETHOD(ImportOpenPGPKey)(/*[in]*/ BSTR AccountAddress, /*[in]*/ BSTR Key, /*[in]*/ VARIANT_BOOL AllowAddressMismatch, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(RemoveOpenPGPKey)(/*[in]*/ BSTR AccountAddress, /*[in]*/ BSTR Fingerprint, /*[out, retval]*/ VARIANT_BOOL *Removed);

   // The outbound webhook subscriptions, as the same JSON documents the REST
   // routes answer and take. See Common/Util/WebhookManager.h.
   STDMETHOD(GetWebhookSubscriptions)(/*[in]*/ BSTR Name, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(SaveWebhookSubscription)(/*[in]*/ BSTR Json, /*[out, retval]*/ BSTR *Json2);

   // The per-domain half of the session policies. The server-wide half is four
   // properties of Settings; SessionPolicies resolves the two into one answer.
   STDMETHOD(GetSessionPolicy)(/*[in]*/ BSTR DomainName, /*[out, retval]*/ BSTR *Json);
   STDMETHOD(SetDomainSessionPolicy)(/*[in]*/ BSTR DomainName, /*[in]*/ long IdleSeconds, /*[in]*/ long LifetimeSeconds,
                                     /*[in]*/ long OnePerDevice, /*[in]*/ long BindToAddress, /*[out, retval]*/ VARIANT_BOOL *Saved);

   // Web Push, for a caller standing in for a browser. See the comment on the
   // implementations: the regression suite cannot open an aes128gcm record for
   // itself, because .NET Framework has no AES-GCM.
   STDMETHOD(GenerateWebPushKeyPair)(/*[out, retval]*/ BSTR *Json);
   STDMETHOD(WebPushDecrypt)(/*[in]*/ BSTR BodyBase64Url, /*[in]*/ BSTR PrivateKey, /*[in]*/ BSTR AuthSecret, /*[out, retval]*/ BSTR *PlainText);

private:
   // The server administrator, or the domain administrator of this domain. Asks
   // the authorisation question, which is also what installs the audit actor.
   bool MayAdministerDomain_(__int64 domainID) const;
   bool MayAdministerAddress_(const HM::String &address) const;
public:
};

OBJECT_ENTRY_AUTO(__uuidof(Utilities), InterfaceUtilities)
