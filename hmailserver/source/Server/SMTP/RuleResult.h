// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{

   class RuleResult
   {
   public:
      RuleResult(void);
      ~RuleResult(void);

      String GetMoveToFolder() const {return move_to_folder_;}
      void SetMoveToFolder(const String &sIMAPFolderName) {move_to_folder_ = sIMAPFolderName;}
      
      String GetDeleteRuleName() const {return delete_rule_name_;}
      void SetDeleteRuleName(const String &sDeleteRuleName) {delete_rule_name_ = sDeleteRuleName;}

      __int64 GetSendUsingRoute() const {return send_using_route_;}
      void SetSendUsingRoute(__int64 iRuleID) {send_using_route_ = iRuleID; }

      bool GetDeleteEmail() const {return delete_email_;}
      void SetDeleteEmail(bool bNewVal) {delete_email_ = bNewVal; }

      String GetBindToAddress() const {return bind_to_address_;}
      void SetBindToAddress(const String &sAddress) {bind_to_address_ = sAddress;}

      // A rule asked for TLS to every external destination of this message,
      // for the message to be quarantined rather than delivered, or - after
      // acceptance, where a refusal is no longer possible - for it to be
      // dropped as rejected. Each names the rule, for the log and the reason.
      bool GetRequireTls() const {return !require_tls_rule_.IsEmpty();}
      String GetRequireTlsRuleName() const {return require_tls_rule_;}
      void SetRequireTls(const String &ruleName) {require_tls_rule_ = ruleName;}
      bool GetQuarantine() const {return !quarantine_rule_.IsEmpty();}
      String GetQuarantineRuleName() const {return quarantine_rule_;}
      void SetQuarantine(const String &ruleName) {quarantine_rule_ = ruleName;}

   private:

      String move_to_folder_;
      __int64 send_using_route_;
      
      bool delete_email_;
      String delete_rule_name_;

      String bind_to_address_;
      String require_tls_rule_;
      String quarantine_rule_;

   };
}