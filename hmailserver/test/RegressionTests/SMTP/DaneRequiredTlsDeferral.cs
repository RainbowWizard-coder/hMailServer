// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    A host whose TLSA records require TLS, and that offers no STARTTLS, is
   ///    deferred and reported to TLS reporting as the failure it is.
   ///
   ///    RFC 7672 section 2.2: with usable TLSA records the client MUST NOT deliver
   ///    without TLS, and a failure to have it is a temporary one - the offer may
   ///    have been taken away on the way. Until 6.3.6 the missing STARTTLS was the
   ///    permanent 500 5.7.0 "Server does not support STARTTLS." and the message was
   ///    bounced at the first attempt, where TLSA records none of which is usable
   ///    already deferred; and the session was counted in the TLS-RPT store as a
   ///    SUCCESS, because what decided that was the connection's security setting
   ///    and not whether TLS was had. Now it is a deferral naming the records, and a
   ///    failure of type starttls-not-supported.
   ///
   ///    The destination is the suite's zone: an MX naming a host at 127.0.0.1 and a
   ///    DANE-EE TLSA record for its port 25, where the suite's SMTP simulator
   ///    answers without offering STARTTLS. An MX host is always reached on 25, and
   ///    this server will not deliver to a port it listens on itself, so where it
   ///    listens on 25 (the Windows bench) its SMTP port moves aside for the test
   ///    and the message is submitted there. DNSSEC validation is off for the test, so the record is
   ///    taken as published (the legacy mode, DnssecValidationEnabled=0); validated
   ///    or not, records that are there and usable require TLS the same way. The
   ///    suite runs at no retries, so the deferral comes back as a bounce that says
   ///    "Tried 1 time(s)", which a permanent refusal never says. What was recorded
   ///    is read from the debug log, where the store says it from 6.3.6.
   /// </summary>
   [TestFixture]
   public class DaneRequiredTlsDeferral : TestFixtureBase
   {
      private const string RemoteDomain = "dane-defer.example.test";
      private const string MailHost = "mail." + RemoteDomain;

      [TearDown]
      public void TakeTheRecordsBack()
      {
         IniFileSetting.Delete("DnssecValidationEnabled");
         _application.Reinitialize();
         SuiteDns.Reset();
      }

      [Test]
      [Description("A host whose usable TLSA records require TLS, and that offers no STARTTLS, is deferred (4.7.0, retried) rather than bounced with a permanent 5.7.0, and its session is a TLS-RPT failure of type starttls-not-supported, not a success.")]
      public void AHostWithUsableTlsaRecordsAndNoStartTlsIsDeferredAndReportedAsAFailure()
      {
         string log;
         string bounce = DeliverToTheHost(true, out log);

         AssertDeferredAndReported(bounce, log);
         StringAssert.DoesNotContain("Server does not support STARTTLS", bounce, "The permanent 5.7.0 of a misconfigured relay was given.");
      }

      /// <summary>
      ///    A server that refuses EHLO offers no STARTTLS either: the extension is
      ///    named only in EHLO's reply. Until 6.3.6 such a host, whose TLSA records
      ///    required TLS, was refused with the permanent 500 5.5.1 "Server does not
      ///    support EHLO command." and the message bounced at the first attempt, and
      ///    the session was not counted at all; it is the deferral and the failure
      ///    of the host above now. (A route's own STARTTLS (required) keeps the
      ///    5.5.1: SMTPClientStartTLSTests.TestDeliverToServerNotSupportingEHLORequiredConnectionSecurity.)
      /// </summary>
      [Test]
      [Description("A host whose usable TLSA records require TLS, and that refuses EHLO - so offers no STARTTLS - is deferred (4.7.0, retried) rather than refused with the permanent 5.5.1 of a server without EHLO, and its session is a TLS-RPT failure of type starttls-not-supported.")]
      public void AHostWithUsableTlsaRecordsThatRefusesEhloIsDeferredAndReportedAsAFailure()
      {
         string log;
         string bounce = DeliverToTheHost(false, out log);

         AssertDeferredAndReported(bounce, log);
         StringAssert.DoesNotContain("Server does not support EHLO command", bounce, "The permanent 5.5.1 of a server without EHLO was given.");
      }

      // A message to the zone's MX host, which its TLSA records require TLS of and
      // which offers none - its EHLO answered without STARTTLS, or refused - and the
      // bounce that comes back at no retries, with the debug log of the attempt.
      private string DeliverToTheHost(bool supportsEhlo, out string log)
      {
         Account sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "dane-sender@example.test", "test");

         // DANE-EE (3), the subject public key (1), SHA-256 (1): usable whatever
         // it matches, which no handshake gets as far as checking here.
         byte[] tlsa = new byte[3 + 32];
         tlsa[0] = 3;
         tlsa[1] = 1;
         tlsa[2] = 1;
         for (int i = 3; i < tlsa.Length; i++)
            tlsa[i] = (byte) (0xA0 + i);

         SuiteDns.Zone
            .WithMx(RemoteDomain, 10, MailHost)
            .WithA(MailHost, "127.0.0.1")
            .WithRaw("_25._tcp." + MailHost, 52, tlsa);

         IniFileSetting.Write("DnssecValidationEnabled", "0");
         _application.Reinitialize();

         // The simulator on 25 is the destination's host, offering no STARTTLS.
         int submissionPort = TestPorts.Smtp;
         TCPIPPort moved = null;
         if (TestPorts.Smtp == 25)
         {
            submissionPort = TestSetup.GetNextFreePort();
            TCPIPPorts ports = _settings.TCPIPPorts;
            for (int i = 0; i < ports.Count; i++)
            {
               TCPIPPort port = ports[i];
               if (port.PortNumber == 25 && port.Protocol == eSessionType.eSTSMTP)
                  moved = port;
            }
            Assert.IsNotNull(moved, "No SMTP port 25 to move aside.");
            moved.PortNumber = submissionPort;
            moved.Save();
            _application.Stop();
            _application.Start();
         }

         var host = new SmtpServerSimulator(1, 25, eConnectionSecurity.eCSNone);
         host.ServerSupportsEhlo = supportsEhlo;
         try
         {
            host.AddRecipientResult(new Dictionary<string, int> { ["someone@" + RemoteDomain] = 250 });
            host.StartListen();

            LogHandler.DeleteCurrentDefaultLog();

            new SmtpClientSimulator(false, submissionPort).Send(sender.Address, "someone@" + RemoteDomain, "DANE without STARTTLS", "Must not travel in the clear.");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            host.WaitForCompletion();
            Assert.IsEmpty(host.MailFromCommand, "MAIL FROM was sent to a host its TLSA records require TLS of, over a session without it.");
         }
         finally
         {
            host.Dispose();
            if (moved != null)
            {
               moved.PortNumber = 25;
               moved.Save();
               _application.Stop();
               _application.Start();
            }
         }

         string bounce = Decoded(Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test"));
         log = LogHandler.ReadCurrentDefaultLog();
         return bounce;
      }

      private static void AssertDeferredAndReported(string bounce, string log)
      {
         StringAssert.Contains("TLSA records of " + MailHost, bounce, "The failure does not name the records that required TLS.");
         StringAssert.Contains("NOT been sent in the clear", bounce, "The failure does not say the message was withheld.");
         StringAssert.Contains("Tried 1 time(s)", bounce,
            "The refusal was permanent: a host whose TLSA records require TLS and that offers no STARTTLS must be deferred (RFC 7672 section 2.2), and the retries decide.");

         StringAssert.Contains("TLS-RPT: a failed session with " + RemoteDomain + " recorded: starttls-not-supported at " + MailHost + " (policy tlsa)", log,
            "The session was not recorded as a TLS-RPT failure of type starttls-not-supported.");
         StringAssert.DoesNotContain("TLS-RPT: a successful session with " + RemoteDomain, log,
            "A session that had no TLS was counted as a successful one.");
      }

      // A bounce's text as written, its quoted-printable soft breaks and escapes undone.
      private static string Decoded(string raw)
      {
         string joined = raw.Replace("=\r\n", "").Replace("=\n", "");
         return Regex.Replace(joined, "=([0-9A-F]{2})", match => ((char) System.Convert.ToInt32(match.Groups[1].Value, 16)).ToString());
      }
   }
}
