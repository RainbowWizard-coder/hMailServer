// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "../TCPIP/IPAddress.h"

namespace HM
{
   class SSLCertificate;

   // Optional ManageSieve (RFC 5804) listener for uploading and managing
   // per-account Sieve scripts. Implemented as a raw-socket + std::thread service
   // (like MetricsServer / RestApiServer), enabled with
   // [Settings] ManageSieveServerPort in hMailServer.ini and disabled (port 0) by
   // default. Authentication is SASL PLAIN against the regular account database.
   //
   // TLS: STARTTLS (RFC 5804 2.2) is offered when a TLS certificate is available,
   // and the SSL context it uses is built by SslContextInitializer - the same
   // function, reading the same settings, that configures TLS for SMTP, POP3 and
   // IMAP. That is deliberate and load bearing: it is the only way the cipher
   // list, the TLS version floor and the key-exchange groups (including the
   // hybrid post-quantum KEMs in TlsKeyExchangeGroups) cannot silently diverge
   // between this listener and the protocols people actually read the release
   // notes about. The bridge is one cast: SslContextInitializer works on a
   // boost::asio::ssl::context, and this listener's blocking sockets use the
   // SSL_CTX underneath it (context::native_handle). Re-hosting the listener on
   // the shared Boost.Asio stack, so that it stops being a separate accept loop
   // at all, is Phase 1 work - see the comment above InitializeTls_.
   //
   // With no certificate available STARTTLS is not advertised and the listener
   // behaves exactly as it did before: plain text only, so bind to 127.0.0.1
   // unless fronted by a TLS terminator.
   //
   // REQUIRING TLS BEFORE A PASSWORD: the listener honours the SAME per-IP-range
   // setting the mailbox protocols do - SecurityRange::GetRequireTLSForAuth, the
   // "Require SSL/TLS for authentication" option on an IP range, which POP3
   // (ProtocolUSER_/ProtocolAUTH_), IMAP (IMAPCommandLogin/Authenticate) and SMTP
   // all consult. It is deliberately NOT a second, ManageSieve-specific setting:
   // an operator who has already declared that a range must not authenticate in
   // the clear means it for the Sieve filters of the same mailbox, and two
   // settings that could disagree would eventually disagree in the direction of
   // the weaker one. It costs nothing to read, because IsConnectionAllowed_
   // already fetches the matching range for every accepted connection.
   //
   // The consequence, when that option is set and the connection is still in
   // clear text, is that the capability line offers no SASL mechanism at all
   // ("SASL" "") and AUTHENTICATE is answered NO (ENCRYPT-NEEDED) before the SASL
   // payload is parsed or read - so the password is never taken off the wire.
   // Advertising a mechanism and refusing it afterwards is the same defect that
   // was fixed in POP3's CAPA: a client builds its logon UI from that line and
   // sends the password before it can learn the connection was unacceptable.
   //
   // With the option clear - the shipped default on every range - nothing changes:
   // SASL PLAIN is advertised and accepted in clear text exactly as before.
   //
   // Because it does not run on the shared Boost.Asio listener, this class has to
   // apply the security-range check itself when it accepts a connection - see
   // IsConnectionAllowed_. Without it the auto-ban written on repeated logon
   // failures would never be enforced, and the per-connection attempt cap could be
   // sidestepped simply by reconnecting.
   //
   // ONE PARTY CANNOT HOLD THE SERVICE (since 6.3.6's re-check). Until then one
   // thread served one connection at a time, with a backlog of five, so whoever
   // kept a connection going held the whole service: a signed-in client sending
   // NOOP every 29 seconds for ever, or a client that never signed in and came
   // back the moment each connection was cut, with five more waiting behind it.
   // Now one thread accepts (Run_) and MaxSessions threads serve (Serve_):
   //
   //   - Before it has signed in, an address has at most one connection being
   //     served; its others wait their turn in its own line, MaxWaitingPerAddress
   //     long, and a connection past that, or past MaxWaiting in all, is refused
   //     with BYE (TRYLATER). So connections from one address take one thread.
   //     The address is read as SignInBudget::GuessSource reads it: an IPv6 one
   //     by its /64, which one machine can fill with as many addresses as it
   //     likes, and an IPv4 client of NAT64 or Teredo by its own IPv4 address.
   //   - A connection that runs out of time before signing in - a line, the TLS
   //     handshake or the PreAuthenticationSeconds of the whole - counts as a
   //     failed logon from its address, so the auto-ban sees a client that comes
   //     back to hold the service and bans it as it bans a guesser.
   //   - Signed in, a session ends after MaxKeepAliveLines lines in a row that do
   //     nothing (NOOP, CAPABILITY, an empty or unknown command, a STARTTLS or
   //     AUTHENTICATE it refuses), or once it has waited SignedInIdleSeconds in
   //     all for its commands; and an account has at most MaxSessionsPerAccount
   //     sessions at once.
   class ManageSieveServer
   {
   public:
      ManageSieveServer();
      ~ManageSieveServer();

