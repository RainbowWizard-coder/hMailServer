// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Net.Sockets;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   /// The authorization identity of SASL PLAIN (RFC 4616 section 2): the message is
   /// authzid NUL authcid NUL passwd, and the server "will verify that the
   /// authentication credentials permit the client to act as" the authzid. A password
   /// permits its own account, and - on IMAP, with a master user configured - the
   /// master user's grant (IMAP/MasterUserImpersonation.cs).
   ///
   /// SMTP, POP3 and ManageSieve decoded the authzid and ignored it: a client asking
   /// to act as another account was signed in as the authcid all the same, and told
   /// it had succeeded. They refuse it now (upstream #652/#656). IMAP did the
   /// opposite for the client's own name: an authzid spelling out the account that
   /// was signing in fell into the master-user path and was answered BAD "No master
   /// user defined." It is taken as no authzid now.
   ///
   /// The refusals are made before the password is looked at and are charged to no
   /// lockout: they are not guesses at a password. The authzid is compared prepared
   /// with SASLprep, as the authcid is, so one spelt as the authcid is never refused
   /// as another account's.
   /// </summary>
   [TestFixture]
   public class SaslPlainAuthorizationIdentity : TestFixtureBase
   {
      private const string Me = "authzid-me@example.test";
      private const string Other = "authzid-other@example.test";
      private const string Password = "test";

      // Me with a soft hyphen (U+00AD) in it, which SASLprep maps to nothing (RFC 3454
      // table B.1): the authcid it is sent as signs in as Me.
      private static readonly string MeWithASoftHyphen = "authzid-" + (char) 0x00AD + "me@example.test";

      // From the ports set aside for this file's unit (47100-47119).
      private const int ManageSievePort = 47100;

      private bool _smtpPlainBefore;
      private bool _imapPlainBefore;

      private static string Plain(string authzid, string authcid, string password)
      {
         return Convert.ToBase64String(Encoding.UTF8.GetBytes(authzid + "\0" + authcid + "\0" + password));
      }

      [SetUp]
      public void CreateAccountsAndAllowPlain()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Me, Password);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Other, Password);

         // Both off in the shipped configuration, where the two protocols would refuse
         // PLAIN for that reason alone.
         _smtpPlainBefore = _settings.AllowSMTPAuthPlain;
         _imapPlainBefore = _settings.IMAPSASLPlainEnabled;
         _settings.AllowSMTPAuthPlain = true;
         _settings.IMAPSASLPlainEnabled = true;
      }

      [TearDown]
      public void RestorePlain()
      {
         _settings.AllowSMTPAuthPlain = _smtpPlainBefore;
         _settings.IMAPSASLPlainEnabled = _imapPlainBefore;
      }

      private static string SmtpAuthPlain(string authzid, string authcid, string password)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");

         try
         {
            socket.ReadUntil("220");
            socket.Send("EHLO example.test\r\n");
            socket.ReadUntil("250 ");

            string reply = socket.SendAndReceive("AUTH PLAIN " + Plain(authzid, authcid, password) + "\r\n");

            socket.Send("QUIT\r\n");
            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      private static string Pop3AuthPlain(string authzid, string authcid, string password)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(110), "Could not connect to POP3.");

         try
         {
            StringAssert.StartsWith("+OK", socket.Receive());

            string reply = socket.SendAndReceive("AUTH PLAIN " + Plain(authzid, authcid, password) + "\r\n");

            socket.Send("QUIT\r\n");
            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      private static string ImapAuthenticatePlain(string authzid, string authcid, string password)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(143), "Could not connect to IMAP.");

         try
         {
            socket.ReadUntil("* OK");
            socket.Send("A01 AUTHENTICATE PLAIN\r\n");

            string continuation = socket.Receive();
            StringAssert.StartsWith("+", continuation, "The server did not ask for the SASL response. Got: " + continuation);

            socket.Send(Plain(authzid, authcid, password) + "\r\n");
            string result = socket.ReadUntil("A01 ");

            socket.Send("A02 LOGOUT\r\n");
            socket.ReadUntil("A02 ");
            return result;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      /// <summary>
      /// The ManageSieve listener's reply to AUTHENTICATE "PLAIN" with the message as a
      /// quoted string: the completion line, OK or NO.
      /// </summary>
      private string ManageSieveAuthenticatePlain(string authzid, string authcid, string password)
      {
         IniFileSetting.Write("ManageSieveServerBindAddress", "127.0.0.1");
         IniFileSetting.Write("ManageSieveServerPort", ManageSievePort.ToString());
         _application.Reinitialize();

         try
         {
            using (var client = new TcpClient())
            {
               Exception last = null;
               for (int attempt = 0; attempt < 25; attempt++)
               {
                  try
                  {
                     client.Connect("127.0.0.1", ManageSievePort);
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

               var stream = client.GetStream();
               stream.ReadTimeout = 15000;

               // The greeting is the capability lines and then OK.
               ReadManageSieveCompletion(stream);

               byte[] command = Encoding.ASCII.GetBytes("AUTHENTICATE \"PLAIN\" \"" + Plain(authzid, authcid, password) + "\"\r\n");
               stream.Write(command, 0, command.Length);

               string completion = ReadManageSieveCompletion(stream);

               byte[] logout = Encoding.ASCII.GetBytes("LOGOUT\r\n");
               stream.Write(logout, 0, logout.Length);

               return completion;
            }
         }
         finally
         {
            IniFileSetting.Write("ManageSieveServerPort", "0");
            _application.Reinitialize();
         }
      }

      private static string ReadManageSieveCompletion(NetworkStream stream)
      {
         while (true)
         {
            var line = new StringBuilder();
            int previous = -1;

            while (true)
            {
               int current = stream.ReadByte();
               if (current == -1)
                  return line.ToString();

               if (previous == '\r' && current == '\n')
               {
                  line.Length--;
                  break;
               }

               line.Append((char) current);
               previous = current;
            }

            string text = line.ToString().TrimStart();
            if (text.StartsWith("OK", StringComparison.OrdinalIgnoreCase) ||
                text.StartsWith("NO", StringComparison.OrdinalIgnoreCase) ||
                text.StartsWith("BYE", StringComparison.OrdinalIgnoreCase))
               return text;
         }
      }

      [Test]
      [Description("SMTP: an authzid naming another account is refused. It was ignored, and the client " +
                   "signed in as the authcid with 235.")]
      public void SmtpAnAuthzidNamingAnotherAccountIsRefused()
      {
         string reply = SmtpAuthPlain(Other, Me, Password);
         StringAssert.StartsWith("535", reply, "An authzid naming another account must be refused. Got: " + reply);
      }

      [Test]
      [Description("SMTP: an authzid naming the client's own account, in any case, is accepted.")]
      public void SmtpAnAuthzidNamingTheClientItselfIsAccepted()
      {
         StringAssert.StartsWith("235", SmtpAuthPlain(Me, Me, Password));
         StringAssert.StartsWith("235", SmtpAuthPlain(Me.ToUpperInvariant(), Me, Password));
         StringAssert.StartsWith("235", SmtpAuthPlain(string.Empty, Me, Password));
      }

      [Test]
      [Description("POP3: an authzid naming another account is refused. It was ignored, and the client " +
                   "signed in as the authcid with +OK.")]
      public void Pop3AnAuthzidNamingAnotherAccountIsRefused()
      {
         string reply = Pop3AuthPlain(Other, Me, Password);
         StringAssert.StartsWith("-ERR", reply, "An authzid naming another account must be refused. Got: " + reply);
      }

      [Test]
      [Description("POP3: an authzid naming the client's own account is accepted.")]
      public void Pop3AnAuthzidNamingTheClientItselfIsAccepted()
      {
         StringAssert.StartsWith("+OK", Pop3AuthPlain(Me, Me, Password));
      }

      [Test]
      [Description("IMAP: an authzid naming the client's own account is no masquerade and signs in. It fell " +
                   "into the master-user path and was answered BAD \"No master user defined.\"")]
      public void ImapAnAuthzidNamingTheClientItselfIsAccepted()
      {
         string result = ImapAuthenticatePlain(Me, Me, Password);
         StringAssert.Contains("A01 OK", result, "An authzid naming the client itself must sign in. Got: " + result);
      }

      [Test]
      [Description("IMAP: with no master user, an authzid naming another account is still refused.")]
      public void ImapAnAuthzidNamingAnotherAccountIsStillRefused()
      {
         string result = ImapAuthenticatePlain(Other, Me, Password);
         StringAssert.DoesNotContain("A01 OK", result, "A masquerade with no master user must be refused. Got: " + result);
      }

      [Test]
      [Description("ManageSieve: an authzid naming another account is refused. It was ignored, and the " +
                   "client signed in as the authcid - managing the authcid's scripts while it believed it " +
                   "was managing the authzid's.")]
      public void ManageSieveAnAuthzidNamingAnotherAccountIsRefused()
      {
         string completion = ManageSieveAuthenticatePlain(Other, Me, Password);
         StringAssert.StartsWith("NO", completion, "An authzid naming another account must be refused. Got: " + completion);
      }

      [Test]
      [Description("ManageSieve: an authzid naming the client's own account is accepted.")]
      public void ManageSieveAnAuthzidNamingTheClientItselfIsAccepted()
      {
         string completion = ManageSieveAuthenticatePlain(Me, Me, Password);
         StringAssert.StartsWith("OK", completion, "An authzid naming the client itself must sign in. Got: " + completion);
      }

      // An authzid sent byte for byte as the authcid, which SASLprep changes. Each
      // protocol prepares the authcid before it asks AccountLogon::AuthorizationIdentityIsOwn,
      // and the authzid was compared as it came: the client's own name, refused as
      // another account's. On SMTP, POP3 and ManageSieve that signed in while every
      // authzid was set aside.

      [Test]
      [Description("SMTP: an authzid spelt as the authcid, with a soft hyphen SASLprep takes out of both, is the " +
                   "client's own and signs in. It was compared unprepared with the prepared authcid and refused.")]
      public void SmtpAnAuthzidSpeltAsTheAuthcidIsTheClientsOwnOncePrepared()
      {
         string reply = SmtpAuthPlain(MeWithASoftHyphen, MeWithASoftHyphen, Password);
         StringAssert.StartsWith("235", reply, "The authzid names the client itself once prepared. Got: " + reply);
      }

      [Test]
      [Description("POP3: an authzid spelt as the authcid, with a soft hyphen SASLprep takes out of both, is the " +
                   "client's own and signs in. It was compared unprepared with the prepared authcid and refused.")]
      public void Pop3AnAuthzidSpeltAsTheAuthcidIsTheClientsOwnOncePrepared()
      {
         string reply = Pop3AuthPlain(MeWithASoftHyphen, MeWithASoftHyphen, Password);
         StringAssert.StartsWith("+OK", reply, "The authzid names the client itself once prepared. Got: " + reply);
      }

      [Test]
      [Description("IMAP: an authzid spelt as the authcid, with a soft hyphen SASLprep takes out of both, is the " +
                   "client's own and signs in, as one spelt plainly does. It was compared unprepared with the " +
                   "prepared authcid, fell into the master-user path and was answered BAD.")]
      public void ImapAnAuthzidSpeltAsTheAuthcidIsTheClientsOwnOncePrepared()
      {
         string result = ImapAuthenticatePlain(MeWithASoftHyphen, MeWithASoftHyphen, Password);
         StringAssert.Contains("A01 OK", result, "The authzid names the client itself once prepared. Got: " + result);
      }

      [Test]
      [Description("ManageSieve: an authzid spelt as the authcid, with a soft hyphen SASLprep takes out of both, is " +
                   "the client's own and signs in. It was compared unprepared with the prepared authcid and refused.")]
      public void ManageSieveAnAuthzidSpeltAsTheAuthcidIsTheClientsOwnOncePrepared()
      {
         string completion = ManageSieveAuthenticatePlain(MeWithASoftHyphen, MeWithASoftHyphen, Password);
         StringAssert.StartsWith("OK", completion, "The authzid names the client itself once prepared. Got: " + completion);
      }
   }
}
