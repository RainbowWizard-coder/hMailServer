// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    The PROXY protocol (HAProxy, versions 1 and 2) on the IMAP listener, since
   ///    6.3.7: a load balancer named on IMAPProxyProtocolTrustedIPs passes on the real
   ///    client's address, and the IP ranges, the auto-ban and the account's devices act
   ///    on that client instead of on the balancer. Until then only the SMTP listener
   ///    read the header, and behind a balancer every IMAP client was the balancer.
   ///
   ///    Off by default, with nobody trusted, and a list of its own: a peer the SMTP
   ///    listener trusts is not trusted here. The positive tests are their own negative
   ///    controls: each asserts a refusal that only the range of the address the header
   ///    named can give, where the loopback range the connection really comes from lets
   ///    the same command through.
   /// </summary>
   [TestFixture]
   public class ProxyProtocol : TestFixtureBase
   {
      private Account _account;
      private string _thisMachine;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "proxyimap@example.test", "test");
         _thisMachine = ProxyProtocolClient.AddressTheServerSees();
         _settings.ClearLogonFailureList();
      }

      [TearDown]
      public void TurnTheProxyProtocolOff()
      {
         ProxyProtocolClient.TurnOffImapAndPop3(_settings);
      }

      /// <summary>
      ///    A range for one address that outranks the defaults ("My computer" is at 30,
      ///    "Internet" at 10). PerformBasicSetup puts the default ranges back before every
      ///    test, so it needs no clean-up.
      /// </summary>
      private void AddRange(string name, string address, bool allowImap, bool requireTlsForAuth)
      {
         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.Priority = 50;
         range.LowerIP = address;
         range.UpperIP = address;
         range.AllowSMTPConnections = true;
         range.AllowIMAPConnections = allowImap;
         range.AllowPOP3Connections = true;
         range.AllowDeliveryFromRemoteToLocal = true;
         range.AllowDeliveryFromLocalToLocal = true;
         range.RequireSSLTLSForAuth = requireTlsForAuth;
         range.EnableSpamProtection = false;
         range.Save();
      }

      private string Login(ProxyProtocolClient client, string password)
      {
         client.Send("a1 LOGIN " + _account.Address + " " + password + "\r\n");
         return client.ReadTaggedReply("a1");
      }

      [Test]
      [Description("A version 1 header from a trusted peer gives the session the client's address, " +
                   "and the client's IP range governs it: a range that requires TLS for authentication " +
                   "refuses LOGIN on the plain port. The loopback range the connection comes from requires " +
                   "nothing, so were the header not read the LOGIN would succeed.")]
      public void Version1FromTrustedPeerPutsTheClientUnderItsOwnRange()
      {
         AddRange("proxy-imap-v1-tls", "198.51.100.71", true, true);
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(143))
         {
            // The header goes first; the server greets once it has read it.
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.71", 45101, TestPorts.Actual(143)));
            client.ReadUntil("* OK");

            string reply = Login(client, "test");

            StringAssert.StartsWith("a1 BAD", reply,
               "LOGIN must be refused, because the range of the address the header named requires TLS " +
               "for authentication. An OK means the session ran as the proxy. Got: " + reply);
            StringAssert.Contains("SSL/TLS", reply);
         }
      }

      [Test]
      [Description("A binary version 2 header from a trusted peer is read the same way: the client's " +
                   "range refuses LOGIN on the plain port.")]
      public void Version2FromTrustedPeerPutsTheClientUnderItsOwnRange()
      {
         AddRange("proxy-imap-v2-tls", "198.51.100.72", true, true);
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send(ProxyProtocolClient.Version2Tcp4("198.51.100.72", 45102, TestPorts.Actual(143)));
            client.ReadUntil("* OK");

            string reply = Login(client, "test");

            StringAssert.StartsWith("a1 BAD", reply,
               "LOGIN must be refused by the range of the address the version 2 header named. Got: " + reply);
            StringAssert.Contains("SSL/TLS", reply);
         }
      }

      [Test]
      [Description("The header is required from a peer on the list: an IMAP command where the header is due " +
                   "is an invalid header, answered with nothing and reported in the error log, because a " +
                   "proxy that sends none - a health check that speaks IMAP, say - is misconfigured.")]
      public void TrustedPeerThatSendsNoHeaderIsClosedAndReported()
      {
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send("a1 CAPABILITY\r\n");

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "A peer on the list that sends no header must be answered with nothing. Got: " + received);
         }

         CustomAsserts.AssertReportedError("Invalid PROXY protocol header received from the trusted proxy");
      }

      [Test]
      [Description("A client whose range does not allow IMAP is turned away at the header, before the " +
                   "greeting, as it would have been had it connected itself.")]
      public void ClientWhoseRangeRefusesImapIsClosedBeforeTheGreeting()
      {
         AddRange("proxy-imap-refused", "198.51.100.73", false, false);
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.73", 45103, TestPorts.Actual(143)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "The client's range does not allow IMAP, so the connection must be closed without a greeting. Got: " + received);
         }
      }

      [Test]
      [Description("The auto-ban bans the client behind the proxy and not the proxy: three failed LOGINs " +
                   "from the address the headers named make an auto-ban range for that address alone; the " +
                   "same client is then turned away at its next header, and another client behind the same " +
                   "proxy is still greeted. Were the header not read, the ban would be on this machine.")]
      public void AutoBanActsOnTheClientBehindTheProxy()
      {
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 3;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;

         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         for (int attempt = 1; attempt <= 3; attempt++)
         {
            using (var client = new ProxyProtocolClient(143))
            {
               client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.74", 45110 + attempt, TestPorts.Actual(143)));
               client.ReadUntil("* OK");

               string reply = Login(client, "wrong-password");

               if (attempt < 3)
                  StringAssert.StartsWith("a1 NO", reply, "Attempt " + attempt + ": " + reply);
               else
                  StringAssert.StartsWith("a1 BAD", reply, "The third failure ends the session. Got: " + reply);
            }
         }

         SecurityRange ban = null;
         int bans = 0;

         for (int i = 0; i < _settings.SecurityRanges.Count; i++)
         {
            var range = _settings.SecurityRanges[i];

            if (range.Name.StartsWith("Auto-ban", StringComparison.OrdinalIgnoreCase))
            {
               ban = range;
               bans++;
            }
         }

         ClassicAssert.AreEqual(1, bans, "Exactly one auto-ban range must have been made.");
         ClassicAssert.AreEqual("198.51.100.74", ban.LowerIP, "The ban must be on the client the headers named, not on " + _thisMachine);
         ClassicAssert.AreEqual("198.51.100.74", ban.UpperIP);

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.74", 45120, TestPorts.Actual(143)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received, "The banned client must be turned away at its header. Got: " + received);
         }

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.75", 45121, TestPorts.Actual(143)));
            client.ReadUntil("* OK");

            string reply = Login(client, "test");

            StringAssert.StartsWith("a1 OK", reply, "Another client behind the same proxy must be served. Got: " + reply);
         }
      }

      [Test]
      [Description("While the switch is on, a PROXY header from a peer that is not on the list is an attempt " +
                   "to name a client without the right to: the connection, greeted at once as an ordinary one, " +
                   "is closed without an answer, as the SMTP listener does.")]
      public void HeaderFromAnUntrustedPeerClosesTheConnection()
      {
         ProxyProtocolClient.SetImap(_settings, true, "192.0.2.99");

         using (var client = new ProxyProtocolClient(143))
         {
            client.ReadUntil("* OK");

            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.76", 45104, TestPorts.Actual(143)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "A header from a peer that is not trusted must close the connection without an answer. Got: " + received);
         }
      }

      [Test]
      [Description("Only a line in the header's own form is refused: an IMAP tag is the client's to choose, " +
                   "and a command tagged PROXY is answered as any other, with the switch on.")]
      public void CommandTaggedProxyIsAnsweredWithTheSwitchOn()
      {
         ProxyProtocolClient.SetImap(_settings, true, "192.0.2.99");

         using (var client = new ProxyProtocolClient(143))
         {
            client.ReadUntil("* OK");

            client.Send("PROXY NOOP\r\n");
            string reply = client.ReadTaggedReply("PROXY");

            StringAssert.StartsWith("PROXY OK", reply, "Got: " + reply);

            client.Send("a2 LOGOUT\r\n");
            client.ReadTaggedReply("a2");
         }
      }

      [Test]
      [Description("Off, the shipped default, means off: a PROXY line is the unknown command it always was " +
                   "and the session carries on.")]
      public void ProxyLineIsAnUnknownCommandWhenTheSwitchIsOff()
      {
         using (var client = new ProxyProtocolClient(143))
         {
            client.ReadUntil("* OK");

            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.77", 45105, TestPorts.Actual(143)));
            string reply = client.ReadTaggedReply("PROXY");

            StringAssert.StartsWith("PROXY BAD", reply, "Got: " + reply);

            client.Send("a2 NOOP\r\n");
            StringAssert.StartsWith("a2 OK", client.ReadTaggedReply("a2"));
         }
      }

      [Test]
      [Description("A version 1 UNKNOWN header names no client, so the session keeps the address it comes " +
                   "from, whose range lets LOGIN through.")]
      public void Version1UnknownKeepsTheConnectionsOwnAddress()
      {
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(143))
         {
            client.Send("PROXY UNKNOWN\r\n");
            client.ReadUntil("* OK");

            StringAssert.StartsWith("a1 OK", Login(client, "test"));
         }
      }

      [Test]
      [Description("Each listener has its own list. With SMTP trusting this machine and IMAP's list empty, " +
                   "an IMAP connection from this machine is an ordinary one and is greeted without a header; " +
                   "with IMAP trusting it and SMTP off, an SMTP connection is greeted without one.")]
      public void ThePeersTrustedByOneListenerAreNotTrustedByAnother()
      {
         try
         {
            IniFileSetting.Write("SMTPProxyProtocolEnabled", "1");
            IniFileSetting.Write("SMTPProxyProtocolTrustedIPs", _thisMachine);
            _application.Reinitialize();

            ProxyProtocolClient.SetImap(_settings, true, string.Empty);

            using (var client = new ProxyProtocolClient(143))
            {
               client.ReadUntil("* OK");
               StringAssert.StartsWith("a1 OK", Login(client, "test"));
            }

            IniFileSetting.Delete("SMTPProxyProtocolEnabled");
            IniFileSetting.Delete("SMTPProxyProtocolTrustedIPs");
            _application.Reinitialize();

            ProxyProtocolClient.SetImap(_settings, true, _thisMachine);

            using (var client = new ProxyProtocolClient(25))
            {
               client.ReadUntil("220 ");
               client.Send("QUIT\r\n");
               client.ReadUntil("221");
            }
         }
         finally
         {
            IniFileSetting.Delete("SMTPProxyProtocolEnabled");
            IniFileSetting.Delete("SMTPProxyProtocolTrustedIPs");
            _application.Reinitialize();
         }
      }
   }
}
