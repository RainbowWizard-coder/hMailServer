// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Every client that has signed in to an account, and the one press that ends it.
//
// WHAT A DEVICE IS HERE. One row of hm_signins per account and per client, where
// a client is the protocol it speaks, the address it came from and the user agent
// it names - a phone fetching mail over IMAP is one row, the same phone sending
// over SMTP is another, and the same person's browser is a third. That is the
// identity the roadmap row asks for ("by protocol, address and last use"), and it
// is the only one this server can honestly offer: no mail protocol carries a
// device identifier, so anything more specific would be invented rather than
// observed. The row records when the client was first and last seen, how many
// times it has signed in, and WHICH CREDENTIAL it used - the account's password,
// one of its app passwords, an OAuth2 token, a client certificate or single
// sign-on - because "which of my credentials is that thing using" is the question
// a person actually has when they see a client they do not recognise.
//
// The key is a hash and not the three fields concatenated, so that the unique
// index is a fixed 32 characters whatever a user agent contains, and so that an
// id quoted in a URL or a log line reveals nothing about where its owner was.
//
// WHAT REVOKING DOES, exactly. It ends every session the device holds now:
//
//    * a browser session (the portal, the webmail, the Control Deck) has its
//      token invalidated, so the next request on it is refused with 401;
//    * a live IMAP, POP3 or SMTP connection is dropped - EnqueueDisconnect on
//      the connection itself, which is why the registry below holds a weak
//      pointer to each authenticated connection;
//    * and, when the caller asks for it, the app password the device signed in
//      with is deactivated, which is what stops a stolen phone simply signing
//      in again.
//
// It is NOT a ban, and the row says so rather than pretending otherwise: the
// entry stays, marked revoked and with the time, and a later sign-in from that
// client with a credential that still works clears the mark and starts the
// record again. A device that must never come back is one whose credential has
// been taken away, which is the third bullet - so the interfaces offer it beside
// the revoke and the documents say to use it.
//
// WHAT IT COSTS. A sign-in is a read and at most one write, and the write is
// throttled to once a minute per client, so an IMAP client that reconnects every
// thirty seconds costs one UPDATE a minute rather than two a minute for ever.
// The first sign-in of a client is always written. The rows of one account are
// capped (MaxDevicesPerAccount): the least recently seen goes when the cap is
// reached, so a client whose address changes on every connection cannot grow the
// table without bound.
//
// WHAT IT DOES NOT COVER. A connection a trusted upstream vouches for over
// XCLIENT LOGIN=, which authenticates nobody on this server and carries no
// account object; and anything that never authenticates at all.

#pragma once

#include "../TCPIP/IPAddress.h"

namespace HM
{
   class Account;
   class DALRecordset;
   class TCPConnection;

   struct SignInRecord
   {
      SignInRecord()
         : id(0), account_id(0), protocol(0), credential(0), count(0), revoked(false)
      {
      }

      __int64 id;
      __int64 account_id;

      // The device key: 32 lower-case hex characters. This is the id every
      // interface names a device by.
      String device;

      int protocol;
      String address;
      String agent;
      int credential;

      // The app password's name, where the sign-in used one; empty otherwise.
      // Never the password, and never anything derived from it.
      String credential_name;

      String first_seen;
      String last_seen;
      int count;
      bool revoked;
      String revoked_time;
   };

   class SignInStore
   {
   public:
      enum Protocol
      {
         ProtocolUnknown = 0,
         ProtocolSmtp = 1,
         ProtocolPop3 = 2,
         ProtocolImap = 3,
         ProtocolWeb = 4,
         ProtocolDav = 5,
         ProtocolManageSieve = 6,
         // Outlook's Autodiscover answer, when it was asked with the account's
         // credentials for the mailboxes delegated to it. Its own entry, not
         // dav, so that an Outlook user agent in the list is not read as a
         // calendar client.
         ProtocolAutodiscover = 7
      };

