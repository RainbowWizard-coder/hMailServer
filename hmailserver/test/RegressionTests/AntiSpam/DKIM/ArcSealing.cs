// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using System.Text;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam.DKIM
{
   /// <summary>
   /// ARC sealing (RFC 8617) of relayed mail.
   ///
   /// The whole point of ARC is mail that is *not* ours: when we forward or relay
   /// somebody else's message, SPF and DKIM break at the next hop, and an ARC set
   /// carries the authentication results we observed across that hop. Sealing was
   /// only ever reached at the end of the DKIM signing path, which returns early
   /// unless the From: domain is hosted here with DKIM enabled - so exactly the
   /// mail ARC exists for was never sealed. These tests drive the three relay
   /// shapes hMailServer supports with a third-party author domain, and therefore
   /// find no ARC headers at all until the sealing decision is made independently
   /// of author-domain DKIM signing.
   ///
   /// The negative control (sealing disabled) and the author-domain test guard the
   /// two things the fix must not change: the ArcSealingEnabled gate, and DKIM
   /// signing itself.
   /// </summary>
   [TestFixture]
   public class ArcSealing : TestFixtureBase
   {

      private const string ExternalSender = "alice@external-sender.example";

      private void WriteSetting(string key, string value)
      {
         // Stored in the settings store over COM rather than written into
         // hMailServer.ini, whose [Settings] section is only the store's copy from
         // schema 6042 and would have the edit put back at the next start.
         IniFileSetting.Write(key, value);
      }

      private static string GetPrivateKeyFile()
      {
         var sslPath = Paths.Combine(TestContext.CurrentContext.TestDirectory, "..\\..\\..\\..\\SSL examples");

         var exampleKeyFile = Paths.Combine(sslPath, "example.key");
         if (!File.Exists(exampleKeyFile))
            throw new Exception("Example key file could not be found.");

         return exampleKeyFile;
      }

      private void EnableDkimOnTestDomain()
      {
         _domain.DKIMPrivateKeyFile = GetPrivateKeyFile();
         _domain.DKIMSelector = "TestSelector";
         _domain.DKIMSignEnabled = true;
         _domain.Save();
      }

      private static void AssertArcSealed(string messageData, string expectedSealingDomain)
      {
         Assert.IsTrue(Regex.IsMatch(messageData, @"^ARC-Seal:", RegexOptions.IgnoreCase | RegexOptions.Multiline),
            "Expected an ARC-Seal header on the relayed message but none was found.\r\n" + messageData);
         Assert.IsTrue(Regex.IsMatch(messageData, @"^ARC-Message-Signature:", RegexOptions.IgnoreCase | RegexOptions.Multiline),
            "Expected an ARC-Message-Signature header on the relayed message but none was found.\r\n" + messageData);
         Assert.IsTrue(Regex.IsMatch(messageData, @"^ARC-Authentication-Results:", RegexOptions.IgnoreCase | RegexOptions.Multiline),
            "Expected an ARC-Authentication-Results header on the relayed message but none was found.\r\n" + messageData);

         string seal = ExtractHeader(messageData, "ARC-Seal");
         Assert.IsNotNull(seal, "Could not read the ARC-Seal value.\r\n" + messageData);

         Assert.IsTrue(seal.Contains("i=1"), "First ARC set should be instance 1. ARC-Seal: " + seal);
         Assert.IsTrue(seal.Contains("cv=none"), "First ARC set should record cv=none. ARC-Seal: " + seal);
         Assert.IsTrue(seal.ToLower().Contains("d=" + expectedSealingDomain.ToLower()),
            "ARC-Seal should be signed by the local domain handling the relay. ARC-Seal: " + seal);
         Assert.IsTrue(seal.Contains("s=TestSelector"), "ARC-Seal should name the domain's DKIM selector. ARC-Seal: " + seal);
         Assert.IsTrue(Regex.IsMatch(seal, @"b=\S"), "ARC-Seal should carry a signature. ARC-Seal: " + seal);

         string messageSignature = ExtractHeader(messageData, "ARC-Message-Signature");
         Assert.IsNotNull(messageSignature, "Could not read the ARC-Message-Signature value.\r\n" + messageData);
         Assert.IsTrue(messageSignature.ToLower().Contains("d=" + expectedSealingDomain.ToLower()),
            "ARC-Message-Signature should be signed by the same domain. ARC-Message-Signature: " + messageSignature);
         Assert.IsTrue(Regex.IsMatch(messageSignature, @"bh=\S"),
            "ARC-Message-Signature should carry a body hash. ARC-Message-Signature: " + messageSignature);
      }

      private static void AssertNotArcSealed(string messageData)
      {
         Assert.IsFalse(messageData.ToLower().Contains("arc-seal"),
            "The message should not have been ARC-sealed.\r\n" + messageData);
         Assert.IsFalse(messageData.ToLower().Contains("arc-message-signature"),
            "The message should not have been ARC-sealed.\r\n" + messageData);
      }

      /// <summary>
      /// Returns the unfolded value of a header field, or null when it is absent.
      /// </summary>
      private static string ExtractHeader(string messageData, string fieldName)
      {
         StringBuilder value = null;

         foreach (string rawLine in messageData.Replace("\r\n", "\n").Split('\n'))
         {
            if (value != null)
            {
               if (rawLine.StartsWith(" ") || rawLine.StartsWith("\t"))
               {
                  value.Append(" ").Append(rawLine.Trim());
                  continue;
               }

               return value.ToString();
            }

            if (rawLine.Length == 0)
               break;

            if (rawLine.StartsWith(fieldName + ":", StringComparison.OrdinalIgnoreCase))
               value = new StringBuilder(rawLine.Substring(fieldName.Length + 1).Trim());
         }

         return value?.ToString();
      }

      [Test]
      [Description("An alias at a local domain that expands to an external address is a relay of " +
                   "third-party mail: the author domain is not ours, so no DKIM signature is added, " +
                   "but with ArcSealingEnabled the relayed copy must carry an ARC set sealed by the " +
                   "local domain that owns the alias. Fails before the fix because ARC sealing was " +
                   "only reachable at the end of the author-domain DKIM signing path.")]
      public void WhenRelayingThirdPartyMailViaLocalAlias_RelayedMessageShouldBeArcSealed()
      {
         try
         {
            WriteSetting("ArcSealingEnabled", "1");
            _application.Reinitialize();

            EnableDkimOnTestDomain();

            SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "arclist@example.test", "external@example.com");

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               Signing.AddRoutePointingAtLocalhost(5, port);

               SmtpClientSimulator.StaticSend(ExternalSender, "arclist@example.test", "ARC test", "Test body");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);
               smtpServer.WaitForCompletion();

               var messageData = smtpServer.MessageData;

               // We are not the author domain, so there must be no DKIM signature of
               // ours. That is precisely the case that used to go unsealed.
               Assert.IsFalse(messageData.ToLower().Contains("dkim-signature"),
                  "Third-party mail must not be DKIM-signed as if it were ours.\r\n" + messageData);

               AssertArcSealed(messageData, "example.test");
            }
         }
         finally
         {
            WriteSetting("ArcSealingEnabled", "0");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("Forwarding third-party mail from a local mailbox to an external address, with " +
                   "envelope-From rewriting enabled, must produce an ARC set sealed by the forwarding " +
                   "domain. Fails before the fix: the From: domain is external, so DKIM signing " +
                   "returns early and sealing was never reached.")]
      public void WhenForwardingThirdPartyMail_ForwardedMessageShouldBeArcSealed()
      {
         try
         {
            WriteSetting("ArcSealingEnabled", "1");
            _application.Reinitialize();

            EnableDkimOnTestDomain();

            var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "arcfwd@example.test", "test");

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               Signing.AddRoutePointingAtLocalhost(5, port);

               forwarder.ForwardEnabled = true;
               forwarder.ForwardAddress = "external@example.com";
               forwarder.ForwardKeepOriginal = false;
               forwarder.Save();

               _settings.RewriteEnvelopeFromWhenForwarding = true;
               try
               {
                  SmtpClientSimulator.StaticSend(ExternalSender, forwarder.Address, "ARC test", "Test body");

                  CustomAsserts.AssertRecipientsInDeliveryQueue(0);
                  smtpServer.WaitForCompletion();

                  var messageData = smtpServer.MessageData;

                  Assert.IsFalse(messageData.ToLower().Contains("dkim-signature"),
                     "Third-party mail must not be DKIM-signed as if it were ours.\r\n" + messageData);

                  AssertArcSealed(messageData, "example.test");
               }
               finally
               {
                  _settings.RewriteEnvelopeFromWhenForwarding = false;
               }
            }
         }
         finally
         {
            WriteSetting("ArcSealingEnabled", "0");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A plain account forward leaves nothing local in the envelope or the recipient " +
                   "list of the queued copy; the forwarding mailbox is only recoverable from the " +
                   "X-Original-Rcpt-To header written at reception. With that header enabled the " +
                   "forwarded copy must still be sealed by the forwarding domain. Fails before the " +
                   "fix for the same reason as the other relay shapes.")]
      public void WhenForwardingThirdPartyMailWithoutEnvelopeRewrite_ForwardedMessageShouldBeArcSealed()
      {
         try
         {
            WriteSetting("ArcSealingEnabled", "1");
            WriteSetting("AddXOriginalRcptTo", "1");
            _application.Reinitialize();

            EnableDkimOnTestDomain();

            var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "arcfwd2@example.test", "test");

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               Signing.AddRoutePointingAtLocalhost(5, port);

               forwarder.ForwardEnabled = true;
               forwarder.ForwardAddress = "external@example.com";
               forwarder.ForwardKeepOriginal = false;
               forwarder.Save();

               SmtpClientSimulator.StaticSend(ExternalSender, forwarder.Address, "ARC test", "Test body");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);
               smtpServer.WaitForCompletion();

               var messageData = smtpServer.MessageData;

               AssertArcSealed(messageData, "example.test");
            }
         }
         finally
         {
            WriteSetting("AddXOriginalRcptTo", "0");
            WriteSetting("ArcSealingEnabled", "0");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("Negative control: with ArcSealingEnabled off, relayed third-party mail must not " +
                   "be sealed. Passes both before and after the fix - it exists to prove that the " +
                   "new sealing path is still gated on the setting.")]
      public void WhenArcSealingDisabled_RelayedMessageShouldNotBeSealed()
      {
         WriteSetting("ArcSealingEnabled", "0");
         _application.Reinitialize();

         EnableDkimOnTestDomain();

         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "arclist@example.test", "external@example.com");

         var port = TestSetup.GetNextFreePort();
         using (var smtpServer = new SmtpServerSimulator(1, port))
         {
            smtpServer.SecondsToWaitBeforeTerminate = 60;
            smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
            smtpServer.StartListen();

            Signing.AddRoutePointingAtLocalhost(5, port);

            SmtpClientSimulator.StaticSend(ExternalSender, "arclist@example.test", "ARC test", "Test body");

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
            smtpServer.WaitForCompletion();

            AssertNotArcSealed(smtpServer.MessageData);
         }
      }

      [Test]
      [Description("Author-domain mail must keep both its DKIM signature and, with sealing enabled, " +
                   "the ARC set it already got before this change. Passes both before and after the " +
                   "fix - it exists to prove the DKIM path was not disturbed.")]
      public void WhenSendingAuthorDomainMail_MessageShouldStillBeDkimSignedAndSealed()
      {
         try
         {
            WriteSetting("ArcSealingEnabled", "1");
            _application.Reinitialize();

            EnableDkimOnTestDomain();

            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sender@example.test", "test");

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               Signing.AddRoutePointingAtLocalhost(5, port);

               SmtpClientSimulator.StaticSend("sender@example.test", "external@example.com", "ARC test", "Test body");

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);
               smtpServer.WaitForCompletion();

               var messageData = smtpServer.MessageData;

               Assert.IsTrue(messageData.ToLower().Contains("dkim-signature"),
                  "Author-domain mail must still be DKIM-signed.\r\n" + messageData);
               Assert.IsTrue(messageData.ToLower().Contains("d=example.test"),
                  "Expected the DKIM signature of the author domain.\r\n" + messageData);

               AssertArcSealed(messageData, "example.test");
            }
         }
         finally
         {
            WriteSetting("ArcSealingEnabled", "0");
            _application.Reinitialize();
         }
      }
      // ---- Which messages are signed as the domain their From names ----

      // A message from outside whose From names the test domain, which signs: the
      // forgery the tests below send through each way a copy of it can leave.
      private static string FromOutsideAsTheTestDomain(string to)
      {
         return
            "From: The CEO <ceo@example.test>\r\n" +
            "To: " + to + "\r\n" +
            "Subject: Signed as ours?\r\n" +
            "\r\n" +
            "Please pay the invoice.\r\n";
      }

      // What reaches external@example.com through the route after send, with the test
      // domain signing and ARC sealing on, and the envelope of a forward rewritten to the
      // forwarder, so that the forwarding domain seals it.
      private string DeliveredToTheRoute(Action send)
      {
         try
         {
            WriteSetting("ArcSealingEnabled", "1");
            _application.Reinitialize();
            _settings.RewriteEnvelopeFromWhenForwarding = true;

            EnableDkimOnTestDomain();

            var port = TestSetup.GetNextFreePort();
            using (var smtpServer = new SmtpServerSimulator(1, port))
            {
               smtpServer.SecondsToWaitBeforeTerminate = 60;
               smtpServer.AddRecipientResult(new Dictionary<string, int> { { "external@example.com", 250 } });
               smtpServer.StartListen();

               Signing.AddRoutePointingAtLocalhost(5, port);

               send();

               CustomAsserts.AssertRecipientsInDeliveryQueue(0);
               smtpServer.WaitForCompletion();

               return smtpServer.MessageData;
            }
         }
         finally
         {
            _settings.RewriteEnvelopeFromWhenForwarding = false;
            WriteSetting("ArcSealingEnabled", "0");
            _application.Reinitialize();
         }
      }

      private static void AssertNotSignedAsTheTestDomain(string messageData, string how)
      {
         Assert.IsFalse(messageData.ToLower().Contains("dkim-signature"),
            how + " of a message from outside whose From names a domain hosted here left this server signed as that " +
            "domain: a forgery DMARC passes.\r\n" + messageData);
      }

      [Test]
      [Description("A message an account submits after signing in is signed as its From's domain, as it always was.")]
      public void AnAuthenticatedSubmissionIsSignedAsItsFromDomain()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "signed-in-sender@example.test", "test");

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, "test", sender.Address, "external@example.com",
               "From: " + sender.Address + "\r\nTo: external@example.com\r\nSubject: Signed\r\n\r\nBody.\r\n"));

         string signature = ExtractHeader(messageData, "DKIM-Signature");
         Assert.IsNotNull(signature, "A signed-in submission was not DKIM-signed.\r\n" + messageData);
         StringAssert.Contains("d=example.test", signature, messageData);
      }

      // A second hosted domain that signs, with the test domain's key and selector.
      private hMailServer.Domain AddSigningDomain(string name)
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddDomain(name);
         domain.DKIMPrivateKeyFile = GetPrivateKeyFile();
         domain.DKIMSelector = "TestSelector";
         domain.DKIMSignEnabled = true;
         domain.Save();
         return domain;
      }

      /// <summary>
      /// Authenticated SMTP holds no From header to the account, and a signed-in user's
      /// message was signed as whatever domain hosted here its From named: an account of
      /// example.test that wrote From: ceo@ another hosted domain had its message signed
      /// with that domain's key. It is now signed as another hosted domain only when the
      /// account may send as the address, and neither the message nor a forward of it is
      /// signed otherwise.
      ///
      /// Against the server before the change both carry a DKIM-Signature of
      /// d=othersigner.test.
      /// </summary>
      [Test]
      [Description("A signed-in account's message whose From is another hosted domain's address it may not send as is not signed as that domain, nor is a forward of it.")]
      public void ASignedInSubmissionAsAnotherHostedDomainIsNotSignedAsIt()
      {
         AddSigningDomain("othersigner.test");
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "submits-as-other@example.test", "test");

         string direct = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, "test", sender.Address, "external@example.com",
               "From: The CEO <ceo@othersigner.test>\r\nTo: external@example.com\r\nSubject: Signed as theirs?\r\n\r\nBody.\r\n"));
         StringAssert.DoesNotContain("d=othersigner.test", direct,
            "A signed-in account's message was signed as another hosted domain whose address it may not send as.\r\n" + direct);

         var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "forwards-submission@example.test", "test");
         forwarder.ForwardEnabled = true;
         forwarder.ForwardAddress = "external@example.com";
         forwarder.ForwardKeepOriginal = false;
         forwarder.Save();

         string forwarded = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, "test", sender.Address, forwarder.Address,
               "From: The CEO <ceo@othersigner.test>\r\nTo: " + forwarder.Address + "\r\nSubject: Signed as theirs?\r\n\r\nBody.\r\n"));
         StringAssert.DoesNotContain("d=othersigner.test", forwarded,
            "A forward of a signed-in account's message whose From it may not send as was signed as that domain.\r\n" + forwarded);
      }

      [Test]
      [Description("A signed-in account's message is signed as its own domain whatever the address in it, and as another hosted domain whose address is an alias of its own.")]
      public void ASignedInSubmissionIsSignedAsItsOwnDomainAndAsAnAddressItMaySendAs()
      {
         var other = AddSigningDomain("aliassigner.test");
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "submits-as-own@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAlias(other, "desk@aliassigner.test", sender.Address);

         string ownDomain = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, "test", sender.Address, "external@example.com",
               "From: Information <info@example.test>\r\nTo: external@example.com\r\nSubject: Ours\r\n\r\nBody.\r\n"));
         string ownSignature = ExtractHeader(ownDomain, "DKIM-Signature");
         Assert.IsNotNull(ownSignature, "A signed-in account's message as another address of its own domain was not signed.\r\n" + ownDomain);
         StringAssert.Contains("d=example.test", ownSignature, ownDomain);

         string alias = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(sender.Address, "test", sender.Address, "external@example.com",
               "From: The desk <desk@aliassigner.test>\r\nTo: external@example.com\r\nSubject: Theirs, and mine to send as\r\n\r\nBody.\r\n"));
         string aliasSignature = ExtractHeader(alias, "DKIM-Signature");
         Assert.IsNotNull(aliasSignature, "A signed-in account's message as an alias of its own in another hosted domain was not signed.\r\n" + alias);
         StringAssert.Contains("d=aliassigner.test", aliasSignature, alias);
      }

      /// <summary>
      /// Mail from outside whose From named a domain hosted here was signed as that
      /// domain on its first delivery pass, before local delivery, and an account's
      /// forward of it then left this server with the signature - a forgery any receiver
      /// would take as the domain's own. Neither the message stored nor the forward is
      /// signed now; the forward goes with an ARC set of the forwarding domain.
      ///
      /// Against the unfixed server both carry a DKIM-Signature with d=example.test.
      /// </summary>
      [Test]
      [Description("An account's forward of mail from outside whose From names a hosted domain is not signed as that domain, and neither is the copy kept.")]
      public void AnAccountsForwardOfOutsideMailIsNotSignedAsItsFromDomain()
      {
         var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "outside-forwarder@example.test", "test");
         forwarder.ForwardEnabled = true;
         forwarder.ForwardAddress = "external@example.com";
         forwarder.ForwardKeepOriginal = true;
         forwarder.Save();

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, forwarder.Address, FromOutsideAsTheTestDomain(forwarder.Address)));

         AssertNotSignedAsTheTestDomain(messageData, "An account's forward");
         AssertArcSealed(messageData, "example.test");

         string kept = Pop3ClientSimulator.AssertGetFirstMessageText(forwarder.Address, "test");
         AssertNotSignedAsTheTestDomain(kept, "The copy kept in the mailbox");
      }

      [Test]
      [Description("An account rule's forward of mail from outside whose From names a hosted domain is not signed as that domain.")]
      public void AnAccountRulesForwardOfOutsideMailIsNotSignedAsItsFromDomain()
      {
         var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "outside-ruleforward@example.test", "test");

         var rule = forwarder.Rules.Add();
         rule.Name = "Forward everything";
         rule.Active = true;
         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = hMailServer.eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = hMailServer.eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();
         var forward = rule.Actions.Add();
         forward.Type = hMailServer.eRuleActionType.eRAForwardEmail;
         forward.To = "external@example.com";
         forward.Save();
         rule.Save();

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, forwarder.Address, FromOutsideAsTheTestDomain(forwarder.Address)));

         AssertNotSignedAsTheTestDomain(messageData, "An account rule's forward");
         AssertArcSealed(messageData, "example.test");
      }

      [Test]
      [Description("A Sieve redirect of mail from outside whose From names a hosted domain is not signed as that domain.")]
      public void ASieveRedirectOfOutsideMailIsNotSignedAsItsFromDomain()
      {
         var redirector = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "outside-redirect@example.test", "test");
         redirector.GetType().InvokeMember("SieveScript", System.Reflection.BindingFlags.SetProperty, null, redirector,
            new object[] { "redirect \"external@example.com\";\r\n" });

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, redirector.Address, FromOutsideAsTheTestDomain(redirector.Address)));

         AssertNotSignedAsTheTestDomain(messageData, "A Sieve redirect");
         AssertArcSealed(messageData, "example.test");
      }

      /// <summary>
      /// A distribution list expands to its members while the message is received, so a
      /// member outside is a recipient of the very message that arrived - and the list
      /// does not rewrite From. A posting from outside is therefore the outside sender's,
      /// and is not signed as the domain its From names; a member's posting after signing
      /// in is that member's, and is.
      ///
      /// Against the unfixed server the posting from outside reaches the member signed
      /// d=example.test.
      /// </summary>
      [Test]
      [Description("A distribution list's copy of a posting from outside whose From names a hosted domain is not signed as that domain; a signed-in member's posting is.")]
      public void ADistributionListsCopyIsSignedOnlyForASignedInPoster()
      {
         var member = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "list-poster@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "outside-list@example.test",
            new List<string> { "external@example.com" });

         string fromOutside = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, "outside-list@example.test", FromOutsideAsTheTestDomain("outside-list@example.test")));
         AssertNotSignedAsTheTestDomain(fromOutside, "A distribution list's copy");

         string fromMember = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(member.Address, "test", member.Address, "outside-list@example.test",
               "From: " + member.Address + "\r\nTo: outside-list@example.test\r\nSubject: Posted\r\n\r\nBody.\r\n"));
         string signature = ExtractHeader(fromMember, "DKIM-Signature");
         Assert.IsNotNull(signature, "A signed-in member's posting was not DKIM-signed.\r\n" + fromMember);
         StringAssert.Contains("d=example.test", signature, fromMember);
      }

      /// <summary>
      /// A rule's reply may go from no address, the null sender, and its From was then the
      /// rule's name before an empty &lt;&gt;. The address-list reader this server picks a
      /// From's address with takes a name that is one email address for the address
      /// itself, and the signer signs as that address's domain - so an account of another
      /// domain, replying from no address under the name ceo@example.test, had its reply
      /// signed as example.test. Such a name is now left out, and the From is &lt;&gt; alone.
      ///
      /// Against the server before the fix the reply's From is "ceo@example.test" &lt;&gt; and
      /// it carries a DKIM-Signature of d=example.test.
      /// </summary>
      [Test]
      [Description("A rule's reply from no address, under a name that is an address of a domain hosted here, is not signed as that domain and names no address in its From.")]
      public void ARuleReplyFromNoAddressIsNotSignedAsTheDomainItsNameNames()
      {
         var otherDomain = SingletonProvider<TestSetup>.Instance.AddDomain("replynull.test");
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(otherDomain, "owner@replynull.test", "test");

         var rule = owner.Rules.Add();
         rule.Name = "Reply from no address";
         rule.Active = true;
         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = hMailServer.eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = hMailServer.eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();
         var reply = rule.Actions.Add();
         reply.Type = hMailServer.eRuleActionType.eRAReply;
         reply.FromAddress = "";
         reply.FromName = "ceo@example.test";
         reply.Subject = "Autoreply";
         reply.Body = "Away.";
         reply.Save();
         rule.Save();

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw("external@example.com", owner.Address,
               "From: external@example.com\r\nTo: " + owner.Address + "\r\nSubject: Anyone there?\r\n\r\nBody.\r\n"));

         AssertNotSignedAsTheTestDomain(messageData, "A rule's reply from no address under the name ceo@example.test");
         string from = ExtractHeader(messageData, "From");
         Assert.IsNotNull(from, messageData);
         StringAssert.DoesNotContain("@", from, messageData);
      }
      // ---- What the seal attests of SPF ----

      private static void AssertSealDoesNotAttestAPass(string messageData, string how)
      {
         AssertArcSealed(messageData, "example.test");

         string results = ExtractHeader(messageData, "ARC-Authentication-Results");
         Assert.IsNotNull(results, "No ARC-Authentication-Results.\r\n" + messageData);
         StringAssert.Contains("spf=", results, results);
         StringAssert.DoesNotContain("spf=pass", results,
            "The ARC seal attests an SPF pass this server never reached: it read " + how + ".\r\n" + messageData);
      }

      /// <summary>
      /// The ARC-Authentication-Results this server seals read spf= from the first
      /// Received-SPF field of the message - the sender's own, when this server wrote
      /// none, as it does not by default. It is now the result this server's own check
      /// reached when the message arrived, recorded on the message.
      ///
      /// Against the unfixed server the seal attests spf=pass.
      /// </summary>
      [Test]
      [Description("An ARC seal over relayed mail attests this server's own SPF result, not a Received-SPF field the sender wrote.")]
      public void TheSealAttestsTheServersSpfResultNotTheSenders()
      {
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "spf-alias@example.test", "external@example.com");

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, "spf-alias@example.test",
               "Received-SPF: pass (written by the sender) client-ip=192.0.2.1;\r\n" +
               "From: " + ExternalSender + "\r\n" +
               "To: spf-alias@example.test\r\n" +
               "Subject: A verdict of its own\r\n" +
               "\r\n" +
               "Body.\r\n"));

         AssertSealDoesNotAttestAPass(messageData, "the Received-SPF field the sender wrote");
      }

      /// <summary>
      /// The same, with the field added on this server: a Sieve addheader - which
      /// prepends - ahead of a redirect put a Received-SPF field of the script's choosing
      /// above any this server wrote, and the seal over the redirected copy attested it.
      ///
      /// Against the unfixed server the seal attests spf=pass.
      /// </summary>
      [Test]
      [Description("An ARC seal over a Sieve redirect attests this server's own SPF result, not a Received-SPF field the script added.")]
      public void TheSealAttestsTheServersSpfResultNotAScripts()
      {
         var redirector = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spf-redirect@example.test", "test");
         redirector.GetType().InvokeMember("SieveScript", System.Reflection.BindingFlags.SetProperty, null, redirector,
            new object[]
            {
               "require \"editheader\";\r\n" +
               "addheader \"Received-SPF\" \"pass (added by a script) client-ip=192.0.2.1;\";\r\n" +
               "redirect \"external@example.com\";\r\n"
            });

         string messageData = DeliveredToTheRoute(() =>
            new SmtpClientSimulator().SendRaw(ExternalSender, redirector.Address,
               "From: " + ExternalSender + "\r\n" +
               "To: " + redirector.Address + "\r\n" +
               "Subject: Redirected\r\n" +
               "\r\n" +
               "Body.\r\n"));

         StringAssert.Contains("added by a script", messageData, "The script's field is not in the redirected copy, so this proves nothing.");
         AssertSealDoesNotAttestAPass(messageData, "the Received-SPF field a Sieve script added");
      }
   }
}
