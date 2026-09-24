// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The IMAP client a migration job reads its source with
// (Application/MigrationManager.h).
//
// WHY NOT THE FETCHER'S. ExternalFetcher/IMAPClientConnection is an
// asynchronous state machine on the server's io_context, owned by one fetch
// account and ending every message in the fetcher's delivery path or its mirror
// filing; a job needs things that machine has no place for - a pause or a cancel
// honoured between any two commands, a sleep between fetches that throttles the
// source, a resume mark written every fifty messages, a UID range above the
// mark rather than "what is not on record", EXAMINE rather than SELECT, the
// keywords and the special-use attributes kept. So a job runs on a thread of its
// own and speaks to its source through this: one command at a time, each
// bounded by a deadline, abandoned within a quarter of a second of the job
// being told to stop. What the two share is what IMAP is - the LIST reply read
// as the fetcher reads it (the name quoted or atom, the delimiter or NIL, the
// attributes), XOAUTH2 as the same SASL initial response, the same certificate
// check - and the message filed through MailImporter, as the Import Tool files.
//
// Every read and write runs as an asynchronous operation waited on in slices,
// as HttpsClient's do, for the reasons written there: a synchronous read has no
// deadline that holds on every platform. A literal the reply announces is read
// exactly - the message body straight to the file the job names, any other
// literal (a mailbox name sent as one) into the reply's text as a quoted
// string - so a reply is always read to its end and the next command starts on
// a clean line.

#pragma once

