// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net.Sockets;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    MaxNumberOfInvalidCommands (upstream issue 653, PR 664): which 5xx replies count
   ///    towards it, which do not, and where the count starts again.
   ///
   ///    Every reply the counted tests provoke went out through SendResponse_, which
   ///    does not count, until the fix - so with DisconnectInvalidClients on, a client
   ///    could send TURN, VRFY, a line too long, a command out of sequence, an XCLIENT
   ///    it may not use or a message the server refuses as often as it liked, while a
   ///    verb the server does not know or a HELO without a name was counted and the
   ///    session closed past the limit. Each test of a counted reply therefore fails on
   ///    the code before the fix at its fourth attempt, which was answered as the first
   ///    three were; so do the tests of where the count starts again, which count the
   ///    same replies. The three controls at the end passed before and pass after: a
   ///    4xx is not counted, nothing is with the limit off, and a counted refusal whose
   ///    text happens to read "Unknown user" refuses no recipient and does not fire
   ///    OnRecipientUnknown.
   ///
   ///    The limit is three in every test, so the fourth counted reply is the 421 that
   ///    closes the session. The sessions greet with HELO, so their replies carry no
   ///    RFC 3463 code, but for the one that greets with EHLO to show that a counted
   ///    reply keeps its own. Codes and texts are SMTPConnection's, unchanged by the fix.
   /// </summary>
   [TestFixture]
   public class InvalidCommandLimit : TestFixtureBase
   {
      private const int Limit = 3;
      private const string Recipient = "invalid-limit@example.test";
      private const string Sender = "sender@example.com";
      private const string BareLineFeeds = "Subject: bare\r\n\r\nLine 1\nLine 2";

      // Long enough for any reply these tests wait for; short enough that a session
      // which neither answers nor closes fails its test rather than holding the run.
      private const int ReceiveTimeoutMilliseconds = 30000;

      private bool _scriptInstalled;
      private int _receiveTimeoutBefore;

      // Named apart from the base class's SetUp and TearDown, which run first and last
      // and which methods of the same names would hide.
      [SetUp]
      public void LimitOfThree()
      {
         _settings.DisconnectInvalidClients = true;
         _settings.MaxNumberOfInvalidCommands = Limit;
         _scriptInstalled = false;

         // A read on the Windows bench otherwise waits for ever (TestPorts): set before
         // any connection is made, since a connection takes the value it finds then.
         _receiveTimeoutBefore = TestPorts.ReceiveTimeoutMilliseconds;
         if (_receiveTimeoutBefore == 0 || _receiveTimeoutBefore > ReceiveTimeoutMilliseconds)
            TestPorts.ReceiveTimeoutMilliseconds = ReceiveTimeoutMilliseconds;
      }

      [TearDown]
      public void PutBackWhatTheTestChanged()
      {
         TestPorts.ReceiveTimeoutMilliseconds = _receiveTimeoutBefore;

         if (!_scriptInstalled)
            return;

         // The next test's set-up switches scripting off, but leaves the file: a test
         // that switched it on again over the same file would meet these handlers.
         var scripting = _application.Settings.Scripting;
         scripting.Enabled = false;

         var file = scripting.CurrentScriptFile;
         if (File.Exists(file))
            File.WriteAllText(file, string.Empty);

         scripting.Reload();
      }

      private static TcpConnection ConnectAndHelo()
      {
         var socket = new TcpConnection();
         Assert.IsTrue(socket.Connect(25));

         var banner = socket.Receive();
         Assert.IsTrue(banner.StartsWith("220"), "No banner. Got: " + banner);

         var helo = socket.SendAndReceive("HELO example.com\r\n");
         Assert.IsTrue(helo.StartsWith("250"), "HELO was not accepted. Got: " + helo);

         return socket;
      }

      private static void Setup(TcpConnection socket, string commands)
      {
         if (string.IsNullOrEmpty(commands))
            return;

         foreach (var command in commands.Split('|'))
         {
            var reply = socket.SendAndReceive(command + "\r\n");
            Assert.IsTrue(reply.StartsWith("250"), "The set-up command " + command + " was not accepted. Got: " + reply);
         }
      }

      /// <summary>
      ///    The first <see cref="Limit"/> attempts are answered with the refusal itself,
      ///    the next with the 421 that closes the session, and then the session is gone.
      /// </summary>
      private static void AssertCountedUpToTheLimit(TcpConnection socket, Func<TcpConnection, string> attempt,
         string expected, Action<TcpConnection> between = null)
      {
         for (var i = 1; i <= Limit; i++)
         {
            var reply = attempt(socket);
            Assert.IsTrue(reply.StartsWith(expected),
               "Attempt " + i + " must be answered \"" + expected + "\". Got: " + reply);

            if (between != null)
               between(socket);
         }

         var last = attempt(socket);
         Assert.IsTrue(last.StartsWith("421 Too many invalid commands"),
            "Attempt " + (Limit + 1) + " takes the count past the limit of " + Limit +
            " and must be answered with the 421 that closes the session. Got: " + last);

         AssertSessionClosed(socket);
      }

      private static void AssertSessionClosed(TcpConnection socket)
      {
         string answer;

         try
         {
            socket.Send("NOOP\r\n");
            answer = socket.Receive();
         }
         catch (IOException waited) when (waited.InnerException is SocketException timedOut &&
                                          timedOut.SocketErrorCode == SocketError.TimedOut)
         {
            Assert.Fail("The session neither answered NOOP nor closed within " +
                        TestPorts.ReceiveTimeoutMilliseconds + " ms of the 421 that said it would close.");
            return;
         }
         catch (Exception gone) when (gone is IOException || gone is SocketException ||
                                      gone is InvalidOperationException || gone is ObjectDisposedException)
         {
            // The server has closed the connection, which is what the 421 said it would do.
            return;
         }

         Assert.AreEqual(string.Empty, answer, "The session went on after the 421: NOOP was answered " + answer);
      }

      /// <summary>
      ///    MAIL, RCPT, DATA and the body: the first reply that is not a step forward,
      ///    which is the refusal of the message - or the 421 in its place.
      /// </summary>
      private static string SendMessage(TcpConnection socket, string body)
      {
         var reply = socket.SendAndReceive("MAIL FROM:<" + Sender + ">\r\n");
         if (!reply.StartsWith("250"))
            return reply;

         reply = socket.SendAndReceive("RCPT TO:<" + Recipient + ">\r\n");
         if (!reply.StartsWith("250"))
            return reply;

         reply = socket.SendAndReceive("DATA\r\n");
         if (!reply.StartsWith("354"))
            return reply;

         socket.Send(body + "\r\n.\r\n");
         return socket.ReadUntil("\r\n");
      }

      private static void Rset(TcpConnection socket)
      {
         var reply = socket.SendAndReceive("RSET\r\n");
         Assert.IsTrue(reply.StartsWith("250"), "RSET was not accepted. Got: " + reply);
      }

      /// <summary>
      ///    A refusal of a message is made on the accept thread once the data is in,
      ///    and the one that passes the limit leaves that thread with no command mode
      ///    to go back to. It must end there - not arm a read on a connection that is
      ///    closing, which throws out of the task and puts "Asynchronous task ... did
      ///    not finish" in the application log for a session that ended as it should.
      /// </summary>
      private static void AssertTheAcceptTaskEndedQuietly()
      {
         Thread.Sleep(1000);

         var log = LogHandler.ReadCurrentDefaultLog();
         Assert.IsFalse(log.Contains("did not finish"),
            "The accept task of the refused message did not finish cleanly:\r\n" + log);
      }

      /// <summary>
      ///    The event handlers in the language the server under test runs: JScript on the
      ///    Windows builds this project tests, JavaScript on the Linux builds the Linux
      ///    project tests (ScriptLanguages.JavaScriptRequired is that project's yes). The
      ///    text is the same in both.
      /// </summary>
      private void InstallScript(string script)
      {
         var scripting = _settings.Scripting;
         scripting.Language = ScriptLanguages.JavaScriptRequired ? "JavaScript" : "JScript";

         _scriptInstalled = true;
         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();
      }

      private void InstallHandler(string signature, int resultValue)
      {
         InstallScript(
            "function " + signature + " {\r\n" +
            "   Result.Value = " + resultValue + ";\r\n" +
            "   Result.Message = \"Rejected by script\";\r\n" +
            "}\r\n");
      }

      // ------------------------------------------------------------------ counted

      [TestCase("", "VRFY postmaster@example.test", "502 VRFY disallowed.")]
      [TestCase("", "TURN", "502 TURN disallowed.")]
      [TestCase("", "RCPT TO:<" + Recipient + ">", "503 Must have sender first.")]
      [TestCase("", "DATA", "503 Must have sender and recipient first.")]
      [TestCase("MAIL FROM:<" + Sender + ">", "DATA", "503 Must have sender and recipient first.")]
      [TestCase("MAIL FROM:<" + Sender + ">", "MAIL FROM:<" + Sender + ">", "503 Issue a reset if you want to start over")]
      [TestCase("MAIL FROM:<" + Sender + ">|RCPT TO:<" + Recipient + ">|BDAT 0", "DATA",
         "503 Bad sequence of commands: BDAT already used in this transaction.")]
      [TestCase("MAIL FROM:<" + Sender + "> BODY=BINARYMIME|RCPT TO:<" + Recipient + ">", "DATA",
         "503 Bad sequence of commands: DATA is not permitted with BODY=BINARYMIME")]
      [TestCase("", "XCLIENT ADDR=192.0.2.1", "550 Error: insufficient authorization.")]
      [TestCase("", "ETRN example.test", "501 ETRN not supported for example.test")]
      [Description("A command the client should not have sent is counted, whatever its reply code: " +
                   "TURN, VRFY, a command out of sequence, XCLIENT from a peer that may not use it, " +
                   "ETRN for a domain that is not a route.")]
      public void CommandsTheClientShouldNotHaveSentAreCounted(string setup, string command, string expected)
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         using (var socket = ConnectAndHelo())
         {
            Setup(socket, setup);
            AssertCountedUpToTheLimit(socket, s => s.SendAndReceive(command + "\r\n"), expected);
         }
      }

      [Test]
      [Description("BDAT without a sender and a recipient is counted, as DATA's is.")]
      public void BdatWithoutSenderAndRecipientIsCounted()
      {
         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s =>
            {
               // RFC 3030 lets a client send the chunk without waiting for the reply.
               // It is sent after the reply here, and only after a refusal: the server
               // reads and drops the five octets of a refused BDAT, and the session the
               // 421 closes is left holding nothing it has not read - a socket closed
               // over unread octets is reset, and the reset can overtake the 421.
               var reply = s.SendAndReceive("BDAT 5\r\n");
               if (reply.StartsWith("503"))
                  s.Send("ABCDE");

               return reply;
            }, "503 Must have sender and recipient first.");
         }
      }

      [Test]
      [Description("A command line over 510 octets is counted.")]
      public void LineTooLongIsCounted()
      {
         var line = "NOOP " + new string('A', 600) + "\r\n";

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => s.SendAndReceive(line), "500 Line too long.");
         }
      }

      [TestCase("XCLIENT", "501 Bad command parameter syntax.")]
      [TestCase("XCLIENT NAME", "501 Bad command parameter syntax.")]
      [TestCase("XCLIENT DESTINY=yes", "501 Bad command parameter syntax.")]
      [TestCase("XCLIENT ADDR=not-an-address", "501 Bad ADDR syntax.")]
      [TestCase("XCLIENT PORT=65536", "501 Bad PORT syntax.")]
      [TestCase("XCLIENT PROTO=UUCP", "501 Bad PROTO syntax.")]
      [TestCase("MAIL FROM:<" + Sender + ">|XCLIENT ADDR=192.0.2.1", "503 XCLIENT is not permitted inside a mail transaction.")]
      [Description("XCLIENT from a trusted peer that is malformed, or sent inside a transaction, is counted.")]
      public void MalformedXclientFromATrustedPeerIsCounted(string commands, string expected)
      {
         var parts = commands.Split('|');
         var setup = parts.Length > 1 ? parts[0] : "";
         var command = parts[parts.Length - 1];

         try
         {
            IniFileSetting.Write("SMTPXClientEnabled", "1");
            IniFileSetting.Write("SMTPXClientTrustedIPs", "127.0.0.1");
            _application.Reinitialize();

            using (var socket = ConnectAndHelo())
            {
               Setup(socket, setup);
               AssertCountedUpToTheLimit(socket, s => s.SendAndReceive(command + "\r\n"), expected);
            }
         }
         finally
         {
            IniFileSetting.Delete("SMTPXClientEnabled");
            IniFileSetting.Delete("SMTPXClientTrustedIPs");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A SIZE= declaration over the maximum message size is counted.")]
      public void DeclaredSizeOverTheMaximumIsCounted()
      {
         _settings.MaxMessageSize = 1; // KB

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => s.SendAndReceive("MAIL FROM:<" + Sender + "> SIZE=100000\r\n"),
               "552 Message size exceeds fixed maximum message size.");
         }
      }

      [Test]
      [Description("A message found to be over the maximum size once its data is in is counted.")]
      public void MessageOverTheMaximumSizeIsCounted()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");
         _settings.MaxMessageSize = 1; // KB

         var body = new string('A', 70) + "\r\n";
         for (var i = 0; i < 5; i++)
            body += body;

         LogHandler.DeleteCurrentDefaultLog();

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => SendMessage(s, body),
               "552 Rejected - Message size exceeds fixed maximum message size.", Rset);
         }

         AssertTheAcceptTaskEndedQuietly();
      }

      [Test]
      [Description("A message refused for bare LFs is counted.")]
      public void MessageWithBareLineFeedsIsCounted()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         var allowed = _settings.AllowIncorrectLineEndings;
         _settings.AllowIncorrectLineEndings = false;

         try
         {
            LogHandler.DeleteCurrentDefaultLog();

            using (var socket = ConnectAndHelo())
            {
               AssertCountedUpToTheLimit(socket, s => SendMessage(s, BareLineFeeds),
                  "554 Rejected - Message containing bare LF's.", Rset);
            }

            AssertTheAcceptTaskEndedQuietly();
         }
         finally
         {
            _settings.AllowIncorrectLineEndings = allowed;
         }
      }

      [Test]
      [Description("A message whose transmission was cancelled - a line over 100,000 octets - is counted.")]
      public void CancelledTransmissionIsCounted()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         var body = new string('A', 100001);

         LogHandler.DeleteCurrentDefaultLog();

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => SendMessage(s, body),
               "554 Too long line was received. Transmission aborted.", Rset);
         }

         AssertTheAcceptTaskEndedQuietly();
      }

      [Test]
      [Description("A message the spam score refuses is counted.")]
      public void SpamRefusalIsCounted()
      {
         // Behind an incoming relay the tests run on the originating client the Received
         // field names, after the data, so the HELO address literal in it is compared
         // with that client's address without a DNS lookup - and the client is not the
         // loopback address, which the HELO test passes over. First, before any setting
         // is changed: the Linux project's REST API has no incoming relays, and its
         // IncomingRelays skips the test here (dynamic, because its Add has no relay
         // type to return).
         dynamic relay = _settings.IncomingRelays.Add();
         relay.Name = "Invalid command limit";
         relay.LowerIP = "127.0.0.1";
         relay.UpperIP = "127.0.0.1";
         relay.Save();

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         var antiSpam = _settings.AntiSpam;
         antiSpam.SpamDeleteThreshold = 1;
         antiSpam.CheckHostInHelo = true;
         antiSpam.CheckHostInHeloScore = 5;

         var message = "Received: from [198.51.100.7] (unknown [203.0.113.99])\r\n" +
                       "\tby mail.example.test with ESMTP\r\n" +
                       "\t; Mon, 12 Oct 2026 21:00:00 +0100\r\n" +
                       "Subject: Spam\r\n" +
                       "\r\n" +
                       "Body";

         LogHandler.DeleteCurrentDefaultLog();

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => SendMessage(s, message),
               "550 The host name specified in HELO does not match IP address.", Rset);
         }

         AssertTheAcceptTaskEndedQuietly();
      }

      [TestCase("OnSMTPData(oClient, oMessage)")]
      [TestCase("OnAcceptMessage(oClient, oMessage)")]
      [Description("A message an event script refuses with 554 is counted.")]
      public void ScriptRefusalOfAMessageIsCounted(string signature)
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");
         InstallHandler(signature, 2);

         LogHandler.DeleteCurrentDefaultLog();

         using (var socket = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(socket, s => SendMessage(s, "Subject: script\r\n\r\nBody"),
               "554 Rejected by script", Rset);
         }

         AssertTheAcceptTaskEndedQuietly();
      }

      [TestCase("HELO example.com")]
      [TestCase("EHLO example.com")]
      [Description("A HELO or EHLO an OnHELO script refuses with 554 is counted.")]
      public void ScriptRefusalOfTheGreetingIsCounted(string greeting)
      {
         InstallHandler("OnHELO(oClient)", 2);

         using (var socket = new TcpConnection())
         {
            Assert.IsTrue(socket.Connect(25));
            Assert.IsTrue(socket.Receive().StartsWith("220"));

            AssertCountedUpToTheLimit(socket, s => s.SendAndReceive(greeting + "\r\n"), "554 Rejected by script");
         }
      }

      [Test]
      [Description("In an ESMTP session a counted reply keeps the RFC 3463 code it had before it was counted, " +
                   "and the 421 that closes the session has its own.")]
      public void CountedRepliesKeepTheirEnhancedCodes()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         var allowed = _settings.AllowIncorrectLineEndings;
         _settings.AllowIncorrectLineEndings = false;

         try
         {
            LogHandler.DeleteCurrentDefaultLog();

            using (var socket = new TcpConnection())
            {
               Assert.IsTrue(socket.Connect(25));
               Assert.IsTrue(socket.Receive().StartsWith("220"));

               socket.Send("EHLO example.com\r\n");
               var ehlo = socket.ReadUntil("250 HELP");
               Assert.IsTrue(ehlo.StartsWith("250"), "EHLO was not accepted. Got: " + ehlo);

               // Two of the three codes are not the ones the reply code alone would give
               // (5.7.1 for a 550, 5.5.0 for a 554): each reply carries its own.
               var vrfy = socket.SendAndReceive("VRFY postmaster@example.test\r\n");
               Assert.IsTrue(vrfy.StartsWith("502 5.5.1 VRFY disallowed."), "VRFY. Got: " + vrfy);

               var xclient = socket.SendAndReceive("XCLIENT ADDR=192.0.2.1\r\n");
               Assert.IsTrue(xclient.StartsWith("550 5.7.0 Error: insufficient authorization."), "XCLIENT. Got: " + xclient);

               var bare = SendMessage(socket, BareLineFeeds);
               Assert.IsTrue(bare.StartsWith("554 5.6.0 Rejected - Message containing bare LF's."), "Bare LFs. Got: " + bare);

               Rset(socket);

               var fourth = SendMessage(socket, BareLineFeeds);
               Assert.IsTrue(fourth.StartsWith("421 4.7.0 Too many invalid commands. Bye!"),
                  "The fourth counted reply of the session must be the 421. Got: " + fourth);

               AssertSessionClosed(socket);
            }

            AssertTheAcceptTaskEndedQuietly();
         }
         finally
         {
            _settings.AllowIncorrectLineEndings = allowed;
         }
      }

      // ------------------------------------------------------------------ where the count starts again

      [Test]
      [Description("The count is the connection's: a client closed at the limit starts at zero on its next connection.")]
      public void TheNextConnectionStartsAtZero()
      {
         using (var first = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(first, s => s.SendAndReceive("VRFY postmaster@example.test\r\n"), "502 VRFY disallowed.");
         }

         using (var second = ConnectAndHelo())
         {
            AssertCountedUpToTheLimit(second, s => s.SendAndReceive("VRFY postmaster@example.test\r\n"), "502 VRFY disallowed.");
         }
      }

      [Test]
      [Description("The replies counted before the fix and those counted now make one count, and RSET, " +
                   "a command that succeeds and a delivered message do not reset it.")]
      public void OneCountThatRsetASuccessAndADeliveredMessageLeaveAsItIs()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         using (var socket = ConnectAndHelo())
         {
            // 1: counted now.
            var vrfy = socket.SendAndReceive("VRFY postmaster@example.test\r\n");
            Assert.IsTrue(vrfy.StartsWith("502 VRFY disallowed."), "VRFY. Got: " + vrfy);

            Rset(socket);
            Assert.IsTrue(socket.SendAndReceive("NOOP\r\n").StartsWith("250"));

            // 2: counted before the fix as well - a HELO without a name.
            var helo = socket.SendAndReceive("HELO\r\n");
            Assert.IsTrue(helo.StartsWith("501 HELO Invalid domain address."), "HELO. Got: " + helo);

            var delivered = SendMessage(socket, "Subject: between\r\n\r\nBody");
            Assert.IsTrue(delivered.StartsWith("250"), "The message between them was not accepted. Got: " + delivered);

            // 3: counted now.
            var turn = socket.SendAndReceive("TURN\r\n");
            Assert.IsTrue(turn.StartsWith("502 TURN disallowed."), "TURN. Got: " + turn);

            // 4: a verb this server does not know, counted before the fix as well.
            var fourth = socket.SendAndReceive("XYZZY\r\n");
            Assert.IsTrue(fourth.StartsWith("421 Too many invalid commands"),
               "The fourth counted reply of the session must be the 421, whatever came between. Got: " + fourth);

            AssertSessionClosed(socket);
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      // ------------------------------------------------------------------ not counted, and the controls

      [Test]
      [Description("Control - passed before the fix. A 453, a temporary refusal by a script, is not counted.")]
      public void TemporaryRefusalIsNotCounted()
      {
         InstallHandler("OnHELO(oClient)", 3);

         using (var socket = new TcpConnection())
         {
            Assert.IsTrue(socket.Connect(25));
            Assert.IsTrue(socket.Receive().StartsWith("220"));

            for (var i = 0; i < Limit + 2; i++)
            {
               var reply = socket.SendAndReceive("HELO example.com\r\n");
               Assert.IsTrue(reply.StartsWith("453 Rejected by script"), "Attempt " + (i + 1) + ". Got: " + reply);
            }

            Assert.IsTrue(socket.SendAndReceive("NOOP\r\n").StartsWith("250"));
         }
      }

      [Test]
      [Description("Control - passed before the fix. With DisconnectInvalidClients off, the default, nothing closes the session.")]
      public void NothingIsCountedAgainstASessionWithTheLimitOff()
      {
         _settings.DisconnectInvalidClients = false;

         using (var socket = ConnectAndHelo())
         {
            for (var i = 0; i < Limit + 2; i++)
            {
               var reply = socket.SendAndReceive("VRFY postmaster@example.test\r\n");
               Assert.IsTrue(reply.StartsWith("502 VRFY disallowed."), "Attempt " + (i + 1) + ". Got: " + reply);
            }

            Assert.IsTrue(socket.SendAndReceive("NOOP\r\n").StartsWith("250"));
         }
      }

      [Test]
      [Description("Control - passed before the fix. A counted 554 whose text reads \"Unknown user\" - a script's " +
                   "Result.Message here - refuses no recipient and does not fire OnRecipientUnknown, which the " +
                   "refusal of a recipient the server does not have still fires.")]
      public void OnlyAnUnknownRecipientFiresOnRecipientUnknown()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Recipient, "test");

         InstallScript(
            "function OnSMTPData(oClient, oMessage) {\r\n" +
            "   Result.Value = 2;\r\n" +
            "   Result.Message = \"Unknown user\";\r\n" +
            "}\r\n" +
            "function OnRecipientUnknown(oClient, oMessage) {\r\n" +
            "   EventLog.Write(\"OnRecipientUnknown fired\");\r\n" +
            "}\r\n");

         LogHandler.DeleteEventLog();

         using (var socket = ConnectAndHelo())
         {
            var refused = SendMessage(socket, "Subject: script\r\n\r\nBody");
            Assert.IsTrue(refused.StartsWith("554 Unknown user"), "The script's refusal at DATA. Got: " + refused);

            Rset(socket);

            Assert.IsTrue(socket.SendAndReceive("MAIL FROM:<" + Sender + ">\r\n").StartsWith("250"));
            var unknown = socket.SendAndReceive("RCPT TO:<nobody-here@example.test>\r\n");
            Assert.IsTrue(unknown.StartsWith("550 Unknown user"), "The refusal of an unknown recipient. Got: " + unknown);
         }

         // The session's two refusals were made one after the other on its own thread, so
         // an event the first had fired is in the log before the second's.
         var log = string.Empty;
         for (var i = 0; i < 100 && !log.Contains("OnRecipientUnknown fired"); i++)
         {
            Thread.Sleep(100);
            log = LogHandler.ReadEventLog();
         }

         Assert.AreEqual(1, Regex.Matches(log, "OnRecipientUnknown fired").Count,
            "OnRecipientUnknown must fire once, for the unknown recipient, and not for the script's 554. Event log:\r\n" + log);
      }
   }
}
