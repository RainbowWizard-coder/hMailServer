// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    The spam quarantine: hold for review instead of refusing.
   ///
   ///    Before this, the two outcomes were "mark it" and "refuse it", and refusing
   ///    happens DURING the SMTP conversation with a 550 - the message is never
   ///    accepted, so there is nothing to review and nothing to release. Quarantining
   ///    is therefore a different decision rather than a tidier one, and the tests
   ///    below are shaped around the thing that actually changes: the SMTP reply.
   ///
   ///    A refused message is answered 550/554 and the sender knows. A quarantined one
   ///    is answered 250 and the sender believes it was delivered - which is what makes
   ///    a false positive recoverable without backscatter, and also what makes the
   ///    review queue the only place that message now exists. TheSenderIsToldItWasAccepted
   ///    is the test that pins that difference, and ANonQuarantinedServerStillRefuses is
   ///    its negative control: with the feature off, nothing about the old behaviour
   ///    moves.
   /// </summary>
   [TestFixture]
   public class Quarantine : TestFixtureBase
   {
      private const string SurblTestPoint = "surbl-org-permanent-test-point.com.multi.surbl.org";
      private const string SpamBody =
         "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-";

      private Account _recipient;

      [OneTimeSetUp]
      public void ServeTheNamesThisFixtureNeeds()
      {
         // The verdict has to be real - this fixture is about what happens AFTER a
         // message is judged spam - but it must not depend on a live blacklist, for
         // the reasons recorded against every other fixture that used to.
         SuiteDns.Zone.WithA(SurblTestPoint, "127.0.0.2");
      }

      [OneTimeTearDown]
      public void RestoreTheDefaults()
      {
         SuiteDns.Reset();

         ServerIniFile.SetSetting("QuarantineEnabled", null);
         RestartServerAndReacquireCom();
      }

      [SetUp]
      public new void SetUp()
      {
         _recipient = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quarantined@example.test", "test");

         var antiSpam = _application.Settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 1;
         antiSpam.SpamDeleteThreshold = 5;

         var surbl = antiSpam.SURBLServers[0];
         surbl.Active = true;
         surbl.Score = 10;
         surbl.Save();

         EmptyTheQuarantine();
      }

      [TearDown]
      public new void TearDown()
      {
         EmptyTheQuarantine();

         ServerIniFile.SetSetting("QuarantineEnabled", null);
         _application.Reinitialize();

         var surbl = _application.Settings.AntiSpam.SURBLServers[0];
         surbl.Active = false;
         surbl.Save();
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      private void EnableQuarantine()
      {
         ServerIniFile.SetSetting("QuarantineEnabled", "1");
         _application.Reinitialize();
      }

      /// <summary>Sends the spam message and returns the server's reply to the final dot.</summary>
      private string SendSpam(string subject = "Quarantine test")
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
         socket.ReadUntil("220");

         socket.Send("HELO test\r\n");
         socket.ReadUntil("250");

         socket.Send("MAIL FROM:<outsider@example.com>\r\n");
         socket.ReadUntil("\r\n");

         socket.Send("RCPT TO:<" + _recipient.Address + ">\r\n");
         socket.ReadUntil("\r\n");

         socket.Send("DATA\r\n");
         socket.ReadUntil("\r\n");

         socket.Send("From: outsider@example.com\r\n" +
                     "To: " + _recipient.Address + "\r\n" +
                     "Subject: " + subject + "\r\n" +
                     "\r\n" +
                     SpamBody + "\r\n" +
                     ".\r\n");

         string reply = socket.ReadUntil("\r\n");

         socket.Send("QUIT\r\n");
         socket.Disconnect();

         return reply;
      }

      [Test]
      [Description("THE difference: a quarantined message is ACCEPTED, where a refused one is not")]
      public void TheSenderIsToldItWasAccepted()
      {
         EnableQuarantine();

         string reply = SendSpam();

         StringAssert.StartsWith("250", reply,
            "A quarantined message must be accepted. The sender believing it was delivered is the " +
            "whole point - it is what makes a false positive recoverable without bouncing to a " +
            "return path that is probably forged. Got: " + reply);

         // ...and it is nowhere near the mailbox.
         ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, "test"),
            "Accepted is not delivered. A quarantine that also delivers is just a slow mark.");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         ClassicAssert.AreEqual(1, quarantine.Count, "The message must be held.");
      }

      [Test]
      [Description("The negative control: with quarantine off, spam is refused exactly as it always was")]
      public void ANonQuarantinedServerStillRefuses()
      {
         string reply = SendSpam();

         StringAssert.StartsWith("55", reply,
            "Off by default means off. If this ever returns 250 then an upgrade has silently " +
            "started accepting and storing mail that used to be turned away at the door. Got: " + reply);

         ClassicAssert.AreEqual(0, _application.Settings.AntiSpam.Quarantine.Count,
            "...and nothing is stored.");
      }

      [Test]
      [Description("The review queue carries what a human needs to judge it: who, to whom, subject, why and how badly")]
      public void TheQueueRecordsEnoughToDecideWith()
      {
         EnableQuarantine();
         SendSpam("Invoice 4417");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         var message = quarantine[0];

         ClassicAssert.AreEqual("outsider@example.com", message.Sender);
         StringAssert.Contains(_recipient.Address, message.Recipients,
            "Who it would be released to is the first thing an administrator needs.");
         ClassicAssert.AreEqual("Invoice 4417", message.Subject,
            "The subject is read from the stored message - a queue that cannot show one is not reviewable.");
         ClassicAssert.GreaterOrEqual(message.Score, 5, "The score that made the decision.");
         ClassicAssert.Greater(message.Size, 0);
         ClassicAssert.IsNotEmpty(message.Reason, "Why it was held.");
         ClassicAssert.IsNotEmpty(message.CreatedTime);
      }

      [Test]
      [Description("Releasing delivers the message and removes it from the store - the workflow the whole feature exists for")]
      public void ReleasingDeliversItAndClearsTheEntry()
      {
         EnableQuarantine();
         SendSpam("Released invoice");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         ClassicAssert.AreEqual(1, quarantine.Count);
         quarantine[0].ReleaseMessage();

         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 1);
         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(_recipient.Address, "test");
         StringAssert.Contains("Released invoice", delivered,
            "The released message itself must arrive. Got: " + delivered);

         var after = _application.Settings.AntiSpam.Quarantine;
         ClassicAssert.AreEqual(0, after.Count,
            "A released message is no longer held - otherwise releasing twice delivers twice.");
      }

      [Test]
      [Description("Releasing does not re-run the filters that quarantined it, or it would simply be quarantined again")]
      public void AReleasedMessageIsNotQuarantinedAgain()
      {
         EnableQuarantine();
         SendSpam("Still spam by the rules");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         quarantine[0].ReleaseMessage();

         // The message is still exactly as spammy as it was; the administrator has
         // simply overruled that. Re-entering at the delivery queue rather than at
         // SMTP is what makes the override stick.
         Pop3ClientSimulator.AssertMessageCount(_recipient.Address, "test", 1);

         var after = _application.Settings.AntiSpam.Quarantine;
         ClassicAssert.AreEqual(0, after.Count,
            "If this is 1, releasing has fed the message back through the filters and the " +
            "administrator can never get it out.");
      }

      [Test]
      [Description("Deleting removes the entry and the stored file - there is no other copy, so it is a deliberate act")]
      public void DeletingRemovesTheEntryAndTheFile()
      {
         EnableQuarantine();
         SendSpam();

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         ClassicAssert.AreEqual(1, quarantine.Count);

         quarantine.DeleteByDBID(quarantine[0].ID);

         var after = _application.Settings.AntiSpam.Quarantine;
         ClassicAssert.AreEqual(0, after.Count);

         ClassicAssert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(_recipient.Address, "test"),
            "Deleting must not deliver it either.");

         // The file goes with the row. A file with no row is invisible and nothing
         // would ever sweep it.
         string quarantineDirectory = Paths.Combine(_application.Settings.Directories.DataDirectory, "Quarantine");

         if (Directory.Exists(quarantineDirectory))
         {
            ClassicAssert.AreEqual(0, Directory.GetFiles(quarantineDirectory, "*.eml", SearchOption.AllDirectories).Length,
               "The stored message file must be deleted with its row.");
         }
      }

      /// <summary>
      /// A release decided whether to sign a message as the domain its From names from the
      /// Received field on top of the stored copy - which a domain's rule may rewrite
      /// before another of its actions holds the message, here to say that this server
      /// took it from a client that signed in. The release then signed a From an outside
      /// sender wrote with the domain's key, and the recipient's forward carried the
      /// signature out: a forgery DMARC passes. The quarantine now keeps where the message
      /// came from with its stored copy, and a release reads it from there.
      ///
      /// Against the server before the fix the forward arrives with a DKIM-Signature of
      /// d=example.test.
      /// </summary>
      [Test]
      [Description("A message from outside released from the quarantine is not signed as the domain its From names, whatever a rule wrote in its Received field before the hold.")]
      public void AReleasedMessageFromOutsideIsNotSignedAsItsFromDomain()
      {
         EnableQuarantine();

         _domain.DKIMPrivateKeyFile = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples\\example.key");
         _domain.DKIMSelector = "TestSelector";
         _domain.DKIMSignEnabled = true;
         _domain.Save();

         _recipient.ForwardEnabled = true;
         _recipient.ForwardAddress = "external@example.com";
         _recipient.ForwardKeepOriginal = true;
         _recipient.Save();

         var rule = _domain.Rules.Add();
         rule.Name = "Claim a signed-in client, then hold";
         rule.Active = true;
         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();
         var received = rule.Actions.Add();
         received.Type = eRuleActionType.eRASetHeaderValue;
         received.HeaderName = "Received";
         received.Value = "from client (client [192.0.2.9]) by " + Environment.MachineName +
                          " with ESMTPSA id 1; Mon, 5 Oct 2026 09:00:00 +0000";
         received.Save();
         var hold = rule.Actions.Add();
         hold.Type = eRuleActionType.eRAQuarantine;
         hold.Save();
         rule.Save();

         var port = TestSetup.GetNextFreePort();
         using (var smtpServer = new SmtpServerSimulator(1, port))
         {
            smtpServer.SecondsToWaitBeforeTerminate = 60;
            smtpServer.AddRecipientResult(new System.Collections.Generic.Dictionary<string, int> { { "external@example.com", 250 } });
            smtpServer.StartListen();

            RegressionTests.AntiSpam.DKIM.Signing.AddRoutePointingAtLocalhost(5, port);

            new SmtpClientSimulator().SendRaw("outsider@example.com", _recipient.Address,
               "From: The CEO <ceo@example.test>\r\n" +
               "To: " + _recipient.Address + "\r\n" +
               "Subject: Held and released\r\n" +
               "\r\n" +
               "Please pay the invoice.\r\n");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            var quarantine = _application.Settings.AntiSpam.Quarantine;
            quarantine.Refresh();
            ClassicAssert.AreEqual(1, quarantine.Count, "The domain's rule did not hold the message.");
            quarantine[0].ReleaseMessage();

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            smtpServer.WaitForCompletion();

            string forwarded = smtpServer.MessageData;
            ClassicAssert.IsFalse(forwarded.ToLower().Contains("dkim-signature"),
               "A message from outside whose From names a domain hosted here left this server signed as that domain " +
               "after its release, because a rule had written in its Received field that a client signed in.\r\n" + forwarded);
         }
      }

      /// <summary>
      /// The ARC set this server seals over a message it relays attests the SPF result
      /// its own check reached when the message arrived, recorded on the queued message.
      /// The quarantine kept nothing of it, so a released message - and a forward of it -
      /// was sealed spf=none whatever the check had found. The stored copy's name now
      /// keeps the result beside where the message came from, and a release puts it back.
      ///
      /// The sender's domain publishes "v=spf1 -all" in the suite's zone, so the check
      /// fails for the loopback client; the score is 0, so that the failure marks nothing.
      ///
      /// Against the server before the fix the forward is sealed spf=none.
      /// </summary>
      [Test]
      [Description("A message released from the quarantine keeps the SPF result this server reached for it, which the ARC set sealed over its forward attests.")]
      public void AReleasedMessageKeepsItsSpfResultForTheArcSeal()
      {
         EnableQuarantine();

         try
         {
            IniFileSetting.Write("ArcSealingEnabled", "1");
            _application.Reinitialize();
            _settings.RewriteEnvelopeFromWhenForwarding = true;

            SuiteDns.Zone.WithTxt("spf-held.example", "v=spf1 -all");

            var antiSpam = _application.Settings.AntiSpam;
            antiSpam.UseSPF = true;
            antiSpam.UseSPFScore = 0;

            _domain.DKIMPrivateKeyFile = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples\\example.key");
            _domain.DKIMSelector = "TestSelector";
            _domain.DKIMSignEnabled = true;
            _domain.Save();

            _recipient.ForwardEnabled = true;
            _recipient.ForwardAddress = "external@example.com";
            _recipient.ForwardKeepOriginal = true;
            _recipient.Save();

            // What is addressed to the recipient, and not everything: the forward's envelope
            // is rewritten to the recipient's address, and a domain's rules act on the mail
            // its addresses send (RuleApplier::DomainRuleScopes), so a rule holding every
            // message would hold the forward the release makes too, and nothing would reach
            // the route.
            var rule = _domain.Rules.Add();
            rule.Name = "Hold what is addressed to the recipient";
            rule.Active = true;
            var criteria = rule.Criterias.Add();
            criteria.UsePredefined = true;
            criteria.PredefinedField = eRulePredefinedField.eFTRecipientList;
            criteria.MatchType = eRuleMatchType.eMTContains;
            criteria.MatchValue = _recipient.Address;
            criteria.Save();
            var hold = rule.Actions.Add();
            hold.Type = eRuleActionType.eRAQuarantine;
            hold.Save();
            rule.Save();

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new System.Collections.Generic.Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               RegressionTests.AntiSpam.DKIM.Signing.AddRoutePointingAtLocalhost(5, port);

               new SmtpClientSimulator().SendRaw("someone@spf-held.example", _recipient.Address,
                  "From: someone@spf-held.example\r\n" +
                  "To: " + _recipient.Address + "\r\n" +
                  "Subject: Held, released and forwarded\r\n" +
                  "\r\n" +
                  "Hello.\r\n");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);

               var quarantine = _application.Settings.AntiSpam.Quarantine;
               quarantine.Refresh();
               ClassicAssert.AreEqual(1, quarantine.Count, "The domain's rule did not hold the message.");
               quarantine[0].ReleaseMessage();

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);
               smtpServer.WaitForCompletion();

               string forwarded = smtpServer.MessageData;
               StringAssert.Contains("ARC-Authentication-Results:", forwarded, "The forward was not sealed, so this proves nothing.\r\n" + forwarded);
               StringAssert.Contains("spf=fail", forwarded,
                  "The ARC set sealed over the forward of a released message does not attest the SPF result this server reached " +
                  "when the message arrived.\r\n" + forwarded);
               StringAssert.Contains("smtp.mailfrom=spf-held.example", forwarded,
                  "The ARC set sealed over the forward of a released message does not name the domain the SPF check evaluated.\r\n" + forwarded);
            }
         }
         finally
         {
            _settings.RewriteEnvelopeFromWhenForwarding = false;
            IniFileSetting.Write("ArcSealingEnabled", "0");
            _application.Reinitialize();
            SuiteDns.Reset();
         }
      }

      /// <summary>
      /// A forward of mail from outside whose From names a hosted domain is given an
      /// envelope in that domain's organisation - SRS here - only when the message passed
      /// DMARC for it when it arrived (docs/SendAs.md, the envelope a copy is sent on
      /// with). The quarantine keeps that it passed in the stored copy's name, and a
      /// release puts it back, so that the forward of a released message is sent on as
      /// the forward of one never held is.
      ///
      /// example.test publishes a DMARC policy, and its subdomain bounces.example.test SPF
      /// for the loopback, so mail from bounce@bounces.example.test with a From at
      /// example.test passes DMARC for example.test on arrival (SuiteDns). Against the
      /// server before the stored copy's name kept the pass, the released message's
      /// forward is not sent on at all: the SRS rewrite is withheld, and the envelope the
      /// message came with is at a subdomain of the hosted domain.
      /// </summary>
      [Test]
      [Description("A message from outside whose From names a hosted domain and that passed DMARC for it when it arrived, held by a domain's rule and released, is forwarded with the SRS envelope its forward has when it is never held.")]
      public void AReleasedMessageThatPassedDmarcIsForwardedAsBefore()
      {
         EnableQuarantine();

         var antiSpam = _application.Settings.AntiSpam;
         bool dmarcBefore = antiSpam.DMARCEnabled;

         try
         {
            IniFileSetting.Write("SRSEnabled", "1");
            IniFileSetting.Write("SRSSecret", "quarantine-release-secret");
            antiSpam.SpamMarkThreshold = 100;
            antiSpam.SpamDeleteThreshold = 1000;
            antiSpam.DMARCEnabled = true;
            _application.Reinitialize();

            SuiteDns.Zone
               .WithTxt("_dmarc.example.test", "v=DMARC1; p=none")
               .WithTxt("bounces.example.test", "v=spf1 ip4:127.0.0.1 -all");

            _recipient.ForwardEnabled = true;
            _recipient.ForwardAddress = "external@example.com";
            _recipient.ForwardKeepOriginal = true;
            _recipient.Save();

            // What is addressed to the recipient, and not everything, so that the forward
            // the release makes is not held as well (as above).
            var rule = _domain.Rules.Add();
            rule.Name = "Hold what is addressed to the recipient";
            rule.Active = true;
            var criteria = rule.Criterias.Add();
            criteria.UsePredefined = true;
            criteria.PredefinedField = eRulePredefinedField.eFTRecipientList;
            criteria.MatchType = eRuleMatchType.eMTContains;
            criteria.MatchValue = _recipient.Address;
            criteria.Save();
            var hold = rule.Actions.Add();
            hold.Type = eRuleActionType.eRAQuarantine;
            hold.Save();
            rule.Save();

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new System.Collections.Generic.Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               RegressionTests.AntiSpam.DKIM.Signing.AddRoutePointingAtLocalhost(1, port);

               new SmtpClientSimulator().SendRaw("bounce@bounces.example.test", _recipient.Address,
                  "From: The CEO <ceo@example.test>\r\n" +
                  "To: " + _recipient.Address + "\r\n" +
                  "Subject: Held, released and forwarded\r\n" +
                  "\r\n" +
                  "Hello.\r\n");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);

               var quarantine = _application.Settings.AntiSpam.Quarantine;
               quarantine.Refresh();
               ClassicAssert.AreEqual(1, quarantine.Count, "The domain's rule did not hold the message.");
               quarantine[0].ReleaseMessage();

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);

               // The envelope sender in MAIL FROM, between its angle brackets.
               string mailFrom = smtpServer.NumberOfConnectedClients > 0 ? (smtpServer.MailFromCommand ?? "") : "";
               int start = mailFrom.IndexOf('<');
               int end = mailFrom.LastIndexOf('>');
               string envelope = start >= 0 && end > start ? mailFrom.Substring(start + 1, end - start - 1).ToLowerInvariant() : "";

               ClassicAssert.IsNotEmpty(envelope,
                  "The forward of a released message that passed DMARC for example.test when it arrived was not sent on.");
               ClassicAssert.IsTrue(envelope.StartsWith("srs0=") && envelope.EndsWith("@example.test"),
                  "The forward of a released message that passed DMARC for example.test should be SRS-rewritten at example.test, " +
                  "as the forward of one never held is: " + mailFrom);
            }
         }
         finally
         {
            IniFileSetting.Write("SRSEnabled", "0");
            IniFileSetting.Write("SRSSecret", "");
            antiSpam.DMARCEnabled = dmarcBefore;
            _application.Reinitialize();
            SuiteDns.Reset();
            SingletonProvider<TestSetup>.Instance.RemoveAllRoutes();
         }
      }
   }
}
