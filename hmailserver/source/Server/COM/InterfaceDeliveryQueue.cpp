// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "InterfaceDeliveryQueue.h"
#include "../SMTP/DeliveryQueue.h"
#include "../Common/AntiVirus/Sandbox.h"
#include "../Common/Util/AuditTrail.h"
#include "COMError.h"
void
InterfaceDeliveryQueue::SetAuthentication(std::shared_ptr<HM::COMAuthentication> pAuthentication)
{
   authentication_ = pAuthentication;
}

STDMETHODIMP 
InterfaceDeliveryQueue::Clear()
{
   try
   {
      if (!authentication_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
      {
         return COMError::GenerateError("Server admin privileges are required to clear queue.");
      }
   
      HM::DeliveryQueue::Clear();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceDeliveryQueue::ResetDeliveryTime(hyper iMessageID)
{
   try
   {
      if (!authentication_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return S_FALSE;
   
      HM::DeliveryQueue::ResetDeliveryTime(iMessageID);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceDeliveryQueue::Remove(hyper iMessageID)
{
   try
   {
      if (!authentication_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return S_FALSE;

      // A message held for a sandbox's verdict is removed as the sandbox's own
      // discard removes it - the row, the file and the hold's records - and
      // recorded on the audit trail as that discard is, under the actor
      // GetIsServerAdmin installed: hm_sandboxholds is traffic and the queue
      // row's delete is not an audited statement, so nothing else would say
      // that a message the server was waiting to judge was destroyed.
      HM::Sandbox::Held held;
      if (HM::Sandbox::ReadHeld(iMessageID, held))
      {
         HM::String problem;
         if (!HM::Sandbox::Discard(iMessageID, problem))
            return COMError::GenerateError(HM::String(_T("The message held for the sandbox's verdict could not be removed: ")) + problem);

         HM::AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), HM::StringParser::IntToString(held.message_id), _T("discarded"),
            HM::Sandbox::Describe(held) + _T(" (DeliveryQueue.Remove)"));
         return S_OK;
      }
   
      HM::DeliveryQueue::Remove(iMessageID);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceDeliveryQueue::StartDelivery()
{
   try
   {
      if (!authentication_)
         return GetAccessDenied();

      if (!authentication_->GetIsServerAdmin())
         return S_FALSE;
   
      HM::DeliveryQueue::StartDelivery();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

