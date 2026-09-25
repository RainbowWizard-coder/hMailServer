// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "../COM/InterfaceServerMessage.h"

#include "../Common/Persistence/PersistentServerMessage.h"
#include "../Common/BO/ServerMessage.h"

#include "COMError.h"
#include "../Common/Util/AuditTrail.h"

STDMETHODIMP 
InterfaceServerMessage::Save()
{
   try
   {
      HM::AuditScope auditScope(GetAuditActor());

      if (!object_)
         return GetAccessDenied();

      // Onto a copy even when nothing was put before it: the running item is
      // never what the persister is handed, and neither is one a refresh has
      // replaced since this object was given it (COMCollection.h).
      Writable_();

      if (HM::PersistentServerMessage::SaveObject(object_))
      {
         // A new item joins the running collection; a changed one - this
         // object's copy - reaches it by the collection being read again
         // (COMCollection.h).
         PublishSaved_("InterfaceServerMessage::Save");
   
         return S_OK;
      }
   
      return COMError::GenerateError("Failed to save object. See hMailServer error log.");
      
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceServerMessage::get_ID(long *pVal)
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

STDMETHODIMP InterfaceServerMessage::put_Name(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetName(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceServerMessage::get_Name(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceServerMessage::put_Text(BSTR newVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      Writable_()->SetText(newVal);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceServerMessage::get_Text(BSTR *pVal)
{
   try
   {
      if (!object_)
         return GetAccessDenied();

      *pVal = object_->GetText().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