#include "../TCPIP/SocketConstants.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace HM
{
   class MigrationImapClient
   {
   public:
      // A mailbox as LIST named it.
      struct Mailbox
      {
         Mailbox() : selectable(true) { }

         AnsiString name;        // as the server sent it: modified UTF-7, unquoted
         AnsiString delimiter;   // empty for NIL
         AnsiString attributes;  // what is inside the parentheses, as sent
         bool selectable;
      };

      // What a FETCH said about one message.
      struct Fetched
      {
         Fetched() : uid(0), bytes(0), body(false), tooLarge(false) { }

         unsigned int uid;
         AnsiString flags;          // inside FLAGS (...), as sent
         AnsiString internalDate;   // the INTERNALDATE string, unquoted
         __int64 bytes;             // the octets written to the file
         bool body;                 // a BODY[] literal arrived and was written
         bool tooLarge;             // one arrived, larger than allowed, and was read past
      };

      // stopRequested is asked between the slices of every wait: true abandons
      // the command, closes the connection and answers false with stopped set.
      explicit MigrationImapClient(std::function<bool()> stopRequested);
      ~MigrationImapClient();

      // Connects, reads the greeting, and for STARTTLS asks for it: required
      // refuses a server that will not, optional carries on in the clear.
      // timeoutSeconds bounds each step, not the session.
      bool Connect(const AnsiString &host, int port, ConnectionSecurity security, int timeoutSeconds, String &error);

      // LOGIN, with the credentials sent as literals when they are not plain
      // ASCII. refused is set only when the server refused the credentials
      // themselves - a NO with no response code, or with AUTHENTICATIONFAILED,
      // AUTHORIZATIONFAILED or EXPIRED (RFC 5530) - which no retry mends. Any
      // other answer - UNAVAILABLE, INUSE, LIMIT, an ALERT, a BAD - and a
      // connection that drops are false with refused clear: the job tries again
      // later rather than failing a mailbox for a minute's outage.
      bool Login(const AnsiString &user, const AnsiString &password, bool &refused, String &error);

      // AUTHENTICATE XOAUTH2 with the SASL initial response (RFC 7628 as Google
      // and Microsoft take it). refused as for Login.
      bool LoginXOAuth2(const AnsiString &user, const AnsiString &token, bool &refused, String &error);

      // NAMESPACE (RFC 2342): the prefixes of other users' mailboxes and of the
      // shared ones, which a job leaves to their owners' own jobs. A server that
      // refuses the command, or does not know it, answers none.
      bool Namespace(std::vector<AnsiString> &otherPrefixes, String &error);

      // LIST "" "*".
      bool List(std::vector<Mailbox> &mailboxes, String &error);

      // STATUS (MESSAGES): how many the mailbox holds, for the job's total.
      bool Status(const AnsiString &mailbox, __int64 &messages, String &error);

      // EXAMINE: read-only, so nothing the copy does marks a message \Seen or
      // takes \Recent from anybody on the source. refused is set when the
      // server would not open the mailbox; transient then says whether time
      // mends the refusal - a NO carrying UNAVAILABLE, SERVERBUG, INUSE, LIMIT
      // or THROTTLED, and Microsoft 365's "not connected" BAD - rather than
      // leaving the folder uncopied for good (NONEXISTENT, NOPERM, a NO with no
      // code, any other BAD: a name the server will never take).
      bool Examine(const AnsiString &mailbox, __int64 &uidValidity, __int64 &exists, bool &refused, bool &transient, String &error);

      // UID SEARCH UID <from>:* - every UID at or above from, ascending. (A
      // server answers n:* with its highest UID even below n; those are dropped.)
      bool UidsFrom(unsigned int from, std::vector<unsigned int> &uids, String &error);

      // UID FETCH <uid> (UID FLAGS INTERNALDATE BODY.PEEK[]), the body written
      // to fileName, created. A body larger than maxBytes is read past and not
      // written (tooLarge). gone is set when the server answered with no body -
      // the message was expunged since the search. refused is set when the
      // server answered the FETCH with a NO or a BAD on a connection still in
      // step - a message it will not give, a damaged one - as against a
      // network failure, which leaves it clear.
      bool FetchToFile(unsigned int uid, const String &fileName, __int64 maxBytes, Fetched &fetched, bool &gone, bool &refused, String &error);

      // LOGOUT, briefly, then the connection closed whatever the answer.
      void Logout();

      bool Stopped() const { return stopped_; }

      // The last line the server sent, for the report and the error.
      const AnsiString &LastLine() const { return last_line_; }

      // Public for MigrationImapClientTester.
      static AnsiString Quote(const AnsiString &value);
      static bool NeedsLiteral(const AnsiString &value);
      static bool ParseListLine(const AnsiString &line, Mailbox &mailbox);
      static void ParseFetchItems(const AnsiString &text, Fetched &fetched);
      static bool ParseLiteralLength(const AnsiString &line, __int64 &length, AnsiString &before);

      // A tagged completion's response code, upper-cased: "UNAVAILABLE" of
      // "M4 NO [UNAVAILABLE] Try later", empty when it carries none.
      static AnsiString ResponseCode(const AnsiString &taggedLine);

      // Whether a completion to LOGIN or AUTHENTICATE refuses the credentials
      // themselves (see Login), and whether one to EXAMINE is a refusal time
      // mends (see Examine).
      static bool IsCredentialRefusal(const AnsiString &taggedLine);
      static bool IsTransientRefusal(const AnsiString &taggedLine);

      // The other users' and shared prefixes of a NAMESPACE reply.
      static void ParseNamespace(const AnsiString &line, std::vector<AnsiString> &otherPrefixes);

      // The XOAUTH2 initial response: base64 of user=<user>^Aauth=Bearer <token>^A^A.
      static AnsiString XOAuth2InitialResponse(const AnsiString &user, const AnsiString &token);

   private:
      class Transport;

      // One tagged command: the untagged replies before its completion, and
      // the completion's status word (OK, NO, BAD) and text.
      bool Command_(const AnsiString &command, std::vector<AnsiString> &untagged, AnsiString &status, String &error);

      // The same, for a command whose arguments go as synchronizing literals.
      bool CommandWithLiterals_(const std::vector<AnsiString> &pieces, const std::vector<AnsiString> &literals,
                                std::vector<AnsiString> &untagged, AnsiString &status, String &error);

      // One reply, with every literal it announces read: into text as a quoted
      // string, or - for the body of a FETCH, when bodyFile is given - to the file.
      bool ReadReply_(AnsiString &text, const String *bodyFile, __int64 maxBody, Fetched *fetched, String &error);

      AnsiString NextTag_();

      std::unique_ptr<Transport> transport_;
      std::function<bool()> stop_requested_;
      int tag_;
      AnsiString current_tag_;
      AnsiString last_line_;
      bool stopped_;
   };

   class MigrationImapClientTester
   {
   public:
      void Test();
   };
}