      bool Start(const String &bind_address, int port);
      void Stop();

      // The listener's half of a drain (Application::CloseTheDoors_ and
      // LeaveDrain), because this listener is not one of IOService's acceptors
      // and would otherwise keep accepting through it. StopAccepting closes the
      // listening socket - a connection attempted from then on is refused at
      // the socket - and parks the accepting thread; StartAccepting opens the
      // socket again on the same address and port and wakes it. The serving
      // threads are never stopped or joined by either, so the sessions in flight,
      // and the connections accepted before the door closed, finish at their own
      // pace.
      // StartAccepting is false when the port could not be bound again, which
      // the caller reports; the listener then stays closed.
      bool StopAccepting();
      bool StartAccepting();
      bool IsAccepting() const { return accepting_; }

      // Connections being served right now, and those accepted and waiting for a
      // thread. Counted into a drain's wait with the mailbox protocols' sessions.
      int GetNumberOfSessions() const { return sessions_; }

      // The address and port Start was given, for a report that names them.
      String GetBindAddress() const { return bind_address_; }
      int GetPort() const { return port_; }

   private:
      // Failed AUTHENTICATE attempts allowed on one connection before it is
      // dropped, matching the other protocol servers.
      enum { MaxAuthenticationFailures = 3 };

      // Upper bound on a SASL response, in either of the two forms RFC 5804
      // allows it to arrive in - a literal, or a quoted string on the command
      // line. A base64-encoded PLAIN response holding a real address and password
      // is an order of magnitude smaller; the cap stops an unauthenticated client
      // from making us buffer an arbitrary amount of data. It applies to the
      // inline form too because a cap that only covered the literal form was no
      // cap at all: the same payload sent as a quoted string was bounded only by
      // the one-megabyte line guard in ReadLine_.
      enum { MaxSaslResponseSize = 8192 };

      // Commands accepted from a client that has not authenticated yet, after
      // which the session is closed.
      //
      // This is not the same protection as MaxAuthenticationFailures, which only
      // counts commands that actually carried a credential - CAPABILITY, NOOP and
      // AUTHENTICATE naming an unknown mechanism were all unlimited, and a client
      // looping NOOP held a thread for as long as it liked (until 6.3.6 the only
      // thread). Set far above any real client, which needs at most CAPABILITY,
      // STARTTLS, CAPABILITY and AUTHENTICATE.
      enum { MaxPreAuthenticationCommands = 25 };

      // How many connections are served at once, how many accepted ones may wait
      // for a thread, and how many of those one address may have waiting (the
      // class comment). Constants: the service is one administrator's tool for
      // a mailbox's filters, and no operator has a reason to choose between them.
      enum { MaxSessions = 16 };
      enum { MaxWaiting = 64 };
      enum { MaxWaitingPerAddress = 4 };

      // A signed-in session ends after this many lines in a row that change
      // nothing - NOOP, CAPABILITY, an empty or unknown command, a STARTTLS or
      // AUTHENTICATE it refuses - or once it has waited this long in all for its
      // command lines; an account holds at most this many sessions at once. Each
      // wait is still LineSeconds at most. Until 6.3.6's re-check a signed-in
      // session had no bound at all.
      enum { MaxKeepAliveLines = 10 };
      enum { SignedInIdleSeconds = 120 };
      enum { MaxSessionsPerAccount = 4 };

