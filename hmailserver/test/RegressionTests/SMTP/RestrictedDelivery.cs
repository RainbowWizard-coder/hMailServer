// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    Restricted delivery (Common/Util/RestrictedDelivery.h,
   ///    docs/RestrictedDelivery.md): accounts, or a whole domain, that may
   ///    only exchange mail with named domains.
   ///
   ///    What is proved here, each with its negative control: the 550 at RCPT
   ///    TO in both directions with the exact sentence, and the 250 for the
   ///    named domain, the suffix, the own domain and the null sender; the
   ///    account's three values; a distribution list reaching the exempt
   ///    member and not the restricted one, and both once the sender is
   ///    named; a forward withheld while the mailbox still gets the message;
   ///    the REST refusals and round-trips; and the two COM properties.
   ///
   ///    The two new COM properties are reached through dynamic, as the
   ///    zero-access fixture reaches Account.ZeroAccess: the suite's interop
   ///    assembly is built from the type library registered on the machine,
   ///    which carries them only once the built server has been registered.
   ///    The suite's own domain (test.com, remade for every test) is the
   ///    restricted one; a second hosted domain is made in SetUp for the
   ///    "another hosted domain, not named" cases and removed in TearDown.
   /// </summary>
   [TestFixture]
   public class RestrictedDelivery : TestFixtureBase
   {
      // The fixture's own port, from the range this row was allocated
      // (9796-9805), so that it never contends with another fixture's listener.
      private static int RestPort = 9796;
      private const string AdminPassword = "testar";
      private const string Password = "Restricted-Passw0rd!";

      private const string Stranger = "stranger@elsewhere.test";
      private const string Partner = "friend@partner.example";

      private Domain _other;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");

         _other = SingletonProvider<TestSetup>.Instance.AddDomain("other-" + TestSetup.UniqueString() + ".test");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // The second domain this test made. The suite's baseline removes it
            // before the next test, but nothing removed the LAST test's, and a
            // bench with two domains fails the pre-flight of the gate after it.
            if (_other != null)
            {
               _other.Delete();
               _other = null;
            }
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An outside sender is answered 550 5.7.1 with the exact sentence at RCPT TO for a restricted account, and 250 for an exempt account in the same domain, for a sender in a named domain, for a sender under a .suffix, and as the null sender, whose bounce is delivered.")]
      public void AnOutsideSenderIsRefusedForARestrictedAccountAndAcceptedForTheNamedAndTheExempt()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account head = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "head@" + _domain.Name, Password);

         SetList(_domain.Name, "partner.example, .allowed.example");
         SetMode(head.Address, "exempt");

         string refused = Rcpt(Stranger, pupil.Address);
         StringAssert.StartsWith("550 5.7.1 " + pupil.Address + ": this mailbox accepts mail from named domains only.", refused,
            "A stranger to a restricted account is refused at RCPT with the sentence.");

         StringAssert.StartsWith("250", Rcpt(Stranger, head.Address), "The same stranger to the exempt account in the same domain is accepted.");
         StringAssert.StartsWith("250", Rcpt(Partner, pupil.Address), "A sender in a named domain is accepted.");
         StringAssert.StartsWith("250", Rcpt("friend@mail.allowed.example", pupil.Address), "A sender under the .suffix is accepted.");
         StringAssert.StartsWith("250", Rcpt("friend@allowed.example", pupil.Address), "The suffix covers the apex too, as the domain policy list's matcher does.");
         StringAssert.StartsWith("550 5.7.1", Rcpt("friend@notallowed.example", pupil.Address), "A name that merely ends in the suffix's letters is not under it.");
         StringAssert.StartsWith("550 5.7.1", Rcpt("friend@mail.partner.example", pupil.Address), "A whole-domain name does not cover what is beneath it.");

         // The null sender: a bounce must reach a restricted account.
         StringAssert.StartsWith("250", Rcpt("", pupil.Address), "MAIL FROM:<> is always allowed.");
         SmtpClientSimulator.StaticSend("", pupil.Address, "Undelivered Mail Returned to Sender", "The bounce.");
         StringAssert.Contains("Subject: Undelivered Mail Returned to Sender", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password),
            "The bounce is delivered.");

         // And the ordinary message from a named domain is delivered, while the
         // stranger's is refused before DATA.
         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "From a partner", "The body.");
         StringAssert.Contains("Subject: From a partner", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password));
         Assert.Throws<DeliveryFailedException>(() => SmtpClientSimulator.StaticSend(Stranger, pupil.Address, "From a stranger", "The body."));
      }

      [Test]
      [Description("A restricted account sending over an authenticated session is refused 550 5.7.1 for a domain off the list and for another hosted domain that is not named, and accepted for a named domain, for its own domain, and for the other hosted domain once it is named.")]
      public void ARestrictedAccountMaySendToNamedDomainsOnly()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague@" + _domain.Name, Password);
         Account outsider = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outsider@" + _other.Name, Password);

         SetList(_domain.Name, "partner.example");

         string refused = Rcpt(pupil.Address, "someone@elsewhere.test", pupil.Address, Password);
         StringAssert.StartsWith("550 5.7.1 someone@elsewhere.test: this account may send to named domains only.", refused,
            "A domain off the list is refused at RCPT with the sentence.");

         StringAssert.StartsWith("250", Rcpt(pupil.Address, "someone@partner.example", pupil.Address, Password), "A named domain is accepted.");
         StringAssert.StartsWith("250", Rcpt(pupil.Address, colleague.Address, pupil.Address, Password), "The own domain is accepted.");
         StringAssert.StartsWith("550 5.7.1 " + outsider.Address + ": this account may send to named domains only.",
            Rcpt(pupil.Address, outsider.Address, pupil.Address, Password),
            "Another domain this server hosts is outside until it is named.");

         // Named, it is inside.
         SetList(_domain.Name, "partner.example, " + _other.Name);
         StringAssert.StartsWith("250", Rcpt(pupil.Address, outsider.Address, pupil.Address, Password), "The other hosted domain, once named, is accepted.");

         // And the message itself: refused before DATA to the stranger, delivered
         // to the colleague.
         Assert.Throws<DeliveryFailedException>(() => new SmtpClientSimulator().SendRaw(pupil.Address, Password, pupil.Address, "someone@elsewhere.test",
            "From: " + pupil.Address + "\r\nTo: someone@elsewhere.test\r\nSubject: Out\r\n\r\nThe body.\r\n"));
         new SmtpClientSimulator().SendRaw(pupil.Address, Password, pupil.Address, colleague.Address,
            "From: " + pupil.Address + "\r\nTo: " + colleague.Address + "\r\nSubject: Inside\r\n\r\nThe body.\r\n");
         StringAssert.Contains("Subject: Inside", Pop3ClientSimulator.AssertGetFirstMessageText(colleague.Address, Password));
      }

      [Test]
      [Description("An account set to restricted in a domain with an EMPTY list is confined to its own domain in both directions; an account set to exempt in a restricted domain is refused nothing; an account that follows a restricted domain is the control.")]
      public void TheAccountsThreeValues()
      {
         Account desk = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "desk@" + _domain.Name, Password);
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague@" + _domain.Name, Password);

         // The domain names nothing; the desk alone is restricted.
         SetList(_domain.Name, "");
         SetMode(desk.Address, "restricted");

         StringAssert.StartsWith("550 5.7.1", Rcpt(desk.Address, "someone@elsewhere.test", desk.Address, Password), "Restricted with an empty list: refused everything outside.");
         StringAssert.StartsWith("250", Rcpt(desk.Address, colleague.Address, desk.Address, Password), "Restricted with an empty list: its own domain.");
         StringAssert.StartsWith("550 5.7.1", Rcpt(Stranger, desk.Address), "Restricted with an empty list: a stranger is refused.");
         StringAssert.StartsWith("250", Rcpt(colleague.Address, desk.Address), "Restricted with an empty list: a colleague is accepted.");

         // The colleague, following an unrestricted domain, is the control.
         StringAssert.StartsWith("250", Rcpt(Stranger, colleague.Address), "Following a domain with no list restricts nothing.");
         StringAssert.StartsWith("250", Rcpt(colleague.Address, "someone@elsewhere.test", colleague.Address, Password));

         // The domain restricted; the head exempt; the pupil following it.
         Account head = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "head@" + _domain.Name, Password);
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         SetList(_domain.Name, "partner.example");
         SetMode(head.Address, "exempt");

         StringAssert.StartsWith("250", Rcpt(Stranger, head.Address), "Exempt: refused nothing inbound.");
         StringAssert.StartsWith("250", Rcpt(head.Address, "someone@elsewhere.test", head.Address, Password), "Exempt: refused nothing outbound.");
         StringAssert.StartsWith("550 5.7.1", Rcpt(Stranger, pupil.Address), "Following the restricted domain: refused inbound.");
         StringAssert.StartsWith("550 5.7.1", Rcpt(pupil.Address, "someone@elsewhere.test", pupil.Address, Password), "Following the restricted domain: refused outbound.");
      }

      [Test]
      [Description("A distribution list in a restricted domain with a restricted member and an exempt member, mailed from outside: the sender gets 250 at RCPT (the list's address is not restricted), the exempt member gets the message, the restricted one does not; with the sender in a named domain both get it.")]
      public void AListReachesTheExemptMemberAndNotTheRestrictedOne()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account head = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "head@" + _domain.Name, Password);
         SetList(_domain.Name, "partner.example");
         SetMode(head.Address, "exempt");

         string listAddress = "everyone@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, listAddress, new List<string> { pupil.Address, head.Address });

         StringAssert.StartsWith("250", Rcpt(Stranger, listAddress), "The list's address is not an account and is not restricted.");
         SmtpClientSimulator.StaticSend(Stranger, listAddress, "To the list", "The body.");

         StringAssert.Contains("Subject: To the list", Pop3ClientSimulator.AssertGetFirstMessageText(head.Address, Password), "The exempt member gets it.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 0);

         SmtpClientSimulator.StaticSend(Partner, listAddress, "From a partner", "The body.");
         Pop3ClientSimulator.AssertMessageCount(head.Address, Password, 1);
         StringAssert.Contains("Subject: From a partner", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password), "With the sender named, the restricted member gets it too.");
      }

      [Test]
      [Description("A forward from a restricted account to a domain off the list is not made - the mailbox gets the message and the other side receives nothing - and is made once the target's domain is named.")]
      public void AForwardToAnUnnamedDomainIsNotMadeAndTheMailboxStillGetsTheMessage()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account outside = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outside@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");

         pupil.ForwardEnabled = true;
         pupil.ForwardAddress = outside.Address;
         pupil.ForwardKeepOriginal = true;
         pupil.Save();

         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Forwarded?", "The body.");
         StringAssert.Contains("Subject: Forwarded?", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password), "The mailbox gets the message.");
         // Zero waits for the delivery queue to drain first, which is the bound.
         Pop3ClientSimulator.AssertMessageCount(outside.Address, Password, 0);

         SetList(_domain.Name, "partner.example, " + _other.Name);
         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Forwarded now", "The body.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 1);
         StringAssert.Contains("Subject: Forwarded now", Pop3ClientSimulator.AssertGetFirstMessageText(outside.Address, Password), "Named, the forward is made.");
      }

      [Test]
      [Description("Over REST: a list of 2,001 characters is 400, a list with an address instead of a domain is 400 with a sentence, GET answers what PUT wrote lower-cased, and the account's three values round-trip as digits and as words while a fourth is 400.")]
      public void TheRestRefusalsAndRoundTrips()
      {
         string domainPath = "/api/v1/domains/" + _domain.Name;

         var tooLong = Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"" + new string('a', 2001) + "\"}");
         Assert.AreEqual(400, tooLong.status, tooLong.body);
         StringAssert.Contains("2000", tooLong.body);

         var address = Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"partner.example, someone@partner.example\"}");
         Assert.AreEqual(400, address.status, address.body);
         StringAssert.Contains("names domains, not addresses", address.body);

         var literal = Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"[192.0.2.1]\"}");
         Assert.AreEqual(400, literal.status, literal.body);

         var nonAscii = Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"b\\u00fccher.example\"}");
         Assert.AreEqual(400, nonAscii.status, nonAscii.body);
         StringAssert.Contains("ASCII", nonAscii.body);

         var written = Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"Partner.Example  .Allowed.Example,partner.example\"}");
         Assert.AreEqual(200, written.status, written.body);
         StringAssert.Contains("\"restricted_domains\":\"partner.example, .allowed.example\"", written.body, "Lower-cased, canonical separators, duplicates dropped.");
         StringAssert.Contains("\"restricted_domains\":\"partner.example, .allowed.example\"", Admin("GET", domainPath, null).body, "GET answers what PUT wrote.");

         // A PUT of another field leaves the list alone.
         Assert.AreEqual(200, Admin("PUT", domainPath, "{\"active\":true,\"postmaster\":\"postmaster@" + _domain.Name + "\"}").status);
         StringAssert.Contains("\"restricted_domains\":\"partner.example, .allowed.example\"", Admin("GET", domainPath, null).body);

         Assert.AreEqual(200, Admin("PUT", domainPath, "{\"active\":true,\"restricted_domains\":\"\"}").status);
         StringAssert.Contains("\"restricted_domains\":\"\"", Admin("GET", domainPath, null).body, "Empty clears it.");

         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         string accountPath = "/api/v1/accounts/" + Uri.EscapeDataString(pupil.Address);

         StringAssert.Contains("\"restricted_delivery\":0", Admin("GET", accountPath, null).body, "Follows the domain by default.");

         Assert.AreEqual(200, Admin("PUT", accountPath, "{\"restricted_delivery\":1}").status);
         StringAssert.Contains("\"restricted_delivery\":1", Admin("GET", accountPath, null).body);
         Assert.AreEqual(200, Admin("PUT", accountPath, "{\"restricted_delivery\":\"exempt\"}").status);
         StringAssert.Contains("\"restricted_delivery\":2", Admin("GET", accountPath, null).body);
         Assert.AreEqual(200, Admin("PUT", accountPath, "{\"restricted_delivery\":\"domain\"}").status);
         StringAssert.Contains("\"restricted_delivery\":0", Admin("GET", accountPath, null).body);
         Assert.AreEqual(200, Admin("PUT", accountPath, "{\"restricted_delivery\":\"2\"}").status);
         StringAssert.Contains("\"restricted_delivery\":2", Admin("GET", accountPath, null).body);

         var fourth = Admin("PUT", accountPath, "{\"restricted_delivery\":3}");
         Assert.AreEqual(400, fourth.status, fourth.body);
         StringAssert.Contains("restricted_delivery must be 0", fourth.body);
         Assert.AreEqual(400, Admin("PUT", accountPath, "{\"restricted_delivery\":\"sometimes\"}").status);
         Assert.AreEqual(400, Admin("PUT", accountPath, "{\"restricted_delivery\":1.5}").status);
         Assert.AreEqual(400, Admin("PUT", accountPath, "{\"restricted_delivery\":true}").status);
         StringAssert.Contains("\"restricted_delivery\":2", Admin("GET", accountPath, null).body, "A refused value changes nothing.");
      }

      [Test]
      [Description("Over COM: Domain.RestrictedDomains and Account.RestrictedDelivery are read, written and kept by Save(), lower-cased and canonical; a name that is not a domain and a fourth value are refused by the property.")]
      public void TheTwoComPropertiesRoundTrip()
      {
         dynamic domain = _domain;
         ClassicAssert.AreEqual("", (string) domain.RestrictedDomains, "Empty by default.");

         domain.RestrictedDomains = "Partner.Example, .Allowed.Example";
         _domain.Save();

         dynamic reread = _application.Domains.get_ItemByName(_domain.Name);
         ClassicAssert.AreEqual("partner.example, .allowed.example", (string) reread.RestrictedDomains, "Lower-cased and canonical after Save.");

         var refusedName = Assert.Throws<COMException>(() => { domain.RestrictedDomains = "someone@partner.example"; });
         StringAssert.Contains("names domains, not addresses", refusedName.Message);

         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         dynamic account = pupil;
         ClassicAssert.AreEqual(0, (int) account.RestrictedDelivery, "Follows the domain by default.");

         account.RestrictedDelivery = 2;
         pupil.Save();

         dynamic after = _domain.Accounts.ItemByAddress[pupil.Address];
         ClassicAssert.AreEqual(2, (int) after.RestrictedDelivery);

         var refusedValue = Assert.Throws<COMException>(() => { account.RestrictedDelivery = 3; });
         StringAssert.Contains("RestrictedDelivery must be 0", refusedValue.Message);

         // What COM wrote is what the decision reads: the exempt pupil hears
         // from a stranger while the domain is restricted.
         StringAssert.StartsWith("250", Rcpt(Stranger, pupil.Address));
      }

      [Test]
      [Description("The webmail's send is the account sending: a restricted account's POST /api/v1/me/messages to a stranger is 400 with the sentence and queues nothing, to a named domain 201 and delivered; an exempt account in the same domain sends to the stranger.")]
      public void TheWebmailSendIsJudgedOnTheCallersAccount()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account head = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "head@" + _domain.Name, Password);
         Account outsider = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outsider@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");
         SetMode(head.Address, "exempt");

         string asPupil = Basic(pupil.Address, Password);
         string asHead = Basic(head.Address, Password);

         var refused = Send("POST", "/api/v1/me/messages", asPupil, "{\"to\":\"" + outsider.Address + "\",\"subject\":\"Out\",\"text\":\"Body.\"}");
         Assert.AreEqual(400, refused.status, refused.body);
         StringAssert.Contains(outsider.Address + ": this account may send to named domains only.", refused.body);
         Pop3ClientSimulator.AssertMessageCount(outsider.Address, Password, 0);

         var accepted = Send("POST", "/api/v1/me/messages", asHead, "{\"to\":\"" + outsider.Address + "\",\"subject\":\"From the head\",\"text\":\"Body.\"}");
         Assert.AreEqual(201, accepted.status, accepted.body);
         StringAssert.Contains("Subject: From the head", Pop3ClientSimulator.AssertGetFirstMessageText(outsider.Address, Password), "The exempt account sends to anyone.");

         SetList(_domain.Name, "partner.example, " + _other.Name);
         var named = Send("POST", "/api/v1/me/messages", asPupil, "{\"to\":\"" + outsider.Address + "\",\"subject\":\"Named now\",\"text\":\"Body.\"}");
         Assert.AreEqual(201, named.status, named.body);
         StringAssert.Contains("Subject: Named now", Pop3ClientSimulator.AssertGetFirstMessageText(outsider.Address, Password), "Named, the pupil's send goes.");
      }

      [Test]
      [Description("A restricted account's out-of-office is not sent to a stranger who reached it through a distribution list - the one message the stranger, a local account here, gets back is the non-delivery report, and once the queue has drained nothing else - and is sent to a sender in a named domain.")]
      public void TheOutOfOfficeIsWithheldFromAStranger()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account outsider = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outsider@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");

         pupil.VacationMessageIsOn = true;
         pupil.VacationSubject = "Away";
         pupil.VacationMessage = "Back next week.";
         pupil.Save();

         string listAddress = "everyone@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, listAddress, new List<string> { pupil.Address });

         // The stranger reaches the pupil only through the list: 250 at RCPT
         // and no delivery. The stranger is a local account, so it is told by
         // a non-delivery report - the one message it gets - and no
         // out-of-office follows it; the zero waits for the queue to drain.
         StringAssert.StartsWith("250", Rcpt(outsider.Address, listAddress));
         SmtpClientSimulator.StaticSend(outsider.Address, listAddress, "To the list", "The body.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(outsider.Address, Password, 1);
         string told = Pop3ClientSimulator.AssertGetFirstMessageText(outsider.Address, Password);
         StringAssert.Contains("5.7.1", told, "The local sender is told by a non-delivery report.");
         StringAssert.DoesNotContain("Subject: Away", told, "The out-of-office did not reach the stranger.");
         Pop3ClientSimulator.AssertMessageCount(outsider.Address, Password, 0);

         // Named, and written to directly (a list posting carries List-* headers,
         // which suppress auto-replies on their own): the reply arrives.
         SetList(_domain.Name, "partner.example, " + _other.Name);
         SmtpClientSimulator.StaticSend(outsider.Address, pupil.Address, "Direct", "The body.");
         StringAssert.Contains("Subject: Direct", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password));
         StringAssert.Contains("Away", Pop3ClientSimulator.AssertGetFirstMessageText(outsider.Address, Password), "The out-of-office reaches a named sender.");
      }

      [Test]
      [Description("An alias to a restricted account, mailed from a local colleague in another hosted domain that is not named: 250 at RCPT (the alias is not an account), the mailbox stays empty, and the colleague gets the 5.7.1 report; from an outside sender the mailbox stays empty and nothing is reported anywhere; with the colleague's domain named the alias delivers.")]
      public void AnAliasTargetIsDroppedAtDeliveryAndOnlyALocalSenderIsTold()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "colleague@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");

         string aliasAddress = "tutor@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, aliasAddress, pupil.Address);

         StringAssert.StartsWith("250", Rcpt(colleague.Address, aliasAddress), "The alias is not an account and is not judged at RCPT.");
         SmtpClientSimulator.StaticSend(colleague.Address, aliasAddress, "Through the alias", "The body.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 0);

         string report = Pop3ClientSimulator.AssertGetFirstMessageText(colleague.Address, Password);
         StringAssert.Contains("5.7.1", report, "The local sender gets the non-delivery report.");
         StringAssert.Contains("this mailbox accepts mail from named domains only", Unfolded(report), "The report carries the sentence.");

         // An outside sender: dropped in silence, and no report to anyone here.
         SmtpClientSimulator.StaticSend(Stranger, aliasAddress, "Stranger through the alias", "The body.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 0);
         Pop3ClientSimulator.AssertMessageCount(colleague.Address, Password, 0);

         SetList(_domain.Name, "partner.example, " + _other.Name);
         SmtpClientSimulator.StaticSend(colleague.Address, aliasAddress, "Named through the alias", "The body.");
         StringAssert.Contains("Subject: Named through the alias", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password), "Named, the alias delivers.");
         Pop3ClientSimulator.AssertMessageCount(colleague.Address, Password, 0);
      }

      [Test]
      [Description("An account rule's forward from a restricted account to a domain off the list is not made while the mailbox gets the message, and is made once the domain is named.")]
      public void ARuleForwardToAnUnnamedDomainIsNotMade()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account outside = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outside@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");

         Rule rule = pupil.Rules.Add();
         rule.Name = "Forward everything";
         rule.Active = true;
         RuleCriteria criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();
         RuleAction action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAForwardEmail;
         action.To = outside.Address;
         action.Save();
         rule.Save();

         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Rule forward?", "The body.");
         StringAssert.Contains("Subject: Rule forward?", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password), "The mailbox gets the message.");
         Pop3ClientSimulator.AssertMessageCount(outside.Address, Password, 0);

         SetList(_domain.Name, "partner.example, " + _other.Name);
         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Rule forward now", "The body.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 1);
         StringAssert.Contains("Subject: Rule forward now", Pop3ClientSimulator.AssertGetFirstMessageText(outside.Address, Password), "Named, the rule's forward is made.");
      }

      [Test]
      [Description("A Sieve script whose only action is a redirect to a domain off the list keeps the message in the mailbox while the redirect is refused - the message is not lost - and, once the domain is named, redirects it and drops the local copy as the script asks.")]
      public void ARedirectOnlyScriptKeepsTheMessageWhileTheRedirectIsRefused()
      {
         Account pupil = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "pupil@" + _domain.Name, Password);
         Account outside = SingletonProvider<TestSetup>.Instance.AddAccount(_other, "outside@" + _other.Name, Password);
         SetList(_domain.Name, "partner.example");

         var script = Admin("PUT", "/api/v1/accounts/" + Uri.EscapeDataString(pupil.Address), "{\"sieve_script\":\"redirect \\\"" + outside.Address + "\\\";\"}");
         Assert.AreEqual(200, script.status, script.body);

         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Redirect refused", "The body.");
         StringAssert.Contains("Subject: Redirect refused", Pop3ClientSimulator.AssertGetFirstMessageText(pupil.Address, Password), "Every redirect refused: the message is kept, not lost.");
         Pop3ClientSimulator.AssertMessageCount(outside.Address, Password, 0);

         SetList(_domain.Name, "partner.example, " + _other.Name);
         SmtpClientSimulator.StaticSend(Partner, pupil.Address, "Redirect made", "The body.");
         StringAssert.Contains("Subject: Redirect made", Pop3ClientSimulator.AssertGetFirstMessageText(outside.Address, Password), "Named, the redirect is made.");
         Pop3ClientSimulator.AssertMessageCount(pupil.Address, Password, 0);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }

      /// <summary>
      ///    One MAIL FROM and one RCPT TO on a fresh session, authenticated when
      ///    a username is given, and the RCPT reply line back; nothing is sent
      ///    after it, so an accepted recipient queues nothing.
      /// </summary>
      /// <summary>A quoted-printable text with its soft line breaks removed and =20 read as the space it is, so a sentence can be looked for whole.</summary>
      private static string Unfolded(string quotedPrintable)
      {
         return quotedPrintable.Replace("=\r\n", "").Replace("=20", " ");
      }

      private static string Rcpt(string from, string to, string username = null, string password = null)
      {
         var client = new SmtpClientSimulator();

         if (username != null)
         {
            client.ConnectAndLogon(Convert.ToBase64String(Encoding.UTF8.GetBytes(username)),
                                   Convert.ToBase64String(Encoding.UTF8.GetBytes(password)), out string logon);
         }
         else
         {
            client.Connect();
            string welcome = client.Receive();
            StringAssert.StartsWith("220", welcome);
            StringAssert.StartsWith("250", client.SendAndReceive("EHLO test\r\n"));
         }

         StringAssert.StartsWith("250", client.SendAndReceive("MAIL FROM:<" + from + ">\r\n"), "MAIL FROM was refused.");
         string reply = client.SendAndReceive("RCPT TO:<" + to + ">\r\n");

         client.SendAndReceive("QUIT\r\n");
         client.Disconnect();

         return reply;
      }

      private static void SetList(string domainName, string list)
      {
         var answer = Admin("PUT", "/api/v1/domains/" + domainName, "{\"active\":true,\"restricted_domains\":\"" + list + "\"}");
         Assert.AreEqual(200, answer.status, "The domain's list was refused: " + answer.body);
      }

      private static void SetMode(string address, string mode)
      {
         var answer = Admin("PUT", "/api/v1/accounts/" + Uri.EscapeDataString(address), "{\"restricted_delivery\":\"" + mode + "\"}");
         Assert.AreEqual(200, answer.status, "The account's setting was refused: " + answer.body);
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         string authorization = "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
