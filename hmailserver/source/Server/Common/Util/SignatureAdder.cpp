// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "SignatureAdder.h"
#include "DisclaimerAdder.h"
#include "InlineImageParts.h"

#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/MessageData.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"

#include "../Mime/Mime.h"

#include "../../SMTP/SMTPConfiguration.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   SignatureAdder::SignatureAdder()
   {
      
   }

   SignatureAdder::~SignatureAdder()
   {
      
   }

   bool 
   SignatureAdder::SetSignature(std::shared_ptr<Message> message, 
      std::shared_ptr<const Domain> sender_domain, 
      std::shared_ptr<const Account> sender_account,
      std::shared_ptr<MessageData> &message_data,
      bool account_signature_in_body)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Sets the signature of the message, based on the signature in the account
   // settings and domain settings.
   //---------------------------------------------------------------------------()
   {
      if (!message)
      {
         HM_ASSERT(0);
         // Input error
         return false;
      }

      if (!sender_domain)
      {
         // Not a local sender - nothing to do.
         return false;
      }

      if (!sender_domain->GetAddSignaturesToLocalMail() && GetMessageIsLocal_(message))
      {
         // The message is local, but we have configured
         // the server not to add signatures to local email.
         return false;
      }

      String account_plain_text;
      String account_html;

      // First check if an account signature has been specified.
      if (sender_account && sender_account->GetEnableSignature())
      {
         account_plain_text = sender_account->GetSignaturePlainText();
         account_html = sender_account->GetSignatureHTML();

         if (!account_plain_text.IsEmpty() && account_html.IsEmpty())
         {
            // Plain text specified but not HTML. Copy plain text to HTML.
            account_html = account_plain_text;
            account_html.Replace(_T("\r\n"), _T("<br>\r\n"));
         }
      }

      // What is appended starts from the account's signature - unless the client
      // has already written that into the body, as the webmail does in its
      // compose box, where adding it again would sign the message twice. The
      // domain's part below is still decided against the account's signature,
      // so it is the same whichever way the message was sent.
      String signature_plain_text = account_signature_in_body ? String() : account_plain_text;
      String signature_html = account_signature_in_body ? String() : account_html;

      if (sender_domain->GetEnableSignature())
      {
         String sDomainPlainText = sender_domain->GetSignaturePlainText();
         String sDomainHTML = sender_domain->GetSignatureHTML();

         if (!sDomainPlainText.IsEmpty() && sDomainHTML.IsEmpty())
         {
            // Plain text specified but not HTML. Copy plain text to HTML.
            sDomainHTML = sDomainPlainText;
            sDomainHTML.Replace(_T("\r\n"), _T("<br>\r\n"));
         }

         // Check if we should overwrite the account signature with the 
         // domain signature, if we should append it, or if we just should
         // keep it.
         if (sender_domain->GetSignatureMethod() == HM::Domain::SMSetIfNotSpecifiedInAccount)
         {
            if (account_plain_text.IsEmpty())
               signature_plain_text = sDomainPlainText;
            if (account_html.IsEmpty())
               signature_html = sDomainHTML;
         }
         else if (sender_domain->GetSignatureMethod() == HM::Domain::SMAppendToAccountSignature)
         {
            signature_plain_text += "\r\n\r\n" + sDomainPlainText;
            signature_html += "<br><br>" + sDomainHTML;
         }
         else if (sender_domain->GetSignatureMethod() == HM::Domain::SMOverwriteAccountSignature)
         {
            signature_plain_text = sDomainPlainText;
            signature_html = sDomainHTML;
         }            
      }

      if (signature_plain_text.IsEmpty() && signature_html.IsEmpty())  
      {
         // No signature should be created.
         return false;
      }

      // A signature should be created. Loaded into a local and handed back only
      // when it is actually appended: a MessageData left in the caller's
      // variable by one of the guards below would have the caller rewrite a
      // message this function decided not to touch - and a signed message
      // re-serialised is a signed message whose bytes may have changed.
      std::shared_ptr<MessageData> data = message_data;
      const bool loadedHere = !data;

      if (loadedHere)
      {
         data = std::shared_ptr<MessageData>(new MessageData());
         std::shared_ptr<Account> emptyAccount;
         if (!data->LoadFromMessage(emptyAccount, message))
            return false;
      }

      if (!sender_domain->GetAddSignaturesToReplies() && GetMessageIsReply_(data))
      {
         // The message is a reply, but we have configured the
         // server not to add signatures to replies
         return false;
      }

      if (DisclaimerAdder::IsProtectedContentType(data->GetFieldValue(_T("Content-Type"))))
      {
         // The disclaimer's rule, for the same reason: GetBodyTextPlainPart finds
         // the text inside a multipart/signed, and appending to it breaks the
         // signature in every client that checks. The webmail's S/MIME send is
         // one of these, and so is a desktop client's. Sent as written, and said
         // in the log where an administrator missing a signature will look.
         String logText;
         logText.Format(_T("SignatureAdder - Message %I64d from %s is signed or encrypted (%s), so the signature was not appended: appending to it would break the signature or the encryption. The message was sent unchanged."),
            message->GetID(), message->GetFromAddress().c_str(), data->GetFieldValue(_T("Content-Type")).c_str());
         LOG_APPLICATION(logText);
         return false;
      }

      if (sender_account)
      {
         signature_plain_text.ReplaceNoCase(_T("%User.FirstName%"), sender_account->GetPersonFirstName());
         signature_plain_text.ReplaceNoCase(_T("%User.LastName%"), sender_account->GetPersonLastName());
         signature_plain_text.ReplaceNoCase(_T("%User.Address%"), sender_account->GetAddress());

         signature_html.ReplaceNoCase(_T("%User.FirstName%"), sender_account->GetPersonFirstName());
         signature_html.ReplaceNoCase(_T("%User.LastName%"), sender_account->GetPersonLastName());
         signature_html.ReplaceNoCase(_T("%User.Address%"), sender_account->GetAddress());
      }

      auto text_plain_body_part = data->GetBodyTextPlainPart();
      if (text_plain_body_part)
      {
         String text_plan_body_content = text_plain_body_part->GetUnicodeText();
         text_plan_body_content += "\r\n" + signature_plain_text;
         text_plain_body_part->SetUnicodeText(text_plan_body_content);
      }

      auto text_html_body_part = data->GetBodyTextHtmlPart();
      if (text_html_body_part)
      {
         String text_html_body_content = text_html_body_part->GetUnicodeText();
         text_html_body_content += "<br/>\r\n" + signature_html;
         text_html_body_part->SetUnicodeText(text_html_body_content);

         // The account's signature is the one the webmail's settings save, and
         // an image in it is a data: URL, which Gmail and Outlook do not show:
         // sent as a cid: part instead (InlineImageParts.h).
         if (InlineImageParts::HasDataImages(signature_html))
            InlineImageParts::FromDataUrls(*data);
      }

      if (loadedHere)
         message_data = data;

      return true;


   }

   bool 
   SignatureAdder::GetMessageIsReply_(std::shared_ptr<MessageData> &message_data)
   {
      String sHeader = message_data->GetFieldValue("References");
      if (!sHeader.IsEmpty())
         return true; // Contains Reference header - It's a reply.

      sHeader = message_data->GetFieldValue("In-Reply-To");
      if (!sHeader.IsEmpty())
         return true; // Contains In-Reply-To header - It's a reply.

      return false;
   }

   bool 
   SignatureAdder::GetMessageIsLocal_(std::shared_ptr<Message> message)
   {
      String sFromAddressDomain = StringParser::ExtractDomain(message->GetFromAddress());
      
      // Loop over the recipients and check if they are on the same domain.if
      
      std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = message->GetRecipients()->GetVector();
      auto iter = vecRecipients.begin();
      auto iterEnd = vecRecipients.end();

      for (; iter != iterEnd; iter++)
      {
         String sRecipientAddress = (*iter)->GetAddress();
         String sRecipientDomain = StringParser::ExtractDomain(sRecipientAddress);

         if (sFromAddressDomain.CompareNoCase(sRecipientDomain) != 0)
            return false;

      }
      

      return true;
   }
}
