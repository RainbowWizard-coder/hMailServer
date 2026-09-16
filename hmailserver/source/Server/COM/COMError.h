// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

class COMError
{
public:
   COMError(void);
   ~COMError(void);

   static HRESULT GenerateGenericMessage();
   static HRESULT GenerateError(HM::String sDescription);

   // A FAILURE THAT CARRIES NO SENTENCE OF ITS OWN.
   //
   // GenerateError calls SetErrorInfo, which puts an IErrorInfo on the THREAD,
   // and nothing ever takes it off: GetErrorInfo removes it, and the CLR calls
   // that only when the object answers S_OK for ISupportErrorInfo. So an
   // interface that did not declare ISupportErrorInfo left its refusal's
   // description lying on the thread - which is the state seventy-two classes
   // were in until 16 September 2026, and the state a class is still in for an
   // interface its InterfaceSupportsErrorInfo does not name.
   //
   // A method that then fails with a bare HRESULT - DISP_E_BADINDEX for an index
   // nothing is at, E_POINTER for a null out-parameter - sets no error info of
   // its own, so the caller asking for the rich error of THIS failure is handed
   // the description of THAT one: "there is no account by that name" for an
   // index out of range, on an entirely different object. Now that nearly every
   // class here declares ISupportErrorInfo, nearly every class here can be the
   // one that reads it.
   //
   // So a bare failure says so, by clearing the thread. The caller is then given
   // the HRESULT's own meaning - "Invalid index." for DISP_E_BADINDEX - which is
   // what the failure means. The HRESULT is returned unchanged, because callers
   // and tests check for the code: return COMError::WithoutMessage(DISP_E_BADINDEX).
   //
   // build/check-com-error-info.py refuses a bare failure return that does not
   // come through here.
   static HRESULT WithoutMessage(HRESULT result);
};
