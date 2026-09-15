// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once


namespace HM
{
   class Account;
   class Message;
   class Domain;
   class MessageData;
   
   class SignatureAdder
   {
   public:
      SignatureAdder();
      virtual ~SignatureAdder();

      bool SetSignature(std::shared_ptr<Message> message,
                        std::shared_ptr<const Domain> sender_domain,
                        std::shared_ptr<const Account> sender_account,
                        std::shared_ptr<MessageData> &message_data,
                        bool account_signature_in_body = false);
      // Sets the signature of the message, based on the signature in the account
      // settings and domain settings. Returns true when message_data was changed
      // and must be written; it is handed back loaded only then.
      //
      // account_signature_in_body: the client has already written the account's
      // own signature into the body (the webmail does, in the compose box), so
      // only what the domain adds is appended - decided against the account's
      // signature exactly as it is when both are added here. Called through
      // OutgoingFooters, which every sending path uses.

      
   private:

      bool GetMessageIsReply_(std::shared_ptr<MessageData> &message_data);
      bool GetMessageIsLocal_(std::shared_ptr<Message> message);
   

   };
}

