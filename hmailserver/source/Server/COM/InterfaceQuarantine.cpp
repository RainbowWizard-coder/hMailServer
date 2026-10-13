// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceQuarantine.h"
#include "InterfaceQuarantinedMessage.h"

#include "COMError.h"

#include "../Common/AntiSpam/QuarantineReview.h"
#include "../Common/Util/Unicode.h"

STDMETHODIMP InterfaceQuarantine::Refresh()
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      messages_ = HM::QuarantineStore::List(max_listed_);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::get_Count(LONG* pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // The TOTAL held, deliberately, and not the size of the page loaded by Refresh.
      // "How much is in quarantine" is the question this answers, and a count that
      // silently stopped at 500 would tell an administrator the backlog had levelled
      // off exactly when it had not.
      *pVal = (LONG) HM::QuarantineStore::GetCount();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::get_Item(long Index, IInterfaceQuarantinedMessage** pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (Index < 0 || Index >= (long) messages_.size())
         return COMError::GenerateError("Index out of range. Call Refresh before indexing the quarantine.");

      CComObject<InterfaceQuarantinedMessage>* pItem = new CComObject<InterfaceQuarantinedMessage>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(messages_[Index]);
      pItem->AddRef();

      *pVal = pItem;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::get_ItemByDBID(long DBID, IInterfaceQuarantinedMessage** pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      // Read straight from the store rather than searched in the loaded page: an
      // identifier the caller already holds should not depend on whether Refresh
      // happened to include that row.
      HM::QuarantinedMessage message;

      if (!HM::QuarantineStore::GetById(DBID, message))
         return COMError::GenerateError("No quarantined message with that identifier.");

      CComObject<InterfaceQuarantinedMessage>* pItem = new CComObject<InterfaceQuarantinedMessage>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(message);
      pItem->AddRef();

      *pVal = pItem;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::ReleaseByDBID(long DBID)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String error;

      if (!HM::QuarantineStore::Release(DBID, error))
         return COMError::GenerateError(error);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::DeleteByDBID(long DBID)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      if (!HM::QuarantineStore::Delete(DBID))
         return COMError::GenerateError("The quarantined message could not be deleted.");

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::DeleteExpired(LONG* pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      *pVal = (LONG) HM::QuarantineStore::DeleteExpired();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::Bulk(BSTR Request, BSTR* Result)
{
   try
   {
      // Releasing, deleting and writing the server's own lists: each of them
      // is the server administrator's, so the whole of it is.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::AnsiString body;
      HM::Unicode::WideToMultiByte(HM::String(Request), body);

      HM::QuarantineReview::Request request;
      HM::String error;

      if (!HM::QuarantineReview::ParseRequest(body, request, error))
         return COMError::GenerateError(error);

      HM::AnsiString answer;
      const HM::QuarantineReview::Outcome outcome = HM::QuarantineReview::Run(request, answer, error);

      if (outcome != HM::QuarantineReview::OutcomeDone)
         return COMError::GenerateError(error);

      HM::String text;
      HM::Unicode::MultiByteToWide(answer, text);

      *Result = text.AllocSysString();
      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceQuarantine::Search(BSTR Text, long Offset, long Count, long* pMatches)
{
   try
   {
      // Every held message, where Refresh reads the newest page of them (review
      // finding F261): the Control Panel's search found nothing it had not
      // already listed.
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      HM::String text = Text ? HM::String(Text) : HM::String();
      text.Trim();

      // Refused rather than cut or clamped: a search cut short finds what was not
      // asked for, and a page that is quietly not the one asked for is read as
      // the end of the list.
      if (text.GetLength() > HM::QuarantineStore::MaxSearchText)
         return COMError::GenerateError("The search text is longer than 255 characters.");

      if (Offset < 0 || Offset > HM::QuarantineStore::MaxSearchOffset)
         return COMError::GenerateError("Offset is a whole number from 0 to 999999999.");

      if (Count < 1 || Count > max_listed_)
         return COMError::GenerateError("Count is a whole number from 1 to 500.");

      HM::QuarantineStore::SearchRequest request;
      request.text = text;
      request.offset = (int) Offset;
      request.count = (int) Count;

      std::vector<HM::QuarantinedMessage> page;
      int matches = 0;

      if (!HM::QuarantineStore::Search(request, page, matches))
         return COMError::GenerateError("The quarantine could not be searched.");

      messages_ = page;
      *pMatches = (long) matches;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
