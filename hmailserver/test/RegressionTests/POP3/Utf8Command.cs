// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using RegressionTests.SSL;

namespace RegressionTests.POP3
{
   /// <summary>
   ///    The UTF8 command (RFC 6856) after login - work item 85.
   ///
   ///    RFC 6856 lists UTF8 as a command of the AUTHORIZATION state, and the server
   ///    accepted it there alone. Classic Outlook for Windows (2016 to 2024) reads UTF8
   ///    in CAPA and sends the command after USER and PASS instead; the server answered
   ///    "-ERR Invalid command in current state." from the command dispatcher's state
   ///    table, and Outlook gave up with 0x800CCC90 before downloading anything.
   ///    Accepting it there departs from RFC 6856 section 2 ("Commands valid in states:
   ///    AUTHORIZATION") and from RFC 1939 section 3 ("A server MUST respond to a command
   ///    issued when the session is in an incorrect state by responding with a negative
   ///    status indicator"), on purpose and narrowly: after login the server accepts UTF8
   ///    only while the session has issued none of STAT, LIST, UIDL, RETR, TOP, DELE and
   ///    RSET, before which nothing the mode could change has been sent. After one of them
   ///    it is still refused, with a reply that says so, because the mode has to hold for
   ///    every message the session lists or sends.
   ///
   ///    Outlook's own sequence is driven twice: in the clear, and over implicit TLS as the
   ///    work item's reporter ran it (port 995 there, 11001 here).
   /// </summary>
   [TestFixture]
   public class Utf8Command : TestFixtureBase
   {
      internal const string Password = "test";

      // The reply to UTF8 once the session has used the maildrop, as the CHANGELOG quotes
      // it. The base answered "-ERR Invalid command in current state." there.
      private const string RefusedAfterTheMaildropIsUsed =
         "-ERR UTF8 command only valid before the first STAT, LIST, UIDL, RETR, TOP, DELE or RSET.";

      private static TcpConnection ConnectPop3()
      {
         var connection = new TcpConnection();
         Assert.IsTrue(connection.Connect(110), "Could not connect to the POP3 server on port 110.");
         connection.ReadUntil("+OK"); // banner
         return connection;
      }

      internal static void LogOn(TcpConnection connection, string address)
      {
         Assert.IsTrue(connection.SendAndReceive("USER " + address + "\r\n").StartsWith("+OK"),
            "USER was not accepted.");
         Assert.IsTrue(connection.SendAndReceive("PASS " + Password + "\r\n").StartsWith("+OK"),
            "PASS was not accepted.");
      }

