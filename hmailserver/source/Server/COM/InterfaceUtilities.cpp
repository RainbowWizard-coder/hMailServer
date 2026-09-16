// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceUtilities.h"
#include "../Common/Util/AddressTraceEraser.h"
#include "../Common/Util/Unicode.h"
#include "../SMTP/BounceTracker.h"
#include "../SMTP/ComplaintTracker.h"
#include "../Common/Application/MailboxRetentionTask.h"
#include "../Common/Application/MetricsHistoryTask.h"
#include "../Common/Persistence/PersistentArchiveIndex.h"
#include "../Common/TCPIP/DNSResolver.h"
#include "../Common/TCPIP/HostNameAndIpAddress.h"
#include "../Common/Util/ServiceManager.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Util/MailImporter.h"
#include "../Common/Util/EmailAllUsers.h"
#include "../Common/Util/GUIDCreator.h"
#include "../Common/Util/ClassTester.h"
#include "../Common/Util/CountryBlocking.h"
#include "../Common/TCPIP/IPAddress.h"
#include "../Common/Util/Utilities.h"
#include "../Common/Util/PasswordGenerator.h"
#include "../SMTP/RuleApplier.h"
#include "../SMTP/DeliveryQueue.h"
#include "../SMTP/TlsRptReporterTask.h"
#include "../SMTP/DmarcRptReporterTask.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/Maintenance/Maintenance.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Util/SessionPolicies.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Domains.h"
#include "../Common/Persistence/PersistentDomain.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Sieve/SieveScript.h"
#include "COMError.h"
#include "../Common/Util/WebPush.h"
#include "../Common/Util/Unicode.h"
#include "../Common/AntiSpam/DKIM/DkimRotation.h"
#include "../Common/Util/WebhookManager.h"
#include "../Common/Util/JsonDocument.h"
#include "../Common/Util/OpenPgpKeyStore.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Account.h"

#include <ctime>

