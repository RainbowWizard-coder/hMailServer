// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "InterfaceRoute.h"

#include "InterfaceRouteAddresses.h"

#include "../Common/BO/Routes.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP InterfaceRoute::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] = 
      {
         &IID_IInterfaceRoute,
      };
   
      for (int i=0;i<sizeof(arr)/sizeof(arr[0]);i++)
      {
         if (InlineIsEqualGUID(*arr[i],riid))
            return S_OK;
      }
      return S_FALSE;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

std::shared_ptr<HM::Route>
InterfaceRoute::WritableRoute_()
{
   // As COMCollectionItem::Writable_, with the copy Route::CopyFields makes: a
   // route holds the lock that guards its address list, and has no copy
   // constructor.
   if (ObjectIsShared_())
      HoldCopy_(RunningOrHeld_()->CopyFields());

   return object_;
}

STDMETHODIMP InterfaceRoute::get_ID(long *pVal)
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

STDMETHODIMP InterfaceRoute::get_DomainName(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->DomainName().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_DomainName(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->DomainName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_Description(BSTR *pVal)
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

STDMETHODIMP InterfaceRoute::put_Description(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetDescription(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_TargetSMTPHost(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->TargetSMTPHost().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_TargetSMTPHost(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->TargetSMTPHost(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_TargetSMTPPort(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->TargetSMTPPort();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_TargetSMTPPort(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->TargetSMTPPort(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_NumberOfTries(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->NumberOfTries();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_NumberOfTries(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->NumberOfTries(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_MinutesBetweenTry(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->MinutesBetweenTry();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_MinutesBetweenTry(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->MinutesBetweenTry(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      HM::String sErrorMessage;

      // Onto a copy even when nothing was put before it: the running item is
      // never what the persister is handed, and neither is one a refresh has
      // replaced since this object was given it (COMCollection.h).
      WritableRoute_();

      if (HM::PersistentRoute::SaveObject(object_, sErrorMessage, HM::PersistenceModeNormal))
      {
         // A new route joins the running configuration; a changed one - this
         // object's copy - reaches it by the routes being read again.
         PublishSaved_("InterfaceRoute::Save");

         return S_OK;
      }
   
      return COMError::GenerateError(sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_AllAddresses(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->ToAllAddresses() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_AllAddresses(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->ToAllAddresses(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_Addresses(IInterfaceRouteAddresses **pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      CComObject<InterfaceRouteAddresses>* pInterfaceAddresses = new CComObject<InterfaceRouteAddresses>;
      pInterfaceAddresses->SetAuthentication(authentication_);
      
      // The running route's list, while the route is in the running
      // configuration, whether or not this object holds a copy of it: an
      // address is saved and deleted by itself, by its own Save and Delete,
      // and takes effect at once, as it always has - a copy's list would be
      // one the running server never reads. An address changed in place is
      // written onto a copy of its own (COMCollection.h). A list held while a
      // refresh of the routes replaces the route is no longer the running
      // route's; a save or a delete through it reads the running route's list
      // again (InterfaceRouteAddresses::ReachRunningRoute).
      std::shared_ptr<HM::RouteAddresses> pAdresses = RunningOrHeld_()->GetAddresses();
   
      pInterfaceAddresses->Attach(pAdresses);
      pInterfaceAddresses->AddRef();
   
      *pVal = pInterfaceAddresses;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_RelayerRequiresAuth(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetRelayerRequiresAuth() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_RelayerRequiresAuth(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      WritableRoute_()->SetRelayerRequiresAuth(newVal == VARIANT_TRUE ? true : false);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_TreatSecurityAsLocalDomain(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetTreatRecipientAsLocalDomain() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_TreatSecurityAsLocalDomain(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetTreatRecipientAsLocalDomain(newVal == VARIANT_TRUE ? true : false);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_TreatSenderAsLocalDomain(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetTreatSenderAsLocalDomain() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_TreatSenderAsLocalDomain(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetTreatSenderAsLocalDomain(newVal == VARIANT_TRUE ? true : false);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_TreatRecipientAsLocalDomain(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
      *pVal = object_->GetTreatRecipientAsLocalDomain() ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_TreatRecipientAsLocalDomain(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetTreatRecipientAsLocalDomain(newVal == VARIANT_TRUE ? true : false);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_RelayerAuthUsername(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetRelayerAuthUsername().AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_RelayerAuthUsername(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetRelayerAuthUsername(HM::String(newVal));
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::SetRelayerAuthPassword(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetRelayerAuthPassword(HM::String(newVal));
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_UseSSL(VARIANT_BOOL *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetConnectionSecurity() == HM::CSSSL ? VARIANT_TRUE : VARIANT_FALSE;
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_UseSSL(VARIANT_BOOL newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      if (newVal == VARIANT_TRUE)
         WritableRoute_()->SetConnectionSecurity(HM::CSSSL);
      else
         WritableRoute_()->SetConnectionSecurity(HM::CSNone);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::put_ConnectionSecurity(eConnectionSecurity newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      WritableRoute_()->SetConnectionSecurity((HM::ConnectionSecurity) newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::get_ConnectionSecurity(eConnectionSecurity *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (eConnectionSecurity) object_->GetConnectionSecurity();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceRoute::Delete()
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();
   
      if (!parent_collection_)
         return HM::PersistentRoute::DeleteObject(object_) ? S_OK : S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

