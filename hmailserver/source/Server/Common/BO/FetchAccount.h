// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../TCPIP/SocketConstants.h"

namespace HM
{

   class FetchAccountUIDs;

   class FetchAccount : public BusinessObject<FetchAccount>
   {
   public:

      enum ServerType
      {
         POP3 = 0,
         // The remote INBOX collected once by UID, and left intact unless
         // DaysToKeepMessages says otherwise. See IMAPClientConnection.
         IMAP = 1,
      };


      FetchAccount(void);
      ~FetchAccount(void);

      __int64 GetAccountID() const {return account_id_; }
      void SetAccountID(__int64 iNewVal) {account_id_ = iNewVal; }

      String GetName() const {return name_;}
      void SetName(const String &sNewVal) {name_ = sNewVal; }

      void SetServerAddress(const String &sNewVal) {server_address_ = sNewVal; }
      String GetServerAddress() const {return server_address_; }

      ServerType GetServerType() const {return server_type_; }
      void SetServerType(ServerType iValue) {server_type_ = iValue; }

      int GetPort() const {return port_;}
      void SetPort(int iNewVal) {port_ = iNewVal; }

      String GetUsername() const {return username_; }
      void SetUsername(const String &sValue) {username_ = sValue; }

      String GetPassword() const {return password_; }
      void SetPassword(const String &Value) {password_ = Value; }

      // True when the username and the password are one line each, with no NUL:
      // both go into a command line to the remote server (POP3 USER and PASS,
      // IMAP LOGIN), where a CR or LF would end the command and start another
      // of whoever set them. Every place that saves one asks it, and the
      // fetchers ask it again before sending, for a row saved before.
      bool CredentialsAreOneLine() const;
      
      int GetMinutesBetweenTry() const {return minutes_; }
      void SetMinutesBetweenTry(int iMinutes) {minutes_ = iMinutes; }

      // DaysToKeepMessages has one meaning, whichever fetcher reads it and whatever
      // writes it - the meaning the COM API, the old Administrator and WebAdmin gave
      // it from hMailServer 5 on, and the one the POP3 fetcher has always kept:
      //   negative (-1)  delete the message from the remote server as soon as it
      //                  has been collected;
      //   0              never delete it: every message stays on the remote server;
      //   N > 0          delete it once it has been on record here for more than
      //                  N days.
      // The OnExternalAccountDownload event's answers map onto the same three (1 is
      // -1, 2 is its parameter, 3 is 0; ExternalFetchClientBase::GetDaysToKeep_).
      // From 6.2.25 to 6.3.5 the IMAP fetcher read 0 and a negative number the other
      // way round; schema 6106 translated what its accounts had stored, so that each
      // kept doing what it did (Upgrade6105to6106*.sql). Both fetchers ask these two,
      // so the meaning cannot part again.
      int GetDaysToKeep() const {return days_to_keep_; }
      void SetDaysToKeep(int iDays) {days_to_keep_ = iDays; }

      // True when a message is to be deleted on the remote server in the session
      // that collects it.
      static bool DeletesOnCollection(int daysToKeep) { return daysToKeep < 0; }

      // True when a message on record for daysOnRecord days is due to be deleted on
      // the remote server.
      static bool RemoteCopyDue(int daysToKeep, double daysOnRecord)
      {
         if (daysToKeep < 0)
            return true;

         if (daysToKeep == 0)
            return false;

         return daysOnRecord > daysToKeep;
      }

      bool GetActive() const {return is_active_; }
      void SetActive(bool bActive) {is_active_ = bActive; }

      String GetMIMERecipientHeaders() const { return mime_recipient_headers_; }
      void SetMIMERecipientHeaders(const String &sNewVal) { mime_recipient_headers_ = sNewVal; }

      bool GetProcessMIMERecipients() const {return process_mimerecipients_; }
      void SetProcessMIMERecipients(bool bNewVal) {process_mimerecipients_ = bNewVal; }

      bool GetProcessMIMEDate() const {return process_mimedate_; }
      void SetProcessMIMEDate(bool bNewVal) {process_mimedate_ = bNewVal; }

      ConnectionSecurity GetConnectionSecurity() const  {return connection_security_; }
      void SetConnectionSecurity(ConnectionSecurity connection_security) {connection_security_ = connection_security; }

      String GetNextTry() const {return next_try_;}
      void SetNextTry(const String &sNextTry) {next_try_ = sNextTry;}

      std::shared_ptr<FetchAccountUIDs> GetUIDs();

      bool GetUseAntiSpam() const {return use_anti_spam_; }
      void SetUseAntiSpam(bool bNewVal) {use_anti_spam_ = bNewVal; }

      bool GetUseAntiVirus() const {return use_anti_virus_; }
      void SetUseAntiVirus(bool bNewVal) {use_anti_virus_ = bNewVal; }

      bool GetEnableRouteRecipients() const {return enable_route_recipients_;}
      void SetEnableRouteRecipients(bool enable) {enable_route_recipients_ = enable;}

      // ServerType IMAP only. Every folder the remote server lists is collected into
      // a local folder of the same name - the message verbatim, with its flags and its
      // internal date, filed straight into the folder rather than delivered - which is
      // a migration, not a collection. Off, the INBOX alone is collected and delivered.
      // See IMAPClientConnection.
      bool GetMirrorFolders() const {return mirror_folders_; }
      void SetMirrorFolders(bool bNewVal) {mirror_folders_ = bNewVal; }

