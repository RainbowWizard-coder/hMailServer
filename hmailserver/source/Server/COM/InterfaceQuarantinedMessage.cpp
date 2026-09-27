// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceQuarantinedMessage.h"

#include "COMError.h"

#include "../Common/AntiSpam/QuarantineReview.h"

void
InterfaceQuarantinedMessage::Attach(const HM::QuarantinedMessage &message)
{
   message_ = message;
}

STDMETHODIMP InterfaceQuarantinedMessage::get_ID(LONG* pVal)
{
   try
   {
      *pVal = (LONG) message_.id;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Sender(BSTR* pVal)
{
   try
   {
      *pVal = message_.sender.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Recipients(BSTR* pVal)
{
   try
   {
      *pVal = message_.recipients.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Subject(BSTR* pVal)
{
   try
   {
      *pVal = message_.subject.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Reason(BSTR* pVal)
{
   try
   {
      *pVal = message_.reason.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Score(LONG* pVal)
{
   try
   {
      *pVal = (LONG) message_.score;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Size(LONG* pVal)
{
   try
   {
      *pVal = (LONG) message_.size;
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_CreatedTime(BSTR* pVal)
{
   try
   {
      *pVal = message_.created.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::ReleaseMessage()
{
   try
   {
      // Releasing puts a message the server judged to be spam into somebody's
      // mailbox, which is a server-wide decision about somebody else's mail rather
      // than an account-level one - so it is held to the same bar as the rest of the
      // administration surface.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String error;

      if (!HM::QuarantineStore::Release(message_.id, error))
         return COMError::GenerateError(error);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::Delete()
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // There is no other copy: the sender was told 250, so nothing will retry, and
      // the original was not delivered. Deleting here is final, which is why it is an
      // explicit action rather than something the review queue does on its own.
      if (!HM::QuarantineStore::Delete(message_.id))
         return COMError::GenerateError("The quarantined message could not be deleted.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_Helo(BSTR* pVal)
{
   try
   {
      *pVal = message_.helo.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_SenderDomain(BSTR* pVal)
{
   try
   {
      *pVal = HM::QuarantineReview::SenderDomain(message_.sender).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::get_HeloDomain(BSTR* pVal)
{
   try
   {
      *pVal = HM::QuarantineReview::HeloDomain(message_.helo).AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::ReadHeaders(BSTR* pVal)
{
   try
   {
      // The message itself, not the row about it: held to the bar the release
      // is held to.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String headers;
      HM::String error;
      bool truncated = false;

      if (!HM::QuarantineStore::ReadHeaders(message_.id, headers, truncated, error))
         return COMError::GenerateError(error);

      *pVal = headers.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantinedMessage::ReadPreview(BSTR* pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String text;
      HM::String error;
      bool truncated = false;
      bool fromHtml = false;

      if (!HM::QuarantineStore::ReadPreview(message_.id, text, truncated, fromHtml, error))
         return COMError::GenerateError(error);

      *pVal = text.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
