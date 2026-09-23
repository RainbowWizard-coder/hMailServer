// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "./COMError.h"
#include "../Common/Application/EnvironmentSettings.h"

#include <exception>

COMError::COMError(void)
{
}

COMError::~COMError(void)
{
}

HRESULT
COMError::GenerateGenericMessage()
{
   // Called from the catch (...) that ends every COM method. One exception has a
   // sentence of its own and is not an error at all: an administrator's change to
   // a setting this server's environment manages, refused at the store every
   // setter writes through (EnvironmentSettings::Refusal) - so every setter
   // refuses it in the same words, with no setter of its own changed. Asked of
   // the exception being handled, if there is one; a call from outside a handler
   // has none and gets the generic sentence as before.
   std::exception_ptr current = std::current_exception();
   if (current)
   {
      try
      {
         std::rethrow_exception(current);
      }
      catch (const HM::EnvironmentSettings::Refusal &refusal)
      {
         return GenerateError(refusal.Sentence());
      }
      catch (...)
      {
      }
   }

   return GenerateError("An error occurred processing the request.");
}

HRESULT
COMError::WithoutMessage(HRESULT result)
{
   // The thread's error info belongs to whatever failed on it last. This failure
   // has none, so it says none: see the comment in COMError.h.
   SetErrorInfo(0, NULL);

   return result;
}

HRESULT
COMError::GenerateError(HM::String sDescription)
{
   if (sDescription == _T(""))
   {
      sDescription = "The operation failed. Please check the hMailServer log for details.";
   }

   // Get the ICreateErrorInfo Interface
   ICreateErrorInfo *pCreateErrorInfo = NULL;
   HRESULT hSuccess = CreateErrorInfo(&pCreateErrorInfo);
   ATLASSERT(SUCCEEDED(hSuccess));
   // pCreateErrorInfo->SetGUID(CLSID_BaseApp);
   pCreateErrorInfo->SetDescription(sDescription.AllocSysString());
   HM::String sSource = "hMailServer COM library";
   pCreateErrorInfo->SetSource(sSource.AllocSysString());

   IErrorInfo *pErrInfo;

   if (SUCCEEDED(pCreateErrorInfo->QueryInterface(IID_IErrorInfo, 
      reinterpret_cast<void**>(&pErrInfo))))
   {
      SetErrorInfo(0, pErrInfo);
      pErrInfo->Release();
   }

   pCreateErrorInfo->Release();

   return MAKE_HRESULT(1, FACILITY_ITF, 1001);
}