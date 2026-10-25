// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class RuleCriteria : public BusinessObject<RuleCriteria>
   {
   public:

      enum PredefinedField
      {
         FTUnknown = 0,
         FTFrom = 1,
         FTTo = 2,
         FTCC = 3,
         FTSubject = 4,
         FTBody = 5,
         FTMessageSize = 6,
         FTRecipientList = 7,
         FTDeliveryAttempts = 8,
         FTAttachmentName = 9,

         // Outbound content policy (Util/ContentPolicy.h): each answered as a
         // list with ";" between the entries, as the attachment name is.
         FTSensitiveContent = 10,   // the detectors that found something: card_number, iban, account_number
         FTAttachmentType = 11,     // every attachment's type by its first bytes, not its name
         FTExternalRecipient = 12,  // every recipient this server does not host
         FTContent = 13             // the readable text: the parts and every attachment whose bytes are text
      };

      enum MatchType
      {
         None = 0,
         Equals = 1,
         Contains = 2,
         LessThan = 3,
         GreaterThan = 4,
         MatchesRegEx = 5,
         NotContains = 6,
         NotEquals = 7,
         Wildcard = 8,
         
      };

      RuleCriteria(void);
      ~RuleCriteria(void);

      String GetName() const {return ""; }

      __int64 GetRuleID() const {return rule_id_; }
      void SetRuleID(__int64 newval) {rule_id_ = newval; }

      String GetMatchValue() const {return match_string_; }
      void SetMatchValue(const String &iNewVal) {match_string_ = iNewVal; }

      PredefinedField GetPredefinedField() const {return field_type_; }
      void SetPredefinedField(PredefinedField i) {field_type_ = i; }

      MatchType GetMatchType() const {return match_type_; }
      void SetMatchType(MatchType mt) {match_type_ = mt; }

      String GetHeaderField() const {return header_field_; }
      void SetHeaderField(const String &sValue) {header_field_ = sValue; }

      bool GetUsePredefined() const {return use_predefined_field_;}
      void SetUsePredefined(bool bValue) {use_predefined_field_ = bValue; }

      bool XMLStore(XNode *pRuleNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) {return true;}

   private:

      __int64 rule_id_;

      String match_string_;

      PredefinedField field_type_;
      MatchType match_type_;

      String header_field_;
      
      bool use_predefined_field_;
   };
}