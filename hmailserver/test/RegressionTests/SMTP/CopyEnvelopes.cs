// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Reflection;
using hMailServer;
using NUnit.Framework;
using RegressionTests.ExternalAccounts;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Which envelope this server gives a copy of mail from outside when it sends it on -
   ///    a forward, a Sieve redirect, a rule's forward, a forwarded external download -
   ///    decided in SMTP/SMTPForwarding.cpp (EnvelopeOfCopy): an envelope sender, or for a
   ///    null one the host name it is sent under, in the organisation of a domain the
   ///    message's From names only when the message passed DMARC for that organisation when
   ///    it arrived.
   ///
   ///    Each case sends, from outside, a message whose From names the hosted test domain
   ///    example.test and that did not pass DMARC for it, and captures what this server
   ///    sends on through a route for example.com: the rewrite is withheld and the copy
   ///    keeps the envelope sender the message came with, or a copy that would still be
   ///    sent with an identity of example.test - a null sender under a host name of
   ///    example.test, or the envelope sender it came with at a subdomain of example.test -
   ///    is not sent on, and the message stays in the mailbox; and a second forward of it
   ///    keeps the envelope sender it came with too. The positive controls: a forward of
   ///    mail that passed DMARC for example.test on arrival is still sent on, SRS-rewritten,
   ///    and so are a second forward of it and an alias's copy of a forward of it, since
   ///    a copy carries what DMARC passed for when the mail arrived; and an ordinary
   ///    forward of third-party mail is SRS-rewritten as before. (A managed list's member
   ///    forwarding a posting, a released message's forward and a journal copy's forward
   ///    are in SMTP.MailingLists, AntiSpam.Quarantine and SMTP.Journaling.)
   /// </summary>
   [TestFixture]
   public class CopyEnvelopes : TestFixtureBase
   {
      private const string ForwardTarget = "external@example.com";
      private const string HostedFrom = "The CEO <ceo@example.test>";
      private const string OutsideSender = "x@outside-sender.example";
      private const string ThirdPartySender = "alice@external-sender.example";

      private static string MessageText(string to, string from)
      {
         return
            "From: " + from + "\r\n" +
            "To: " + to + "\r\n" +
            "Subject: copy-envelope probe\r\n" +
            "\r\n" +
            "Please pay the invoice.\r\n";
      }

      // A route for example.com pointing at a capture server, the copy sent on captured
      // on the wire. DeliverThenDrain runs the send and waits for the delivery queue to
      // empty, so that a copy that WAS made has reached the capture by the time its
      // envelope is read, and one that was not leaves the capture silent.
      private sealed class Capture : IDisposable
      {
         private readonly SmtpServerSimulator _server;
         private readonly Route _route;
         private readonly Settings _settings;

         public Capture(Settings settings)
         {
            _settings = settings;

            int port = TestSetup.GetNextFreePort();
            _server = new SmtpServerSimulator(1, port);
            _server.SecondsToWaitBeforeTerminate = 60;
            _server.AddRecipientResult(new Dictionary<string, int> { { ForwardTarget, 250 } });
            _server.StartListen();

            _route = settings.Routes.Add();
            _route.DomainName = "example.com";
            _route.TargetSMTPHost = "localhost";
            _route.TargetSMTPPort = port;
            _route.NumberOfTries = 1;
            _route.MinutesBetweenTry = 5;
            _route.Save();
         }

         public void DeliverThenDrain(Action send)
         {
            send();
            // Force every pending delivery out and wait for the queue to empty: a copy
            // that was made reaches the capture; one that was withheld never queues.
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }

         // The MAIL FROM the copy was sent on with, or "" when no copy was sent on.
         public string SentEnvelope
         {
            get
            {
               string command = _server.MailFromCommand ?? "";
               if (command.Length == 0 || _server.NumberOfConnectedClients == 0)
                  return "";

               int start = command.IndexOf('<');
               int end = command.LastIndexOf('>');
               if (start < 0 || end <= start)
                  return "";

               return command.Substring(start + 1, end - start - 1);
            }
         }

         public bool AnythingSentOn
         {
            get { return _server.NumberOfConnectedClients > 0 && (_server.Conversation ?? "").ToUpperInvariant().Contains("MAIL FROM"); }
         }

         public void Dispose()
         {
            try { _settings.Routes.DeleteByDBID(_route.ID); } catch (Exception) { }
            _server.Dispose();
         }
      }

      private Account AddForwarder(string address, string target = ForwardTarget)
      {
         var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, "test");
         forwarder.ForwardEnabled = true;
         forwarder.ForwardAddress = target;
         forwarder.ForwardKeepOriginal = true;
         forwarder.Save();
         return forwarder;
      }

      // SRS on, the DMARC check on, scoring out of the way, and example.test's DMARC policy
      // with SPF for the loopback at its subdomain bounces.example.test: mail from
      // bounce@bounces.example.test with a From at example.test then passes SPF for that
      // subdomain, which aligns organisationally with the From, and so passes DMARC for
      // example.test on arrival (SuiteDns, the suite's own resolver).
      private void WithDmarcPassing(Action body)
      {
         int markBefore = (int) _settings.AntiSpam.SpamMarkThreshold;
         int deleteBefore = (int) _settings.AntiSpam.SpamDeleteThreshold;
         bool dmarcBefore = _settings.AntiSpam.DMARCEnabled;
         try
         {
            IniFileSetting.Write("SRSEnabled", "1");
            IniFileSetting.Write("SRSSecret", "copy-envelope-secret");
            _settings.AntiSpam.SpamMarkThreshold = 100;
            _settings.AntiSpam.SpamDeleteThreshold = 1000;
            _settings.AntiSpam.DMARCEnabled = true;
            _application.Reinitialize();

            SuiteDns.Zone
               .WithTxt("_dmarc.example.test", "v=DMARC1; p=none")
               .WithTxt("bounces.example.test", "v=spf1 ip4:127.0.0.1 -all");

            body();
         }
         finally
         {
            SuiteDns.Reset();
            IniFileSetting.Write("SRSEnabled", "0");
            IniFileSetting.Write("SRSSecret", "");
            _settings.AntiSpam.SpamMarkThreshold = markBefore;
            _settings.AntiSpam.SpamDeleteThreshold = deleteBefore;
            _settings.AntiSpam.DMARCEnabled = dmarcBefore;
            _application.Reinitialize();
         }
      }

      private static void AssertSrsRewrittenInExampleTest(string envelope, string how)
      {
         Assert.IsNotEmpty(envelope, how + " was not sent on; it should be, as a forward of the message itself is.");
         Assert.IsTrue(envelope.ToLowerInvariant().StartsWith("srs0=") && envelope.ToLowerInvariant().EndsWith("@example.test"),
            how + " should be sent on with the SRS envelope at example.test that a forward of the message itself is given. MAIL FROM:<" + envelope + ">");
      }

      private void AssertNotInTheFromsOrganisation(string envelope, string how)
      {
         Assert.IsNotEmpty(envelope,
            how + " was not sent on at all; it should keep the envelope sender it came with and still be delivered.");
         Assert.IsFalse(envelope.ToLowerInvariant().EndsWith("@example.test"),
            how + " was sent on with an envelope sender in example.test, the organisation its From names, though it did not pass DMARC for it: MAIL FROM:<" + envelope +
            ">.");
         Assert.IsFalse(envelope.ToLowerInvariant().StartsWith("srs0="),
            how + " was SRS-rewritten into the organisation its From names, though it did not pass DMARC for it: MAIL FROM:<" + envelope + ">.");
      }

      [Test]
      [Description("With SRS on, a forward of mail from outside whose From names the hosted test domain, and that did not pass DMARC for it on arrival, keeps the envelope sender it came with, not an SRS0 address at example.test.")]
      public void AnSrsForwardOfOutsideMailKeepsTheEnvelopeItCameWith()
      {
         try
         {
            IniFileSetting.Write("SRSEnabled", "1");
            IniFileSetting.Write("SRSSecret", "copy-envelope-secret");
            _application.Reinitialize();

            var forwarder = AddForwarder("ce-srs-forwarder@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw(OutsideSender, forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

               AssertNotInTheFromsOrganisation(capture.SentEnvelope, "An SRS forward of the message");
            }
         }
         finally
         {
            IniFileSetting.Write("SRSEnabled", "0");
            IniFileSetting.Write("SRSSecret", "");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("With RewriteEnvelopeFromWhenForwarding on, a forward of mail from outside whose From names the hosted test domain, and that did not pass DMARC for it on arrival, keeps the envelope sender it came with, not the forwarding account's address at example.test.")]
      public void ARewrittenForwardOfOutsideMailKeepsTheEnvelopeItCameWith()
      {
         bool before = _settings.RewriteEnvelopeFromWhenForwarding;
         try
         {
            _settings.RewriteEnvelopeFromWhenForwarding = true;

            var forwarder = AddForwarder("ce-rewrite-forwarder@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw(OutsideSender, forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

               AssertNotInTheFromsOrganisation(capture.SentEnvelope, "A rewritten forward of the message");
            }
         }
         finally
         {
            _settings.RewriteEnvelopeFromWhenForwarding = before;
         }
      }

      [Test]
      [Description("A null-sender forward of mail from outside whose From names the hosted test domain, and that did not pass DMARC for it on arrival, is not sent on under a host name of example.test; the message stays in the mailbox.")]
      public void ANullSenderForwardOfOutsideMailIsNotSentUnderAHostNameOfTheFromsOrganisation()
      {
         string hostNameBefore = _settings.HostName;
         try
         {
            _settings.HostName = "mail.example.test";

            var forwarder = AddForwarder("ce-null-forwarder@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw("", forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

               Assert.IsFalse(capture.AnythingSentOn,
                  "A null-sender forward was sent on under EHLO mail.example.test, a host name of the organisation its From names, though the message did not pass DMARC for it.");
            }

            Pop3ClientSimulator.AssertMessageCount(forwarder.Address, "test", 1);
         }
         finally
         {
            _settings.HostName = hostNameBefore;
         }
      }

      [Test]
      [Description("A null-sender Sieve redirect of mail from outside whose From names the hosted test domain, and that did not pass DMARC for it on arrival, is not sent on under a host name of example.test; the message stays in the mailbox.")]
      public void ANullSenderSieveRedirectOfOutsideMailIsNotSentUnderAHostNameOfTheFromsOrganisation()
      {
         string hostNameBefore = _settings.HostName;
         try
         {
            _settings.HostName = "mail.example.test";

            var redirector = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ce-null-redirect@example.test", "test");
            redirector.GetType().InvokeMember("SieveScript", BindingFlags.SetProperty, null, redirector,
               new object[] { "redirect \"" + ForwardTarget + "\";\r\n" });

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw("", redirector.Address, MessageText(redirector.Address, HostedFrom)));

               Assert.IsFalse(capture.AnythingSentOn,
                  "A null-sender Sieve redirect was sent on under EHLO mail.example.test, a host name of the organisation its From names, though the message did not pass DMARC for it.");
            }

            Pop3ClientSimulator.AssertMessageCount(redirector.Address, "test", 1);
         }
         finally
         {
            _settings.HostName = hostNameBefore;
         }
      }

      [Test]
      [Description("A forward of mail from outside whose From names the hosted test domain, that did not pass DMARC for it on arrival, and whose own envelope sender is at a subdomain of that domain, is not sent on to an address outside; the message stays in the mailbox.")]
      public void AForwardKeepingAnEnvelopeAtASubdomainOfTheHostedDomainIsWithheld()
      {
         var forwarder = AddForwarder("ce-subdomain-forwarder@example.test");

         using (var capture = new Capture(_settings))
         {
            // SRS and the envelope-from rewrite are off (the defaults), so the forward keeps
            // the inbound envelope sender; it is at bounces.example.test, a subdomain of the
            // hosted example.test, which is in example.test's organisation.
            capture.DeliverThenDrain(() =>
               new SmtpClientSimulator().SendRaw("x@bounces.example.test", forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

            Assert.IsFalse(capture.AnythingSentOn,
               "A forward was sent on with the envelope sender it came with at bounces.example.test, a subdomain of the hosted example.test, though the message did not pass DMARC for example.test.");
         }

         Pop3ClientSimulator.AssertMessageCount(forwarder.Address, "test", 1);
      }

      [Test]
      [Description("A download from an external account whose From is an address hosted here keeps the null envelope sender rather than taking that From, so a forward of it is not sent on under a host name of example.test; the download stays in the mailbox.")]
      public void AForwardedDownloadDoesNotAdoptItsHostedFromAsTheEnvelopeSender()
      {
         string hostNameBefore = _settings.HostName;
         try
         {
            _settings.HostName = "mail.example.test";

            var forwarder = AddForwarder("ce-fetch-forwarder@example.test");

            int popPort = TestSetup.GetNextFreePort();
            var messages = new List<ScriptedPop3Server.RemoteMessage>
            {
               new ScriptedPop3Server.RemoteMessage("ce-uid-1", MessageText(forwarder.Address, HostedFrom))
            };

            var fetchAccount = forwarder.FetchAccounts.Add();
            fetchAccount.Enabled = true;
            fetchAccount.MinutesBetweenFetch = 60;
            fetchAccount.Name = "copy-envelope probe POP3";
            fetchAccount.ServerType = 0;
            fetchAccount.Username = "remote@dummy-example.com";
            fetchAccount.Password = "far-side-secret";
            fetchAccount.UseSSL = false;
            fetchAccount.ServerAddress = "localhost";
            fetchAccount.Port = popPort;
            fetchAccount.ProcessMIMERecipients = false;
            fetchAccount.DaysToKeepMessages = -1;
            fetchAccount.UseAntiSpam = false;
            fetchAccount.UseAntiVirus = false;
            fetchAccount.Save();

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
               {
                  using (var server = new ScriptedPop3Server(popPort, messages))
                  {
                     server.StartListen();
                     fetchAccount.DownloadNow();
                     server.WaitForCompletion();
                  }
               });

               Assert.IsFalse(capture.AnythingSentOn,
                  "A forward of the download was sent on to an address outside: the download should keep the null envelope sender, not its hosted From, ceo@example.test, and so not be sent on under mail.example.test.");
            }

            Pop3ClientSimulator.AssertMessageCount(forwarder.Address, "test", 1);
         }
         finally
         {
            _settings.HostName = hostNameBefore;
         }
      }

      [Test]
      [Description("A positive control: a forward of mail that passed DMARC for example.test on arrival - the From names example.test and the envelope's subdomain publishes SPF that aligns - is still SRS-rewritten and sent on, because the organisation it names is one the message authenticated as.")]
      public void AForwardOfMailThatPassedDmarcForItsFromIsStillSentOn()
      {
         WithDmarcPassing(() =>
         {
            var forwarder = AddForwarder("ce-dmarc-forwarder@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

               string envelope = capture.SentEnvelope;
               Assert.IsNotEmpty(envelope, "A forward of mail that passed DMARC for example.test was not sent on; it should be.");
               Assert.IsTrue(envelope.ToLowerInvariant().StartsWith("srs0=") && envelope.ToLowerInvariant().EndsWith("@example.test"),
                  "A forward of mail that passed DMARC for example.test should keep its SRS rewrite at example.test. MAIL FROM:<" + envelope + ">");
            }
         });
      }

      [Test]
      [Description("A positive control: mail that passed DMARC for example.test on arrival, forwarded by an account here to another account here that forwards it to an address outside, is sent on by the second forward with the SRS envelope the first gave it: the first forward's copy carries what DMARC passed for when the mail arrived.")]
      public void ASecondForwardOfMailThatPassedDmarcIsStillSentOn()
      {
         WithDmarcPassing(() =>
         {
            var second = AddForwarder("ce-second-forwarder@example.test");
            var first = AddForwarder("ce-first-forwarder@example.test", second.Address);

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", first.Address, MessageText(first.Address, HostedFrom)));

               Pop3ClientSimulator.AssertMessageCount(second.Address, "test", 1);
               AssertSrsRewrittenInExampleTest(capture.SentEnvelope, "The second forward of mail that passed DMARC for example.test");
            }
         });
      }

      [Test]
      [Description("A positive control: mail that passed DMARC for example.test on arrival, forwarded by an account here to an alias here that expands to an address outside, is sent on to that address: the forward's copy carries what DMARC passed for when the mail arrived.")]
      public void AForwardToAnAliasExpandingOutsideOfMailThatPassedDmarcIsStillSentOn()
      {
         WithDmarcPassing(() =>
         {
            SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "ce-alias@example.test", ForwardTarget);
            var forwarder = AddForwarder("ce-alias-forwarder@example.test", "ce-alias@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", forwarder.Address, MessageText(forwarder.Address, HostedFrom)));

               AssertSrsRewrittenInExampleTest(capture.SentEnvelope, "The forward of mail that passed DMARC for example.test, to an alias that expands to " + ForwardTarget + ",");
            }
         });
      }

      [Test]
      [Description("Mail from outside whose From names the hosted test domain and that did not pass DMARC for it on arrival, forwarded by an account here to another that forwards it outside, is sent on by the second forward with the envelope sender it came with, never with one in example.test: a copy carries no pass the message did not have.")]
      public void ASecondForwardOfOutsideMailKeepsTheEnvelopeItCameWith()
      {
         try
         {
            IniFileSetting.Write("SRSEnabled", "1");
            IniFileSetting.Write("SRSSecret", "copy-envelope-secret");
            _application.Reinitialize();

            var second = AddForwarder("ce-second-srs-forwarder@example.test");
            var first = AddForwarder("ce-first-srs-forwarder@example.test", second.Address);

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw(OutsideSender, first.Address, MessageText(first.Address, HostedFrom)));

               Pop3ClientSimulator.AssertMessageCount(second.Address, "test", 1);
               AssertNotInTheFromsOrganisation(capture.SentEnvelope, "The second forward of the message");
            }
         }
         finally
         {
            IniFileSetting.Write("SRSEnabled", "0");
            IniFileSetting.Write("SRSSecret", "");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A positive control: an ordinary forward of third-party mail, whose From names a domain that is not the forwarder's organisation, is SRS-rewritten and sent on as before.")]
      public void AnOrdinaryForwardOfThirdPartyMailIsStillSrsRewritten()
      {
         try
         {
            IniFileSetting.Write("SRSEnabled", "1");
            IniFileSetting.Write("SRSSecret", "copy-envelope-secret");
            _application.Reinitialize();

            var forwarder = AddForwarder("ce-third-party-forwarder@example.test");

            using (var capture = new Capture(_settings))
            {
               capture.DeliverThenDrain(() =>
                  new SmtpClientSimulator().SendRaw(ThirdPartySender, forwarder.Address,
                     MessageText(forwarder.Address, "Alice <" + ThirdPartySender + ">")));

               string envelope = capture.SentEnvelope;
               Assert.IsNotEmpty(envelope, "An ordinary third-party forward was not sent on.");
               Assert.IsTrue(envelope.ToLowerInvariant().StartsWith("srs0=") && envelope.ToLowerInvariant().EndsWith("@example.test"),
                  "An ordinary third-party forward should be SRS-rewritten at the forwarding domain. MAIL FROM:<" + envelope + ">");
            }
         }
         finally
         {
            IniFileSetting.Write("SRSEnabled", "0");
            IniFileSetting.Write("SRSSecret", "");
            _application.Reinitialize();
         }
      }
   }
}