      // The clock every read, write and handshake on a connection is held to.
      // Run_ serves one connection at a time, so a client that kept a read going
      // held the whole service: until 6.3.6 the only bound was 30 seconds for
      // each read, which a client sending one octet every 29 seconds never
      // met. The connection's socket is non-blocking, and no wait for it lasts
      // past the deadline of the step in progress (Connection):
      //
      //   IdleSeconds              the longest one wait may last: nothing
      //                            arriving or leaving for this long ends the
      //                            session, as the socket timeouts did
      //   LineSeconds              a command line, whole, from when it is
      //                            waited for - which is also how long a session
      //                            may sit idle between commands - and the TLS
      //                            handshake after STARTTLS
      //   LiteralSeconds           a literal (a script, a SASL response), and
      //                            each response written
      //   PreAuthenticationSeconds everything before AUTHENTICATE succeeds,
      //                            from the connection on: the greeting, STARTTLS
      //                            and its handshake, the commands and the logon
      //
      // Far above what a client needs. Constants, as MaxScriptSize is: no
      // operator has a reason to choose a number in between.
      enum { IdleSeconds = 30 };
      enum { LineSeconds = 30 };
      enum { LiteralSeconds = 300 };
      enum { PreAuthenticationSeconds = 60 };

      // Largest script PUTSCRIPT and CHECKSCRIPT will accept, and the figure
      // HAVESPACE answers against. One megabyte is the same order as the limit
      // other ManageSieve implementations ship with, and a Sieve script that
      // approaches it is generated rather than written.
      //
      // Deliberately a constant and not a setting. The value only has to be high
      // enough that no genuine script meets it and low enough that a malicious one
      // cannot make the server buffer without bound, and no operator has a reason
      // to pick a number in between - whereas the previous behaviour, where the
      // only bound was the ten-megabyte buffer guard inside ReadBytes_ and
      // exceeding it closed the connection without a response, had no defensible
      // value at all.
      enum { MaxScriptSize = 1024 * 1024 };

      // The outcome of a single AUTHENTICATE command.
      enum AuthenticationOutcome
      {
         AuthenticationSucceeded,
         AuthenticationFailed,    // keep the connection, the client may retry
         AuthenticationAborted    // the connection must be closed
      };

      // One accepted client connection: the socket, the bytes read from it but not
      // yet consumed, and - only after a completed STARTTLS handshake - the
      // OpenSSL session that every subsequent read and write has to go through.
      // Grouping the three means no code path can read past the TLS upgrade and
      // still be talking to the bare socket.
      struct Connection
      {
         explicit Connection(SOCKET client_socket) :
            socket(client_socket),
            tls_session(nullptr),
            require_tls_for_auth(false),
            step_deadline(std::chrono::steady_clock::time_point::max()),
            session_deadline(std::chrono::steady_clock::time_point::max()),
            out_of_time(false),
            write_failed(false),
            holds_address_turn(false)
         {
         }

         // Owns a socket and an SSL object, so copying it would mean closing and
         // freeing both twice. Nothing here needs a copy; say so rather than leave
         // it to a future caller to find out.
         Connection(const Connection &) = delete;
         Connection &operator=(const Connection &) = delete;

         SOCKET socket;
         SSL *tls_session;

         // The authentication policy of the security range this peer matched,
         // captured once when the connection was accepted. Kept on the connection
         // rather than passed around because both the capability line and the
         // AUTHENTICATE handler have to answer the same question, and they must
         // never answer it differently - advertising a mechanism that will then be
         // refused is precisely the defect being fixed.
         bool require_tls_for_auth;

         std::string buffer;

         // The deadline of the step in progress (a line, a literal, a response,
         // the handshake) and of the phase (until authentication; the end of
         // time after it). No wait for the socket goes past the earlier.
         std::chrono::steady_clock::time_point step_deadline;
         std::chrono::steady_clock::time_point session_deadline;

         // A wait ran out of time; a response could not be written whole. Either
         // ends the session at the next read.
         bool out_of_time;
         bool write_failed;

         // The peer's address as the waiting lines key it, whether this connection
         // holds its address's one turn to be served before signing in, and the
         // account it is signed in as (empty when it is not), whose session count
         // it holds.
         std::string address_key;
         bool holds_address_turn;
         String account;
      };

      // An accepted connection that waits for a thread.
      struct Waiting
      {
         SOCKET socket = INVALID_SOCKET;
         IPAddress address;
         std::string address_key;
         bool require_tls_for_auth = false;
         std::chrono::steady_clock::time_point accepted_at;
      };