STDMETHODIMP InterfaceUtilities::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] = 
      {
         &IID_IInterfaceUtilities,
      };
   
      for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
      {
         if (InlineIsEqualGUID(*arr[i],riid))
            return S_OK;
      }
      return S_FALSE;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::ResolveMXRecords(BSTR DomainName, BSTR *Result)
{
   try
   {
      if (!Result)
         return COMError::GenerateGenericMessage();

      *Result = nullptr;

      HM::String sDomainName(DomainName);
      sDomainName.Trim();

      if (sDomainName.IsEmpty())
         return COMError::GenerateError("No domain name was given.");

      // The server's own resolver, which is the point of the method - issue #29 was
      // the Control Panel's MX-query tool shelling out to nslookup, whose answer
      // comes from the OPERATING SYSTEM's resolver and disagrees with the server
      // whenever hMailServer.ini names a custom DNSServer. This is the same path
      // SMTP delivery resolves through, so what it reports is where mail will
      // actually go, from this server, today.
      std::vector<HM::HostNameAndIpAddress> hosts;

      HM::DNSResolver oDNSResolver;
      bool lookupSucceeded = oDNSResolver.GetEmailServers(sDomainName, hosts);

      // "The lookup failed" and "this domain has no mail servers" are different
      // answers and must not arrive as the same empty string. The resolver already
      // distinguishes them - false means the MX query itself failed, or every
      // address lookup for the exchange hosts did - and the server's own bounce
      // text keeps them apart too. Collapsing both into an empty result told an
      // administrator whose custom DNS server was unreachable that the RECIPIENT's
      // domain has no mail servers, which is the wrong diagnosis in exactly the
      // scenario this method was added for (issue #29).
      if (!lookupSucceeded && hosts.empty())
         return COMError::GenerateError("The DNS lookup failed, so where mail for this domain would go could not be determined. This is a failure of the lookup itself - the domain may well have mail servers. Check the server's DNS configuration (a custom DNSServer in hMailServer.ini, if set, must be reachable) and the DNS entries in the server log.");

      // Hostname and address per line, in the order delivery would try them:
      // GetEmailServers sorts by MX preference. It does NOT randomise among equal
      // preferences - RFC 5321 5.1 asks for that and this server has never done it,
      // so saying otherwise here would misdescribe both this tool and delivery.
      // A tab separates the two so a caller can split them apart without guessing
      // at the hostname's own characters.
      HM::String sResult;

      for (size_t i = 0; i < hosts.size(); i++)
      {
         if (i > 0)
            sResult += _T("\r\n");

         sResult += hosts[i].GetHostName();
         sResult += _T("\t");
         sResult += hosts[i].GetIpAddress();
      }

      *Result = sResult.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SendTlsRptReports(VARIANT_BOOL IncludeCurrentDay, long *ReportCount)
{
   try
   {
      if (!ReportCount)
         return COMError::GenerateGenericMessage();

      *ReportCount = 0;

      // Sends mail to third parties, so it is an administrator's call to make.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Refused rather than run: SendReportsNow POPS each day's statistics, and
      // with no sender address it can only discard what it popped. A diagnostic
      // that destroys the data it was asked to show - silently, returning 0 as
      // if there were simply nothing to send - would be worse than no
      // diagnostic. The hourly task keeps its discard behaviour, which is what
      // bounds the store's memory on an unconfigured server; this entry point
      // exists for an administrator who believes reporting is set up, so tell
      // them when it is not.
      if (HM::IniFileSettings::Instance()->GetTlsRptFromAddress().IsEmpty())
         return COMError::GenerateError("TlsRptFromAddress is not set in the [Settings] section of hMailServer.ini, so no report can be sent. The collected statistics have been preserved.");

      *ReportCount = HM::TlsRptReporterTask::SendReportsNow(IncludeCurrentDay != VARIANT_FALSE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SendDmarcReports(VARIANT_BOOL IncludeCurrentDay, long *ReportCount)
{
   try
   {
      if (!ReportCount)
         return COMError::GenerateGenericMessage();

      *ReportCount = 0;

      // Sends mail to third parties, so it is an administrator's call to make.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Refused for the same reason SendTlsRptReports refuses: the send pass
      // POPS each day's statistics, and with no sender address it can only
      // discard what it popped. A diagnostic that destroys the data it was
      // asked to show would be worse than none.
      if (HM::IniFileSettings::Instance()->GetDmarcRptFromAddress().IsEmpty())
         return COMError::GenerateError("DmarcRptFromAddress is not set in the [Settings] section of hMailServer.ini, so no report can be sent. The collected statistics have been preserved.");

      *ReportCount = HM::DmarcRptReporterTask::SendReportsNow(IncludeCurrentDay != VARIANT_FALSE);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RunMessageRetention(long *DeletedCount)
{
   try
   {
      if (!DeletedCount)
         return COMError::GenerateGenericMessage();

      *DeletedCount = 0;

      // Deletes mail across every domain with a policy, so it is the server
      // administrator's call, the same as setting the policy.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      *DeletedCount = HM::MailboxRetentionTask::Sweep();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SampleMetricsNow(long *RowsWritten)
{
   try
   {
      if (!RowsWritten)
         return COMError::GenerateGenericMessage();

      *RowsWritten = 0;

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      *RowsWritten = HM::MetricsHistoryTask::SampleNow();

      if (*RowsWritten > 0)
         HM::MetricsHistoryTask::Prune();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GetMetricHistory(BSTR Metric, long MinutesBack, long BucketMinutes, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String metric = Metric;
      HM::String json = HM::MetricsHistoryTask::QueryAsJson(metric, MinutesBack, BucketMinutes);

      *Json = json.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::EraseAddressTraces(BSTR Address, VARIANT_BOOL IncludeArchive, long *RemovedCount)
{
   try
   {
      if (!RemovedCount)
         return COMError::GenerateGenericMessage();

      *RemovedCount = 0;

      // Destroys records across half a dozen stores, so it is the server
      // administrator's call - a domain administrator's authority ends at
      // their domain, and greylisting triplets and quarantine rows do not
      // belong to one.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String address = Address;
      address.Trim();

      if (address.IsEmpty() || address.Find(_T("@")) <= 0)
         return COMError::GenerateError("A full email address must be given.");

      int removed = 0;
      if (!HM::AddressTraceEraser::Erase(address, IncludeArchive != VARIANT_FALSE, removed))
      {
         *RemovedCount = removed;
         return COMError::GenerateError("Some traces could not be removed - the hMailServer error log names the stores to retry. What could be removed has been.");
      }

      *RemovedCount = removed;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GetMailServer(BSTR EMailAddress, BSTR *MailServer)
{
   try
   {
      HM::String sDomainName;
      HM::String sEMail(EMailAddress);
   
      sDomainName = HM::StringParser::ExtractDomain (EMailAddress);
   
      std::vector<HM::String> saDomainNames;

      std::vector<HM::HostNameAndIpAddress> hostname_and_ipaddresses;
   
      HM::DNSResolver oDNSResolver;
      oDNSResolver.GetEmailServers(sDomainName, hostname_and_ipaddresses);
   
      HM::String sMailServer = "";
      for (unsigned int i = 0; i < hostname_and_ipaddresses.size(); i++)
      {
         if (!sMailServer.IsEmpty())
            sMailServer += ",";
   
         sMailServer += hostname_and_ipaddresses[i].GetIpAddress();
      }
   
      *MailServer = sMailServer.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::MD5(BSTR Input, BSTR *Output)
{
   try
   {
      HM::String sInput(Input);
   
      HM::String sOutput = HM::Crypt::Instance()->EnCrypt(sInput, HM::Crypt::ETMD5);
   
      *Output = sOutput.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SHA256(BSTR Input, BSTR *Output)
{
   try
   {
      HM::String sInput(Input);
   
      HM::String sOutput = HM::Crypt::Instance()->EnCrypt(sInput, HM::Crypt::ETSHA256);
   
      *Output = sOutput.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::CheckSieveSyntax(BSTR Script, BSTR *Result)
{
   try
   {
      HM::String sScript(Script);

      HM::String sError = HM::SieveScript::CheckSyntax(sScript);

      *Result = sError.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::EvaluateSieveScript(BSTR Script, BSTR RawMessage, BSTR *Result)
{
   try
   {
      HM::String sScript(Script);
      HM::String sRawMessage(RawMessage);

      HM::String sResult = HM::SieveScript::Evaluate(sScript, sRawMessage);

      *Result = sResult.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::BlowfishEncrypt(BSTR Input, BSTR *Output)
{
   try
   {
      HM::String sInput(Input);  
   
      HM::String sOutput = HM::Crypt::Instance()->EnCrypt(sInput, HM::Crypt::ETBlowFish);
   
      *Output = sOutput.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::BlowfishDecrypt(BSTR Input, BSTR *Output)
{
   try
   {
      HM::String sInput(Input);
   
      HM::String sOutput = HM::Crypt::Instance()->DeCrypt(sInput, HM::Crypt::ETBlowFish);
   
      *Output = sOutput.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GenerateGUID(BSTR *Output)
{
   try
   {
      HM::String sOutput = HM::GUIDCreator::GetGUID();
      *Output = sOutput.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::IsValidEmailAddress(BSTR EMailAddress, VARIANT_BOOL *bIsValid)
{
   try
   {
      bool bValid = HM::StringParser::IsValidEmailAddress(EMailAddress);
      if (bValid)
         *bIsValid = VARIANT_TRUE;
      else
         *bIsValid = VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::IsValidIPAddress(BSTR IPAddress, VARIANT_BOOL *bIsValid)
{
   try
   {
      HM::Utilities utilities;
      
      *bIsValid = utilities.IsValidIPAddress(IPAddress) ? VARIANT_TRUE : VARIANT_FALSE;
      
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::IsStrongPassword(BSTR Username, BSTR Password, VARIANT_BOOL *bIsValid)
{
   try
   {
      bool bStrong = HM::PasswordGenerator::IsStrongPassword(Username, Password);
   
      if (bStrong)
         *bIsValid = VARIANT_TRUE;
      else
         *bIsValid = VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::IsValidDomainName(BSTR sDomainName, VARIANT_BOOL *bIsValid)
{
   try
   {
      bool bValid = HM::StringParser::IsValidDomainName(sDomainName);
      if (bValid)
         *bIsValid = VARIANT_TRUE;
      else
         *bIsValid = VARIANT_FALSE;
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::IsLocalHost(BSTR sHostname, VARIANT_BOOL *bIsValid)
{
   try
   {
      *bIsValid = HM::Utilities::IsLocalHost(sHostname) ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::CriteriaMatch(BSTR MatchValue, eRuleMatchType matchType, BSTR TestValue, VARIANT_BOOL *bMatch)
{
   try
   {
      HM::RuleApplier ruleApplier;
   
      *bMatch = ruleApplier.TestMatch(MatchValue, (HM::RuleCriteria::MatchType) matchType, TestValue) ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::MakeDependent(BSTR sOtherService)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::ServiceManager oManager; 
      oManager.MakeDependentOn(sOtherService);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::ImportMessageFromFile(BSTR sFilename, long iAccountID, VARIANT_BOOL *bIsSuccessful)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      *bIsSuccessful = HM::MailImporter::Import(sFilename, iAccountID, "") ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RetrieveMessageID(BSTR sFilename, hyper *messageID)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      __int64 id = 0;
      bool partialFileName;
      bool result = HM::PersistentMessage::GetMessageID(sFilename, id, partialFileName);
   
      *messageID = id;
      
      if (result == false)
         return COMError::GenerateError("Retrieval of message-ID failed. Please check hMailServer error log.");
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::ImportMessageFromFileToIMAPFolder(BSTR sFilename, long iAccountID, BSTR sIMAPFolder, VARIANT_BOOL *bIsSuccessful)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      *bIsSuccessful = HM::MailImporter::Import(sFilename, iAccountID, sIMAPFolder) ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP
InterfaceUtilities::EmailAllAccounts(BSTR sRecipientWildcard, BSTR sFromAddress, BSTR sFromName, BSTR sSubject, BSTR sBody, VARIANT_BOOL *bIsSuccessful)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::EmailAllUsers oEmailAllUsers;
      *bIsSuccessful = oEmailAllUsers.Start(sRecipientWildcard, sFromAddress, sFromName, sSubject, sBody) ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

/*
   A subscription key pair, and the reverse of the push encryption. Neither
   belongs to the running server: they exist so that a caller standing in for a
   browser - the regression suite's fake push service - can open a push message
   and assert what it carried. .NET Framework has no AES-GCM, so there is no way
   for that caller to do it for itself, and a test that asserts only that a push
   was sent asserts nothing about what left the building.
*/
STDMETHODIMP InterfaceUtilities::GenerateWebPushKeyPair(BSTR *Json)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::AnsiString publicKey;
      HM::AnsiString privateKey;
      HM::String error;

      if (!HM::WebPush::GenerateKeyPair(publicKey, privateKey, error))
         return COMError::GenerateError(error);

      HM::AnsiString json = "{\"public_key\":\"" + publicKey + "\",\"private_key\":\"" + privateKey + "\"}";

      *Json = HM::String(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::WebPushDecrypt(BSTR BodyBase64Url, BSTR PrivateKey, BSTR AuthSecret, BSTR *PlainText)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      std::vector<unsigned char> body;
      if (!HM::WebPush::Base64UrlDecode(HM::AnsiString(HM::String(BodyBase64Url)), body) || body.empty())
         return COMError::GenerateError("The body is not base64url, or is empty.");

      HM::AnsiString bytes;
      bytes.append((const char*) body.data(), body.size());

      HM::AnsiString plainText;
      HM::String error;

      if (!HM::WebPush::Decrypt(bytes, HM::AnsiString(HM::String(PrivateKey)), HM::AnsiString(HM::String(AuthSecret)), plainText, error))
         return COMError::GenerateError(error);

      // The payload is UTF-8; a BSTR is UTF-16, so it is widened rather than
      // copied byte for byte, or a subject outside ASCII would come back mangled.
      HM::String wide;
      HM::Unicode::MultiByteToWide(plainText, wide);

      *PlainText = wide.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RunTestSuite(BSTR sTestPassword)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::String sCorrectPassword = _T("I know what I am doing.");
      HM::String sPassword = sTestPassword;
   
      if (sPassword.Compare(sCorrectPassword) != 0)
         return S_FALSE;
   
      HM::ClassTester oTester;
      oTester.DoTests();
   
      return S_OK;
   }
   catch (const std::exception &failure)
   {
      // The testers say what failed by throwing std::logic_error with a sentence,
      // and a generic "an error occurred" threw that sentence away - so the
      // regression test that runs this could only report that some tester, of
      // thirty, had failed.
      return COMError::GenerateError(HM::String(HM::AnsiString("The internal test suite failed: ") + failure.what()));
   }
   catch (...)
   {
      return COMError::GenerateError(_T("The internal test suite failed in a tester that threw without saying why. Run it under a debugger with OutputDebugString captured: the last \"hMailServer: Testing\" line names it."));
   }
}

STDMETHODIMP InterfaceUtilities::PerformMaintenance(eMaintenanceOperation operation)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      HM::Maintenance maintenance;
   
      HM::Maintenance::MaintenanceOperation internalOperation;
   
      switch (operation)
      {
      case eUpdateIMAPFolderUID:
         internalOperation = HM::Maintenance::RecalculateFolderUID;
         break;
      default:
         return COMError::GenerateError("Unknown maintenance operation.");
      }
   
      bool result = maintenance.Perform(internalOperation);
      if (result == false)
         return COMError::GenerateError("The maintenance operation failed. Please see hMailServer log for details.");
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SearchArchive(BSTR Domain, BSTR Mailbox, BSTR Sender, BSTR Recipient, BSTR Subject, BSTR Since, BSTR Until, VARIANT_BOOL HoldOnly, long MaxRows, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::PersistentArchiveIndex::Criteria criteria;
      criteria.domain = Domain;
      criteria.mailbox = Mailbox;
      criteria.sender = Sender;
      criteria.recipient = Recipient;
      criteria.subject = Subject;
      criteria.since = Since;
      criteria.until = Until;
      criteria.holdOnly = HoldOnly == VARIANT_TRUE;
      criteria.maxRows = MaxRows;

      std::vector<HM::PersistentArchiveIndex::Entry> entries;
      HM::PersistentArchiveIndex::Search(criteria, entries);

      HM::String json = HM::PersistentArchiveIndex::ToJson(entries);
      *Json = json.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SetArchiveHold(long ArchiveID, VARIANT_BOOL Hold, VARIANT_BOOL *Found)
{
   try
   {
      if (!Found)
         return COMError::GenerateGenericMessage();

      *Found = VARIANT_FALSE;

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::PersistentArchiveIndex::Entry entry;
      if (!HM::PersistentArchiveIndex::Get(ArchiveID, entry))
         return S_OK;

      if (HM::PersistentArchiveIndex::SetHold(ArchiveID, Hold == VARIANT_TRUE))
         *Found = VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RunDkimRotation(long SecondsAhead, BSTR *Summary)
{
   try
   {
      if (!Summary)
         return COMError::GenerateGenericMessage();

      // Every domain's rotation, and a clock an administrator can move: the server
      // administrator's, like the retention sweep.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Bounded and never negative. A clock moved backwards would make a first
      // sighting later than a check that followed it; ten years forward is further
      // than any period the policy can hold.
      if (SecondsAhead < 0 || (__int64) SecondsAhead > (__int64) 3660 * 86400)
         return COMError::GenerateError("SecondsAhead must be between 0 and 316224000 (3660 days).");

      if (SecondsAhead > 0 && (HM::Logger::Instance()->GetLogMask() & HM::Logger::LSApplication))
         HM::Logger::Instance()->LogApplication(HM::Formatter::Format("DKIM rotation: a pass was run with the clock moved {0} second(s) ahead, on request.", (int) SecondsAhead));

      HM::DkimRotation::RunSummary summary = HM::DkimRotation::RunAll((__int64) time(nullptr) + (__int64) SecondsAhead);

      HM::String json = HM::String(summary.ToJson());
      *Summary = json.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

namespace
{
   HM::String WideFromUtf8(const HM::AnsiString &utf8)
   {
      HM::String wide;
      HM::Unicode::MultiByteToWide(utf8, wide);
      return wide;
   }
}

bool
InterfaceUtilities::MayAdministerDomain_(__int64 domainID) const
{
   if (authentication_->GetIsServerAdmin())
      return true;

   return authentication_->GetIsDomainAdmin() && authentication_->GetDomainID() == domainID;
}

// Whether the caller may administer the domain an address names, whether or not
// the address is an account. This is what a refusal for a missing account has
// to be behind: if the question were only "is a domain administrator", a domain
// administrator of one domain could ask about an address in another and read
// from the two different answers - "no account of that address" against
// access-denied - whether that address exists.
bool
InterfaceUtilities::MayAdministerAddress_(const HM::String &address) const
{
   if (authentication_->GetIsServerAdmin())
      return true;

   if (!authentication_->GetIsDomainAdmin())
      return false;

   HM::String domainName = HM::StringParser::ExtractDomain(address);
   if (domainName.IsEmpty())
      return false;

   std::shared_ptr<const HM::Domain> domain = HM::CacheContainer::Instance()->GetDomain(domainName);
   if (!domain)
      return false;

   return MayAdministerDomain_(domain->GetID());
}

STDMETHODIMP InterfaceUtilities::GetSuppressionList(BSTR DomainName, BSTR Search, long MaxRows, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      const __int64 domainID = HM::BounceTracker::DomainIDByName(DomainName);
      if (domainID <= 0)
      {
         // Only a server administrator is told that a domain does not exist. A
         // domain administrator gets the same access-denied for a name that is
         // not theirs whether it is hosted here or not, so the answer says
         // nothing about which domains this server has.
         if (!authentication_->GetIsServerAdmin())
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No domain of that name exists.");
      }

      if (!MayAdministerDomain_(domainID))
         return authentication_->GetAccessDenied();

      HM::AnsiString json;
      if (!HM::BounceTracker::SuppressionsJson(domainID, Search, MaxRows <= 0 ? 100 : MaxRows, 0, json))
         return COMError::GenerateError("The suppression list could not be read from the database.");

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::AddSuppression(BSTR DomainName, BSTR Address, BSTR Note, long ExpiresDays, VARIANT_BOOL *Added)
{
   try
   {
      if (!Added)
         return COMError::GenerateGenericMessage();

      *Added = VARIANT_FALSE;

      const __int64 domainID = HM::BounceTracker::DomainIDByName(DomainName);
      if (domainID <= 0)
      {
         // Only a server administrator is told that a domain does not exist. A
         // domain administrator gets the same access-denied for a name that is
         // not theirs whether it is hosted here or not, so the answer says
         // nothing about which domains this server has.
         if (!authentication_->GetIsServerAdmin())
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No domain of that name exists.");
      }

      if (!MayAdministerDomain_(domainID))
         return authentication_->GetAccessDenied();

      HM::AuditScope auditScope(authentication_->GetAuditActor());

      switch (HM::BounceTracker::AddManual(domainID, Address, Note, ExpiresDays))
      {
      case HM::BounceTracker::AddAdded:
         *Added = VARIANT_TRUE;
         return S_OK;
      case HM::BounceTracker::AddAlreadyListed:
         return S_OK;
      case HM::BounceTracker::AddInvalidAddress:
         return COMError::GenerateError("That is not a valid email address.");
      case HM::BounceTracker::AddHostedAddress:
         return COMError::GenerateError("That address is hosted on this server. The suppression list only ever applies to addresses outside it.");
      case HM::BounceTracker::AddInvalidExpiry:
         return COMError::GenerateError("ExpiresDays must be between 0 and 3650.");
      default:
         return COMError::GenerateError("The entry could not be written to the database.");
      }
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RemoveSuppression(BSTR DomainName, BSTR Address, VARIANT_BOOL *Removed)
{
   try
   {
      if (!Removed)
         return COMError::GenerateGenericMessage();

      *Removed = VARIANT_FALSE;

      const __int64 domainID = HM::BounceTracker::DomainIDByName(DomainName);
      if (domainID <= 0)
      {
         // Only a server administrator is told that a domain does not exist. A
         // domain administrator gets the same access-denied for a name that is
         // not theirs whether it is hosted here or not, so the answer says
         // nothing about which domains this server has.
         if (!authentication_->GetIsServerAdmin())
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No domain of that name exists.");
      }

      if (!MayAdministerDomain_(domainID))
         return authentication_->GetAccessDenied();

      HM::AuditScope auditScope(authentication_->GetAuditActor());

      const int result = HM::BounceTracker::Remove(domainID, Address);

      if (result < 0)
         return COMError::GenerateError("The entry could not be removed from the database.");

      *Removed = result > 0 ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GetBounceCounts(BSTR DomainName, BSTR Search, long MaxRows, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      const __int64 domainID = HM::BounceTracker::DomainIDByName(DomainName);
      if (domainID <= 0)
      {
         // Only a server administrator is told that a domain does not exist. A
         // domain administrator gets the same access-denied for a name that is
         // not theirs whether it is hosted here or not, so the answer says
         // nothing about which domains this server has.
         if (!authentication_->GetIsServerAdmin())
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No domain of that name exists.");
      }

      if (!MayAdministerDomain_(domainID))
         return authentication_->GetAccessDenied();

      HM::AnsiString json;
      if (!HM::BounceTracker::CountsJson(domainID, Search, MaxRows <= 0 ? 100 : MaxRows, 0, json))
         return COMError::GenerateError("The bounce counts could not be read from the database.");

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::ClassifyBounce(BSTR Status, BSTR Diagnostic, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String status = Status;
      status.Trim();

      *Json = WideFromUtf8(HM::BounceTracker::ClassifyJson(status, Diagnostic)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// An account's OpenPGP public keys, for an administrator: the upload and the
// removal. The account's own switch and its key list are properties of
// IInterfaceAccount; these two are here because they name an account by address
// and because a key upload is a piece of work rather than a property.
//
// Only public keys, ever. OpenPgpKeyStore::Store parses the bytes and refuses a
// secret key with a sentence saying so; there is no path through this interface,
// or any other, that stores one.

STDMETHODIMP InterfaceUtilities::ImportOpenPGPKey(BSTR AccountAddress, BSTR Key, VARIANT_BOOL AllowAddressMismatch, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      HM::String address = AccountAddress;
      address.Trim();
      address.ToLower();

      std::shared_ptr<const HM::Account> account = HM::CacheContainer::Instance()->GetAccount(address);
      if (!account)
      {
         // The refusal is behind the authorisation question, so that a caller who
         // may administer nothing, or only another domain, cannot use this to find
         // out which addresses exist: a domain administrator is told about a
         // missing address only in a domain they administer, and gets the same
         // access-denied for any other, whether the address is there or not.
         if (!MayAdministerAddress_(address))
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No account of that address exists.");
      }

      if (!MayAdministerDomain_(account->GetDomainID()))
         return authentication_->GetAccessDenied();

      // Waiving the rule that the key names the account's own address is the
      // server administrator's. A domain administrator publishing a key that does
      // not name the person it is published for is the mistake the rule catches.
      const bool allowMismatch = AllowAddressMismatch == VARIANT_TRUE;
      if (allowMismatch && !authentication_->GetIsServerAdmin())
         return COMError::GenerateError("Only a server administrator may store a key whose user ids do not carry the account's own address.");

      HM::AuditScope auditScope(authentication_->GetAuditActor());

      HM::OpenPgpKeyStore::Row row;
      bool replaced = false;
      HM::AnsiString problem;
      if (!HM::OpenPgpKeyStore::Store(account, HM::AnsiString(HM::String(Key)), allowMismatch,
                                      authentication_->GetAuditActor().name, row, replaced, problem))
         return COMError::GenerateError(HM::String(problem));

      *Json = HM::String(HM::OpenPgpKeyStore::RowJson(row, account->GetAddress(), false)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::RemoveOpenPGPKey(BSTR AccountAddress, BSTR Fingerprint, VARIANT_BOOL *Removed)
{
   try
   {
      if (!Removed)
         return COMError::GenerateGenericMessage();

      *Removed = VARIANT_FALSE;

      HM::String address = AccountAddress;
      address.Trim();
      address.ToLower();

      std::shared_ptr<const HM::Account> account = HM::CacheContainer::Instance()->GetAccount(address);
      if (!account)
      {
         // As in ImportOpenPGPKey: the same refusal whether the address exists.
         if (!MayAdministerAddress_(address))
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No account of that address exists.");
      }

      if (!MayAdministerDomain_(account->GetDomainID()))
         return authentication_->GetAccessDenied();

      HM::AuditScope auditScope(authentication_->GetAuditActor());

      HM::String fingerprint = Fingerprint;
      fingerprint.Trim();

      if (HM::OpenPgpKeyStore::Remove(account->GetID(), HM::AnsiString(fingerprint)))
         *Removed = VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


namespace
{
   // UTF-8, not the machine's code page. CStdStr's own conversion is CP_ACP,
   // which loses every character the page does not have - a domain in Greek, an
   // address with an accent - and the document this builds is handed back as
   // UTF-8 by WideFromUtf8 at the end.
   HM::AnsiString WebhookUtf8_(const HM::String &value)
   {
      HM::AnsiString utf8;
      HM::Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   // The way back: UTF-8 bytes to characters, for the same reason WebhookUtf8_
   // exists.
   HM::String WebhookFromUtf8_(const std::string &utf8)
   {
      HM::String value;
      HM::Unicode::MultiByteToWide(HM::AnsiString(utf8.c_str()), value);
      return value;
   }

   HM::AnsiString WebhookJsonEscape_(const HM::AnsiString &value)
   {
      HM::AnsiString result;
      result.reserve(value.GetLength() + 8);

      for (int i = 0; i < value.GetLength(); i++)
      {
         const char character = value[i];

         switch (character)
         {
         case '"':  result += "\\\""; break;
         case '\\': result += "\\\\"; break;
         case '\r': result += "\\r"; break;
         case '\n': result += "\\n"; break;
         case '\t': result += "\\t"; break;
         default:
            if ((unsigned char) character >= 0x20)
               result += character;
            break;
         }
      }

      return result;
   }
}

namespace
{
   // One subscription as the REST listing writes it. The shared secret is not in
   // it: secret_set says whether there is one, which is what an administrator
   // needs to know and what a reader must not be able to take away.
   HM::AnsiString WebhookSubscriptionJson_(const HM::WebhookManager::Subscription &subscription)
   {
      int pending = 0;
      int delivered = 0;
      int deadLettered = 0;
      HM::WebhookManager::Instance()->CountDeliveries(subscription.id, pending, delivered, deadLettered);

      HM::AnsiString json;
      json += "{";
      json += "\"name\":\"" + WebhookJsonEscape_(WebhookUtf8_(subscription.name)) + "\",";
      json += "\"url\":\"" + WebhookJsonEscape_(WebhookUtf8_(subscription.url)) + "\",";
      json += "\"secret_set\":" + HM::AnsiString(subscription.secret.IsEmpty() ? "false" : "true") + ",";
      json += "\"events\":\"" + WebhookJsonEscape_(WebhookUtf8_(subscription.events)) + "\",";
      json += "\"domains\":\"" + WebhookJsonEscape_(WebhookUtf8_(subscription.domains)) + "\",";
      json += "\"addresses\":\"" + WebhookJsonEscape_(WebhookUtf8_(subscription.addresses)) + "\",";
      json += "\"enabled\":" + HM::AnsiString(subscription.enabled ? "true" : "false") + ",";
      json += "\"include_subject\":" + HM::AnsiString(subscription.include_subject ? "true" : "false") + ",";

      HM::AnsiString numbers;
      numbers.Format("\"max_per_minute\":%d,\"created\":%I64d,\"pending\":%d,\"delivered\":%d,\"dead_lettered\":%d,\"dropped\":%d",
         subscription.max_per_minute, subscription.created, pending, delivered, deadLettered,
         HM::WebhookManager::Instance()->DroppedSinceStart(subscription.name));
      json += numbers;

      json += "}";

      return json;
   }
}

STDMETHODIMP InterfaceUtilities::RunBounceMaintenance(long *Removed)
{
   try
   {
      if (!Removed)
         return COMError::GenerateGenericMessage();

      *Removed = 0;

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      int suppressionsExpired = 0;
      int countsPruned = 0;
      int reportKeysPruned = 0;

      const bool succeeded = HM::BounceTracker::RunMaintenance(suppressionsExpired, countsPruned, reportKeysPruned);

      *Removed = suppressionsExpired + countsPruned + reportKeysPruned;

      if (!succeeded)
         return COMError::GenerateError("The maintenance sweep failed part way; what it removed before failing has been removed.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

/*
   The webhook subscriptions over COM, as JSON rather than as a collection, for the
   reason the suppression list is: a collection would be two new interfaces and two
   new coclasses, and registering those needs an elevated step this feature does not
   otherwise need. The documents are exactly the ones GET /api/v1/webhooks and
   POST /api/v1/webhooks answer and take, so a client written against either surface
   reads the same shapes - and the shared secret is absent from both.
*/
STDMETHODIMP InterfaceUtilities::GetWebhookSubscriptions(BSTR Name, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String wanted = Name;
      wanted.Trim();
      wanted.MakeLower();

      std::vector<HM::WebhookManager::Subscription> subscriptions;

      if (!HM::WebhookManager::Instance()->GetSubscriptions(subscriptions))
         return COMError::GenerateError("The webhook subscriptions could not be read.");

      HM::AnsiString json = "{\"webhooks\":[";
      bool first = true;

      for (const HM::WebhookManager::Subscription &subscription : subscriptions)
      {
         if (!wanted.IsEmpty() && subscription.name != wanted)
            continue;

         if (!first)
            json += ",";

         first = false;
         json += WebhookSubscriptionJson_(subscription);
      }

      json += "],\"events\":[";

      const std::vector<HM::String> known = HM::WebhookManager::KnownEvents();

      for (size_t i = 0; i < known.size(); i++)
      {
         if (i > 0)
            json += ",";

         json += "\"" + WebhookUtf8_(known[i]) + "\"";
      }

      json += "]}";

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::SaveWebhookSubscription(BSTR Json, BSTR *Json2)
{
   try
   {
      if (!Json2)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::AuditScope auditScope(authentication_->GetAuditActor());

      // The BSTR is characters; the parser wants UTF-8 bytes.
      const HM::AnsiString requestBody = WebhookUtf8_(HM::String(Json));

      HM::JsonValue document;
      std::string parseError;
      if (!HM::JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), document, parseError) ||
          !document.IsObject())
         return COMError::GenerateError("The body must be a JSON object.");

      const HM::JsonValue *member = document.Get("name");
      if (member == nullptr || !member->IsString())
         return COMError::GenerateError("Name the subscription.");

      HM::String name = WebhookFromUtf8_(member->AsString());
      name.Trim();
      name.MakeLower();

      HM::WebhookManager::Subscription existing;
      const bool exists = HM::WebhookManager::Instance()->GetSubscription(name, existing);

      if (document.GetBool("remove"))
      {
         if (!exists || !HM::WebhookManager::Instance()->DeleteSubscription(name))
            return COMError::GenerateError("There is no webhook subscription of that name.");

         *Json2 = WideFromUtf8(HM::AnsiString("{\"removed\":true}")).AllocSysString();
         return S_OK;
      }

      // A field left out of a change keeps the value it has, which is what stops a
      // caller that wanted to switch one subscription off from clearing its secret
      // by not sending it.
      HM::WebhookManager::Subscription subscription = exists ? existing : HM::WebhookManager::Subscription();
      subscription.name = name;

      if ((member = document.Get("url")) != nullptr && member->IsString())
         subscription.url = WebhookFromUtf8_(member->AsString());

      if ((member = document.Get("secret")) != nullptr && member->IsString())
      {
         const HM::String given = WebhookFromUtf8_(member->AsString());

         if (!given.IsEmpty())
            subscription.secret = given;
      }

      if ((member = document.Get("events")) != nullptr && member->IsString())
         subscription.events = WebhookFromUtf8_(member->AsString());

      if ((member = document.Get("domains")) != nullptr && member->IsString())
         subscription.domains = WebhookFromUtf8_(member->AsString());

      if ((member = document.Get("addresses")) != nullptr && member->IsString())
         subscription.addresses = WebhookFromUtf8_(member->AsString());

      if ((member = document.Get("enabled")) != nullptr && member->IsBool())
         subscription.enabled = member->AsBool();

      if ((member = document.Get("include_subject")) != nullptr && member->IsBool())
         subscription.include_subject = member->AsBool();

      if ((member = document.Get("max_per_minute")) != nullptr && member->IsNumber())
         subscription.max_per_minute = (int) member->AsInt64();

      HM::String refusal;
      if (!HM::WebhookManager::Instance()->SaveSubscription(subscription, !exists, refusal))
         return COMError::GenerateError(refusal);

      HM::WebhookManager::Subscription saved;
      if (!HM::WebhookManager::Instance()->GetSubscription(name, saved))
         return COMError::GenerateError("The subscription was saved but could not be read back.");

      *Json2 = WideFromUtf8(WebhookSubscriptionJson_(saved)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::LookupCountry(BSTR IPAddress, BSTR *Country)
{
   try
   {
      if (!Country)
         return COMError::GenerateGenericMessage();

      *Country = nullptr;

      // A domain administrator may ask: it answers a fact about a public
      // address and nothing about this server's own configuration.
      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::String text = IPAddress;
      text.Trim();

      HM::IPAddress address;

      if (!address.TryParse(HM::AnsiString(text), false))
         return COMError::GenerateError(_T("That is not an IP address. Give an IPv4 or IPv6 address, such as 8.8.8.8 or 2001:db8::1."));

      const HM::AnsiString iso = HM::CountryBlocking::Instance()->CountryOf(address);

      *Country = HM::String(iso).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GeoIpDatabaseStatus(BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      *Json = WideFromUtf8(HM::CountryBlocking::Instance()->StatusJson()).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The session policy in force for a domain, and where each part of it came from.
// Built by SessionPolicies, which is also what the sessions themselves are judged
// by, so this cannot report a policy the server is not applying.
STDMETHODIMP InterfaceUtilities::GetSessionPolicy(BSTR DomainName, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      HM::String name = DomainName;
      name.Trim();

      // A named domain is that domain administrator's to read; the server-wide
      // policy alone is the server administrator's. Asking is also what installs
      // the audit actor on this thread.
      if (name.IsEmpty())
      {
         if (!authentication_->GetIsServerAdmin())
            return authentication_->GetAccessDenied();
      }
      else
      {
         std::shared_ptr<const HM::Domain> domain = HM::CacheContainer::Instance()->GetDomain(name);
         if (!domain)
            return COMError::GenerateError("No such domain.");

         if (!MayAdministerDomain_(domain->GetID()))
            return authentication_->GetAccessDenied();
      }

      const HM::SessionPolicy policy = name.IsEmpty()
         ? HM::SessionPolicies::ServerWide()
         : HM::SessionPolicies::ForDomain(name);

      HM::String json;
      HM::Unicode::MultiByteToWide(HM::SessionPolicies::ToJson(policy), json);

      *Json = json.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// A domain's own session policy, saved. The numbers and the two three-state
// switches are checked in SessionPolicies' own words, so COM, REST and the
// command line refuse the same values for the same reason.
STDMETHODIMP InterfaceUtilities::SetDomainSessionPolicy(BSTR DomainName, long IdleSeconds, long LifetimeSeconds,
                                                        long OnePerDevice, long BindToAddress, VARIANT_BOOL *Saved)
{
   try
   {
      if (!Saved)
         return COMError::GenerateGenericMessage();

      *Saved = VARIANT_FALSE;

      HM::String name = DomainName;
      name.Trim();

      if (name.IsEmpty())
         return COMError::GenerateError("A domain name is required. The server-wide policy is on Settings.");

      HM::Domains domains;
      domains.Refresh();

      std::shared_ptr<HM::Domain> domain = domains.GetItemByName(name);
      if (!domain)
         return S_OK;

      if (!MayAdministerDomain_(domain->GetID()))
         return authentication_->GetAccessDenied();

      // Who is making this change, on the thread that is about to make it - the
      // domain row it writes is an audited table, and the trail records whoever the
      // scope names.
      HM::AuditScope auditScope(authentication_->GetAuditActor());

      HM::String refusal = HM::SessionPolicies::CheckSeconds((int) IdleSeconds);
      if (refusal.IsEmpty())
         refusal = HM::SessionPolicies::CheckSeconds((int) LifetimeSeconds);
      if (refusal.IsEmpty())
         refusal = HM::SessionPolicies::CheckSwitch((int) OnePerDevice);
      if (refusal.IsEmpty())
         refusal = HM::SessionPolicies::CheckSwitch((int) BindToAddress);

      if (!refusal.IsEmpty())
         return COMError::GenerateError(refusal);

      domain->SetSessionIdleSeconds((int) IdleSeconds);
      domain->SetSessionLifetimeSeconds((int) LifetimeSeconds);
      domain->SetSessionOnePerDevice((int) OnePerDevice);
      domain->SetSessionBindToAddress((int) BindToAddress);

      HM::String result;
      if (!HM::PersistentDomain::SaveObject(domain, result, HM::PersistenceModeNormal))
         return COMError::GenerateError(result.IsEmpty() ? HM::String("The domain could not be saved.") : result);

      *Saved = VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GetComplaints(BSTR DomainName, BSTR Search, long MaxRows, BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      const __int64 domainID = HM::BounceTracker::DomainIDByName(DomainName);
      if (domainID <= 0)
      {
         if (!authentication_->GetIsDomainAdmin())
            return authentication_->GetAccessDenied();

         return COMError::GenerateError("No domain of that name exists.");
      }

      if (!MayAdministerDomain_(domainID))
         return authentication_->GetAccessDenied();

      HM::AnsiString json;
      if (!HM::ComplaintTracker::ComplaintsJson(domainID, 0, Search, MaxRows <= 0 ? 100 : MaxRows, 0, json))
         return COMError::GenerateError("The complaints could not be read from the database.");

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceUtilities::GetComplaintRates(BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      // Every domain's rate, so the server administrator's and not a domain
      // administrator's: one domain's rate is in that domain's own document.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      const HM::AnsiString json = HM::ComplaintTracker::RatesJson();
      if (json.IsEmpty())
         return COMError::GenerateError("The complaint rates could not be read from the database.");

      *Json = WideFromUtf8(json).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
