// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A HELO name or an envelope sender's domain that is not a fully qualified
// domain name, refused at MAIL FROM (GitLab issue 51): RefuseHeloNotFqdn and
// RefuseMailFromDomainNotFqdn, both off by default. AntiSpam/FqdnCheck.h on the
// server has the rules; FqdnCheckTester holds them name by name, and this holds
// what a session sees.
//
// What these tests pin, and why each is the one worth having:
//
//   * with both switches off, a session whose HELO and sender are both
//     addresses delivers its message exactly as before - the negative control
//     for every refusal below, and the item's "stock behaviour unchanged";
//   * each switch refuses its own name with 550 5.7.1 at MAIL FROM, for the
//     item's own examples, and lets a name through - mail.co.uk beside co.uk;
//   * MAIL FROM:<> does not excuse a bad HELO, and has no domain for the
//     sender's switch to judge;
//   * the two switches are independent;
//   * every carve-out the item lists - an authenticated session, a range that
//     does not run the spam tests, the anti-spam white list, an incoming relay -
//     is refused without the carve-out and accepted with it, in one test each;
//   * the refusal writes one line with the HELO, the envelope and the peer.
//
// The new COM members are reached through dynamic, as SmtpPolicy does: the
// suite's interop is generated from the registered type library, and naming
// them early-bound would not compile against a bench whose server predates
// them. On Linux the shim reads and writes them over REST.

