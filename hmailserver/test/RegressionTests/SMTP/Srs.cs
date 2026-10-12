// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Linq;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   /// B4 SRS (Sender Rewriting Scheme) for forwarded mail. When SRS is enabled
   /// (the settings store's SRSEnabled/SRSSecret), the envelope MAIL FROM of
   /// a forwarded message from an external sender is rewritten to a signed,
   /// reversible SRS0 address at the local forwarding domain so SPF stays aligned;
   /// a bounce sent back to that address is decoded and relayed to the original
   /// sender. An SRS address whose signature does not validate is rejected.
   /// </summary>
   [TestFixture]
   public class Srs : TestFixtureBase
   {

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      private static string ExtractReturnPath(string messageText)
      {
         string returnPath = messageText.Replace("\r\n", "\n").Split('\n').FirstOrDefault(line => line.StartsWith("Return-Path:"));
         if (returnPath == null)
            return null;

         return returnPath.Substring("Return-Path:".Length).Trim().TrimStart('<').TrimEnd('>');
      }

      [Test]
      [Description("With SRS enabled, forwarding a message from an external sender rewrites the " +
                   "envelope MAIL FROM to a signed SRS0 address at the local forwarding domain; the " +
                   "signed address reverses back to the original sender (RCPT accepted), while a " +
                   "tampered SRS0 address fails signature validation and is rejected.")]
      public void TestSrsRewriteAndReverse()
      {
         try
         {
            WriteSetting("SRSEnabled", "1");
            WriteSetting("SRSSecret", "regression-srs-secret");
            _application.Reinitialize();

            var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "srsfwd@example.test", "test");
            var destination = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "srsdest@example.test", "test");

            forwarder.ForwardAddress = destination.Address;
            forwarder.ForwardEnabled = true;
            forwarder.Save();

            // An external sender (not a local domain) forwarded to a local mailbox.
            const string externalSender = "alice@external-sender.example";
            SmtpClientSimulator.StaticSend(externalSender, forwarder.Address, "SRS test", "Body");
            _application.SubmitEMail();

            var text = Pop3ClientSimulator.AssertGetFirstMessageText(destination.Address, "test");
            string returnPath = ExtractReturnPath(text);

            Assert.IsNotNull(returnPath, "No Return-Path on the forwarded message: " + text);
            Assert.IsTrue(returnPath.StartsWith("SRS0="),
               "Envelope MAIL FROM was not SRS-rewritten. Return-Path: " + returnPath);
            Assert.IsTrue(returnPath.ToLower().EndsWith("@example.test"),
               "SRS address is not at the forwarding domain. Return-Path: " + returnPath);
            Assert.IsFalse(returnPath.ToLower().Contains(externalSender),
               "Original external sender leaked into the envelope. Return-Path: " + returnPath);

            // The signed SRS0 address reverses to the original sender: RCPT is accepted.
            var socket = new TcpConnection();
            Assert.IsTrue(socket.Connect(25));
            Assert.IsTrue(socket.Receive().StartsWith("220"));
            socket.Send("HELO example.com\r\n");
            Assert.IsTrue(socket.Receive().StartsWith("250"));
            socket.Send("MAIL FROM:<>\r\n");
            Assert.IsTrue(socket.Receive().StartsWith("250"));
            var validRcpt = socket.SendAndReceive("RCPT TO:<" + returnPath + ">\r\n");
            Assert.IsTrue(validRcpt.StartsWith("250"),
               "A valid SRS0 bounce address should reverse and be accepted. Got: " + validRcpt);

            // Tampering the signature makes the address unreversible: RCPT is rejected.
            string tampered = TamperSrsHash(returnPath);
            var tamperedRcpt = socket.SendAndReceive("RCPT TO:<" + tampered + ">\r\n");
            Assert.IsFalse(tamperedRcpt.StartsWith("250"),
               "A tampered SRS0 address must be rejected. Got: " + tamperedRcpt);

            socket.Send("QUIT\r\n");
            socket.Disconnect();
         }
         finally
         {
            WriteSetting("SRSEnabled", "0");
            WriteSetting("SRSSecret", "");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An SRS0 address this server handed out is followed back to the original sender, as local " +
                   "delivery, for a bounce only. Mail to it with a reverse-path of its own is mail for an outside " +
                   "address and is held to the relay rules (upstream PR 649). It was taken as local delivery " +
                   "whatever the sender.")]
      public void TestSrsAddressRelaysOnlyBounces()
      {
         var range = _application.Settings.SecurityRanges.get_ItemByName("My computer");
         bool requiredExternalToExternal = range.RequireSMTPAuthExternalToExternal;

         try
         {
            WriteSetting("SRSEnabled", "1");
            WriteSetting("SRSSecret", "regression-srs-secret");
            _application.Reinitialize();

            var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "srsrelayfwd@example.test", "test");
            var destination = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "srsrelaydest@example.test", "test");

            forwarder.ForwardAddress = destination.Address;
            forwarder.ForwardEnabled = true;
            forwarder.Save();

            // The Return-Path of a forwarded message is the address in question.
            SmtpClientSimulator.StaticSend("alice@external-sender.example", forwarder.Address, "SRS relay test", "Body");
            _application.SubmitEMail();

            string returnPath = ExtractReturnPath(Pop3ClientSimulator.AssertGetFirstMessageText(destination.Address, "test"));
            Assert.IsNotNull(returnPath);
            Assert.IsTrue(returnPath.StartsWith("SRS0="), "The forwarded message's sender was not rewritten: " + returnPath);

            // The client is this machine, sending as nobody hosted here to an address that
            // reverses to one outside: external to external, which now needs a sign-in.
            range.RequireSMTPAuthExternalToExternal = true;
            range.Save();

            var socket = new TcpConnection();
            Assert.IsTrue(socket.Connect(25));
            Assert.IsTrue(socket.Receive().StartsWith("220"));
            socket.Send("HELO example.com\r\n");
            Assert.IsTrue(socket.Receive().StartsWith("250"));

            Assert.IsTrue(socket.SendAndReceive("MAIL FROM:<mallory@attacker.example>\r\n").StartsWith("250"));
            var relayRcpt = socket.SendAndReceive("RCPT TO:<" + returnPath + ">\r\n");
            // 530 asks for the sign-in; 550 would be a range that refuses the relay outright.
            Assert.IsTrue(relayRcpt.StartsWith("530") || relayRcpt.StartsWith("550"),
               "Mail with a sender of its own to an SRS address must be held to the relay rules. Got: " + relayRcpt);

            // A bounce to the same address is still taken, unauthenticated, as before.
            Assert.IsTrue(socket.SendAndReceive("RSET\r\n").StartsWith("250"));
            Assert.IsTrue(socket.SendAndReceive("MAIL FROM:<>\r\n").StartsWith("250"));
            var bounceRcpt = socket.SendAndReceive("RCPT TO:<" + returnPath + ">\r\n");
            Assert.IsTrue(bounceRcpt.StartsWith("250"),
               "A bounce to a valid SRS address must still be accepted. Got: " + bounceRcpt);

            socket.Send("QUIT\r\n");
            socket.Disconnect();
         }
         finally
         {
            range.RequireSMTPAuthExternalToExternal = requiredExternalToExternal;
            range.Save();

            WriteSetting("SRSEnabled", "0");
            WriteSetting("SRSSecret", "");
            _application.Reinitialize();
         }
      }

      private static string TamperSrsHash(string srsAddress)
      {
         // The hash is the first field after the "SRS0=" prefix; flip its first char.
         char c = srsAddress[5];
         char replacement = c == '0' ? '1' : '0';
         return srsAddress.Substring(0, 5) + replacement + srsAddress.Substring(6);
      }
   }
}