      // Signed in with a provider rather than a password (ReaderOAuth.h): the
      // provider's word - google, microsoft - and the refresh token its grant
      // gave, stored protected as the password is. Empty for a password account.
      String GetOAuthProvider() const {return oauth_provider_; }
      void SetOAuthProvider(const String &value) {oauth_provider_ = value; }
      String GetOAuthToken() const {return oauth_token_; }
      void SetOAuthToken(const String &value) {oauth_token_ = value; }

      // The mirror's folder choice, one remote name per line as LIST gives it:
      // the folders not to bring over, or "*" for "list them for the reader and
      // bring nothing until they have chosen"; and the folders the other server
      // listed the last time it was asked, which is what the reader chooses from.
      String GetMirrorSkip() const {return mirror_skip_; }
      void SetMirrorSkip(const String &value) {mirror_skip_ = value; }
      String GetMirrorSeen() const {return mirror_seen_; }
      void SetMirrorSeen(const String &value) {mirror_seen_ = value; }

      // Whose word the server and the username are (hm_fetchaccounts.faorigin,
      // schema 6104; RestApiFetchAccounts.cpp and InterfaceFetchAccount::Save
      // set it):
      //
      //   OriginServerAdministrator - a server administrator's, or a row saved
      //      before 6104. As it always was.
      //   OriginReader - a reader made it in the webmail, or moved it to another
      //      server or port there. ExternalFetch connects only to the public
      //      addresses the name resolves to when it connects, unless
      //      ReaderFetchPrivateHosts is on, and the fetchers never give it this
      //      server's own OAuth2 token.
      //   OriginWithoutServerAuthority - somebody without the server's authority
      //      named its server or its username: a domain administrator over COM,
      //      a key restricted to named domains, or a reader who changed only the
      //      username of an administrator's account. The fetchers never give it
      //      this server's own OAuth2 token, which reaches every mailbox the app
      //      registration was granted, as whatever username the account names;
      //      where it connects is not judged, as an administrator's is not.
      //
      // Never lowered: nothing but deleting the account and making it again
      // gives it back to a server administrator. An unknown stored value reads
      // as OriginReader, the strictest.
      enum Origin
      {
         OriginServerAdministrator = 0,
         OriginReader = 1,
         OriginWithoutServerAuthority = 2
      };

      Origin GetOrigin() const {return origin_; }
      void SetOrigin(Origin value) {origin_ = value; }
      static Origin OriginFromNumber(long value);
      bool GetReaderCreated() const {return origin_ == OriginReader; }
      bool GetServerTokenWithheld() const {return origin_ != OriginServerAdministrator; }
      void MarkReaderCreated() {origin_ = OriginReader; }
      void MarkWithoutServerAuthority() { if (origin_ == OriginServerAdministrator) origin_ = OriginWithoutServerAuthority; }
      // The stricter of this copy's origin and the one stored: a COM object read
      // before a reader moved the account does not take the mark away when it is
      // saved (InterfaceFetchAccount::Save).
      void KeepStricterOrigin(Origin stored) { if (stored == OriginReader) origin_ = OriginReader; else if (stored == OriginWithoutServerAuthority) MarkWithoutServerAuthority(); }

      // What a DaysToKeepMessages stored before schema 6106 - by 6.2.25 to 6.3.5 -
      // is in the one meaning above. Upgrade6105to6106*.sql does the same to the
      // database, and XMLLoad to an archive written before 6.3.6:
      //   IMAP: 0 was "delete on collection" and is -1; a negative number was
      //     "keep" and is 0. The account goes on doing what it did.
      //   POP3 made or moved by somebody other than a server administrator (the
      //     webmail, a reader, a restricted key - the surfaces that described -1
      //     as "keep every message", while the fetcher deleted): a negative
      //     number is 0, so the fetcher stops deleting what the person asked it
      //     to leave. Nothing that was being kept starts to be deleted.
      //   Anything else is as it was.
      static int DaysToKeepStoredBefore6106(ServerType serverType, Origin origin, int stored);

      bool XMLStore(XNode *pFetchAccountsNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions);

   private:

      __int64 account_id_;
      String name_;
      String server_address_;
      String next_try_;
      ServerType server_type_;

      long port_;
      String username_;
      String password_;
      int minutes_;
      int days_to_keep_;
      bool is_active_;
      std::shared_ptr<FetchAccountUIDs> uids_;

      String mime_recipient_headers_;
      bool process_mimerecipients_;
      bool process_mimedate_;

      bool use_anti_spam_;
      bool use_anti_virus_;

      bool enable_route_recipients_;
      bool mirror_folders_;
      String oauth_provider_;
      String oauth_token_;
      String mirror_skip_;
      String mirror_seen_;
      Origin origin_;
      ConnectionSecurity connection_security_;
   };

   // DaysToKeepMessages's one meaning (DeletesOnCollection, RemoteCopyDue), what a
   // value stored before schema 6106 means in it (DaysToKeepStoredBefore6106), and
   // an archive written before 6.3.6 read through that rule while one written
   // since is read as it is. No database, configuration or network. ClassTester
   // runs it on Windows, business-object-tests (platform/business_object_tests_main.cpp)
   // on Linux. Throws std::logic_error naming the first case that is wrong.
   class FetchAccountTester
   {
   public:
      void Test();
   };
}