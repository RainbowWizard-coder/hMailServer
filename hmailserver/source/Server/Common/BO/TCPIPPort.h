// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../TCPIP/SocketConstants.h"

namespace HM
{
   class TCPIPPort : public BusinessObject<TCPIPPort>
   {
   public:
      TCPIPPort(void);
      ~TCPIPPort(void);

      String GetName() const;

      SessionType GetProtocol() const  {return port_protocol_; }
      void SetProtocol(SessionType iProtcol) {port_protocol_ = iProtcol;}

      int GetPortNumber() const  {return port_number_; }
      void SetPortNumber(int iPortNumber) {port_number_ = iPortNumber;}

      bool SetAddress(const String &address);
      void SetAddress(const IPAddress &address);
      String GetAddressString() const;
      IPAddress GetAddress() const;

      __int64 GetSSLCertificateID() const  {return sslcertificate_id_; }
      void SetSSLCertificateID(int iSSLCertificateID) {sslcertificate_id_ = iSSLCertificateID;}

      ConnectionSecurity GetConnectionSecurity() const  {return connection_security_; }
      void SetConnectionSecurity(ConnectionSecurity connection_security) {connection_security_ = connection_security;}

      // Mutual TLS for inbound sessions on this port. Off by default: every
      // pre-existing port must keep behaving exactly as it did before the
      // setting existed. See ClientCertificatePolicy in SocketConstants.h.
      ClientCertificatePolicy GetClientCertificatePolicy() const {return client_certificate_policy_; }
      void SetClientCertificatePolicy(ClientCertificatePolicy policy) {client_certificate_policy_ = policy;}

      // Path to a PEM file holding the CA certificate(s) trusted to issue client
      // certificates for this port. Per-port rather than global, for the reason
      // given on ClientCertificatePolicy: different listeners serve different
      // client populations with different issuing CAs.
      String GetClientCertificateCAFile() const {return client_certificate_ca_file_; }
      void SetClientCertificateCAFile(const String &file) {client_certificate_ca_file_ = file;}

      // What an SMTP listener does with AUTH: on, off or a honeypot
      // (SMTPAuthenticationMode in SocketConstants.h, GitLab work item #54). On by
      // default, so that every port made before the setting existed, and every
      // port a create script or SetDefault writes, behaves as it always did; a
      // new installation's port 25 is made a honeypot by NewInstallDefaults.
      // Meaningful on an SMTP port only: PersistentTCPIPPort refuses anything but
      // On on a POP3 or IMAP port.
      SMTPAuthenticationMode GetSMTPAuthentication() const {return smtp_authentication_; }
      void SetSMTPAuthentication(SMTPAuthenticationMode mode) {smtp_authentication_ = mode;}

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems (XNode *pNode, int iOptions) {return true;};

   private:

      int GetSSLCertificateID_(const String &sSSLCertificateName);
      String GetSSLCertificateName_(__int64 iCertificateID);

      SessionType port_protocol_;
      int port_number_;
      int sslcertificate_id_;

      ConnectionSecurity connection_security_;

      ClientCertificatePolicy client_certificate_policy_;
      String client_certificate_ca_file_;

      SMTPAuthenticationMode smtp_authentication_;

      IPAddress address_;
   };
}