      enum Credential
      {
         CredentialPassword = 0,
         CredentialAppPassword = 1,
         CredentialToken = 2,
         CredentialCertificate = 3,
         CredentialSingleSignOn = 4,
         // A WebAuthn credential (PasskeyStore.h): the person proved a device
         // they hold, and the authenticator verified them on it.
         CredentialPasskey = 5
      };

      // The most clients one account's list holds. A cost bound, not a tidiness
      // rule: every entry is one row and one line in a page the account holder
      // reads, and a client behind a carrier-grade NAT can present a new address
      // on every connection.
      static const int MaxDevicesPerAccount = 50;

      // The device key for a client, without touching the database. Pure: the
      // same three values always give the same key, on every node of a cluster.
      static String DeviceKey(int protocol, const String &address, const String &agent);

      // Records that this client has just signed in, and answers its device key
      // (empty when nothing could be recorded - no account, or the write failed,
      // which is reported but never fails the sign-in: refusing a mail client
      // because a record of it could not be kept would be a far worse outcome
      // than an incomplete list).
      static String Record(std::shared_ptr<const Account> account, int protocol, const IPAddress &address,
                           const String &agent, int credential, const String &credentialName);

      // The same, for a caller that already has the address as text (the web and
      // DAV paths, which take it from the request).
      static String RecordFrom(std::shared_ptr<const Account> account, int protocol, const String &address,
                               const String &agent, int credential, const String &credentialName);

      // Registers a live connection against a device, so that revoking the device
      // can drop it. Held as a weak pointer: this never keeps a connection alive,
      // and an entry whose connection has gone is swept the next time anything
      // touches the registry.
      static void Attach(__int64 accountId, const String &device, std::shared_ptr<TCPConnection> connection);

      // Everything one account has signed in from, most recently seen first.
      static std::vector<SignInRecord> List(__int64 accountId);

      static bool Get(__int64 accountId, const String &device, SignInRecord &out_record);

      // Ends every session the device holds. revokeCredential also deactivates
      // the app password it signed in with, where it used one. Answers false only
      // when there is no such device of that account.
      static bool Revoke(__int64 accountId, const String &device, bool revokeCredential,
                         int &out_connectionsDropped, int &out_sessionsEnded, bool &out_credentialDisabled);

      // Forgets one device - the row goes, and with it the record that it was
      // ever seen. Ends its sessions first, so that forgetting is never weaker
      // than revoking. False when there is no such device of that account.
      static bool Forget(__int64 accountId, const String &device, int &out_connectionsDropped, int &out_sessionsEnded);

      // The words the interfaces use, so that REST, COM, the Deck and the webmail
      // all name a protocol and a credential the same way.
      static AnsiString ProtocolName(int protocol);
      static AnsiString CredentialName(int credential);

      // One record as a JSON object, and a whole account's as an array. Shared by
      // the REST reads and the COM method so that the two documents cannot drift.
      // extra, where it is given, is put inside the object before its closing
      // brace - the one thing a listing knows that a record does not, which is
      // whether the reader is looking at the client they are asking from.
      static AnsiString ToJson(const SignInRecord &record, const AnsiString &extra = AnsiString());
      static AnsiString ToJson(const std::vector<SignInRecord> &records);

      // Drops every live connection registered for a device, and answers how many
      // went.
      static int DropConnections(__int64 accountId, const String &device);

      // Everything the registry holds for an account, dropped: what a password
      // change does. Answers how many connections went.
      static int DropConnectionsForAccount(__int64 accountId);

   private:
      static bool ReadRow_(std::shared_ptr<DALRecordset> recordset, SignInRecord &out_record);
      static void PruneAccount_(__int64 accountId);
      static bool RecentlyWritten_(__int64 accountId, const String &device);
      static void MarkWritten_(__int64 accountId, const String &device);
      static void ForgetWritten_(__int64 accountId, const String &device);
      static bool DisableAppPassword_(__int64 accountId, const String &name);
      static String WriteKey_(__int64 accountId, const String &device);
   };
}
