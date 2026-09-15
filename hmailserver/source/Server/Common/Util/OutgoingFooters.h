// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The two footers a hosted domain puts on the mail its people send - the
// signature (SignatureAdder) and the legal disclaimer (DisclaimerAdder) - put
// on in one function, in that order, whichever way the person sent.
//
// There are three ways, and until 15 September 2026 only the first had them:
//
//   * SMTP submission, SMTPConnection::DoPreAcceptMessageModifications_;
//   * the webmail's send, POST /api/v1/me/messages (RestApiServer::HandleMeMessageSend_);
//   * the webmail's send-later, ScheduledMailTask::SendDraft.
//
// The two webmail paths write the message into the queue themselves, so they
// never passed through the SMTP accept path, and a domain's compliance footer
// was silently missing from every message a person wrote in the browser. Each
// of them now calls AddToWebmailMessage once, after the file is written and
// before its size is taken and its row saved; SMTP calls Add. The rules - which
// messages get what - live in the two adders and nowhere else, so the paths
// cannot drift apart.
//
// Every one of these runs before DKIMSigner::Sign, which signs at delivery, so
// this server's own DKIM signature covers the footers rather than being broken
// by them.

#pragma once

namespace HM
{
   class Account;
   class Domain;
   class Message;
   class MessageData;

   class OutgoingFooters
   {
   public:

      // Who wrote the account's own signature into the body. Over SMTP the
      // server adds it, because a mail client does not know it exists. The
      // webmail writes it into the compose box itself, where the person sees
      // it and can change it, so adding it again would sign the message
      // twice; what the DOMAIN adds is decided against the account's signature
      // just the same (SignatureAdder::SetSignature).
      enum AccountSignature
      {
         AccountSignatureAddedHere = 0,
         AccountSignatureWrittenByClient = 1
      };

      // The hosted domain whose settings apply to mail sent from envelope_sender:
      // the domain aliases applied, then the domain looked up by name. It is the
      // lookup SMTPConnection::ProtocolMAIL_ makes for sender_domain_, so a
      // message from an address at domain A uses domain A's signature and
      // disclaimer on every path. Empty for an address at no hosted domain.
      static std::shared_ptr<const Domain> SenderDomainOf(const String &envelope_sender);

      // The signature, then the disclaimer, on message_data - loaded here from
      // the message file when the caller has not loaded it and one of them has
      // something to add. True when message_data was changed and must be
      // written; the caller owns the write. submission_is_ours is
      // DisclaimerAdder::Append's.
      static bool Add(std::shared_ptr<Message> message,
                      std::shared_ptr<const Domain> sender_domain,
                      std::shared_ptr<const Account> sender_account,
                      bool submission_is_ours,
                      AccountSignature account_signature,
                      std::shared_ptr<MessageData> &message_data);

      // For a message the webmail has written into the queue for a signed-in
      // account: the domain resolved from the envelope sender, both footers
      // added, and the file rewritten only when something was added - a
      // message neither changes, a signed or encrypted one above all, keeps its
      // bytes. A failed rewrite is logged and the message goes as written,
      // which is what the SMTP path does with one.
      //
      // writer is the signed-in account, and it stands where SMTP has the
      // account of the MAIL FROM address: the signature the page wrote into the
      // body is the writer's whichever identity the message is from, so the
      // writer's is the one the domain's signature is decided against, and the
      // writer's name is the one that fills the signature's macros.
      static void AddToWebmailMessage(std::shared_ptr<Message> message,
                                      std::shared_ptr<const Account> writer);
   };
}