using System;
using System.Collections.Generic;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class FqdnRefusal : TestFixtureBase
   {
      private const string GoodHelo = "mail.example.com";
      private const string GoodSender = "sender@example.com";

      private bool refuseHeloBefore_;
      private bool refuseSenderBefore_;
      private bool authPlainBefore_;

      [SetUp]
      public new void SetUp()
      {
         dynamic antiSpam = _settings.AntiSpam;

         refuseHeloBefore_ = (bool) antiSpam.RefuseHeloNotFqdn;
         refuseSenderBefore_ = (bool) antiSpam.RefuseMailFromDomainNotFqdn;

         // AnAuthenticatedSessionIsNeverJudged signs in with AUTH PLAIN, which the
         // server offers only with AllowSMTPAuthPlain on - off in the shipped
         // configuration. Turned on here and put back in TearDown: until 6.3.6 this
         // fixture relied on Security.PasswordMasking having turned it on earlier,
         // and failed on a bench where it was off (API/RoomsAndEquipment.cs had the
         // same gap).
         authPlainBefore_ = _settings.AllowSMTPAuthPlain;
         _settings.AllowSMTPAuthPlain = true;
      }

      [TearDown]
      public new void TearDown()
      {
         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.RefuseHeloNotFqdn = refuseHeloBefore_;
         antiSpam.RefuseMailFromDomainNotFqdn = refuseSenderBefore_;
         _settings.AllowSMTPAuthPlain = authPlainBefore_;
      }

      [Test]
      [Description("With both switches off - the default - a session that greets with an address literal and sends from an address literal is accepted and its message delivered, as it always was; so are a single-label HELO and a public-suffix sender. The negative control for every refusal in this fixture.")]
      public void WithBothSwitchesOffNothingChanges()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fqdn-off@example.test", "test");

         Switches(false, false);

         List<string> replies = Session("[192.0.2.7]", "someone@[192.0.2.7]", account.Address);

         StringAssert.StartsWith("250", replies[0], "MAIL FROM: " + replies[0]);
         StringAssert.StartsWith("250", replies[1], "RCPT TO: " + replies[1]);
         StringAssert.StartsWith("354", replies[2], "DATA: " + replies[2]);
         StringAssert.StartsWith("250", replies[3], "The message must be accepted: " + replies[3]);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         string localhost = MailFromReply("localhost", "someone@co.uk");
         StringAssert.StartsWith("250", localhost, "With the switches off a single label and a public suffix are accepted: " + localhost);
      }

      [Test]
      [Description("RefuseHeloNotFqdn refuses at MAIL FROM, with 550 5.7.1, a HELO that is an address with or without brackets, an IPv6 literal, a single label or a public suffix; and lets a name through, mail.co.uk beside co.uk.")]
      public void AHeloThatIsNotANameIsRefusedAtMailFrom()
      {
         Switches(true, false);

         foreach (string helo in new[] { "192.0.2.7", "[192.0.2.7]", "[IPv6:2001:db8::7]", "localhost", "co.uk", "com" })
         {
            string reply = MailFromReply(helo, GoodSender);
            StringAssert.StartsWith("550", reply, "HELO " + helo + " must be refused at MAIL FROM: " + reply);
            StringAssert.Contains("5.7.1", reply, reply);
            StringAssert.Contains("The HELO name is not a fully qualified domain name", reply, reply);
         }

         foreach (string helo in new[] { GoodHelo, "mail.co.uk", "mx1.example.com." })
         {
            string reply = MailFromReply(helo, GoodSender);
            StringAssert.StartsWith("250", reply, "HELO " + helo + " is a name: " + reply);
         }
      }

      [Test]
      [Description("MAIL FROM:<> does not excuse a bad HELO: with the HELO switch on, a null sender from a session that greeted with localhost is refused, and one from a session that greeted with a name is accepted. With only the sender's switch on, the null sender has no domain to judge and the same session is accepted.")]
      public void ANullSenderIsNoExcuseForABadHelo()
      {
         Switches(true, false);

         string refused = MailFromReply("localhost", "");
         StringAssert.StartsWith("550", refused, "A null sender must not excuse HELO localhost: " + refused);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", refused, refused);

         string named = MailFromReply(GoodHelo, "");
         StringAssert.StartsWith("250", named, "A null sender from a session with a name is a delivery report: " + named);

         Switches(false, true);

         string senderSwitchOnly = MailFromReply("localhost", "");
         StringAssert.StartsWith("250", senderSwitchOnly, "The null sender has no domain, so the sender's switch has nothing to refuse: " + senderSwitchOnly);
      }

      [Test]
      [Description("RefuseMailFromDomainNotFqdn refuses at MAIL FROM, with 550 5.7.1, a sender at an address with or without brackets, at an IPv6 literal, at a single label or at a public suffix; and lets a name through, mail.co.uk beside co.uk.")]
      public void ASenderDomainThatIsNotANameIsRefusedAtMailFrom()
      {
         Switches(false, true);

         foreach (string sender in new[] { "someone@192.0.2.7", "someone@[192.0.2.7]", "someone@[IPv6:2001:db8:0:0:0:0:0:7]", "someone@localhost", "someone@co.uk" })
         {
            string reply = MailFromReply(GoodHelo, sender);
            StringAssert.StartsWith("550", reply, "MAIL FROM:<" + sender + "> must be refused: " + reply);
            StringAssert.Contains("5.7.1", reply, reply);
            StringAssert.Contains("The sender's domain is not a fully qualified domain name", reply, reply);
         }

         foreach (string sender in new[] { GoodSender, "someone@mail.co.uk" })
         {
            string reply = MailFromReply(GoodHelo, sender);
            StringAssert.StartsWith("250", reply, "MAIL FROM:<" + sender + "> is at a name: " + reply);
         }
      }

      [Test]
      [Description("The two switches are independent: the HELO switch alone accepts a bad sender from a session with a good HELO, and the sender's switch alone accepts a good sender from a session with a bad HELO.")]
      public void TheTwoSwitchesAreIndependent()
      {
         Switches(true, false);
         string heloOnly = MailFromReply(GoodHelo, "someone@[192.0.2.7]");
         StringAssert.StartsWith("250", heloOnly, "The HELO switch says nothing about the sender: " + heloOnly);

         Switches(false, true);
         string senderOnly = MailFromReply("[192.0.2.7]", GoodSender);
         StringAssert.StartsWith("250", senderOnly, "The sender's switch says nothing about the HELO: " + senderOnly);
      }

      [Test]
      [Description("An authenticated session is never judged: the same HELO is refused without AUTH and accepted after it, which is why the check waits for MAIL FROM.")]
      public void AnAuthenticatedSessionIsNeverJudged()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fqdn-auth@example.test", "test");

         Switches(true, true);

         string unauthenticated = MailFromReply("[192.0.2.7]", account.Address);
         StringAssert.StartsWith("550", unauthenticated, "Without AUTH the address-literal HELO is refused: " + unauthenticated);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", unauthenticated, unauthenticated);

         string authenticated = MailFromReply("[192.0.2.7]", account.Address, account.Address, "test");
         StringAssert.StartsWith("250", authenticated, "After AUTH the session is not judged: " + authenticated);
      }

      [Test]
      [Description("A session from an IP range that does not run the spam tests is never judged: refused under the default ranges, accepted under a range for 127.0.0.1 with spam protection off.")]
      public void ARangeWithoutTheSpamTestsIsNeverJudged()
      {
         Switches(true, true);

         string judged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
         StringAssert.StartsWith("550", judged, "Under a range that runs the spam tests the session is refused: " + judged);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", judged, judged);

         var range = _settings.SecurityRanges.Add();

         try
         {
            range.Name = "fqdn-refusal: no spam tests";
            range.Priority = 50;
            range.LowerIP = "127.0.0.1";
            range.UpperIP = "127.0.0.1";
            range.AllowSMTPConnections = true;
            range.AllowDeliveryFromRemoteToLocal = true;
            range.AllowDeliveryFromLocalToLocal = true;
            range.EnableSpamProtection = false;
            range.Save();

            string notJudged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
            StringAssert.StartsWith("250", notJudged, "A range that does not run the spam tests is not judged: " + notJudged);
         }
         finally
         {
            range.Delete();
         }
      }

      [Test]
      [Description("A sender the anti-spam white list covers is never judged: refused before the entry exists, accepted once it does.")]
      public void AWhiteListedSenderIsNeverJudged()
      {
         Switches(true, true);

         string judged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
         StringAssert.StartsWith("550", judged, "Without the white list entry the session is refused: " + judged);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", judged, judged);

         var addresses = _settings.AntiSpam.WhiteListAddresses;
         var address = addresses.Add();

         try
         {
            address.EmailAddress = "*";
            address.LowerIPAddress = "127.0.0.1";
            address.UpperIPAddress = "127.0.0.1";
            address.Description = "fqdn-refusal";
            address.Save();

            string notJudged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
            StringAssert.StartsWith("250", notJudged, "A white-listed sender is not judged: " + notJudged);
         }
         finally
         {
            addresses.DeleteByDBID(address.ID);
         }
      }

      [Test]
      [Description("A session from an incoming relay is never judged: its HELO is this organisation's own gateway's, and a refusal would become that gateway's bounce. Refused before the relay is declared, accepted once it is.")]
      public void AnIncomingRelayIsNeverJudged()
      {
         Switches(true, true);

         string judged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
         StringAssert.StartsWith("550", judged, "From an address that is not a relay the session is refused: " + judged);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", judged, judged);

         // Through dynamic: the Linux suite's shim has no incoming relays to give
         // (no REST route carries them), and its Add skips the test here, before
         // anything is made.
         dynamic relays = _settings.IncomingRelays;
         dynamic relay = relays.Add();

         try
         {
            relay.Name = "fqdn-refusal relay";
            relay.LowerIP = "127.0.0.1";
            relay.UpperIP = "127.0.0.1";
            relay.Save();

            string notJudged = MailFromReply("[192.0.2.7]", "someone@[192.0.2.7]");
            StringAssert.StartsWith("250", notJudged, "An incoming relay is not judged: " + notJudged);
         }
         finally
         {
            relays.DeleteByDBID(relay.ID);
         }
      }

      [Test]
      [Description("A refusal writes one line to the log with what was refused and why, the HELO, the envelope sender and the connecting address.")]
      public void TheRefusalIsLoggedWithTheHeloTheEnvelopeAndThePeer()
      {
         Switches(true, false);

         string reply = MailFromReply("[192.0.2.7]", "logged@example.com");
         StringAssert.StartsWith("550", reply, reply);
         StringAssert.Contains("5.7.1 The HELO name is not a fully qualified domain name", reply, reply);

         const string line = "hMailServer SpamProtection rejected MAIL FROM: the HELO name is not a fully qualified domain name, it is an address literal (HELO: [192.0.2.7], Sender: <logged@example.com>, IP: 127.0.0.1)";

         ClassicAssert.IsTrue(LogHandler.DefaultLogContains(line), "The refusal's log line is missing: " + LogHandler.ReadCurrentDefaultLog());
      }

      [Test]
      [Description("The two settings read back what is set, each independently of the other.")]
      public void TheSettingsReadBackWhatIsSet()
      {
         dynamic antiSpam = _settings.AntiSpam;

         Switches(true, false);
         ClassicAssert.IsTrue((bool) antiSpam.RefuseHeloNotFqdn);
         ClassicAssert.IsFalse((bool) antiSpam.RefuseMailFromDomainNotFqdn);

         Switches(false, true);
         ClassicAssert.IsFalse((bool) antiSpam.RefuseHeloNotFqdn);
         ClassicAssert.IsTrue((bool) antiSpam.RefuseMailFromDomainNotFqdn);
      }

      // ---------------------------------------------------------------------- helpers

      private void Switches(bool refuseHelo, bool refuseSender)
      {
         dynamic antiSpam = _settings.AntiSpam;

         antiSpam.RefuseHeloNotFqdn = refuseHelo;
         antiSpam.RefuseMailFromDomainNotFqdn = refuseSender;
      }

      /// <summary>
      ///    One session greeting with the HELO name given, AUTH PLAIN when a user
      ///    name is given, and the reply to MAIL FROM. An empty sender is the null
      ///    reverse-path. Raw, because the point of these tests is that reply.
      /// </summary>
      private static string MailFromReply(string heloName, string sender, string username = null, string password = null)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO " + heloName + "\r\n");
            socket.ReadUntil("250 ");

            if (username != null)
            {
               string plain = Convert.ToBase64String(Encoding.ASCII.GetBytes("\0" + username + "\0" + password));
               socket.Send("AUTH PLAIN " + plain + "\r\n");
               string authenticated = socket.ReadUntil("235");
               StringAssert.Contains("235", authenticated, authenticated);
            }

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            string reply = socket.Receive();

            socket.Send("QUIT\r\n");

            return reply;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      /// <summary>
      ///    MAIL FROM, one RCPT TO and DATA with a small message, and every reply
      ///    after the greeting in order: MAIL FROM, RCPT TO, DATA, and the reply to
      ///    the message when DATA was invited.
      /// </summary>
      private static List<string> Session(string heloName, string sender, string recipient)
      {
         var replies = new List<string>();

         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");

         try
         {
            socket.ReadUntil("220");

            socket.Send("EHLO " + heloName + "\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<" + sender + ">\r\n");
            replies.Add(socket.Receive());

            socket.Send("RCPT TO:<" + recipient + ">\r\n");
            replies.Add(socket.Receive());

            socket.Send("DATA\r\n");
            string data = socket.Receive();
            replies.Add(data);

            if (data.StartsWith("354"))
            {
               socket.Send("From: <" + sender + ">\r\nTo: <" + recipient + ">\r\nSubject: Not a name\r\n\r\nThe body.\r\n.\r\n");
               replies.Add(socket.Receive());
            }

            socket.Send("QUIT\r\n");
         }
         finally
         {
            socket.Disconnect();
         }

         return replies;
      }
   }
}
