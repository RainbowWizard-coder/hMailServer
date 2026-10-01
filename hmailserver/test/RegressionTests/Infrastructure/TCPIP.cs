// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   [TestFixture]
   public class TCPIP : TestFixtureBase
   {
      // The MX tests used to resolve hmailserver.com and compare the server's answer
      // to the test process's own Dns.GetHostEntry of mail.hmailserver.com. That made
      // them a test of somebody else's zone and of this machine's resolver, and on
      // 19 August 2026 the control lookup itself threw SocketException 11001 mid-gate
      // and failed the run - reporting "No such host is known" about a server that had
      // done nothing wrong. The name resolved again minutes later.
      //
      // Served locally, the assertion gets stronger rather than weaker: the expected
      // address is a value this fixture chose, so a server that returned the right
      // shape of answer for the wrong host would now be caught, where comparing two
      // live lookups of the same name could not.
      private const string MailDomain = "mxlookup.test";
      private const string MailExchange = "mail.mxlookup.test";

      // TEST-NET-1 (RFC 5737), reserved for documentation. Nothing connects to it -
      // this fixture only asks what the address IS.
      private const string MailExchangeAddress = "192.0.2.25";

      [OneTimeSetUp]
      public void ServeTheNamesThisFixtureNeeds()
      {
         SuiteDns.Zone
            .WithMx(MailDomain, 10, MailExchange)
            .WithA(MailExchange, MailExchangeAddress);
      }

      [OneTimeTearDown]
      public void ForgetThem()
      {
         SuiteDns.Reset();
      }

      [Test]
      [Category("TCP/IP implementation")]
      [Description("Ensure that basic resolution of existing domain names work.")]
      public void TestMXQueryExistingDomain()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         var query = application.Utilities.GetMailServer("martin@" + MailDomain);

         Assert.AreEqual(MailExchangeAddress, query,
            "The MX of " + MailDomain + " is " + MailExchange + ", whose address is " +
            MailExchangeAddress + ". Got: " + query);
      }

      [Test]
      [Category("TCP/IP implementation")]
      [Description("Ensure that basic resolution of non-existing domain names work.")]
      public void TestMXQueryNonExistentDomain()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Answered NODATA by the fixture's resolver, which is the authoritative "no
         // such record" a real lookup of a non-existent domain gets - and, unlike the
         // real thing, it arrives immediately and cannot be a timeout in disguise.
         var query = application.Utilities.GetMailServer("martin@23sdfakm52lvcxbmvxcbmdtapvxcpaasdf.com");
         Assert.IsTrue(query.Length == 0, "Got: " + query);
      }

      [Test]
      [Category("TCP/IP implementation")]
      [Description("Ensure that it's possible to re-configure which ports hMailServer should listen on")]
      public void TestPortOpening()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         application.Settings.TCPIPPorts.SetDefault();

         var tcpConnection = new TcpConnection();

         application.Stop();

         var ports = application.Settings.TCPIPPorts;
         for (var i = 0; i < ports.Count; i++)
         {
            var testPort = ports[i];
            if (testPort.Protocol == eSessionType.eSTIMAP)
               testPort.PortNumber = 14300;
            else if (testPort.Protocol == eSessionType.eSTPOP3)
               testPort.PortNumber = 11000;
            else if (testPort.Protocol == eSessionType.eSTSMTP && testPort.PortNumber == 25)
               testPort.PortNumber = 2500;

            testPort.Save();
         }

         application.Start();

         Assert.IsTrue(tcpConnection.TestConnect(2500));
         Assert.IsTrue(tcpConnection.TestConnect(11000));
         Assert.IsTrue(tcpConnection.TestConnect(14300));

         application.Stop();

         var port = application.Settings.TCPIPPorts.Add();
         port.Protocol = eSessionType.eSTSMTP;
         port.PortNumber = 25000;
         port.Save();

         application.Start();

         // Try to connect to the new port
         Assert.IsTrue(tcpConnection.TestConnect(25000));

         application.Stop();

         // Delete the port again
         application.Settings.TCPIPPorts.SetDefault();

         application.Start();

         Assert.IsTrue(tcpConnection.TestConnect(25));
         Assert.IsTrue(tcpConnection.TestConnect(587));
         Assert.IsTrue(tcpConnection.TestConnect(110));
         Assert.IsTrue(tcpConnection.TestConnect(143));
      }

      /// <summary>
      ///    One port number listed twice, on the wildcard and on 127.0.0.1, is two
      ///    listeners on Windows, the one on 127.0.0.1 taking the connections made to
      ///    it - an administrator's way of giving local programs a port of their own
      ///    settings. Since 6.3.6 a mail listener claims its port with
      ///    SO_EXCLUSIVEADDRUSE, which would refuse the second of such a pair, so
      ///    IOService leaves it off for a number listed twice in one family. Against a
      ///    server that set it on every mail listener the 127.0.0.1 listener is refused
      ///    (HM4316), a connection to 127.0.0.1 is answered by the one on the wildcard,
      ///    and this fails. The pair is SMTP on 0.0.0.0 and POP3 on 127.0.0.1, so the
      ///    greeting says which answered. Linux refuses the second bind of such a pair
      ///    whatever is set; there SetDefault, which no REST route does, skips this.
      /// </summary>
      [Test]
      [Category("TCP/IP implementation")]
      [Description("A port number listed on 0.0.0.0 and again on 127.0.0.1 is two listeners, each answering its own address.")]
      public void OneNumberOnTheWildcardAndOnLoopbackIsTwoListeners()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         application.Settings.TCPIPPorts.SetDefault();
         application.Stop();

         try
         {
            const int shared = 25001;

            var wildcard = application.Settings.TCPIPPorts.Add();
            wildcard.Protocol = eSessionType.eSTSMTP;
            wildcard.Address = "0.0.0.0";
            wildcard.PortNumber = shared;
            wildcard.Save();

            var loopback = application.Settings.TCPIPPorts.Add();
            loopback.Protocol = eSessionType.eSTPOP3;
            loopback.Address = "127.0.0.1";
            loopback.PortNumber = shared;
            loopback.Save();

            application.Start();

            using (var connection = new TcpConnection())
            {
               Assert.IsTrue(connection.Connect(System.Net.IPAddress.Loopback, shared), "Nothing listens on 127.0.0.1:" + shared + ".");
               string greeting = connection.Receive();
               Assert.IsTrue(greeting != null && greeting.StartsWith("+OK"),
                  "127.0.0.1:" + shared + " was not answered by the POP3 listener on 127.0.0.1 but with: " + greeting);
            }

            // Neither listener reported a port it could not bind.
            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            application.Stop();
            application.Settings.TCPIPPorts.SetDefault();
            application.Start();
         }
      }

      [Test]
      public void TestDefaultPortCount()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         application.Settings.TCPIPPorts.SetDefault();

         application.Stop();
         application.Start();

         var ports = application.Settings.TCPIPPorts;

         Assert.AreEqual(4, ports.Count);
      }
   }
}
