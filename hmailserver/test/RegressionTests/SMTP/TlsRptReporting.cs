// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Runtime.InteropServices;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using RegressionTests.SSL;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    SMTP TLS reporting (RFC 8460), end to end: a TLS session outcome recorded
   ///    during a real MX-path delivery is discovered via the recipient domain's
   ///    _smtp._tls TXT record, built into an application/tlsrpt+json report and
   ///    delivered as mail to the rua= mailbox.
   ///
   ///    Nothing in the shipped schedule can be tested directly - the reporter
   ///    runs hourly and only mails days that are over - which is exactly why
   ///    Utilities.SendTlsRptReports exists: it sends what has been collected
   ///    right now, optionally including today's still-accumulating bucket. An
   ///    administrator uses it to see a first report minutes after configuring
   ///    TlsRptFromAddress; this fixture uses it as the trigger that makes the
   ///    whole path observable.
   ///
   ///    The delivery that seeds the statistics is a real one: the suite's DNS
   ///    zone, given this fixture's records, answers the MX lookup with 127.0.0.1,
   ///    where the suite's SMTP simulator on port 25 plays the remote server and
   ///    offers STARTTLS with the test certificate. The outbound client attempts
   ///    opportunistic STARTTLS, certificate verification fails (self-signed), RFC
   ///    7435 forgiveness completes the handshake anyway, and the session lands in
   ///    the TLS-RPT store as a successful one. An MX host is always reached on 25,
   ///    and this server will not connect to a port it listens on itself, so its
   ///    own SMTP port 25 moves aside for the test and the seed is submitted there.
   ///    Until 6.3.6 the fixture delivered to this server's own port 25: the
   ///    connection was refused as one to itself, and the report it read was of a
   ///    session counted as successful by the connection's security setting alone,
   ///    which TLS reporting now refuses to do.
   /// </summary>
   [TestFixture]
   public class TlsRptReporting : TestFixtureBase
   {
      private const string RemoteDomain = "rpt.example.test";

      [Test]
      [Description("SendTlsRptReports refuses when TlsRptFromAddress is not configured, naming the setting - running would discard the statistics it was asked to send")]
      public void SendTlsRptReportsRefusesWhenUnconfigured()
      {
         var error = Assert.Throws<COMException>(
            () => _application.Utilities.SendTlsRptReports(true),
            "With no TlsRptFromAddress the diagnostic can only pop-and-discard, so it must refuse.");

         StringAssert.Contains("TlsRptFromAddress", error.Message,
            "The refusal must name the setting to configure. Got: " + error.Message);
      }

      [Test]
      [Description("A TLS session recorded during MX-path delivery is discovered via _smtp._tls, reported as RFC 8460 JSON and mailed to the rua target")]
      public void AReportIsGeneratedDiscoveredAndDelivered()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sender@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "tlsreports@example.test", "test");

         eConnectionSecurity originalOutboundSecurity = _settings.SMTPConnectionSecurity;

         // The three answers this delivery and its discovery ask for: the MX of the
         // remote domain, the address of that MX host, and the _smtp._tls record
         // naming the rua mailbox. Everything else is NODATA, which the suite's zone
         // answers by default.
         SuiteDns.Zone
            .WithMx(RemoteDomain, 10, "mail." + RemoteDomain)
            .WithA("mail." + RemoteDomain, "127.0.0.1")
            .WithTxt("_smtp._tls." + RemoteDomain, "v=TLSRPTv1; rua=mailto:tlsreports@example.test");

         // This server's own SMTP port 25 moves aside: the remote MX is the
         // simulator, on 25, and the seed is submitted to the port moved to.
         int submissionPort = TestSetup.GetNextFreePort();
         TCPIPPort moved = null;
         TCPIPPorts ports = _settings.TCPIPPorts;
         for (int i = 0; i < ports.Count; i++)
         {
            if (ports[i].PortNumber == 25 && ports[i].Protocol == eSessionType.eSTSMTP)
               moved = ports[i];
         }
         ClassicAssert.IsNotNull(moved, "No SMTP port 25 exists in the test environment.");

         var remote = new SmtpServerSimulator(1, 25, eConnectionSecurity.eCSSTARTTLSOptional);
         try
         {
            moved.PortNumber = submissionPort;
            moved.Save();
            _application.Stop();
            _application.Start();

            remote.SetCertificate(SslSetup.GetCertificate());
            remote.AddRecipientResult(new System.Collections.Generic.Dictionary<string, int> { ["user@" + RemoteDomain] = 250 });
            remote.StartListen();

            // The outbound client attempts opportunistic STARTTLS on MX-path
            // deliveries.
            _settings.SMTPConnectionSecurity = eConnectionSecurity.eCSSTARTTLSOptional;

            SetIniSetting("DNSQueryTimeout", "5");
            SetIniSetting("TlsRptFromAddress", "postmaster@example.test");
            _application.Reinitialize();

            // The seed: a real submission whose delivery is a real MX-path session,
            // over STARTTLS to the simulator, which takes the message.
            string errorMessage;
            new SmtpClientSimulator(false, submissionPort).Send(
               false, "sender@example.test", "test",
               "sender@example.test", "user@" + RemoteDomain, "tls-rpt seed", "Seed.", out errorMessage);

            remote.WaitForCompletion();
            StringAssert.Contains("Seed.", remote.MessageData, "The seed was not delivered to the remote MX.");

            // Delivery is asynchronous; ask until the session has been
            // recorded and reported. Popping an as-yet-empty day is harmless -
            // a session recorded afterwards recreates the bucket.
            int reportCount = 0;
            for (int attempt = 0; attempt < 120; attempt++)
            {
               reportCount = _application.Utilities.SendTlsRptReports(true);
               if (reportCount > 0)
                  break;

               Thread.Sleep(250);
            }

            ClassicAssert.AreEqual(1, reportCount,
               "One report - for the one domain with recorded sessions - should have been submitted.");

            Pop3ClientSimulator.AssertMessageCount("tlsreports@example.test", "test", 1);
            string report = Pop3ClientSimulator.AssertGetFirstMessageText("tlsreports@example.test", "test");

            // RFC 8460 section 5.3: the report mail's envelope.
            StringAssert.Contains("Report Domain: " + RemoteDomain, report,
               "The subject must carry the policy domain. Got: " + report);
            StringAssert.Contains("TLS-Report-Domain: " + RemoteDomain, report,
               "The TLS-Report-Domain field is required. Got: " + report);
            StringAssert.Contains("application/tlsrpt+json", report,
               "The report body must be an application/tlsrpt+json part. Got: " + report);
            StringAssert.Contains("!" + RemoteDomain + "!", report,
               "The attachment filename carries sender!policy-domain!date. Got: " + report);

            // And the report itself: the one session, a successful one.
            StringAssert.Contains("\"policy-domain\":\"" + RemoteDomain + "\"", report,
               "The JSON must name the policy domain. Got: " + report);
            StringAssert.Contains("\"contact-info\":\"postmaster@example.test\"", report,
               "The JSON must carry the configured contact. Got: " + report);
            StringAssert.Contains("\"total-successful-session-count\":1,\"total-failure-session-count\":0", report,
               "The delivery's TLS session must have been counted as the one successful session. Got: " + report);

            // The pop is destructive by design: the day was taken, so a second
            // call with no new sessions has nothing to send.
            ClassicAssert.AreEqual(0, (int) _application.Utilities.SendTlsRptReports(true),
               "The first call popped the day; with no sessions since, the second has nothing.");
         }
         finally
         {
            remote.Dispose();
            SuiteDns.Reset();

            SetIniSetting("DNSQueryTimeout", null);
            SetIniSetting("TlsRptFromAddress", null);
            _settings.SMTPConnectionSecurity = originalOutboundSecurity;

            if (moved != null && moved.PortNumber != 25)
            {
               moved.PortNumber = 25;
               moved.Save();
            }

            _application.Stop();
            _application.Start();
         }
      }

      // Stores a setting in the settings store, or removes it when value is null.
      // This used to rewrite the server's hMailServer.ini, which from schema 6042 is
      // only the store's copy: the restart meant to apply the value would have put
      // the stored one back and reported the edit as HM5804.
      private static void SetIniSetting(string key, string value)
      {
         if (value == null)
            IniFileSetting.Delete(key);
         else
            IniFileSetting.Write(key, value);
      }

   }
}