      // The accepting thread: accepts, applies the security ranges and puts the
      // connection in line (Admit_), or refuses it.
      void Run_();

      // The text of an exception that escaped, or nothing when copying it throws
      // as well - the exception in hand may be the bad_alloc that says allocation
      // is failing.
      static AnsiString WhatOf_(const std::exception &error);

      // Reports an exception that escaped while accepting or serving a connection
      // (what_happened; standard when it was a std::exception, with its text),
      // fenced off: the report runs the operator's scripts (ScriptServer::FireEvent),
      // and what escapes them must not reach the top of a thread, which would be
      // std::terminate() on the whole server.
      static void ReportEscaped_(const String &what_happened, bool standard, const AnsiString &text);

      // A serving thread: takes the first waiting connection whose address has no
      // connection being served before signing in, and serves it.
      void Serve_();
      void ServeOne_(Waiting &waiting);
      void HandleClient_(Connection &connection, const IPAddress &client_address, bool require_tls_for_auth);

      // In line, or refused with BYE (TRYLATER) and closed. True when in line.
      bool Admit_(SOCKET socket, const IPAddress &address, bool require_tls_for_auth);

      // Closes the waiting connections that have waited PreAuthenticationSeconds.
      // With queue_mutex_ held.
      void DropExpiredLocked_(std::chrono::steady_clock::time_point now);

      // Signed in as account: the address's turn goes to its next connection, and
      // the account's sessions are counted. False, with nothing changed, when the
      // account has MaxSessionsPerAccount already.
      bool ClaimAccountSession_(Connection &connection, const String &account);

      // UNAUTHENTICATE, and the end of a session: what the connection holds is let
      // go.
      void ReleaseAccountSession_(Connection &connection);
      void ReleaseAddressTurn_(Connection &connection);

      // UNAUTHENTICATE: the connection is about to be one that has not signed in
      // again, so it takes its address's one turn back. False, with nothing
      // changed, while another connection from the address holds it - the session
      // then stays signed in (NO (TRYLATER)). Without it a client with one
      // password could sign in on connection after connection and UNAUTHENTICATE
      // each, and hold every serving thread from one address before signing in.
      bool ReclaimAddressTurn_(Connection &connection);

      // Releases the TLS session, if any, and closes the socket. Called from the
      // accept loop rather than from HandleClient_, so that the exception barrier
      // firing cannot leak a socket or an SSL object.
      static void CloseConnection_(Connection &connection);

      // Accept-time IP restriction check. This is the equivalent of what
      // TCPServer::HandleAccept does for SMTP/POP3/IMAP and is what makes an
      // auto-ban actually stop a reconnecting client.
      //
      // require_tls_for_auth is an output because the same range lookup answers
      // both questions, and one database read per connection is the whole budget:
      // reading the range twice would also let the access decision and the
      // authentication policy come from two different rows if a range were edited
      // in between.
      static bool IsConnectionAllowed_(const IPAddress &client_address, bool &require_tls_for_auth);

      // True when a credential must not be accepted on this connection: the range
      // that matched the peer requires TLS for authentication and the connection is
      // still in clear text. The single answer used both by the capability line and
      // by AUTHENTICATE.
      static bool IsAuthenticationRefusedOnCleartext_(const Connection &connection);

      static IPAddress GetClientAddress_(const sockaddr *address);

      // Builds the shared server SSL context for STARTTLS. False - which is not an
      // error - simply means STARTTLS is not advertised.
      bool InitializeTls_(const String &bind_address, int port);

      // The certificate this listener presents. There is no ManageSieve-specific
      // certificate setting, so it is borrowed from the TLS-capable mailbox ports.
      static std::shared_ptr<SSLCertificate> FindTlsCertificate_();

      // Performs the server side of the TLS handshake on an accepted connection.
      bool StartTls_(Connection &connection);

      void SendCapabilities_(Connection &connection) const;

