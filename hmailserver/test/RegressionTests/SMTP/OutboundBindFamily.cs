// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Issue 276. A dual-stack server binds its static IPv6 address
   ///    (SMTPDeliveryBindToIP) so that delivery over IPv6 carries the address its
   ///    PTR and SPF records name rather than one of the temporary addresses
   ///    Windows makes for privacy. Before this, that bind was applied to every
   ///    connection: the socket was opened as IPv6, bound, and then told to connect
   ///    to an IPv4 host, which cannot work, so every IPv4-only recipient became
   ///    unreachable and the mail sat in the queue until it was returned.
   ///
   ///    Now an address of the other family is simply not bound for that
   ///    connection, and the setting may name one address of each family, from
   ///    which the one of the recipient's family is taken.
   ///
   ///    Every delivery here goes to a fake SMTP server on 127.0.0.1 through a
   ///    route, so an IPv6 bind (::1) is always the wrong family and an IPv4 one is
   ///    always the right one. TestSetup.PerformBasicSetup clears
   ///    SMTPDeliveryBindToIP before every test.
   /// </summary>
   [TestFixture]
   public class OutboundBindFamily : TestFixtureBase
   {
      // The route TestSetup.AddRoutePointingAtLocalhost adds is for dummy-example.com,
      // so that is the domain the recipient has to be in for the fake server to be reached.
      private const string Recipient = "someone@dummy-example.com";

      [Test]
      [Description("An IPv6 bind address does not stop delivery to an IPv4 host: the bind is skipped for that connection, the log says so, and no bind error is reported.")]
      public void ABindOfTheOtherFamilyIsSkippedRatherThanFailingTheDelivery()
      {
         _settings.SMTPDeliveryBindToIP = "::1";

         string delivered = Deliver();

         StringAssert.Contains("Subject: Bind family", delivered, "The message did not reach the IPv4 host.");

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Not binding to ::1 for the connection to 127.0.0.1", log,
            "The skipped bind has to be in the log, or an administrator cannot tell why the setting was not honoured.");

         // Skipping the bind is not an error: no ERROR log at all is the expectation,
         // and ReadAndDeleteErrorLog would fail on the file not being there.
         CustomAsserts.AssertNoReportedError();
      }

      [Test]
      [Description("With one address of each family in the setting, the one of the recipient's family is bound.")]
      public void OneAddressOfEachFamilyBindsTheMatchingOne()
      {
         _settings.SMTPDeliveryBindToIP = "::1, 127.0.0.1";

         string delivered = Deliver();

         StringAssert.Contains("Subject: Bind family", delivered);

         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("from 127.0.0.1...", log, "The IPv4 address is the one bound for an IPv4 host.");
         StringAssert.DoesNotContain("Not binding to", log, "An address of the right family was there, so nothing was skipped.");
      }

      [Test]
      [Description("The negative control: an address of the right family that cannot be bound still fails the connection and is still reported, as it always was.")]
      public void AnAddressOfTheRightFamilyThatCannotBeBoundStillFails()
      {
         _settings.SMTPDeliveryBindToIP = "255.254.253.252";

         var smtpServerPort = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, smtpServerPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Recipient, 250 } });
            server.StartListen();

            TestSetup.AddRoutePointingAtLocalhost(1, smtpServerPort, true, eConnectionSecurity.eCSNone);

            new SmtpClientSimulator().Send("test@example.test", Recipient, "Bind family", "This one must not be delivered.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            ClassicAssert.AreEqual("", server.MessageData, "A bind that fails must not be followed by a connection from whatever address the system would choose.");
         }

         string errors = LogHandler.ReadAndDeleteErrorLog();
         StringAssert.Contains("Failed to bind to IP address 255.254.253.252.", errors);
      }

      /// <summary>
      ///    Sends one message to the fake server through a route and returns what
      ///    the server received.
      /// </summary>
      private static string Deliver()
      {
         var smtpServerPort = TestSetup.GetNextFreePort();
         using (var server = new SmtpServerSimulator(1, smtpServerPort))
         {
            server.AddRecipientResult(new Dictionary<string, int> { { Recipient, 250 } });
            server.StartListen();

            TestSetup.AddRoutePointingAtLocalhost(1, smtpServerPort, true, eConnectionSecurity.eCSNone);

            new SmtpClientSimulator().Send("test@example.test", Recipient, "Bind family", "Delivered over the family the host speaks.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            server.WaitForCompletion();

            return server.MessageData;
         }
      }
   }
}
