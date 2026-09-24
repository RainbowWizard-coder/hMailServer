// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceDistributionListRecipient.h"

#include "../Common/Persistence/PersistentDistributionListRecipient.h"

#include "../Common/BO/DistributionListRecipients.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"
#include "../Common/Util/ListManager.h"

STDMETHODIMP InterfaceDistributionListRecipient::InterfaceSupportsErrorInfo(REFIID riid)
{
   try
   {
      static const IID* arr[] = 
      {
         &IID_IInterfaceDistributionListRecipient,
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

STDMETHODIMP InterfaceDistributionListRecipient::get_ID(long *pVal)
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

STDMETHODIMP InterfaceDistributionListRecipient::put_ID(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      object_->SetID(newVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::get_RecipientAddress(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sVal = object_->GetAddress();
      *pVal = sVal.AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::put_RecipientAddress(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

   
   
      HM::String sNewVal = newVal;
      object_->SetAddress(sNewVal);
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::Delete()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!object_)
         return S_FALSE;
   
      parent_collection_->DeleteItemByDBID(object_->GetID());
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      if (!object_)
         return S_FALSE;
   
      HM::String sErrorMessage;
      if (HM::PersistentDistributionListRecipient::SaveObject(object_, sErrorMessage, HM::PersistenceModeNormal))
      {
         AddToParentCollection();
   
         return S_OK;
      }
   
      return COMError::GenerateError(sErrorMessage);
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

// ---- the list manager's view of the member (Util/ListManager.h) ---------------

STDMETHODIMP InterfaceDistributionListRecipient::get_Delivery(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetDelivery();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::put_Delivery(long newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      // Refused at the assignment, in the sentence the save would give, so a
      // script learns of the value where it wrote it.
      if (newVal < 0 || newVal > 2)
         return COMError::GenerateError("Delivery must be 0 (each message), 1 (the digest) or 2 (none).");

      object_->SetDelivery((int) newVal);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::get_HardBounces(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetHardBounces();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::get_SoftBounces(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetSoftBounces();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::get_Confirmed(long *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = (long) object_->GetConfirmed();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceDistributionListRecipient::ResetBounces()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // What the owner's reset does on the Control Deck (ListManager::
      // ResetBounces), on this object and in the database at once: the counts
      // and the reason go, and a member the bounces stopped - the reason says
      // so - is sent each message again. A member who paused it by their own
      // choice stays paused.
      if (object_->GetID() == 0)
      {
         const bool stoppedByBounces = !object_->GetReason().IsEmpty();

         object_->SetHardBounces(0);
         object_->SetSoftBounces(0);
         object_->SetSoftSince(0);
         object_->SetReason(HM::String());

         if (stoppedByBounces && object_->GetDelivery() == HM::DistributionListRecipient::DeliveryNone)
            object_->SetDelivery(HM::DistributionListRecipient::DeliveryEach);

         return S_OK;
      }

      HM::String reason;
      if (!HM::ListManager::ResetBounces(object_, reason))
         return COMError::GenerateError(reason);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