      // Handles one AUTHENTICATE command. All credential-carrying paths run
      // through here so that no failure can escape the auto-ban accounting or
      // the per-connection cap. On success, account_address holds the address of
      // the account that was logged on to.
      //
      // Only ever called for a client that has not authenticated yet: HandleClient_
      // refuses a second AUTHENTICATE outright, because a success here sets
      // authentication_failures back to zero, and a client able to reach that path
      // repeatedly could clear the cap between guesses.
      AuthenticationOutcome HandleAuthenticate_(Connection &connection,
                                                const IPAddress &client_address,
                                                const String &line,
                                                int pos,
                                                int &authentication_failures,
                                                String &account_address);

      // Counts a failed attempt, sends the response and reports whether the
      // connection has run out of attempts and must be closed.
      static bool ApplyAuthenticationFailure_(Connection &connection,
                                              int &authentication_failures,
                                              bool disconnect,
                                              const AnsiString &failure_response);

      static bool ReadLine_(Connection &connection, String &line);
      static bool ReadBytes_(Connection &connection, int count, String &data);
      static void Send_(Connection &connection, const AnsiString &data);

      // Single point where bytes come off the wire, so that the plain-socket and
      // TLS cases cannot get out of step. Waits no later than the connection's
      // deadlines; 0 or less is the end of the session.
      static int Receive_(Connection &connection, char *buffer, int size);

      // Waits for the non-blocking socket to be readable (or writable), no longer
      // than IdleSeconds and no later than deadline. False when the time is up,
      // which is recorded on the connection.
      static bool WaitForSocket_(Connection &connection, bool for_write, std::chrono::steady_clock::time_point deadline);

      SOCKET listen_socket_;
      std::thread worker_;
      std::vector<std::thread> servers_;
      std::atomic<bool> running_;

      // One application-log line per service start for the configuration that locks
      // everybody out: a range that requires TLS for authentication reaching a
      // listener with no certificate, where there is neither a SASL mechanism to
      // offer nor a STARTTLS to reach one through. Without it that combination is
      // silent, and the operator sees only clients that cannot log on.
      //
      // Deliberately not an ErrorManager report: it is an operator configuration
      // mistake rather than a defect, and an entry in the ERROR log makes
      // PerformBasicSetup fail for every regression fixture that follows - which
      // includes the fixture that provokes this exact state on purpose.
      //
      // Set by whichever serving thread meets the combination first.
      std::atomic<bool> warned_tls_required_without_certificate_;

      // Held as a Boost.Asio context only because that is what
      // SslContextInitializer configures; the SSL_CTX inside it is what this
      // listener uses. Null when no certificate is available, which is what
      // "STARTTLS is not offered" means. Written by Start() before the worker
      // thread exists and cleared by Stop() after it has been joined, so the
      // worker never sees it change.
      std::shared_ptr<boost::asio::ssl::context> tls_context_;

      // socket(), bind() and listen() on bind_address_:port_, into
      // listen_socket_. Start's first act, and StartAccepting's.
      bool OpenListenSocket_();

      String bind_address_;
      int port_;

      // Whether the worker may accept. Cleared by StopAccepting and set by
      // StartAccepting and Start; the worker waits on accept_changed_ while it
      // is clear, and Stop wakes it too. listen_socket_ is read and written
      // under accept_mutex_ from here on, because the worker and a drain now
      // both touch it while the worker runs.
      std::atomic<bool> accepting_;
      std::atomic<int> sessions_;
      std::mutex accept_mutex_;
      std::condition_variable accept_changed_;

      // The waiting connections in the order they came, how many each address has
      // waiting, the addresses with a connection being served that has not signed
      // in, and the signed-in sessions of each account. All under queue_mutex_;
      // queue_changed_ wakes the serving threads.
      std::mutex queue_mutex_;
      std::condition_variable queue_changed_;
      std::deque<Waiting> waiting_;
      std::map<std::string, int> waiting_by_address_;
      std::set<std::string> served_before_sign_in_;
      std::map<String, int> sessions_by_account_;

      // Held while a command reads or writes an account's scripts
      // (SieveStorage), never while a session waits for its client. Until
      // 6.3.6's re-check one thread served one session at a time, which kept
      // them apart; now sessions are served side by side, an account may have
      // MaxSessionsPerAccount, and SieveStorage writes a file in place (two
      // PUTSCRIPTs of one script at once could leave it a mix of both) and
      // checks before it acts (a DELETESCRIPT beside a SETACTIVE of the same
      // script could delete the active one).
      std::mutex storage_mutex_;
   };
}
