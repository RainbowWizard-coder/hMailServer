// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "TCPIPPort.h"
#include "SSLCertificates.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   TCPIPPort::TCPIPPort(void) :
      port_number_(0),
      port_protocol_(STUnknown),
      sslcertificate_id_(0),
      connection_security_(CSNone),
      client_certificate_policy_(CCPOff),
      smtp_authentication_(SAMOn)
   {

   }

   TCPIPPort::~TCPIPPort(void)
   {
   }

   String 
   TCPIPPort::GetName() const
   {
      String sName;
      sName.Format(_T("%d-%d"), port_protocol_, port_number_);

      return sName;
   }

   bool 
   TCPIPPort::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("TCPIPPort"));

      pNode->AppendAttr(_T("Name"), GetName());
      pNode->AppendAttr(_T("PortProtocol"), StringParser::IntToString(port_protocol_));
      pNode->AppendAttr(_T("PortNumber"), StringParser::IntToString(port_number_));
      pNode->AppendAttr(_T("ConnectionSecurity"), StringParser::IntToString(connection_security_));
      pNode->AppendAttr(_T("ClientCertificatePolicy"), StringParser::IntToString(client_certificate_policy_));
      pNode->AppendAttr(_T("ClientCertificateCAFile"), client_certificate_ca_file_);
      pNode->AppendAttr(_T("SMTPAuthentication"), StringParser::IntToString(smtp_authentication_));
      pNode->AppendAttr(_T("Address"), String(address_.ToString()));
      
      if (sslcertificate_id_ > 0)
      {
         pNode->AppendAttr(_T("SSLCertificateName"), GetSSLCertificateName_(sslcertificate_id_));
      }

      return true;
   }

   bool 
   TCPIPPort::XMLLoad(XNode *pNode, int iOptions)
   {
      port_protocol_ = (SessionType) _ttoi(pNode->GetAttrValue(_T("PortProtocol")));
      port_number_ = _ttoi(pNode->GetAttrValue(_T("PortNumber")));

      address_.TryParse(pNode->GetAttrValue(_T("Address")));
      sslcertificate_id_  = GetSSLCertificateID_(pNode->GetAttrValue(_T("SSLCertificateName")));

      // A backup taken before these attributes existed simply lacks them, and
      // GetAttrValue then returns an empty string. _ttoi("") is 0, which is CCPOff -
      // so restoring an old backup lands every port on today's behaviour rather
      // than on an accidental "request" or "require".
      client_certificate_policy_ = (ClientCertificatePolicy) _ttoi(pNode->GetAttrValue(_T("ClientCertificatePolicy")));
      client_certificate_ca_file_ = pNode->GetAttrValue(_T("ClientCertificateCAFile"));

      // The same for the listener's AUTH mode (GitLab work item #54): a backup
      // taken before 6.3.6 has no attribute, and _ttoi("") is 0, On - what every
      // listener did then. A value this version does not know is On as well, and
      // so is anything on a port that is not SMTP, which has no other mode.
      const int smtpAuthentication = _ttoi(pNode->GetAttrValue(_T("SMTPAuthentication")));
      smtp_authentication_ = port_protocol_ == STSMTP && (smtpAuthentication == SAMOff || smtpAuthentication == SAMHoneypot)
         ? (SMTPAuthenticationMode) smtpAuthentication : SAMOn;

      // Backwards compatibiltiy
      if (pNode->GetAttrValue(_T("UseSSL")) == _T("1"))
      {
         connection_security_ = CSSSL;
      }
      else
      {
         connection_security_ = (ConnectionSecurity) _ttoi(pNode->GetAttrValue(_T("ConnectionSecurity")));
      }

      return true;
   }

   int
   TCPIPPort::GetSSLCertificateID_(const String &sSSLCertificateName)
   {
      if (sSSLCertificateName.IsEmpty())
         return 0;

      std::shared_ptr<SSLCertificates> pSSLCertificates = std::shared_ptr<SSLCertificates>(new SSLCertificates);
      pSSLCertificates->Refresh();

      std::shared_ptr<SSLCertificate> pCertificate = pSSLCertificates->GetItemByName(sSSLCertificateName);
      if (pCertificate)
         return (int) pCertificate->GetID();
      else
         return 0;

   }

   String 
   TCPIPPort::GetSSLCertificateName_(__int64 iCertificateID)
   {
      std::shared_ptr<SSLCertificates> pSSLCertificates = std::shared_ptr<SSLCertificates>(new SSLCertificates);
      pSSLCertificates->Refresh();

      std::shared_ptr<SSLCertificate> pCertificate = pSSLCertificates->GetItemByDBID(iCertificateID);
      if (pCertificate)
         return pCertificate->GetName();
      else
         return "";

   }

   bool
   TCPIPPort::SetAddress(const String &address)
   {
      IPAddress listeningAddress;
      if (!listeningAddress.TryParse(address))
         return false;
      
      SetAddress(listeningAddress);
      return true;
   }

   void 
   TCPIPPort::SetAddress(const IPAddress &address)
   {
      address_ = address;
   }

   String
   TCPIPPort::GetAddressString() const
   {
      return address_.ToString();
   }

   IPAddress
   TCPIPPort::GetAddress() const
   {
      return address_;
   }
}