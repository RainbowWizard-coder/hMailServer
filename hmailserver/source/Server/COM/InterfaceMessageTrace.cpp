// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "InterfaceMessageTrace.h"
#include "InterfaceMessageTraceEvent.h"

#include "COMError.h"
#include "../Common/Application/RoleManager.h"
#include "../Common/Util/Parsing/StringParser.h"

STDMETHODIMP InterfaceMessageTrace::Search(BSTR Address)
{
   try
   {
      // A trace search matches sender OR recipient as a substring across the whole
      // table, so it cannot be scoped to a domain by the query - a domain
      // administrator who could run it would see who every other domain's users
      // correspond with. The server administrator runs it as it always has, and so
      // does any role that holds view_message_trace and reaches every domain.
      const HM::String address = Address;

      if (authentication_->GetIsServerAdmin() ||
          (authentication_->Allows(HM::RolePermission::ViewMessageTrace) && authentication_->ReachesEveryDomain()))
      {
         events_ = HM::MessageTrace::Search(address, max_listed_);
         return S_OK;
      }

      // A role confined to named domains - the help desk - traces one whole address
      // in a domain it reaches, and sees the events that name that address exactly:
      // the substring match would also answer for otheruser@ and for
      // user@example.test.elsewhere, and the second of those is another customer.
      // Every address it may not trace gets the same refusal, whatever is behind it.
      const HM::String domainName = HM::StringParser::ExtractDomain(address);

      if (!authentication_->Allows(HM::RolePermission::ViewMessageTrace) ||
          address.Find(_T("@")) <= 0 || domainName.IsEmpty() ||
          !authentication_->AllowsForDomain(HM::RolePermission::ViewMessageTrace, domainName))
      {
         return authentication_->GetAccessDenied();
      }

      events_.clear();

      for (const HM::MessageTraceEvent &traceEvent : HM::MessageTrace::Search(address, max_listed_))
      {
         if (traceEvent.sender.CompareNoCase(address.c_str()) == 0 ||
             traceEvent.recipient.CompareNoCase(address.c_str()) == 0)
         {
            events_.push_back(traceEvent);
         }
      }

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessageTrace::SearchByQueueID(long QueueID)
{
   try
   {
      // One queued message carries recipients in any number of domains: the
      // server administrator's, or a role's that reaches every domain.
      if (!authentication_->GetIsServerAdmin() &&
          !(authentication_->Allows(HM::RolePermission::ViewMessageTrace) && authentication_->ReachesEveryDomain()))
      {
         return authentication_->GetAccessDenied();
      }

      events_ = HM::MessageTrace::GetByQueueID(QueueID);

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessageTrace::get_Count(LONG* pVal)
{
   try
   {
      // Only a search can load events, and every search is gated above.
      if (!authentication_->GetIsServerAdmin() && !authentication_->Allows(HM::RolePermission::ViewMessageTrace))
         return authentication_->GetAccessDenied();

      // The number of events LOADED, unlike the quarantine's Count which reports the
      // whole store. The difference is what each is for: "how much is in quarantine"
      // is a backlog to work through, while a trace is an answer to a question and
      // the useful number is how many lines that answer has.
      *pVal = (LONG) events_.size();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessageTrace::get_Item(long Index, IInterfaceMessageTraceEvent** pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin() && !authentication_->Allows(HM::RolePermission::ViewMessageTrace))
         return authentication_->GetAccessDenied();

      if (Index < 0 || Index >= (long) events_.size())
         return COMError::GenerateError("Index out of range. Call Search or SearchByQueueID before indexing the trace.");

      CComObject<InterfaceMessageTraceEvent>* pItem = new CComObject<InterfaceMessageTraceEvent>();
      pItem->SetAuthentication(authentication_);
      pItem->Attach(events_[Index]);
      pItem->AddRef();

      *pVal = pItem;

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}

STDMETHODIMP InterfaceMessageTrace::DeleteExpired(LONG* pVal)
{
   try
   {
      if (!authentication_->GetIsServerAdmin())
         return authentication_->GetAccessDenied();

      *pVal = (LONG) HM::MessageTrace::DeleteExpired();

      return S_OK;
   }
   catch (...)
   {
      return COMError::GenerateGenericMessage();
   }
}
