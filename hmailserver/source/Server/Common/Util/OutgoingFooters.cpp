// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OutgoingFooters.h"

#include "DisclaimerAdder.h"
#include "SignatureAdder.h"

#include "../Application/ObjectCache.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   std::shared_ptr<const Domain>
   OutgoingFooters::SenderDomainOf(const String &envelope_sender)
   {
      if (envelope_sender.IsEmpty())
         return nullptr;

      std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
      const String address = domainAliases->ApplyAliasesOnAddress(envelope_sender);

      return CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(address));
   }

   bool
   OutgoingFooters::Add(std::shared_ptr<Message> message,
                        std::shared_ptr<const Domain> sender_domain,
                        std::shared_ptr<const Account> sender_account,
                        bool submission_is_ours,
                        AccountSignature account_signature,
                        std::shared_ptr<MessageData> &message_data)
   {
      SignatureAdder signatureAdder;
      const bool signatureAdded = signatureAdder.SetSignature(message, sender_domain, sender_account, message_data,
                                                              account_signature == AccountSignatureWrittenByClient);

      // Below the signature, on the same MessageData, so that one write carries
      // both.
      const bool disclaimed = DisclaimerAdder::Append(message, sender_domain, submission_is_ours, message_data);

      return signatureAdded || disclaimed;
   }

   void
   OutgoingFooters::AddToWebmailMessage(std::shared_ptr<Message> message,
                                        std::shared_ptr<const Account> writer)
   {
      if (!message)
      {
         HM_ASSERT(0);
         return;
      }

      std::shared_ptr<const Domain> senderDomain = SenderDomainOf(message->GetFromAddress());

      // Both adders need the sender's domain; a sender at no hosted domain -
      // which the webmail does not produce, but which costs nothing to say -
      // has neither footer to add.
      if (!senderDomain)
         return;

      // Signed in: the webmail's own session, or the draft of an account that
      // had one when it scheduled it.
      const bool submissionIsOurs = true;

      std::shared_ptr<MessageData> messageData;
      if (!Add(message, senderDomain, writer, submissionIsOurs, AccountSignatureWrittenByClient, messageData) || !messageData)
         return;

      messageData->WriteReported(PersistentMessage::GetFileName(message), "The message signature and the domain disclaimer");
   }
}
