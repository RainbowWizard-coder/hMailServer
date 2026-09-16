// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "./COMError.h"

COMError::COMError(void)
{
}

COMError::~COMError(void)
{
}

HRESULT
COMError::GenerateGenericMessage()
{
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