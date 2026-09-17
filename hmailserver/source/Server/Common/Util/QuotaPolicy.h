// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class Account;
   class Domain;

   // The quota policy as it applies to one mailbox: the domain's values where
   // the domain has set them, the server's where it has not. A mailbox's limit
   // was one number that mail either fitted under or did not; the policy puts
   // three points around it - the share at which the owner is warned, the share
   // at which the account stops sending, and how far past the limit incoming
   // mail is still accepted - and lets a domain choose its own, in its own
   // words.
   class QuotaPolicy
   {
   public:

      // The warning threshold, as a share of the limit; 0 warns nobody.
      static int WarningPercent(std::shared_ptr<const Account> account);
      static int WarningPercent(std::shared_ptr<const Domain> domain);

      // The subject and text of the warning: the domain's when it has them, the
      // QUOTA_WARNING server messages otherwise. The macros are left in place
      // for the caller to fill.
      static void WarningText(std::shared_ptr<const Account> account, String &subject, String &body);

      // How far past the limit incoming mail is still accepted, as a share of
      // the limit; 0 is not at all.
      static int GracePercent(std::shared_ptr<const Account> account);
      static int GracePercent(std::shared_ptr<const Domain> domain);

      // The share of the limit at which the account can no longer send; 0 is
      // never.
      static int SendStopPercent(std::shared_ptr<const Account> account);
      static int SendStopPercent(std::shared_ptr<const Domain> domain);

      // Whether a delivery of this many bytes is accepted into the mailbox:
      // the limit plus the grace that applies to it.
      static bool AcceptsDelivery(std::shared_ptr<const Account> account, __int64 bytes);

      // The share of the limit the mailbox holds now; 0 for a mailbox without
      // a limit.
      static int UsedPercent(std::shared_ptr<const Account> account);

      // Whether the account has reached its send-stop, and if so why, in the
      // sentence the SMTP session answers with.
      static bool SendingStopped(std::shared_ptr<const Account> account, String &reason);

   private:

      static std::shared_ptr<const Domain> DomainOf_(std::shared_ptr<const Account> account);
   };
}
