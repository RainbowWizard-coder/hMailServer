// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "COMError.h"
#include "InterfaceAttachment.h"

void 
InterfaceAttachment::Attach(std::shared_ptr<HM::Attachment> pAttachment) 
{ 

   attachment_ = pAttachment; 
}

STDMETHODIMP InterfaceAttachment::get_Filename(BSTR *pVal)
{
   try
   {
      if (!attachment_)
         return GetAccessDenied();

      *pVal = attachment_->GetFileName().AllocSysString();
   
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAttachment::get_Size(long *pVal)
{
   try
   {
      if (!attachment_)
         return GetAccessDenied();

      *pVal = attachment_->GetSize();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceAttachment::SaveAs(BSTR sName)
{
   try
   {
      if (!attachment_)
         return GetAccessDenied();

      // The name is a path on the server, and the file is written as the
      // service account - over any file it may write, which on a LocalSystem
      // service is any file of the machine. Until 6.3.6 any holder of an
      // attachment could do it; now the server administrator and the server's
      // own event scripts can (COMAuthentication::CallerIsEventScript).
      if (!GetIsServerAdmin() && !HM::COMAuthentication::CallerIsEventScript())
         return COMError::GenerateError(L"You do not have access to this property / method. Writing a file to the "
                                        L"server's disk is for the server administrator and the server's event scripts.");

      attachment_->SaveAs(sName);
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP 
InterfaceAttachment::Delete()
{
   try
   {
      if (!attachment_)
         return GetAccessDenied();

      attachment_->Delete();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}


