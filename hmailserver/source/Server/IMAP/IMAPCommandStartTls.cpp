// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "IMAPCommandStartTls.h"
#include "IMAPConnection.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPCommandStartTls::IMAPCommandStartTls()
   {

   }

   IMAPCommandStartTls::~IMAPCommandStartTls()
   {

   }

   IMAPResult
   IMAPCommandStartTls::ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument)
   {
      // RFC 4978 section 3: TLS is not started under compression.
      if (pConnection->IsCompressed())
         return IMAPResult(IMAPResult::ResultBad, "STARTTLS is not available once COMPRESS is active");

      // RFC 9051 and RFC 3501 6.2.1: STARTTLS is a command of the not
      // authenticated state, and a client must not start TLS inside a TLS
      // session. Both used to be answered "OK Begin TLS negotiation now" and a
      // handshake started - on a TLS session already running in the second case,
      // where what followed was left to OpenSSL. Refused as SMTP's STARTTLS
      // (SMTPConnection::ProtocolSTARTTLS_) and POP3's STLS already refuse it,
      // and not advertised then either (IMAPCommandCapability).
      if (pConnection->IsSSLConnection())
         return IMAPResult(IMAPResult::ResultBad, "STARTTLS is not allowed: TLS is already active");

      if (pConnection->IsAuthenticated())
         return IMAPResult(IMAPResult::ResultBad, "STARTTLS is only allowed before authentication");

      if (pConnection->GetConnectionSecurity() == CSSTARTTLSOptional ||
         pConnection->GetConnectionSecurity() == CSSTARTTLSRequired)
      {
         pConnection->SendAsciiData(pArgument->Tag() + " OK Begin TLS negotiation now\r\n");

         pConnection->StartHandshake();

         return IMAPResult(IMAPResult::ResultOKSupressRead, "");
      }
      else
      {
         return IMAPResult(IMAPResult::ResultBad, "Unknown or NULL command");
      }
      
   }
}
