// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "./Events.h"

#include "../../SMTP/SMTPConfiguration.h"

#include "../BO/Account.h"
#include "../BO/Message.h"

#include "../Persistence/PersistentMessage.h"

#include "../Util/AWStats.h"

#include "ScriptServer.h"
#include "ScriptObjectContainer.h"
#include "Result.h"
#include "../Persistence/PersistentAccount.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Events::Events(void)
   {

   }

   Events::~Events(void)
   {
   }

   std::shared_ptr<Result>
   Events::FireOnClientValidatePassword(std::shared_ptr<const Account> pAccount, const String &sPassword)
   {
      if (!Configuration::Instance()->GetUseScriptServer())
         return nullptr;

      std::shared_ptr<ScriptObjectContainer> pContainer = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
      std::shared_ptr<Result> pResult = std::shared_ptr<Result>(new Result);

      // We cannot use the provided pAccount directly, since it might come from cache.
      // (that's why it's a const Account)
      // So, we use it to load from the PersistentAccount.
      std::shared_ptr<Account> pPersistentAccount = std::shared_ptr<Account>(new Account);
      PersistentAccount::ReadObject(pPersistentAccount, pAccount->GetAddress());

      pContainer->AddObject("HMAILSERVER_ACCOUNT", pPersistentAccount, ScriptObject::OTAccount);
      pContainer->AddObject("Result", pResult, ScriptObject::OTResult);

      // The password as a value, for the JavaScript engine, which calls the handler
      // with it rather than pasting it into source text; see ScriptArgument. A
      // password is the one string here a remote client chooses.
      pContainer->AddTextArgument(sPassword);

      // By default, pass through.
      pResult->SetValue(2);

      String sEventCaller;

      // Built for the language of the script in force, not the setting's: see
      // ScriptServer::GetLoadedLanguage. Storing the setting reloads nothing, and
      // a call written for the setting's language beside a script loaded in the
      // other one was an empty call - OnClientValidatePassword not asked, and the
      // stored password deciding in its place, without a word.
      String sScriptLanguage = ScriptServer::Instance()->GetLoadedLanguage();

      // The password is the one value here a remote client chooses, and it is
      // chosen BEFORE the logon: this runs for any client that names an existing
      // account. VBScript's doubled quote keeps it inside its literal - a quote
      // becomes a pair, which cannot close the literal, and a line break can only
      // break the call, never start a statement. JScript's escape character is the
      // backslash, and escaping the apostrophe alone let "\'); code; //" close the
      // literal and run the code; see ScriptServer::EscapeForJScriptLiteral.
      if (sScriptLanguage == _T("VBScript"))
      {
         // ScriptServer::EscapeForVBScriptLiteral, as every other VBScript call is
         // made: the quote doubled, and a line break or a NUL joined in from outside
         // the literal. The quote was doubled here and nothing else, so a password
         // holding a line break - an AUTH PLAIN or AUTH LOGIN response decodes to
         // whatever bytes the client chose - left the literal unterminated: the call
         // did not compile and the handler was not asked (upstream #671).
         sEventCaller.Format(_T("OnClientValidatePassword(HMAILSERVER_ACCOUNT, \"%s\")"), ScriptServer::EscapeForVBScriptLiteral(sPassword).c_str());
      }
      else if (sScriptLanguage == _T("JScript"))
      {
         String sEscapedPassword = ScriptServer::EscapeForJScriptLiteral(sPassword);

         sEventCaller.Format(_T("OnClientValidatePassword(HMAILSERVER_ACCOUNT, '%s')"), sEscapedPassword.c_str());
      }

      ScriptServer::Instance()->FireEvent(ScriptServer::EventOnClientValidatePassword, sEventCaller, pContainer, sScriptLanguage);
      return pResult;

   }

   bool 
   Events::FireOnDeliveryStart(std::shared_ptr<Message> pMessage)
   {
      if (!Configuration::Instance()->GetUseScriptServer())
         return true;

      std::shared_ptr<ScriptObjectContainer> pContainer = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
      std::shared_ptr<Result> pResult = std::shared_ptr<Result>(new Result);
      pContainer->AddObject("HMAILSERVER_MESSAGE", pMessage, ScriptObject::OTMessage);
      pContainer->AddObject("Result", pResult, ScriptObject::OTResult);
      String sEventCaller = "OnDeliveryStart(HMAILSERVER_MESSAGE)";
      ScriptServer::Instance()->FireEvent(ScriptServer::EventOnDeliveryStart, sEventCaller, pContainer);

      switch (pResult->GetValue())
      {
      case 1:
         {
            String sMessage;
            sMessage.Format(_T("SMTPDeliverer - Message %I64d: ")
               _T("Message will be deleted. Action triggered by script subscribing to OnDeliveryStart."),
               pMessage->GetID());
            LOG_APPLICATION(sMessage);

            return false;
         }
      }

      return true;
   }

   bool 
   Events::FireOnDeliverMessage(std::shared_ptr<Message> pMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Executes the OnDeliverMessage event.
   //---------------------------------------------------------------------------()
   {
      if (Configuration::Instance()->GetUseScriptServer())
      {
         std::shared_ptr<ScriptObjectContainer> pContainer = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
         std::shared_ptr<Result> pResult = std::shared_ptr<Result>(new Result);
         pContainer->AddObject("HMAILSERVER_MESSAGE", pMessage, ScriptObject::OTMessage);
         pContainer->AddObject("Result", pResult, ScriptObject::OTResult);
         String sEventCaller = "OnDeliverMessage(HMAILSERVER_MESSAGE)";
         ScriptServer::Instance()->FireEvent(ScriptServer::EventOnDeliverMessage, sEventCaller, pContainer);

         switch (pResult->GetValue())
         {
            case 1:
            {
               String sMessage;
               sMessage.Format(_T("SMTPDeliverer - Message %I64d: ")
                  _T("Message will be deleted. Action triggered by script subscribing to OnDeliverMessage."),
                  pMessage->GetID());
               LOG_APPLICATION(sMessage);

               return false;
            }
         }
      }      

      return true;
   }

   void
   Events::FireOnDeliveryFailed(std::shared_ptr<Message> pMessage, const String &sSendersIP, const String &sRecipient, const String &sErrorMessage)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Called once each time delivery to a recipient failed. It may be called
   // several times for a single message.
   //---------------------------------------------------------------------------()
   {
      // Send an event
      if (Configuration::Instance()->GetUseScriptServer())
      {
         String sEventCaller;

         String sRecipientCopy = sRecipient;
         String sErrorMessageCopy = sErrorMessage;

         // For the script in force; see FireOnClientValidatePassword.
         String sScriptLanguage = ScriptServer::Instance()->GetLoadedLanguage();

         if (sScriptLanguage == _T("VBScript"))
         {
            // ScriptServer::EscapeForVBScriptLiteral, as the JScript branch below uses
            // its own: the quote doubled and every line break joined in. The quotes
            // were taken out of both values, and only a CR LF pair joined in, so a lone
            // CR or LF in the remote server's answer - which its sender chooses - left
            // the literal unterminated: the call did not compile and the handler was
            // not told of the failure at all (upstream #671).
            //
            // Trimmed first, as it was trimmed when each CR LF pair was joined in
            // before the trim: of the white space at either end, but never into a
            // pair (ScriptServer::TrimKeepingCrLfPairs). So the handler is still
            // handed the CR LF that ends the error's last line, which the JScript
            // and JavaScript handlers, their error trimmed whole, are not given.
            sErrorMessageCopy = ScriptServer::TrimKeepingCrLfPairs(sErrorMessageCopy);

            sEventCaller.Format(_T("OnDeliveryFailed(HMAILSERVER_MESSAGE, \"%s\", \"%s\")"),
               ScriptServer::EscapeForVBScriptLiteral(sRecipientCopy).c_str(),
               ScriptServer::EscapeForVBScriptLiteral(sErrorMessageCopy).c_str());
         }
         else if (sScriptLanguage == _T("JScript"))
         {
            // The error is the remote server's answer, word for word, so its
            // sender chooses it, and the recipient's address may hold a backslash
            // in a quoted local part: both made literals whole, the backslash
            // first (ScriptServer::EscapeForJScriptLiteral). Trimmed before, as
            // the value the JavaScript engine is handed below is; a line break
            // inside is kept, as \r\n was before.
            sErrorMessageCopy.TrimLeft();
            sErrorMessageCopy.TrimRight();

            sEventCaller.Format(_T("OnDeliveryFailed(HMAILSERVER_MESSAGE, '%s', '%s')"),
               ScriptServer::EscapeForJScriptLiteral(sRecipientCopy).c_str(),
               ScriptServer::EscapeForJScriptLiteral(sErrorMessageCopy).c_str());
         }

         std::shared_ptr<ScriptObjectContainer> pContainer  = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
         pContainer->AddObject("HMAILSERVER_MESSAGE", pMessage, ScriptObject::OTMessage);

         // The same two values as values, for the JavaScript engine: the recipient as
         // it is and the error trimmed, line breaks and quotes kept, since nothing is
         // quoted into source text there.
         String sErrorMessageValue = sErrorMessage;
         sErrorMessageValue.TrimLeft();
         sErrorMessageValue.TrimRight();

         pContainer->AddTextArgument(sRecipient);
         pContainer->AddTextArgument(sErrorMessageValue);

         ScriptServer::Instance()->FireEvent(ScriptServer::EventOnDeliveryFailed, sEventCaller, pContainer, sScriptLanguage);
      }      

   }

   std::shared_ptr<Result>
   Events::FireOnExternalAccountDownload(std::shared_ptr<FetchAccount> fetchAccount, std::shared_ptr<Message> pMessage, const String &sRemoteUID)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Called once each time delivery to a recipient failed. It may be called
   // several times for a single message.
   //---------------------------------------------------------------------------()
   {
      // Send an event
      std::shared_ptr<Result> pResult = std::shared_ptr<Result>(new Result);
      if (!Configuration::Instance()->GetUseScriptServer())
         return pResult;
      
      String sEventCaller;

      String sRemoteUIDCopy = sRemoteUID;

      // For the script in force; see FireOnClientValidatePassword.
      String sScriptLanguage = ScriptServer::Instance()->GetLoadedLanguage();

      if (sScriptLanguage == _T("VBScript"))
      {
         // ScriptServer::EscapeForVBScriptLiteral, as the other VBScript calls are
         // made. The UID is whatever the remote server answered to UIDL, read up to
         // a CR LF pair and trimmed of spaces and tabs only, so a lone CR or LF in it
         // comes this far; with the quote doubled and nothing else, it left the
         // literal open, the call did not compile and the handler was not asked
         // (upstream #671).
         sRemoteUIDCopy = ScriptServer::EscapeForVBScriptLiteral(sRemoteUID);

         if (pMessage)
            sEventCaller.Format(_T("OnExternalAccountDownload(HMAILSERVER_FETCHACCOUNT, HMAILSERVER_MESSAGE, \"%s\")"), sRemoteUIDCopy.c_str());
         else
            sEventCaller.Format(_T("OnExternalAccountDownload(HMAILSERVER_FETCHACCOUNT, Nothing, \"%s\")"), sRemoteUIDCopy.c_str());
      }
      else if (sScriptLanguage == _T("JScript"))
      {
         // The UID is whatever the remote server answered to UIDL - its operator
         // chooses it - so it is made a literal whole, the backslash first
         // (ScriptServer::EscapeForJScriptLiteral).
         sRemoteUIDCopy = ScriptServer::EscapeForJScriptLiteral(sRemoteUID);

         if (pMessage)
            sEventCaller.Format(_T("OnExternalAccountDownload(HMAILSERVER_FETCHACCOUNT, HMAILSERVER_MESSAGE, '%s')"), sRemoteUIDCopy.c_str());
         else
            sEventCaller.Format(_T("OnExternalAccountDownload(HMAILSERVER_FETCHACCOUNT, null, '%s')"), sRemoteUIDCopy.c_str());
      }

      std::shared_ptr<ScriptObjectContainer> pContainer  = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
      
      pContainer->AddObject("HMAILSERVER_FETCHACCOUNT", fetchAccount, ScriptObject::OTFetchAccount);

      if (pMessage)
         pContainer->AddObject("HMAILSERVER_MESSAGE", pMessage, ScriptObject::OTMessage);
      pContainer->AddObject("Result", pResult, ScriptObject::OTResult);

      // The remote UID as a value, for the JavaScript engine; the message argument
      // before it is null there when there is no message, as Nothing and null are
      // in the text above.
      pContainer->AddTextArgument(sRemoteUID);

      ScriptServer::Instance()->FireEvent(ScriptServer::EventOnExternalAccountDownload, sEventCaller, pContainer, sScriptLanguage);

      return pResult;
   }

}