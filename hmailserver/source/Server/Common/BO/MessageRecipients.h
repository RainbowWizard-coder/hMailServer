// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class MessageRecipient;

   class MessageRecipients
   {
   public:
      MessageRecipients(void);
      ~MessageRecipients(void);

      void Clear();
      void Add(std::shared_ptr<MessageRecipient> pRecipient);
      std::vector<std::shared_ptr<MessageRecipient> > & GetVector() {return recipients_; }
      int GetCount() {return (int) recipients_.size(); }
      String GetCommaSeperatedRecipientList();

      // The addresses the message was sent to as its sender gave them, each
      // once, comma-separated: each recipient's original address - the RCPT TO
      // it came from - or its address when it has none.
      // GetCommaSeperatedRecipientList lists what they resolved to instead: a
      // distribution list's members, an alias's target, which are for this
      // server to know and not for the sender.
      String GetCommaSeparatedAddressesAsSent();

      void RemoveNonLocalAccounts();
      void RemoveExternal();

   private:

      std::vector<std::shared_ptr<MessageRecipient> > recipients_;

   };
}