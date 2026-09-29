// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class SURBLServer : public BusinessObject<SURBLServer>
   {
   public:
      SURBLServer(void);
      ~SURBLServer(void);

      // All objects should have an GetName()
      String GetName() const {return dnshost_; }

      bool GetIsActive() const  {return active_; }
      void SetIsActive(bool bNewVal) {active_ = bNewVal;}
   
      int GetScore() {return score_; }
      void SetScore(int iNewVal) {score_ = iNewVal; }

      String GetRejectMessage() const  {return reject_message_; }
      void SetRejectMessage(const String &sNewVal) {reject_message_ = sNewVal;}

      String GetDNSHost() const  {return dnshost_; }
      void SetDNSHost(const String &sNewVal) {dnshost_ = sNewVal;}

      // The answers that mean listed, in the DNSBL syntax - 127.0.1.0-255 or
      // 127.0.0.2*, ranges and wildcards joined by |. Empty means any answer.
      // Either way the codes the lists publish as errors are never a listing:
      // 127.255.255.0/24 (Spamhaus refusing the query), 127.0.0.1 (SURBL and
      // URIBL blocking it), and 127.0.1.255 and 127.0.2.255 (the Spamhaus DBL
      // and ZRD, asked about an address). See SURBL::ClassifyAnswer.
      String GetExpectedResult() const {return expected_result_; }
      void SetExpectedResult(const String &sNewVal) {expected_result_ = sNewVal;}

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems (XNode *pNode, int iOptions) {return true;};

   private:
      bool active_;
      
      String dnshost_;
      String reject_message_;
      String expected_result_;

      int score_;
   };
}