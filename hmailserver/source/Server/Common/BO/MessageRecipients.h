// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

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

      // Each recipient's original address, or its address when it has none, each
      // once, comma-separated. The original address is the one RecipientParser
      // was given, before it resolved it: for mail received by SMTP the RCPT TO
      // (a bare name with the default domain added); for mail an external
      // account downloaded, the account it was downloaded into, or with MIME
      // recipients on the addresses its header and Received fields named. An
      // address that added no recipient - each it resolved to was there
      // already, from an earlier address - is not here: RecipientParser keeps
      // the first. GetCommaSeperatedRecipientList lists what the addresses
      // resolved to instead: a distribution list's members, an alias's target,
      // a masked address's owner.
      String GetCommaSeparatedOriginalAddresses();

      void RemoveNonLocalAccounts();
      void RemoveExternal();

   private:

      std::vector<std::shared_ptr<MessageRecipient> > recipients_;

   };
}