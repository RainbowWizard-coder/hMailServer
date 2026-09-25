// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceRemoteDomainPolicy.h"

#include "../Common/BO/RemoteDomainPolicies.h"
#include "../Common/Persistence/PersistentRemoteDomainPolicy.h"
#include "../Common/Util/GatewayEncryption.h"
#include "../Common/Util/Unicode.h"
#include "../SMTP/RecipientCallout.h"

#include "COMError.h"

#include <ctime>

namespace
{
   // The key a policy holds, judged as its own method - or, while encryption is
   // off, as what it looks like - so that the fingerprint and the expiry answer
   // for a key kept for later as well as for one in use.
   HM::GatewayEncryption::KeyFacts FactsOfHeldKey(std::shared_ptr<HM::RemoteDomainPolicy> policy)
   {
      const HM::String key = policy->GetEncryptKey();
      HM::RemoteEncryption method = policy->GetEncrypt();
      if (method == HM::RemoteEncryptNone)
         method = HM::GatewayEncryption::MethodOfKey(key);

      if (key.IsEmpty() || method == HM::RemoteEncryptNone)
         return HM::GatewayEncryption::KeyFacts();

      return HM::GatewayEncryption::Inspect(method, key, policy->GetDomainName(), (__int64) time(nullptr));
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] =
      {
         &IID_IInterfaceRemoteDomainPolicy,
      };