      /// <summary>
      ///    An account holding one message with the given body, delivered and counted.
      /// </summary>
      internal static string AddAccountWithOneMessage(hMailServer.Domain domain, string address, string body)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, address, Password);
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Utf8Command", body);
         Pop3ClientSimulator.AssertMessageCount(account.Address, Password, 1);
         return account.Address;
      }

      /// <summary>
      ///    What Outlook sends, from CAPA to QUIT, on a connection already open and greeted,
      ///    and what each reply must be. Shared with the TLS fixture below.
      /// </summary>
      internal static void DriveOutlooksSequence(TcpConnection connection, string address, string body)
      {
         connection.Send("CAPA\r\n");
         var capabilities = connection.ReadUntil("\r\n.\r\n");
         StringAssert.Contains("\r\nUTF8\r\n", capabilities,
            "CAPA must advertise UTF8, which is what makes Outlook send the command. Got: " + capabilities);

         LogOn(connection, address);

         // The defect: "-ERR Invalid command in current state.", on which Outlook gives up
         // with 0x800CCC90 and downloads nothing.
         var utf8 = connection.SendAndReceive("UTF8\r\n");
         Assert.IsTrue(utf8.StartsWith("+OK"),
            "UTF8 sent after PASS, before the maildrop is listed or read, must be accepted. Got: " + utf8);

         var stat = connection.SendAndReceive("STAT\r\n");
         Assert.IsTrue(stat.StartsWith("+OK 1 "), "STAT must report the one message. Got: " + stat);

         connection.Send("UIDL\r\n");
         var uidl = connection.ReadUntil("\r\n.\r\n");
         Assert.IsTrue(uidl.StartsWith("+OK"), "UIDL must be answered. Got: " + uidl);
         StringAssert.Contains("\r\n1 ", uidl, "UIDL must list message 1. Got: " + uidl);

         connection.Send("RETR 1\r\n");
         var message = connection.ReadUntil("\r\n.\r\n");
         Assert.IsTrue(message.StartsWith("+OK"), "RETR 1 must be answered. Got: " + message);
         StringAssert.Contains(body, message, "RETR 1 must send the message. Got: " + message);

         var quit = connection.SendAndReceive("QUIT\r\n");
         Assert.IsTrue(quit.StartsWith("+OK"), "QUIT must be answered. Got: " + quit);
      }

      [Test]
      [Description("Work item 85: Outlook's sequence - CAPA, USER, PASS, UTF8, STAT, UIDL, RETR 1, QUIT - is " +
                   "answered in full, UTF8 after login included.")]
      public void OutlooksSequenceWithUtf8AfterLoginIsAnsweredInFull()
      {
         var address = AddAccountWithOneMessage(_domain, "outlookutf8@example.test", "Utf8CommandOutlookBody");

         using (var connection = ConnectPop3())
            DriveOutlooksSequence(connection, address, "Utf8CommandOutlookBody");
      }

      [Test]
      [Description("NOOP and CAPA after login read nothing of the maildrop, so UTF8 is still accepted after them, " +
                   "and a second UTF8 is answered as the first was.")]
      public void Utf8IsStillAcceptedAfterNoopAndCapa()
      {
         var address = AddAccountWithOneMessage(_domain, "noopthenutf8@example.test", "Utf8CommandNoopBody");

         using (var connection = ConnectPop3())
         {
            LogOn(connection, address);

            Assert.IsTrue(connection.SendAndReceive("NOOP\r\n").StartsWith("+OK"), "NOOP must be answered.");

            connection.Send("CAPA\r\n");
            Assert.IsTrue(connection.ReadUntil("\r\n.\r\n").StartsWith("+OK"), "CAPA must be answered.");

            // The defect, as in Outlook's sequence: "-ERR Invalid command in current state.".
            var first = connection.SendAndReceive("UTF8\r\n");
            Assert.IsTrue(first.StartsWith("+OK"),
               "UTF8 after NOOP and CAPA must be accepted: neither reads the maildrop. Got: " + first);

            var second = connection.SendAndReceive("UTF8\r\n");
            Assert.IsTrue(second.StartsWith("+OK"),
               "A second UTF8 before the maildrop is read must be answered as the first was. Got: " + second);

            // The session is a working one afterwards.
            var stat = connection.SendAndReceive("STAT\r\n");
            Assert.IsTrue(stat.StartsWith("+OK 1 "), "STAT must report the one message. Got: " + stat);

            connection.Send("QUIT\r\n");
            connection.ReadUntil("+OK");
         }
      }

      [TestCase("STAT", false)]
      [TestCase("LIST", true)]
      [TestCase("UIDL", true)]
      [TestCase("RETR 1", true)]
      [TestCase("TOP 1 0", true)]
      [TestCase("DELE 1", false)]
      [TestCase("RSET", false)]
      [Description("The mode has to hold for every message the session lists or sends, so UTF8 after any of STAT, " +
                   "LIST, UIDL, RETR, TOP, DELE and RSET is still refused, with the reply that says why, and the " +
                   "session stays in step.")]
      public void Utf8AfterTheMaildropIsUsedIsStillRefused(string command, bool multiLine)
      {
         var address = AddAccountWithOneMessage(_domain, "utf8afteruse@example.test", "Utf8CommandAfterUseBody");

         using (var connection = ConnectPop3())
         {
            LogOn(connection, address);

            string reply;
            if (multiLine)
            {
               connection.Send(command + "\r\n");
               reply = connection.ReadUntil("\r\n.\r\n");
            }
            else
            {
               reply = connection.SendAndReceive(command + "\r\n");
            }

            Assert.IsTrue(reply.StartsWith("+OK"), command + " must be answered. Got: " + reply);

            // Negative control of the fix: it accepts UTF8 after login only until the
            // maildrop is used, not for the whole session - and the refusal is its own,
            // not the command table's "-ERR Invalid command in current state.".
            var utf8 = connection.SendAndReceive("UTF8\r\n");
            Assert.IsTrue(utf8.StartsWith(RefusedAfterTheMaildropIsUsed),
               "UTF8 after " + command + " must be refused with \"" + RefusedAfterTheMaildropIsUsed +
               "\": the maildrop has been used. Got: " + utf8);

            // A refusal, not a broken session: the next command is read as a command.
            Assert.IsTrue(connection.SendAndReceive("NOOP\r\n").StartsWith("+OK"),
               "The session must stay in step after the refused UTF8.");

            connection.Send("QUIT\r\n");
            connection.ReadUntil("+OK");
         }
      }

      [Test]
      [Description("RFC 6856: UTF8 before USER is answered as it always was, and the session logs on and works " +
                   "afterwards.")]
      public void Utf8BeforeUserIsUnchanged()
      {
         var address = AddAccountWithOneMessage(_domain, "utf8beforeuser@example.test", "Utf8CommandBeforeUserBody");

         using (var connection = ConnectPop3())
         {
            var utf8 = connection.SendAndReceive("UTF8\r\n");
            Assert.IsTrue(utf8.StartsWith("+OK UTF8 enabled"),
               "UTF8 in the AUTHORIZATION state must be answered as before. Got: " + utf8);

            LogOn(connection, address);

            var stat = connection.SendAndReceive("STAT\r\n");
            Assert.IsTrue(stat.StartsWith("+OK 1 "), "STAT must report the one message. Got: " + stat);

            connection.Send("RETR 1\r\n");
            var message = connection.ReadUntil("\r\n.\r\n");
            StringAssert.Contains("Utf8CommandBeforeUserBody", message, "RETR 1 must send the message. Got: " + message);

            connection.Send("QUIT\r\n");
            connection.ReadUntil("+OK");
         }
      }
   }

   /// <summary>
   ///    The two UTF8 cases that need the TLS listeners, and therefore an SslSetup call,
   ///    which restarts the server: set up per test, as StlsConformance does.
   /// </summary>
   [TestFixture]
   public class Utf8CommandOverTls : TestFixtureBase
   {
      // The listeners SslSetup.SetupSSLPorts creates.
      private const int ImplicitTlsPort = 11001;     // eCSTLS
      private const int StartTlsOptionalPort = 11002; // eCSSTARTTLSOptional

      /// <summary>
      ///    The four standard listeners and no certificate again, the server rebound to
      ///    them, as every other fixture expects to find it. The next test's
      ///    PerformBasicSetup puts the port table back, but a run that ENDS with one of
      ///    these tests left the twelve TLS ports in it, so the service the next gate
      ///    starts binds sixteen and its pre-flight refuses the bench (TCPIPPorts.Count is
      ///    16, the SSL ports are not bindable) - as the first run of this fixture did on
      ///    a bench whose run ended here. Windows only: the Linux suite's server is thrown
      ///    away with its container, and its shim has neither the COM port defaults nor
      ///    the service control to do it with.
      /// </summary>
      [TearDown]
      public void RestoreTheStandardListeners()
      {
         if (Environment.OSVersion.Platform != PlatformID.Win32NT)
            return;

         _application.Stop();

         try
         {
            _settings.TCPIPPorts.SetDefault();
            _settings.SSLCertificates.Clear();
         }
         finally
         {
            _application.Start();
         }
      }

      [Test]
      [Description("Work item 85 as reported: Outlook's sequence over implicit TLS, UTF8 after login included, is " +
                   "answered in full.")]
      public void OutlooksSequenceOverImplicitTlsIsAnsweredInFull()
      {
         SslSetup.SetupSSLPorts(_application);

         var address = Utf8Command.AddAccountWithOneMessage(_domain, "outlooktls@example.test", "Utf8CommandTlsBody");

         using (var connection = new TcpConnection(true))
         {
            Assert.IsTrue(connection.Connect(ImplicitTlsPort), "Could not connect to the implicit-TLS POP3 port.");
            connection.ReadUntil("+OK"); // banner, after the handshake

            Utf8Command.DriveOutlooksSequence(connection, address, "Utf8CommandTlsBody");
         }
      }

      [Test]
      [Description("RFC 6856 2.1: STLS after UTF8 stays refused - after login STLS is not a command of the session's " +
                   "state, and accepting UTF8 there does not make it one.")]
      public void StlsAfterUtf8AfterLoginIsRefused()
      {
         SslSetup.SetupSSLPorts(_application);

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "utf8thenstls@example.test", Utf8Command.Password);

         using (var connection = new TcpConnection())
         {
            Assert.IsTrue(connection.Connect(StartTlsOptionalPort), "Could not connect to the STARTTLS POP3 port.");
            connection.ReadUntil("+OK"); // banner

            // In the clear: a STARTTLS-optional port takes a logon without TLS.
            Utf8Command.LogOn(connection, account.Address);

            var utf8 = connection.SendAndReceive("UTF8\r\n");
            Assert.IsTrue(utf8.StartsWith("+OK"), "UTF8 after login must be accepted. Got: " + utf8);

            var stls = connection.SendAndReceive("STLS\r\n");
            Assert.IsTrue(stls.StartsWith("-ERR"),
               "STLS after UTF8 must be refused (RFC 6856 2.1), as STLS after login always is. Got: " + stls);

            // Still the cleartext session it was, and in step.
            Assert.IsTrue(connection.SendAndReceive("NOOP\r\n").StartsWith("+OK"),
               "The session must stay in step after the refused STLS.");

            connection.Send("QUIT\r\n");
            connection.ReadUntil("+OK");
         }
      }
   }
}
