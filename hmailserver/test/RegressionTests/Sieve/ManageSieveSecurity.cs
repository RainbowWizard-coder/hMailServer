// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Sieve
{
   /// <summary>
   ///    Authentication and resource-limit hardening on the ManageSieve (RFC 5804)
   ///    listener, covering the holes that were left after the per-IP auto-ban and
   ///    the STARTTLS work (ManageSieveAutoBan / ManageSieveTls).
   ///
   ///    Each test here fails against the code as it stood before this change, and
   ///    each one covers a different hole:
   ///
   ///    1. RE-AUTHENTICATION RESET THE ATTEMPT CAP. AUTHENTICATE was accepted on an
   ///       already-authenticated session, and a successful AUTHENTICATE sets the
   ///       failure counter back to zero. So a client holding one valid password
   ///       could log on, spend two guesses on somebody else's account, log on again
   ///       to clear the counter, and repeat - for as long as it liked, on one
   ///       connection. The per-connection cap of three attempts, which is the whole
   ///       subject of the two sibling fixtures, was therefore unbounded for anyone
   ///       with a single account on the server.
   ///
   ///    2. THE SASL LENGTH CAP ONLY COVERED ONE OF THE TWO FORMS. RFC 5804 allows
   ///       the SASL response as a literal or as a quoted string on the command line.
   ///       The 8 KB cap was applied to the literal only, so the identical payload
   ///       sent inline was bounded solely by ReadLine_'s one-megabyte line guard,
   ///       and was base64-decoded and split at that size, per command, from a client
   ///       that had not authenticated.
   ///
   ///    3. COMMANDS THAT CARRIED NO CREDENTIAL WERE UNLIMITED. The attempt cap only
   ///       counts commands that reached the password check, so CAPABILITY, NOOP and
   ///       AUTHENTICATE naming an unknown mechanism could be repeated forever. That
   ///       is worse here than on the mailbox protocols: this listener serves
   ///       connections one at a time on a single worker thread, so one client
   ///       looping NOOP holds the entire ManageSieve service, not just its own
   ///       connection.
   ///
   ///    4/5. THE SCRIPT SIZE LIMIT WAS A BUFFER GUARD, AND HAVESPACE LIED ABOUT IT.
   ///       The only bound on PUTSCRIPT was the ten-megabyte guard inside ReadBytes_,
   ///       which is reached only after ten megabytes have been buffered and which
   ///       fails by closing the connection without sending anything at all -
   ///       indistinguishable, from the client, from the network dropping mid-upload.
   ///       HAVESPACE, whose entire purpose in RFC 5804 is to let a client be told
   ///       "no" while its session is still usable, answered OK to every request
   ///       regardless of size.
   ///
   ///    6. AN OVERSIZED LITERAL LENGTH WENT THROUGH _wtoi, whose behaviour on
   ///       overflow is undefined, and the result was then trusted as a byte count.
   ///
   ///    7. ONE PARTY HELD THE SERVICE (6.3.6's re-check). One thread served one
   ///       connection at a time behind a backlog of five, so a signed-in client
   ///       sending NOOP every 29 seconds held it for ever, and so did a client that
   ///       never signed in and came back each time it was cut off. Now connections
   ///       are served side by side, an address has one turn before signing in, a
   ///       connection that ran out of time before signing in counts as a failed
   ///       logon - but not one from this computer's loopback address, a webmail
   ///       beside the server (6.3.6's last re-check) - a signed-in session ends
   ///       after ten lines that did nothing or two minutes of waiting in all, and
   ///       an account has four sessions at most.
   /// </summary>
   [TestFixture]
   public class ManageSieveSecurity : TestFixtureBase
   {
      // This fixture's own port, from the range allocated to this change. The three
      // sibling ManageSieve fixtures use 24190-24192, so nothing here can collide
      // with them if they are ever run alongside each other.
      private const int ManageSievePort = 9390;

      // Must match ManageSieveServer::MaxScriptSize.
      private const int MaxScriptSize = 1024 * 1024;

      // Must match ManageSieveServer::MaxPreAuthenticationCommands.
      private const int MaxPreAuthenticationCommands = 25;

      // Must match ManageSieveServer::MaxSaslResponseSize.
      private const int MaxSaslResponseSize = 8192;

      // Must match ManageSieveServer::LineSeconds and PreAuthenticationSeconds.
      private const int LineSeconds = 30;
      private const int PreAuthenticationSeconds = 60;

      // Must match ManageSieveServer::MaxKeepAliveLines, SignedInIdleSeconds and
      // MaxSessionsPerAccount.
      private const int MaxKeepAliveLines = 10;
      private const int SignedInIdleSeconds = 120;
      private const int MaxSessionsPerAccount = 4;

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      // The auto-ban setting as it was before this fixture touched it, so StopListener
      // can put it back.
      private bool _autoBanWasEnabled;

      /// <summary>
      /// Starts the listener, and turns auto-ban off for the duration.
      ///
      /// Auto-ban has to go: tests here make failed logons from 127.0.0.1, and once
      /// an auto-ban range exists for that address the listener refuses the
      /// connection at accept time (that is exactly what ManageSieveAutoBan proves),
      /// so the later attempts would be dropped before reaching the code under test -
      /// and the range would go on blocking SMTP, POP3 and IMAP for whatever fixture
      /// ran next. StopListener restores the previous value.
      /// </summary>
      private void StartListener(string bindAddress = "127.0.0.1")
      {
         _autoBanWasEnabled = _settings.AutoBanOnLogonFailure;
         _settings.AutoBanOnLogonFailure = false;
         _settings.ClearLogonFailureList();

         WriteSetting("ManageSieveServerBindAddress", bindAddress);
         WriteSetting("ManageSieveServerPort", ManageSievePort.ToString());

         // Reinitialize so IniFileSettings re-reads the port and the listener starts.
         _application.Reinitialize();
      }

      private void StopListener()
      {
         WriteSetting("ManageSieveServerPort", "0");
         _application.Reinitialize();

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = _autoBanWasEnabled;
      }

      // A minimal ManageSieve client. Deliberately a local copy rather than a shared
      // helper, for the same reason the sibling fixtures each have one: this one has
      // to be able to send a malformed command and then observe a connection that is
      // closed with, or without, a response.
      private sealed class Session : IDisposable
      {
         private readonly TcpClient _client;
         private readonly NetworkStream _stream;

         public Session() : this(null)
         {
         }

         // From localAddress, a loopback address other than 127.0.0.1, when given:
         // a client at another address.
         public Session(string localAddress) : this(localAddress, "127.0.0.1")
         {
         }

         // To the listener at serverAddress, from localAddress when it is given.
         public Session(string localAddress, string serverAddress)
         {
            _client = localAddress == null ? new TcpClient() : new TcpClient(new IPEndPoint(System.Net.IPAddress.Parse(localAddress), 0));

            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  _client.Connect(serverAddress, ManageSievePort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  System.Threading.Thread.Sleep(200);
               }
            }
            if (last != null)
               throw last;

            _stream = _client.GetStream();
            _stream.ReadTimeout = 15000;
         }

         public void SetReadTimeout(int milliseconds)
         {
            _stream.ReadTimeout = milliseconds;
         }

         // Returns null when the server closed the connection, or the read timed out,
         // instead of sending a complete line.
         private string ReadLine()
         {
            var bytes = new List<byte>();
            int previous = -1;
            while (true)
            {
               int current;
               try
               {
                  current = _stream.ReadByte();
               }
               catch (IOException)
               {
                  // Read timeout. Treated the same as a close: the caller is asking
                  // for a response line and there is none.
                  return null;
               }

               if (current == -1)
               {
                  if (bytes.Count == 0)
                     return null;
                  break;
               }

               if (previous == '\r' && current == '\n')
               {
                  bytes.RemoveAt(bytes.Count - 1); // drop the trailing CR
                  break;
               }

               bytes.Add((byte) current);
               previous = current;
            }

            return Encoding.UTF8.GetString(bytes.ToArray());
         }

         // Reads response lines up to and including the completion line
         // (OK / NO / BYE), or until the server hangs up or stops answering.
         public string ReadResponse()
         {
            var sb = new StringBuilder();
            while (true)
            {
               string line = ReadLine();
               if (line == null)
                  break;

               sb.AppendLine(line);

               string trimmed = line.TrimStart();
               if (trimmed.StartsWith("OK", StringComparison.OrdinalIgnoreCase) ||
                   trimmed.StartsWith("NO", StringComparison.OrdinalIgnoreCase) ||
                   trimmed.StartsWith("BYE", StringComparison.OrdinalIgnoreCase))
                  break;
            }

            return sb.ToString();
         }

         // Reads everything the server sends until it closes the connection. Returns
         // the empty string when the connection is already closed; if the server is
         // instead still holding it open, whatever it sent is returned once the read
         // times out, so the assertion message can show it.
         public string ReadUntilClosed(int timeoutMilliseconds)
         {
            _stream.ReadTimeout = timeoutMilliseconds;

            var sb = new StringBuilder();
            try
            {
               while (true)
               {
                  int current = _stream.ReadByte();
                  if (current == -1)
                     break;

                  sb.Append((char) current);
               }
            }
            catch (IOException)
            {
               // Timed out waiting for either data or a close.
            }

            return sb.ToString();
         }

         public void SendRaw(string text)
         {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            _stream.Write(bytes, 0, bytes.Length);
            _stream.Flush();
         }

         // Sends the octets one at a time, intervalMilliseconds apart, until the
         // server closes the connection. The seconds that took, or -1 when it was
         // still open after maxSeconds.
         public double TrickleUntilClosed(byte[] octets, int intervalMilliseconds, int maxSeconds)
         {
            var watch = System.Diagnostics.Stopwatch.StartNew();
            int next = 0;
            while (watch.Elapsed.TotalSeconds < maxSeconds)
            {
               if (IsClosedByServer())
                  return watch.Elapsed.TotalSeconds;

               try
               {
                  _stream.WriteByte(octets[next % octets.Length]);
                  _stream.Flush();
                  next++;
               }
               catch (IOException)
               {
                  return watch.Elapsed.TotalSeconds;
               }

               System.Threading.Thread.Sleep(intervalMilliseconds);
            }

            return IsClosedByServer() ? watch.Elapsed.TotalSeconds : -1;
         }

         // Readable with nothing to read: the server has closed its side.
         private bool IsClosedByServer()
         {
            Socket socket = _client.Client;
            try
            {
               return socket.Poll(0, SelectMode.SelectRead) && socket.Available == 0;
            }
            catch (SocketException)
            {
               return true;
            }
            catch (ObjectDisposedException)
            {
               return true;
            }
         }

         public string SendCommand(string command)
         {
            SendRaw(command + "\r\n");
            return ReadResponse();
         }

         private static string EncodeSaslPlain(string username, string password)
         {
            string sasl = "\0" + username + "\0" + password;
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(sasl));
         }

         public string Authenticate(string username, string password)
         {
            return SendCommand("AUTHENTICATE \"PLAIN\" \"" + EncodeSaslPlain(username, password) + "\"");
         }

         public void Dispose()
         {
            try { _stream?.Dispose(); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
            try { _client?.Close(); } catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { /* Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding. */ }
         }
      }

      [Test]
      [Description("AUTHENTICATE is refused on an already-authenticated session, so a successful logon cannot be used to reset the failed-attempt counter.")]
      public void ReauthenticationIsRefusedSoTheAttemptCapCannotBeReset()
      {
         const string attacker = "sieve-sec-owner@example.test";
         const string attackerPassword = "Sieve-Pass1";
         const string victim = "sieve-sec-victim@example.test";

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, attacker, attackerPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, victim, "Sieve-Pass2");

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               StringAssert.StartsWith("OK", session.Authenticate(attacker, attackerPassword).TrimStart(),
                  "The first authentication should succeed.");

               // A guess at somebody else's password, from a session that is already
               // logged on to an account we do own.
               //
               // The fixed server does not check it at all - the command is refused
               // because this session has already authenticated. The unfixed server
               // ran the password check and answered NO "Authentication failed.",
               // which is what makes this assertion fail against it.
               //
               // Why that mattered: a failed check is counted against the connection,
               // but a *successful* AUTHENTICATE sets that counter back to zero, and
               // re-authenticating was permitted. So the loop was: log on, spend two
               // guesses, log on again to clear the counter, repeat - unbounded
               // guessing on one connection for anyone holding a single valid
               // password, with the cap of three never once reached.
               string guess = session.Authenticate(victim, "guess-1");
               StringAssert.Contains("Already authenticated", guess,
                  "AUTHENTICATE on an already-authenticated session must be refused without reaching the " +
                  "password check. Got: " + guess);

               // The same refusal for the credentials that did work a moment ago, so
               // the counter-clearing step itself is gone rather than merely being
               // harder to reach.
               string reauth = session.Authenticate(attacker, attackerPassword);
               StringAssert.Contains("Already authenticated", reauth,
                  "A second AUTHENTICATE must be refused rather than resetting the attempt counter. Got: " + reauth);

               // Negative control: refusing re-authentication must not damage the
               // session that was already established.
               string list = session.SendCommand("LISTSCRIPTS");
               Assert.IsFalse(list.TrimStart().StartsWith("NO"),
                  "The established session should still work after a refused re-authentication. Got: " + list);

               StringAssert.StartsWith("OK", session.SendCommand("LOGOUT").TrimStart());
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("An oversized SASL response sent inline as a quoted string is refused on its length, before it is base64-decoded.")]
      public void OversizedInlineSaslResponseIsRefusedBeforeItIsDecoded()
      {
         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               // Comfortably over the 8 KB cap and comfortably under ReadLine_'s
               // one-megabyte line guard, so that the unfixed server does answer
               // rather than dropping the connection - the point is which answer.
               //
               // Before the fix the cap was only applied to the literal form, so this
               // was decoded (a base64 'A' run decodes to NUL bytes, so the split on
               // NUL produced thousands of fields) and answered
               // NO "Invalid SASL response." A megabyte of it, per command, from an
               // unauthenticated client.
               string oversized = new string('A', MaxSaslResponseSize + 12000);

               string response = session.SendCommand("AUTHENTICATE \"PLAIN\" \"" + oversized + "\"");

               StringAssert.Contains("too large", response,
                  "An inline SASL response over the size cap should be refused on its length. Got: " + response);
            }

            // Negative control: a normal-sized inline response is still the form the
            // sibling fixtures use, and still works.
            const string address = "sieve-sec-inline@example.test";
            const string password = "Sieve-Pass1";
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart(),
                  "A normal inline SASL response must still authenticate.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A client that never authenticates is disconnected after a bounded number of commands, rather than holding the single-threaded listener indefinitely.")]
      public void UnauthenticatedCommandsAreBounded()
      {
         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               // NOOP carries no credential, so the failed-attempt cap never saw it.
               // Before the fix every one of these was answered OK for as long as the
               // client cared to keep going, and because Run_ serves one connection at
               // a time on one thread, no other client could be served meanwhile.
               bool disconnected = false;
               int sent = 0;

               // Well past the budget. If the server is still answering after this
               // many, there is no budget.
               int attempts = MaxPreAuthenticationCommands * 2;

               for (int i = 0; i < attempts; i++)
               {
                  string response;
                  try
                  {
                     response = session.SendCommand("NOOP");
                  }
                  catch (IOException)
                  {
                     // The server closed the connection under the write.
                     disconnected = true;
                     sent = i + 1;
                     break;
                  }

                  sent = i + 1;

                  if (response.Length == 0 || response.TrimStart().StartsWith("BYE", StringComparison.OrdinalIgnoreCase))
                  {
                     disconnected = true;
                     break;
                  }
               }

               Assert.IsTrue(disconnected,
                  "The listener answered " + sent + " pre-authentication NOOPs without ever disconnecting, so an " +
                  "unauthenticated client can hold it open indefinitely.");

               // It really is closed, not merely refusing to answer.
               string afterwards = session.ReadUntilClosed(5000);
               Assert.AreEqual(string.Empty, afterwards,
                  "The connection should be closed once the pre-authentication command budget is spent. Received: " + afterwards);
            }

            // Negative control: the budget is far above what a real client needs, so a
            // handful of pre-authentication commands followed by a logon still works.
            const string address = "sieve-sec-budget@example.test";
            const string password = "Sieve-Pass1";
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               // CAPABILITY answers with the capability lines and then OK, so the
               // completion is at the end of the block rather than the start of it.
               for (int i = 0; i < 4; i++)
                  StringAssert.Contains("IMPLEMENTATION", session.SendCommand("CAPABILITY"));

               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart(),
                  "A client that sends a few commands before authenticating must still be able to authenticate.");

               // And the budget does not apply once authenticated: it is a
               // pre-authentication limit, not a command limit. Every other command
               // does something (LISTSCRIPTS), since a signed-in session is ended
               // after MaxKeepAliveLines in a row that do nothing (6.3.6's re-check).
               for (int i = 0; i < MaxPreAuthenticationCommands * 2; i++)
               {
                  string answer = session.SendCommand(i % 2 == 0 ? "NOOP" : "LISTSCRIPTS");
                  StringAssert.Contains("OK", answer, "Command " + (i + 1) + " after authenticating was not answered OK: " + answer);
               }
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A signed-in client sending NOOP every 25 seconds does not keep a second client waiting: the second is greeted within 30 seconds. Until 6.3.6's re-check one thread served one connection at a time, and such a client held the service for as long as it went on.")]
      public void ASignedInClientKeepingItsSessionGoingDoesNotHoldTheService()
      {
         const string address = "sieve-holder@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var holder = new Session())
            {
               holder.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", holder.Authenticate(address, password).TrimStart());

               // The second client is asked for on a task of its own, so that the holder
               // goes on sending its NOOP every 25 seconds while the second waits - as
               // a holder would. Its greeting within 30 seconds is the assertion.
               Task<double> second = null;
               var watch = Stopwatch.StartNew();

               while (watch.Elapsed.TotalSeconds < 180)
               {
                  string answer = holder.SendCommand("NOOP");
                  if (answer.Length == 0)
                     break; // the server ended the holder's session: it may

                  if (second == null && watch.Elapsed.TotalSeconds >= 20)
                  {
                     second = Task.Run(() =>
                     {
                        var waited = Stopwatch.StartNew();
                        using (var other = new Session())
                        {
                           other.SetReadTimeout(30000);
                           string greeting = other.ReadResponse();
                           if (!greeting.Contains("IMPLEMENTATION"))
                              return -1.0;
                           double seconds = waited.Elapsed.TotalSeconds;
                           other.SendCommand("LOGOUT");
                           return seconds;
                        }
                     });
                  }

                  Thread.Sleep(25000);
               }

               Assert.IsNotNull(second, "The holder's session ended before the second client was asked for.");
               double greetedAfter = second.Result;
               Assert.IsTrue(greetedAfter >= 0,
                  "The second client was not greeted within 30 seconds while a signed-in client sent NOOP every 25 seconds: one client holds the service.");
               Assert.Less(greetedAfter, 30.0, "The second client was greeted only after " + greetedAfter.ToString("0.0") + " s.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A signed-in session that sends only lines that do nothing - NOOP, CAPABILITY - is ended after ten of them in a row, with BYE. Until 6.3.6's re-check such a session had no bound.")]
      public void ASignedInSessionThatOnlyKeepsItselfAliveIsEnded()
      {
         const string address = "sieve-keepalive@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               // A command that does something resets the count.
               StringAssert.StartsWith("OK", session.SendCommand("NOOP").TrimStart());
               StringAssert.Contains("OK", session.SendCommand("LISTSCRIPTS"));

               for (int i = 1; i <= MaxKeepAliveLines; i++)
               {
                  string answer = session.SendCommand(i % 2 == 0 ? "CAPABILITY" : "NOOP");
                  StringAssert.Contains("OK", answer, "Line " + i + " of " + MaxKeepAliveLines + " that do nothing was not answered.");
               }

               string ended = session.SendCommand("NOOP");
               StringAssert.StartsWith("BYE", ended.TrimStart(),
                  "The line past " + MaxKeepAliveLines + " in a row that did nothing was answered: " + ended);
               Assert.AreEqual(string.Empty, session.ReadUntilClosed(5000), "The session was not closed after BYE.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A signed-in session is ended once it has waited two minutes in all for its commands, even when each command comes in time and does something. Until 6.3.6's re-check a signed-in session had no bound.")]
      public void ASignedInSessionIsEndedAfterItsWaitingTimeInAll()
      {
         const string address = "sieve-idle@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               var watch = Stopwatch.StartNew();
               string last = null;
               while (watch.Elapsed.TotalSeconds < SignedInIdleSeconds + 60)
               {
                  Thread.Sleep(25000);
                  last = session.SendCommand("LISTSCRIPTS");
                  if (!last.TrimStart().StartsWith("OK") && !last.Contains("\r\nOK") && !last.Contains("\nOK"))
                     break;
               }

               double seconds = watch.Elapsed.TotalSeconds;
               Assert.IsNotNull(last);
               StringAssert.StartsWith("BYE", last.TrimStart(),
                  "A session that waited " + (int) seconds + " s in all for its commands was still served: " + last);
               Assert.IsTrue(seconds >= SignedInIdleSeconds - 5,
                  "The session was ended after " + (int) seconds + " s, before its " + SignedInIdleSeconds + " s of waiting.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("Six connections from one address that do not sign in do not keep a client at another address waiting: it is greeted within 90 seconds (at once). Until 6.3.6's re-check the first held the one thread and five more filled the backlog, each for up to a minute.")]
      public void ConnectionsFromOneAddressThatDoNotSignInDoNotHoldTheService()
      {
         StartListener();

         var holders = new List<TcpClient>();
         try
         {
            // The listener is up once a session is greeted.
            using (var first = new Session())
               StringAssert.Contains("IMPLEMENTATION", first.ReadResponse());

            for (int i = 0; i < 6; i++)
            {
               var holder = new TcpClient();
               holder.Connect("127.0.0.1", ManageSievePort);
               holders.Add(holder);
            }

            var watch = Stopwatch.StartNew();
            using (var other = new Session("127.0.0.2"))
            {
               other.SetReadTimeout(90000);
               string greeting = other.ReadResponse();
               StringAssert.Contains("IMPLEMENTATION", greeting,
                  "A client at another address was not greeted within 90 seconds behind six connections from one address that did not sign in.");
               Assert.Less(watch.Elapsed.TotalSeconds, 90.0);
            }
         }
         finally
         {
            foreach (TcpClient holder in holders)
               holder.Close();
            StopListener();
         }
      }

      // An IPv4 address of this machine that is not a loopback address, or null:
      // what a connection from another computer looks like to the listener, as
      // near as one machine can make it.
      private static string ThisMachinesOtherAddress()
      {
         foreach (System.Net.NetworkInformation.NetworkInterface adapter in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
         {
            if (adapter.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up ||
                adapter.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback)
               continue;

            foreach (System.Net.NetworkInformation.UnicastIPAddressInformation unicast in adapter.GetIPProperties().UnicastAddresses)
            {
               System.Net.IPAddress address = unicast.Address;
               if (address.AddressFamily != AddressFamily.InterNetwork || System.Net.IPAddress.IsLoopback(address))
                  continue;
               byte[] bytes = address.GetAddressBytes();
               if (bytes[0] == 169 && bytes[1] == 254)
                  continue;
               return address.ToString();
            }
         }

         return null;
      }

      // One connection from localAddress to the listener at serverAddress that
      // sends nothing and is cut off at its line's deadline; then whether the next
      // connection from the same address is greeted.
      private bool NextIsGreetedAfterASilentConnection(string localAddress, string serverAddress)
      {
         using (var silent = new Session(localAddress, serverAddress))
         {
            silent.ReadResponse(); // greeting

            // Nothing sent: cut off when the line's deadline passes.
            Assert.AreEqual(string.Empty, silent.ReadUntilClosed((LineSeconds + 15) * 1000).Trim(),
               "The silent connection was answered, or still open after " + (LineSeconds + 15) + " s.");
         }

         using (var next = new Session(localAddress, serverAddress))
         {
            string received = next.ReadUntilClosed(5000);
            return received.Contains("IMPLEMENTATION");
         }
      }

      [Test]
      [Description("A connection that runs out of time before signing in counts as a failed logon from its address, so that a client coming back to hold the service is auto-banned as a guesser is. With one failure the threshold, the next connection from the address is dropped before the greeting. From an address of this machine that is not a loopback one, the nearest one machine has to another computer.")]
      public void AConnectionThatRunsOutOfTimeBeforeSigningInCountsTowardsTheAutoBan()
      {
         string address = ThisMachinesOtherAddress();
         if (address == null)
            Assert.Ignore("This machine has no IPv4 address but a loopback one to connect from.");

         StartListener(address);

         // StartListener turns the auto-ban off for the fixture; on for this test, at
         // one failure.
         int attemptsBefore = _settings.MaxInvalidLogonAttempts;
         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 1;

         try
         {
            Assert.IsFalse(NextIsGreetedAfterASilentConnection(address, address),
               "The address " + address + " of a connection that ran out of time before signing in was not banned: the next one was greeted.");
         }
         finally
         {
            _settings.AutoBanOnLogonFailure = false;
            _settings.MaxInvalidLogonAttempts = attemptsBefore;
            _settings.ClearLogonFailureList();
            DeleteAutoBanRanges();
            StopListener();
         }
      }

      [Test]
      [Description("A connection from this computer's loopback address - a webmail beside the server - that runs out of time before signing in is not counted towards the auto-ban, as SMTP's ban for AUTH on a delivery port spares the loopback: with one failure the threshold, the next connection from 127.0.0.1 is still greeted. Until 6.3.6's last re-check one such timeout banned it.")]
      public void AConnectionFromThisComputerThatRunsOutOfTimeIsNotCountedTowardsTheAutoBan()
      {
         StartListener();

         int attemptsBefore = _settings.MaxInvalidLogonAttempts;
         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 1;

         try
         {
            Assert.IsTrue(NextIsGreetedAfterASilentConnection(null, "127.0.0.1"),
               "127.0.0.1 was banned after one connection from it ran out of time before signing in: a webmail on this computer would be banned for everyone it serves.");
         }
         finally
         {
            _settings.AutoBanOnLogonFailure = false;
            _settings.MaxInvalidLogonAttempts = attemptsBefore;
            _settings.ClearLogonFailureList();
            DeleteAutoBanRanges();
            StopListener();
         }
      }

      [Test]
      [Description("An account has at most four ManageSieve sessions at once: a fifth sign-in is answered NO (TRYLATER), and once one of the four has ended the account signs in again.")]
      public void AnAccountHasAtMostFourSessions()
      {
         const string address = "sieve-four@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         var sessions = new List<Session>();
         try
         {
            for (int i = 0; i < MaxSessionsPerAccount; i++)
            {
               var session = new Session();
               sessions.Add(session);
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart(), "Sign-in " + (i + 1) + " was refused.");
            }

            using (var fifth = new Session())
            {
               fifth.ReadResponse(); // greeting
               string refused = fifth.Authenticate(address, password);
               StringAssert.StartsWith("NO", refused.TrimStart(), "A fifth session of one account was signed in.");
               StringAssert.Contains("TRYLATER", refused);
            }

            StringAssert.StartsWith("OK", sessions[0].SendCommand("LOGOUT").TrimStart());
            sessions[0].Dispose();
            sessions.RemoveAt(0);

            // The count is let go with the session; the server may take a moment.
            string again = null;
            for (int attempt = 0; attempt < 20; attempt++)
            {
               using (var later = new Session())
               {
                  later.ReadResponse(); // greeting
                  again = later.Authenticate(address, password);
               }
               if (again.TrimStart().StartsWith("OK"))
                  break;
               Thread.Sleep(250);
            }
            StringAssert.StartsWith("OK", again.TrimStart(), "The account could not sign in again after one of its sessions ended.");
         }
         finally
         {
            foreach (Session session in sessions)
               session.Dispose();
            StopListener();
         }
      }

      [Test]
      [Description("UNAUTHENTICATE takes the address's one turn before signing in back: while another connection from the address is signing in, it is answered NO (TRYLATER) and the session stays signed in, and once that connection has gone it is taken. As 6.3.6's re-check was first written one password let one address hold every serving thread before signing in, each connection signed in and UNAUTHENTICATEd.")]
      public void UnauthenticateWaitsForItsAddresssTurn()
      {
         const string address = "sieve-unauth@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var signedIn = new Session())
            {
               signedIn.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", signedIn.Authenticate(address, password).TrimStart());

               using (var signingIn = new Session())
               {
                  // Served beside the signed-in session, and holding its address's turn.
                  StringAssert.Contains("IMPLEMENTATION", signingIn.ReadResponse(),
                     "A second connection from the address was not greeted beside a signed-in session.");

                  string refused = signedIn.SendCommand("UNAUTHENTICATE");
                  StringAssert.StartsWith("NO", refused.TrimStart(),
                     "UNAUTHENTICATE was taken while another connection from the address was signing in: " + refused);
                  StringAssert.Contains("TRYLATER", refused);
                  StringAssert.Contains("OK", signedIn.SendCommand("LISTSCRIPTS"),
                     "The session whose UNAUTHENTICATE was refused is no longer signed in.");

                  StringAssert.StartsWith("OK", signingIn.SendCommand("LOGOUT").TrimStart());
               }

               // The turn is let go with the connection; the server may take a moment.
               string taken = null;
               for (int attempt = 0; attempt < 20; attempt++)
               {
                  taken = signedIn.SendCommand("UNAUTHENTICATE");
                  if (taken.TrimStart().StartsWith("OK"))
                     break;
                  Thread.Sleep(250);
               }
               StringAssert.StartsWith("OK", taken.TrimStart(), "UNAUTHENTICATE was refused after the other connection had gone.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A reinitialise ends a signed-in session that keeps sending commands at its next line rather than waiting for its client, which it did until the fix: Stop joins the threads that serve, and such a client held the reinitialise - and every protocol's restart behind it - for as long as it went on.")]
      public void AReinitialiseEndsASessionThatKeepsSendingCommands()
      {
         const string address = "sieve-busy@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();
         bool stopped = false;

         try
         {
            using (var busy = new Session())
            {
               busy.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", busy.Authenticate(address, password).TrimStart());

               // A command that does something every tenth of a second, for up to
               // ninety seconds or until the server ends the session.
               var clientWatch = Stopwatch.StartNew();
               Task client = Task.Run(() =>
               {
                  while (clientWatch.Elapsed.TotalSeconds < 90)
                  {
                     string answer;
                     try
                     {
                        answer = busy.SendCommand("LISTSCRIPTS");
                     }
                     catch (IOException)
                     {
                        return;
                     }

                     if (!answer.Contains("OK"))
                        return;

                     Thread.Sleep(100);
                  }
               });

               Thread.Sleep(1000);

               var stopping = Stopwatch.StartNew();
               StopListener();
               stopped = true;
               double seconds = stopping.Elapsed.TotalSeconds;

               Assert.IsTrue(client.Wait(TimeSpan.FromSeconds(100)), "The client did not finish.");
               Assert.Less(seconds, 40.0,
                  "The reinitialise took " + seconds.ToString("0.0") + " s while a signed-in client kept sending commands.");
               Assert.Less(clientWatch.Elapsed.TotalSeconds, 60.0, "The busy session was not ended by the reinitialise.");
            }
         }
         finally
         {
            if (!stopped)
               StopListener();
         }
      }

      // An auto-ban on 127.0.0.1 would block every other protocol for anything that
      // ran afterwards.
      private void DeleteAutoBanRanges()
      {
         SecurityRanges ranges = _settings.SecurityRanges;

         for (int i = ranges.Count - 1; i >= 0; i--)
         {
            if (ranges[i].Name.StartsWith("Auto-ban:", StringComparison.OrdinalIgnoreCase))
               ranges.Delete(i);
         }
      }

      [Test]
      [Description("A command line that arrives an octet at a time is cut off when its deadline passes, and the next client is served; until 6.3.6 each octet renewed a 30-second read timeout and the line held the one-at-a-time listener for as long as octets kept coming.")]
      public void ALineSentAnOctetAtATimeIsCutOffAtItsDeadline()
      {
         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               // An octet a second, and never a line break.
               double seconds = session.TrickleUntilClosed(Encoding.ASCII.GetBytes("NOOP"), 1000, LineSeconds + 20);

               Assert.IsTrue(seconds >= 0,
                  "The connection was still open after " + (LineSeconds + 20) + " seconds of a line that never ended: nothing bounds a line as a whole.");
               Assert.IsTrue(seconds >= LineSeconds - 5,
                  "The line was cut after " + seconds + " seconds, well before its " + LineSeconds + "-second deadline.");
            }

            // The listener is free again, and serves the next client at once.
            using (var session = new Session())
            {
               StringAssert.Contains("OK", session.ReadResponse(), "The next client is greeted.");
               StringAssert.Contains("IMPLEMENTATION", session.SendCommand("CAPABILITY"));
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A client that sends complete commands but never signs in is cut off when the pre-authentication deadline passes, well before the command budget would stop it.")]
      public void AClientThatNeverSignsInIsCutOffAtThePreAuthenticationDeadline()
      {
         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting

               // A NOOP every ten seconds: each line arrives whole and in time, and
               // seven of them are far inside the budget of 25. Until 6.3.6 the
               // session lasted until the budget was spent, some four minutes.
               var watch = System.Diagnostics.Stopwatch.StartNew();
               bool closed = false;
               while (watch.Elapsed.TotalSeconds < PreAuthenticationSeconds + 20)
               {
                  string response;
                  try
                  {
                     response = session.SendCommand("NOOP");
                  }
                  catch (IOException)
                  {
                     closed = true;
                     break;
                  }

                  if (response.Length == 0)
                  {
                     closed = true;
                     break;
                  }

                  System.Threading.Thread.Sleep(10000);
               }

               double seconds = watch.Elapsed.TotalSeconds;
               Assert.IsTrue(closed,
                  "A client that never signed in was still served after " + (int) seconds + " seconds; the pre-authentication phase has no deadline.");
               Assert.IsTrue(seconds >= PreAuthenticationSeconds - 5,
                  "The session was cut after " + (int) seconds + " seconds, well before the " + PreAuthenticationSeconds + "-second deadline.");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("HAVESPACE answers truthfully against the script size limit instead of returning OK to everything.")]
      public void HaveSpaceAnswersAgainstTheScriptSizeLimit()
      {
         const string address = "sieve-sec-havespace@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               // Before the fix this returned a bare OK: the command was a stub that
               // ignored its arguments. A client that trusted it then went on to
               // PUTSCRIPT and had its connection closed without a response.
               string tooBig = session.SendCommand("HAVESPACE \"sec-havespace\" " + (MaxScriptSize + 1000000));
               StringAssert.StartsWith("NO", tooBig.TrimStart(),
                  "HAVESPACE for a script over the limit should be refused. Got: " + tooBig);
               StringAssert.Contains("QUOTA/MAXSIZE", tooBig,
                  "The refusal should carry the RFC 5804 QUOTA/MAXSIZE response code, which is how a client " +
                  "tells 'too big' from any other failure. Got: " + tooBig);

               // Negative control: a script of an ordinary size is still permitted, so
               // this has not simply broken the command in the other direction.
               string ordinary = session.SendCommand("HAVESPACE \"sec-havespace\" 1024");
               StringAssert.StartsWith("OK", ordinary.TrimStart(),
                  "HAVESPACE for an ordinary script size should still be granted. Got: " + ordinary);

               StringAssert.StartsWith("OK", session.SendCommand("LOGOUT").TrimStart());
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("PUTSCRIPT refuses a script announced over the size limit with QUOTA/MAXSIZE, instead of closing the connection without a response.")]
      public void PutScriptRefusesAnOversizedScriptWithAResponse()
      {
         const string address = "sieve-sec-putscript@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               // The literal is announced but deliberately not sent. The fixed server
               // refuses on the announced size without reading a byte, so it answers
               // immediately.
               //
               // Against the unfixed server this test fails by timing out here: it
               // accepted the announced length, sat in ReadBytes_ waiting for two
               // megabytes that never arrive, and eventually closed the connection on
               // the 30-second socket timeout without ever sending a response. That is
               // the behaviour being fixed - a client whose script is too big is told
               // nothing at all. The read timeout below is shortened so the failure
               // does not cost the suite the full 30 seconds.
               session.SetReadTimeout(10000);
               session.SendRaw("PUTSCRIPT \"sec-toobig\" {" + (MaxScriptSize + 1000000) + "+}\r\n");

               string response = session.ReadResponse();

               StringAssert.StartsWith("NO", response.TrimStart(),
                  "An oversized PUTSCRIPT should be refused with a response, not silence. Got: " + response);
               StringAssert.Contains("QUOTA/MAXSIZE", response,
                  "The refusal should carry the RFC 5804 QUOTA/MAXSIZE response code. Got: " + response);
            }

            // Negative control: an ordinary script still uploads. Without this the
            // test above would also pass against a server that refused every
            // PUTSCRIPT.
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               const string scriptBody = "require \"fileinto\";\r\nkeep;\r\n";
               byte[] payload = Encoding.UTF8.GetBytes(scriptBody);

               session.SendRaw("PUTSCRIPT \"sec-ordinary\" {" + payload.Length + "+}\r\n");
               session.SendRaw(scriptBody + "\r\n");

               string put = session.ReadResponse();
               StringAssert.StartsWith("OK", put.TrimStart(),
                  "An ordinary script should still be accepted. Got: " + put);

               session.SendCommand("DELETESCRIPT \"sec-ordinary\"");
               session.SendCommand("LOGOUT");
            }
         }
         finally
         {
            StopListener();
         }
      }

      [Test]
      [Description("A literal length too large to represent as an int is rejected as a parse failure rather than being passed through _wtoi and trusted.")]
      public void OverflowingLiteralLengthIsRejected()
      {
         const string address = "sieve-sec-overflow@example.test";
         const string password = "Sieve-Pass1";
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, password);

         StartListener();

         try
         {
            using (var session = new Session())
            {
               session.ReadResponse(); // greeting
               StringAssert.StartsWith("OK", session.Authenticate(address, password).TrimStart());

               // 99999999999 does not fit in an int. The old code handed it to _wtoi,
               // whose behaviour on overflow is undefined, and then used whatever came
               // back as a byte count: depending on what that was, the unfixed server
               // either blocked waiting for a payload that will never arrive until the
               // 30-second socket timeout closed the connection with no response, or
               // read zero bytes and cheerfully stored an empty script under the name.
               // Either way it does not answer "Expected a script literal", so this
               // assertion fails against it.
               session.SetReadTimeout(10000);
               session.SendRaw("PUTSCRIPT \"sec-overflow\" {99999999999+}\r\n");

               string response = session.ReadResponse();

               StringAssert.Contains("Expected a script literal", response,
                  "A literal length that overflows an int should be a parse failure. Got: " + response);

               // The stream is still synchronized - nothing was announced that the
               // server half-read - so the session carries on. That is the difference
               // between rejecting the length and trusting it.
               string list = session.SendCommand("LISTSCRIPTS");
               Assert.IsFalse(list.Contains("\"sec-overflow\""),
                  "No script should have been created by the rejected command. Got: " + list);

               StringAssert.StartsWith("OK", session.SendCommand("LOGOUT").TrimStart());
            }
         }
         finally
         {
            StopListener();
         }
      }
   }
}