      for (int i = 0; i < sizeof(arr) / sizeof(arr[0]); i++)
      {
         if (InlineIsEqualGUID(*arr[i], riid))
            return S_OK;
      }
      return S_FALSE;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_ID(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetID();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_DomainName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetDomainName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_DomainName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetDomainName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_Description(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetDescription().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_Description(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetDescription(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_Active(VARIANT_BOOL *pVal)
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

STDMETHODIMP InterfaceRemoteDomainPolicy::put_Active(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetActive(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_OutboundTls(eRemoteTlsRequirement *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eRemoteTlsRequirement) object_->GetOutboundTlsRequirement();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_OutboundTls(eRemoteTlsRequirement newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // A number outside the enum is refused rather than stored. The values
      // decide whether mail is held up, and a stored 7 would be read back by
      // PersistentRemoteDomainPolicy as RemoteTlsDefault at the next start -
      // so the policy would work until the service was restarted and then
      // silently stop. Refusing here is the only place that can be seen.
      if (newVal < HM::RemoteTlsDefault || newVal > HM::RemoteTlsDane)
         return COMError::GenerateError("Invalid TLS requirement. Use 0 (none), 1 (encrypted), 2 (verified) or 3 (DANE).");

      Writable_()->SetOutboundTlsRequirement((HM::RemoteTlsRequirement) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_RequireInboundTls(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetRequireInboundTls() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_RequireInboundTls(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetRequireInboundTls(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_MaxMessageSizeKB(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMaxMessageSizeKB();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_MaxMessageSizeKB(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("The maximum message size cannot be negative. 0 means no limit.");

      Writable_()->SetMaxMessageSizeKB(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_MaxConnections(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMaxConnections();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_MaxConnections(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("The maximum number of connections cannot be negative. 0 means unlimited.");

      Writable_()->SetMaxConnections(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_MaxMessagesPerMinute(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetMaxMessagesPerMinute();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_MaxMessagesPerMinute(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("The message rate cannot be negative. 0 means unlimited.");

      Writable_()->SetMaxMessagesPerMinute(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_AllowAutomaticReplies(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAllowAutomaticReplies() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_AllowAutomaticReplies(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetAllowAutomaticReplies(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_AllowForwarding(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetAllowForwarding() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_AllowForwarding(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetAllowForwarding(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutEnabled(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutEnabled(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetCalloutEnabled(newVal == VARIANT_TRUE);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutHost(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutHost().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutHost(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetCalloutHost(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutPort(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutPort();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutPort(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 1 || newVal > 65535)
         return COMError::GenerateError("The verification port must be between 1 and 65535.");

      Writable_()->SetCalloutPort(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutTimeoutSeconds(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutTimeoutSeconds();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutTimeoutSeconds(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // The ceiling is enforced here as well as in RecipientCallout, because a
      // value refused at the door is a value an administrator sees refused; one
      // silently clamped at use looks like it worked and did not.
      if (newVal < 1 || newVal > HM::RecipientCallout::CalloutMaxTimeoutSeconds)
         return COMError::GenerateError("The verification timeout must be between 1 and 60 seconds. It is spent inside an SMTP session that a sender is waiting on.");

      Writable_()->SetCalloutTimeoutSeconds(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutCacheMinutes(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutCacheMinutes();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutCacheMinutes(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("The verification cache lifetime cannot be negative.");

      Writable_()->SetCalloutCacheMinutes(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_CalloutMaxPerMinute(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetCalloutMaxPerMinute();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_CalloutMaxPerMinute(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal < 0)
         return COMError::GenerateError("The verification rate cannot be negative. 0 means unlimited, which is not recommended.");

      Writable_()->SetCalloutMaxPerMinute(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_Encrypt(eRemoteEncryption *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eRemoteEncryption) object_->GetEncrypt();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_Encrypt(eRemoteEncryption newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // As the TLS requirement: a number outside the enum is refused here,
      // where it can be seen, rather than stored and read back as none at the
      // next start.
      const int value = (int) newVal;
      if (value < (int) HM::RemoteEncryptNone || value > (int) HM::RemoteEncryptSmime)
         return COMError::GenerateError("Invalid encryption. Use 0 (none), 1 (OpenPGP) or 2 (S/MIME).");

      Writable_()->SetEncrypt((HM::RemoteEncryption) value);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_EncryptKey(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Written, never read back, as the REST API does it: what is held is
      // described by EncryptKeyFingerprint, EncryptKeyExpires and
      // EncryptKeySummary. The key is public, and the backup carries it; this
      // is so that an editor shows what the key IS rather than a block of
      // base64 nobody can check against the partner's published fingerprint.
      *pVal = HM::String().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_EncryptKey(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Judged at Save, against the method and the domain the policy has then:
      // private-key material and an over-long paste are refused whatever the
      // method, and a key that differs from the one the database holds must be
      // one a message can be encrypted to. Only white space removes the key.
      Writable_()->SetEncryptKey(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_EncryptFallback(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetEncryptFallback();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::put_EncryptFallback(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal != (long) HM::RemoteEncryptFallbackRefuse && newVal != (long) HM::RemoteEncryptFallbackSendUnencrypted)
         return COMError::GenerateError("Invalid encryption fallback. Use 0 (refuse the message and tell the sender) or 1 (send it unencrypted and log it).");

      Writable_()->SetEncryptFallback((HM::RemoteEncryptFallback) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_EncryptKeyFingerprint(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = HM::String(FactsOfHeldKey(object_).fingerprint).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_EncryptKeyExpires(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = HM::String(HM::GatewayEncryption::ExpiryText(FactsOfHeldKey(object_).expires)).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::get_EncryptKeySummary(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      const HM::AnsiString summary = HM::GatewayEncryption::Summary(object_->GetEncrypt(), object_->GetEncryptKey(),
         object_->GetDomainName(), (__int64) time(nullptr));

      HM::String text;
      HM::Unicode::MultiByteToWide(summary, text);

      *pVal = text.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::Save()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String errorMessage;

      // Onto a copy even when nothing was put before it: the running item is
      // never what the persister is handed, and neither is one a refresh has
      // replaced since this object was given it (COMCollection.h).
      Writable_();

      if (HM::PersistentRemoteDomainPolicy::SaveObject(object_, errorMessage, HM::PersistenceModeNormal))
      {
         // A new policy joins the running collection; a changed one - this
         // object's copy - reaches delivery by the policies being read again
         // (COMCollection.h), and a delivery in progress keeps the record it
         // was given.
         PublishSaved_("InterfaceRemoteDomainPolicy::Save");

         return S_OK;
      }

      // A refused save leaves the running policy as it was: the setters wrote
      // this object's copy, never the record delivery reads, so a refused key -
      // an expired certificate, a key with nothing to encrypt to, a method with
      // no key - governs nothing. The copy itself is put back as the database
      // holds it, so that what this object reads after a refusal is the policy
      // in effect, as it did before the copy existed. A policy never saved (id
      // 0) holds no copy and has nothing to go back to.
      if (HoldsCopy_() && object_->GetID() != 0)
         HM::PersistentRemoteDomainPolicy::ReadObject(object_, (long) object_->GetID());

      return COMError::GenerateError(errorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRemoteDomainPolicy::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (!parent_collection_)
         return HM::PersistentRemoteDomainPolicy::DeleteObject(object_) ? S_OK : S_FALSE;

      parent_collection_->DeleteItemByDBID(object_->GetID());

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
