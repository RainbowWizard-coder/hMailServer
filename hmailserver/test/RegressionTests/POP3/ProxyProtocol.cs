// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.POP3
{
   /// <summary>
   ///    The PROXY protocol (HAProxy, versions 1 and 2) on the POP3 listener, since
   ///    6.3.7: a load balancer named on POP3ProxyProtocolTrustedIPs passes on the real
   ///    client's address, and the IP ranges, the auto-ban and the account's devices act
   ///    on that client instead of on the balancer. Off by default, with nobody trusted,
   ///    and a list of its own. The positive tests are their own negative controls: the
   ///    refusal each asserts comes from the range of the address the header named, and
   ///    the loopback range the connection really comes from would let the command through.
   /// </summary>
   [TestFixture]
   public class ProxyProtocol : TestFixtureBase
   {
      private Account _account;
      private string _thisMachine;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "proxypop3@example.test", "test");
         _thisMachine = ProxyProtocolClient.AddressTheServerSees();
         _settings.ClearLogonFailureList();
      }

      [TearDown]
      public void TurnTheProxyProtocolOff()
      {
         ProxyProtocolClient.TurnOffImapAndPop3(_settings);
      }

      /// <summary>
      ///    A range for one address that outranks the defaults; PerformBasicSetup puts the
      ///    default ranges back before every test.
      /// </summary>
      private void AddRange(string name, string address, bool allowPop3, bool requireTlsForAuth)
      {
         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.Priority = 50;
         range.LowerIP = address;
         range.UpperIP = address;
         range.AllowSMTPConnections = true;
         range.AllowIMAPConnections = true;
         range.AllowPOP3Connections = allowPop3;
         range.AllowDeliveryFromRemoteToLocal = true;
         range.AllowDeliveryFromLocalToLocal = true;
         range.RequireSSLTLSForAuth = requireTlsForAuth;
         range.EnableSpamProtection = false;
         range.Save();
      }

      private string Send(ProxyProtocolClient client, string command)
      {
         client.Send(command + "\r\n");
         return client.ReadUntil("\r\n");
      }

      private void Quit(ProxyProtocolClient client)
      {
         StringAssert.StartsWith("+OK", Send(client, "QUIT"));
      }

      [Test]
      [Description("A version 1 header from a trusted peer gives the session the client's address, and " +
                   "the client's IP range governs it: a range that requires TLS for authentication refuses " +
                   "USER on the plain port. The loopback range the connection comes from requires nothing, " +
                   "so were the header not read USER would be answered +OK.")]
      public void Version1FromTrustedPeerPutsTheClientUnderItsOwnRange()
      {
         AddRange("proxy-pop3-v1-tls", "198.51.100.81", true, true);
         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.81", 45201, TestPorts.Actual(110)));
            client.ReadUntil("+OK");

            string reply = Send(client, "USER " + _account.Address);

            StringAssert.StartsWith("-ERR", reply,
               "USER must be refused, because the range of the address the header named requires TLS for " +
               "authentication. +OK means the session ran as the proxy. Got: " + reply);
            StringAssert.Contains("SSL/TLS", reply);
         }
      }

      [Test]
      [Description("A binary version 2 header from a trusted peer is read the same way: the client's " +
                   "range refuses USER on the plain port.")]
      public void Version2FromTrustedPeerPutsTheClientUnderItsOwnRange()
      {
         AddRange("proxy-pop3-v2-tls", "198.51.100.82", true, true);
         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version2Tcp4("198.51.100.82", 45202, TestPorts.Actual(110)));
            client.ReadUntil("+OK");

            string reply = Send(client, "USER " + _account.Address);

            StringAssert.StartsWith("-ERR", reply,
               "USER must be refused by the range of the address the version 2 header named. Got: " + reply);

            // The refusal of the range, and not of a line the header's bytes made: were
            // the header not read, its bytes would be answered as invalid commands first.
            StringAssert.Contains("SSL/TLS", reply);
         }
      }

      [Test]
      [Description("The header is required from a peer on the list: a POP3 command where the header is due " +
                   "is an invalid header, answered with nothing and reported in the error log.")]
      public void TrustedPeerThatSendsNoHeaderIsClosedAndReported()
      {
         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send("CAPA\r\n");

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "A peer on the list that sends no header must be answered with nothing. Got: " + received);
         }

         CustomAsserts.AssertReportedError("Invalid PROXY protocol header received from the trusted proxy");
      }

      [Test]
      [Description("A client whose range does not allow POP3 is turned away at the header, before the " +
                   "greeting, as it would have been had it connected itself.")]
      public void ClientWhoseRangeRefusesPop3IsClosedBeforeTheGreeting()
      {
         AddRange("proxy-pop3-refused", "198.51.100.83", false, false);
         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.83", 45203, TestPorts.Actual(110)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "The client's range does not allow POP3, so the connection must be closed without a greeting. Got: " + received);
         }
      }

      [Test]
      [Description("The auto-ban bans the client behind the proxy and not the proxy: three failed logons " +
                   "from the address the headers named make an auto-ban range for that address alone; the " +
                   "same client is then turned away at its next header, and another client behind the same " +
                   "proxy is still served. Were the header not read, the ban would be on this machine.")]
      public void AutoBanActsOnTheClientBehindTheProxy()
      {
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 3;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;

         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         for (int attempt = 1; attempt <= 3; attempt++)
         {
            using (var client = new ProxyProtocolClient(110))
            {
               client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.84", 45210 + attempt, TestPorts.Actual(110)));
               client.ReadUntil("+OK");

               StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));

               string reply = Send(client, "PASS wrong-password");

               StringAssert.StartsWith("-ERR", reply, "Attempt " + attempt + ": " + reply);

               if (attempt == 3)
                  StringAssert.Contains("Too many invalid logon attempts", reply);
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
         ClassicAssert.AreEqual("198.51.100.84", ban.LowerIP, "The ban must be on the client the headers named, not on " + _thisMachine);
         ClassicAssert.AreEqual("198.51.100.84", ban.UpperIP);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.84", 45220, TestPorts.Actual(110)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received, "The banned client must be turned away at its header. Got: " + received);
         }

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.85", 45221, TestPorts.Actual(110)));
            client.ReadUntil("+OK");

            StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));
            StringAssert.StartsWith("+OK", Send(client, "PASS test"),
               "Another client behind the same proxy must be served.");
            Quit(client);
         }
      }

      [Test]
      [Description("While the switch is on, a PROXY header from a peer that is not on the list closes the " +
                   "connection without an answer, as the SMTP listener does.")]
      public void HeaderFromAnUntrustedPeerClosesTheConnection()
      {
         ProxyProtocolClient.SetPop3(_settings, true, "192.0.2.99");

         using (var client = new ProxyProtocolClient(110))
         {
            client.ReadUntil("+OK");

            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.86", 45204, TestPorts.Actual(110)));

            string received = client.ReadUntilRemoteClose();

            ClassicAssert.AreEqual("", received,
               "A header from a peer that is not trusted must close the connection without an answer. Got: " + received);
         }
      }

      [Test]
      [Description("Off, the shipped default, means off: PROXY is the invalid command it always was and the " +
                   "session carries on.")]
      public void ProxyLineIsAnInvalidCommandWhenTheSwitchIsOff()
      {
         using (var client = new ProxyProtocolClient(110))
         {
            client.ReadUntil("+OK");

            client.Send(ProxyProtocolClient.Version1Tcp4("198.51.100.87", 45205, TestPorts.Actual(110)));
            string reply = client.ReadUntil("\r\n");

            StringAssert.StartsWith("-ERR", reply, "Got: " + reply);

            StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));
         }
      }

      [Test]
      [Description("A version 1 UNKNOWN header and a version 2 LOCAL command name no client, so the " +
                   "session keeps the address it comes from, whose range lets the logon through.")]
      public void HeadersThatNameNoClientKeepTheConnectionsOwnAddress()
      {
         ProxyProtocolClient.SetPop3(_settings, true, _thisMachine);

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send("PROXY UNKNOWN\r\n");
            client.ReadUntil("+OK");

            StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));
            StringAssert.StartsWith("+OK", Send(client, "PASS test"));

            // The mailbox's lock is let go at QUIT; a close alone lets it go when the
            // server has processed it, which the next logon could arrive before.
            Quit(client);
         }

         using (var client = new ProxyProtocolClient(110))
         {
            client.Send(ProxyProtocolClient.Version2(false /* LOCAL */, 0x00 /* AF_UNSPEC */, new byte[0]));
            client.ReadUntil("+OK");

            StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));
            StringAssert.StartsWith("+OK", Send(client, "PASS test"));
            Quit(client);
         }
      }

      [Test]
      [Description("Each listener has its own list: with IMAP trusting this machine and POP3's switch on " +
                   "with its list empty, a POP3 connection from this machine is greeted without a header.")]
      public void ThePeersTrustedByTheImapListenerAreNotTrustedHere()
      {
         ProxyProtocolClient.SetImap(_settings, true, _thisMachine);
         ProxyProtocolClient.SetPop3(_settings, true, string.Empty);

         using (var client = new ProxyProtocolClient(110))
         {
            client.ReadUntil("+OK");

            StringAssert.StartsWith("+OK", Send(client, "USER " + _account.Address));
            StringAssert.StartsWith("+OK", Send(client, "PASS test"));
            Quit(client);
         }
      }
   }
}
