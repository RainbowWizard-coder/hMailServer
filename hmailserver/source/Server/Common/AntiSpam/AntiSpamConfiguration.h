// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class SURBLServers;
   class GreyListingWhiteAddresses;
   class GreyListingBypassEntries;
   class WhiteListAddresses;
   class BlockedSenders;
   class DNSBlackLists;

   class AntiSpamConfiguration
   {
   public:

      bool Load();

      AntiSpamConfiguration();
      virtual ~AntiSpamConfiguration();

      int GetSpamThreshold();

      bool GetCheckHostInHelo();
      void SetCheckHostInHelo(bool bNewVal);

      int GetCheckHostInHeloScore();
      void SetCheckHostInHeloScore(int bNewVal);

      bool GetCheckPTR();
      void SetCheckPTR(bool bNewVal);

      int GetCheckPTRScore();
      void SetCheckPTRScore(int bNewVal);

      bool GetUseGreyListing();
      void SetUseGreyListing(bool bNewVal);

      bool GetGreylistingTemporarilyDisabled();
      void SetGreylistingTemporarilyDisabled(bool bNewVal);

      int GetGreyListingInitialDelay();
      void SetGreyListingInitialDelay(int iNewVal);

      int GetGreyListingInitialDelete();
      void SetGreyListingInitialDelete(int iNewVal);

      int GetGreyListingFinalDelete();
      void SetGreyListingFinalDelete(int iNewVal);

      // Greylisting, hardened (SMTP/GreyListing.h). All three are hm_settings rows
      // seeded by schema 6072 and EnsureSettingDefaults_, off or inert by default.
      bool GetGreyListingSlideOnEarlyRetry();
      void SetGreyListingSlideOnEarlyRetry(bool bNewVal);

      int GetGreyListingAbuseThreshold();
      void SetGreyListingAbuseThreshold(int iNewVal);

      int GetGreyListingAbuseLockMinutes();
      void SetGreyListingAbuseLockMinutes(int iNewVal);

      bool GetAddHeaderSpam();
      void SetAddHeaderSpam(bool bNewVal);

      bool GetAddHeaderReason();
      void SetAddHeaderReason(bool bNewVal);

      bool GetPrependSubject();
      void SetPrependSubject(bool bNewVal);

      String GetPrependSubjectText();
      void SetPrependSubjectText(const String &sText);

      bool GetUseMXChecks();
      void SetUseMXChecks(bool bNewVal);

      int GetUseMXChecksScore();
      void SetUseMXChecksScore(int bNewVal);

      bool GetUseSPF();
      void SetUseSPF(bool bNewVal);

      int GetUseSPFScore();
      void SetUseSPFScore(int bNewVal);

      // SPF's other results, scored on their own (discussion #286). Fail keeps
      // UseSPFScore; SoftFail, TempError and PermError each have a score of
      // their own, 0 by default, and 0 is "not scored" as it always was.
      // Neutral and None are never scored.
      int GetUseSPFSoftFailScore();
      void SetUseSPFSoftFailScore(int newVal);

      int GetUseSPFTempErrorScore();
      void SetUseSPFTempErrorScore(int newVal);

      int GetUseSPFPermErrorScore();
      void SetUseSPFPermErrorScore(int newVal);

      // Whether a TempError answers 451 for the transaction rather than being
      // accepted and scored: RFC 7208 section 8.6's own answer to a check that
      // did not finish. Off by default.
      bool GetSPFTempErrorDefer();
      void SetSPFTempErrorDefer(bool newVal);

      // MAIL FROM:<> with more than one RCPT TO (discussion #283): refused with
      // a 550, and optionally the connecting address is banned for a while,
      // through the same auto-ban a run of failed logons earns. Both off by
      // default; a single-recipient null sender is never touched.
      bool GetRejectMultiRcptNullSender();
      void SetRejectMultiRcptNullSender(bool newVal);

      bool GetBanMultiRcptNullSender();
      void SetBanMultiRcptNullSender(bool newVal);

      int GetBanMultiRcptNullSenderHours();
      void SetBanMultiRcptNullSenderHours(int newVal);

      // A HELO name, or an envelope sender's domain, that is not a fully
      // qualified domain name (GitLab issue 51, FqdnCheck.h): refused with a 550
      // at MAIL FROM. Two switches, each off by default.
      bool GetRefuseHeloNotFqdn();
      void SetRefuseHeloNotFqdn(bool newVal);

      bool GetRefuseMailFromDomainNotFqdn();
      void SetRefuseMailFromDomainNotFqdn(bool newVal);

      // The score of a message whose links name more domains than the URI lists
      // are asked about (SURBL::MaxDomainsAsked), once per message while a URI
      // list is active; 5 by default, and 0 scores nothing.
      int GetSurblTooManyDomainsScore();
      void SetSurblTooManyDomainsScore(int newVal);


      int GetSpamMarkThreshold();
      void SetSpamMarkThreshold(int bNewVal);

      int GetSpamDeleteThreshold();
      void SetSpamDeleteThreshold(int bNewVal);

      bool GetSpamAssassinEnabled();
      void SetSpamAssassinEnabled(bool bNewVal);

      int GetSpamAssassinScore();
      void SetSpamAssassinScore(int bNewVal);

      bool GetSpamAssassinMergeScore();
      void SetSpamAssassinMergeScore(bool bNewVal);

      String GetSpamAssassinHost();
      void SetSpamAssassinHost(const String &sNewVal);

      int GetSpamAssassinPort();
      void SetSpamAssassinPort(int bNewVal);

      int GetAntiSpamMaxSizeKB();
      void SetAntiSpamMaxSizeKB(int newVal);

      bool GetDKIMVerificationEnabled();
      void SetDKIMVerificationEnabled(bool newValue);

      int GetDKIMVerificationFailureScore();
      void SetDKIMVerificationFailureScore(int newValue);

      bool GetDMARCEnabled();
      void SetDMARCEnabled(bool newValue);

      int GetDMARCFailureScore();
      void SetDMARCFailureScore(int newValue);

      bool GetArcFilteringEnabled();
      void SetArcFilteringEnabled(bool newValue);

      String GetArcTrustedSealers();
      void SetArcTrustedSealers(const String &newValue);

      bool GetBypassGreyListingOnSPFSuccess();
      void SetBypassGreyListingOnSPFSuccess(bool newValue);

      bool GetBypassGreyListingOnMailFromMX();
      void SetBypassGreyListingOnMailFromMX(bool newValue);

      void ClearGreyListingTriplets();

      bool XMLStore(XNode *pBackupNode, int Options);
      bool XMLLoad(XNode *pBackupNode, int iRestoreOptions);

      std::shared_ptr<GreyListingWhiteAddresses> GetGreyListingWhiteAddresses();
      std::shared_ptr<GreyListingBypassEntries> GetGreyListingBypassEntries();
      std::shared_ptr<WhiteListAddresses> GetWhiteListAddresses();
      std::shared_ptr<BlockedSenders> GetBlockedSenders();
      std::shared_ptr<DNSBlackLists> GetDNSBlackLists() const;
      std::shared_ptr<SURBLServers> GetSURBLServers() const;

   private:
      
      std::shared_ptr<PropertySet> GetSettings_();

      bool greylisting_temporarily_disabled_;

      std::shared_ptr<DNSBlackLists> dnsBlackLists_;
      std::shared_ptr<SURBLServers> surblServers_;

      // Guards the two pointers above: the first Load publishes them while other
      // threads may copy them (Configuration's publication_mutex_ has the reason);
      // a later Load reads the same lists again in place. Held only to copy or
      // swap a pointer - a leaf.
      mutable boost::mutex lists_mutex_;

   };
}
