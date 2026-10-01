// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The sender list: a reader's own safe and blocked senders, a domain's allow and
// block list, and the server's - one table with a scope on it, read at local
// delivery rather than during the SMTP conversation, because a conversation
// shared by several recipients cannot be refused for one of them.
//
// What these tests pin, and why each is the one worth having:
//
//   * a safe sender skips the verdict for the account that listed it and for no
//     other recipient of the same message - the per-recipient promise is the
//     whole reason the list is read where it is;
//   * a blocked sender is marked for its own account even when no test objected,
//     and held for review where the server holds spam - never destroyed - and a
//     release of what the block held is delivered rather than held again;
//   * the precedence: the account's list, then its domain's, then the server's,
//     the nearest one with an opinion the only one consulted, the more specific
//     entry winning inside one list, and a second write of the same value
//     replacing the first rather than sitting beside it;
//   * the editors' refusals, and that one account cannot touch another's entry.
//
// New COM members are reached through dynamic: the suite's interop is generated
// from the registered type library, and a fixture that named them early-bound
// would not compile against a bench whose server predates them.
//
// The controlled score source is the one PerAccountSpamSettings uses: the SURBL
// project's permanent test point, served by the suite's own resolver.

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   [TestFixture]
   public class SenderLists : TestFixtureBase
   {
      // 9580 is this fixture's port; QuarantineDigests has 9581.
      private static int RestPort = 9580;
      private const string AdminPassword = "testar";
      private const string Password = "Sender-Lists-1!";

      private const string SurblTestPoint = "surbl-org-permanent-test-point.com.multi.surbl.org";
      private const string SpamBody = "This is a test message with a SURBL url: -> http://surbl-org-permanent-test-point.com/ <-";
      private const string CleanBody = "This is a clean test message.";

      // Whether this test switched the quarantine on, so that the tear-down
      // switches it off again without asking the settings store what it holds.
      private bool quarantineEnabled_;

      [OneTimeSetUp]
      public void ServeTheNamesThisFixtureNeeds()
      {
         SuiteDns.Zone.WithA(SurblTestPoint, "127.0.0.2");
      }

      [OneTimeTearDown]
      public void ForgetThem()
      {
         SuiteDns.Reset();
      }

      [SetUp]
      public void StartRestApiAndScoring()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         // The server's list outlives an account and a domain, so whatever an
         // earlier failure left behind is cleared before anything is sent.
         ClearServerList();

         var antiSpam = _settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 5;
         antiSpam.SpamDeleteThreshold = 100;
         antiSpam.AddHeaderReason = true;
         antiSpam.AddHeaderSpam = true;
         antiSpam.PrependSubject = true;
         antiSpam.PrependSubjectText = "[SPAM]";
         antiSpam.MaximumMessageSize = 0;

         var surbl = antiSpam.SURBLServers[0];
         surbl.Active = true;
         surbl.Score = 5;
         surbl.Save();

         (int status, string body) probe = Http("GET", "/api/v1/status", Basic("Administrator", AdminPassword));
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApiAndScoring()
      {
         ClearServerList();

         var surbl = _settings.AntiSpam.SURBLServers[0];
         surbl.Active = false;
         surbl.Save();

         if (quarantineEnabled_)
         {
            EmptyTheQuarantine();
            IniFileSetting.Delete("QuarantineEnabled");
            quarantineEnabled_ = false;
         }

         RestListener.Stop();
         _application.Reinitialize();
      }

      // ---- the tests -------------------------------------------------------

      [Test]
      [Description("A safe sender skips the spam verdict for the account that listed it, and for no other recipient of the same message")]
      public void ASafeSenderSkipsTheVerdictForItsOwnAccountAndNoOther()
      {
         Account trusting = AddAccount("trusting");
         Account other = AddAccount("other");

         AddOwnEntry(trusting.Address, "example.com", false);

         new SmtpClientSimulator().Send("editor@example.com", new List<string> { trusting.Address, other.Address }, "Weekly", SpamBody);

         // The other recipient's copy is the proof the message really was
         // classified; without it, an unmarked copy below would prove nothing.
         string otherCopy = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam", otherCopy);
         StringAssert.Contains("[SPAM] Weekly", otherCopy);

         string trustingCopy = Pop3ClientSimulator.AssertGetFirstMessageText(trusting.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", trustingCopy,
            "The account listed the sender's domain as safe, so its copy must arrive as though it had never been classified.");
         StringAssert.DoesNotContain("[SPAM]", trustingCopy);
      }

      [Test]
      [Description("A blocked sender is marked as spam for its own account even when nothing else objected, and only for that account")]
      public void ABlockedSenderIsMarkedForItsOwnAccountEvenWhenNothingElseObjects()
      {
         Account blocking = AddAccount("blocking");
         Account other = AddAccount("unbothered");

         AddOwnEntry(blocking.Address, "pest@example.com", true);

         new SmtpClientSimulator().Send("pest@example.com", new List<string> { blocking.Address, other.Address }, "Hello again", CleanBody);

         string otherCopy = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", otherCopy, "Somebody else's block list is not this recipient's business.");

         string blockingCopy = Pop3ClientSimulator.AssertGetFirstMessageText(blocking.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", blockingCopy);
         StringAssert.Contains("[SPAM] Hello again", blockingCopy);
         StringAssert.Contains("account block list", blockingCopy, "The reason header names the list that decided.");
         StringAssert.DoesNotContain("X-hMailServer-Reason-Score", blockingCopy,
            "Nothing measured this message, so no score may be recorded for a threshold to read.");
      }

      [Test]
      [Description("The account's list, then its domain's, then the server's: the nearest with an opinion decides, the more specific entry wins inside a list, and a second write replaces the first")]
      public void TheNearestListWithAnOpinionDecides()
      {
         // The server blocks the domain: an account that says nothing is marked.
         AddEntry(ServerList(), "example.com", true);

         Account serverDecides = AddAccount("server-decides");
         SendClean("someone@example.com", serverDecides.Address, "Server");
         AssertMarked(serverDecides.Address, true, "the server's block is the only opinion");

         // The domain allows it: the domain is nearer than the server.
         AddEntry(DomainList(), "example.com", false);

         Account domainDecides = AddAccount("domain-decides");
         SendClean("someone@example.com", domainDecides.Address, "Domain");
         AssertMarked(domainDecides.Address, false, "the domain's allow is nearer than the server's block");

         // The account blocks it: the account is nearer than the domain.
         Account accountDecides = AddAccount("account-decides");
         AddOwnEntry(accountDecides.Address, "example.com", true);
         SendClean("someone@example.com", accountDecides.Address, "Account");
         AssertMarked(accountDecides.Address, true, "the account's own block is nearer than the domain's allow");

         // Inside one list, an address is more specific than a domain.
         Account specific = AddAccount("specific");
         AddOwnEntry(specific.Address, "example.com", true);
         AddOwnEntry(specific.Address, "someone@example.com", false);
         SendClean("someone@example.com", specific.Address, "Specific");
         AssertMarked(specific.Address, false, "an entry for the one address beats an entry for its whole domain");

         // Writing the same value twice replaces the first entry: a list a person
         // keeps by hand never holds an allow and a block of one sender at once.
         Account changedMind = AddAccount("changed-mind");
         AddOwnEntry(changedMind.Address, "example.com", false);
         (int status, string body) second = Me(changedMind.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"example.com\",\"blocked\":true}");
         ClassicAssert.AreEqual(200, second.status, "A value already on the list is rewritten, which answers 200 rather than 201. Body: " + second.body);

         (int status, string body) listing = Me(changedMind.Address, "GET", "/api/v1/me/sender-list");
         ClassicAssert.AreEqual(1, Regex.Matches(listing.body, "\"value\":\"example.com\"").Count, listing.body);
         StringAssert.Contains("\"blocked\":true", listing.body);

         SendClean("someone@example.com", changedMind.Address, "Changed");
         AssertMarked(changedMind.Address, true, "the later write is the entry");
      }

      [Test]
      [Description("Where the server holds spam, a blocked sender's message is held for the account that blocked it, and releasing it delivers it rather than holding it again")]
      public void ABlockedSendersMessageIsHeldAndItsReleaseIsDelivered()
      {
         IniFileSetting.Write("QuarantineEnabled", "1");
         quarantineEnabled_ = true;
         _application.Reinitialize();
         EmptyTheQuarantine();

         Account blocking = AddAccount("holding");
         Account other = AddAccount("receiving");

         AddOwnEntry(blocking.Address, "pest@example.com", true);

         new SmtpClientSimulator().Send("pest@example.com", new List<string> { blocking.Address, other.Address }, "Held for one", CleanBody);

         string otherCopy = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, Password);
         StringAssert.Contains("Held for one", otherCopy);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(blocking.Address, Password, 0);

         (int status, string body) held = Me(blocking.Address, "GET", "/api/v1/me/quarantine");
         ClassicAssert.AreEqual(200, held.status, held.body);
         StringAssert.Contains("Held for one", held.body);
         StringAssert.Contains("account block list", held.body);

         (int status, string body) notHeld = Me(other.Address, "GET", "/api/v1/me/quarantine");
         StringAssert.DoesNotContain("Held for one", notHeld.body,
            "The quarantined copy is the blocking account's alone; the other recipient already has theirs.");

         string id = Regex.Match(held.body, "\"id\":(\\d+)").Groups[1].Value;
         (int status, string body) release = Me(blocking.Address, "POST", "/api/v1/me/quarantine/" + id + "/release", "");
         ClassicAssert.AreEqual(200, release.status, release.body);

         // The block still stands, and the release must not meet it again.
         Pop3ClientSimulator.AssertMessageCount(blocking.Address, Password, 1);

         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         ClassicAssert.AreEqual(0, quarantine.Count, "A released message that the same block list held again would be back here.");
      }

      [Test]
      [Description("An entry covers the address in the From header as well as the envelope sender, which is what a mailing service's message carries - and a bounce is never decided by a list")]
      public void AnEntryCoversTheFromHeaderAndABounceIsNeverDecided()
      {
         Account reader = AddAccount("reader");

         // The newsletter comes through a mailing service: its envelope is the
         // service's, its From header is the brand's, and the reader - like the
         // webmail's "Block this sender" - names the brand.
         AddOwnEntry(reader.Address, "brand.example", true);

         SendRaw("bounces-12345@mailer.example", reader.Address, "News <news@brand.example>", "This week");
         AssertMarked(reader.Address, true, "the From header's address is on the reader's block list");

         // A delivery status notification has the null sender. Whatever its From
         // header says, no list decides it.
         SendRaw("", reader.Address, "news@brand.example", "Undeliverable: This week");
         AssertMarked(reader.Address, false, "a bounce is never decided by a list");
      }

      [Test]
      [Description("An entry that is neither one address nor one domain is refused, and nothing is stored")]
      public void AnEntryThatIsNeitherAnAddressNorADomainIsRefused()
      {
         Account account = AddAccount("careful");

         foreach (string value in new[] { "not an address", "nodot", "two@@example.com", "@", "user@nodot", ".example.com" })
         {
            (int status, string body) refused = Me(account.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"" + value + "\"}");
            ClassicAssert.AreEqual(400, refused.status, "'" + value + "' must be refused. Body: " + refused.body);
         }

         (int status, string body) unknown = Me(account.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"a.example\",\"colour\":\"red\"}");
         ClassicAssert.AreEqual(400, unknown.status, unknown.body);
         StringAssert.Contains("unknown field", unknown.body);

         (int status, string body) listing = Me(account.Address, "GET", "/api/v1/me/sender-list");
         StringAssert.Contains("\"items\":[]", listing.body, "A refusal changes nothing.");

         // A leading @ is the documented domain spelling, and the case is not kept.
         (int status, string body) accepted = Me(account.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"@Example.ORG\"}");
         ClassicAssert.AreEqual(201, accepted.status, accepted.body);
         StringAssert.Contains("\"value\":\"example.org\"", accepted.body);
         StringAssert.Contains("\"blocked\":false", accepted.body, "An entry that does not say otherwise allows.");

         // A URL is stored as its host and a slash, the scheme, the path, the
         // query and a leading www. gone; a scheme with no host is refused.
         (int status, string body) url = Me(account.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"https://www.Bad.Example/x?y=1\",\"blocked\":true}");
         ClassicAssert.AreEqual(201, url.status, url.body);
         StringAssert.Contains("\"value\":\"bad.example/\"", url.body);

         foreach (string value in new[] { "https://", "http:///deals", "https://nodot/x" })
         {
            (int status, string body) refusedUrl = Me(account.Address, "POST", "/api/v1/me/sender-list", "{\"value\":\"" + value + "\"}");
            ClassicAssert.AreEqual(400, refusedUrl.status, "'" + value + "' must be refused. Body: " + refusedUrl.body);
         }
      }

      [Test]
      [Description("A URL entry blocks a message that links to that host - or a subdomain of it, whatever the path - for the account that listed it, and for no other recipient; the sender is not what decided")]
      public void AUrlEntryBlocksTheLinkForItsOwnAccountAlone()
      {
         Account blocking = AddAccount("linkblocking");
         Account other = AddAccount("linkother");

         AddOwnEntry(blocking.Address, "https://Shop.Example/deals?x=1", true);

         new SmtpClientSimulator().Send("friend@example.com", new List<string> { blocking.Address, other.Address }, "Look at this",
            "A sale: http://www.cdn.shop.example/sale/today - and nothing else objectionable.");

         string otherCopy = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", otherCopy, "Somebody else's URL entry is not this recipient's business.");

         string blockingCopy = Pop3ClientSimulator.AssertGetFirstMessageText(blocking.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", blockingCopy);
         StringAssert.Contains("[SPAM] Look at this", blockingCopy);
         StringAssert.Contains("account block list", blockingCopy, "The reason header names the list that decided.");
         StringAssert.Contains("shop.example/", blockingCopy, "And the entry.");

         // The same sender, no link: the entry is about the link, not the sender.
         Account plain = AddAccount("linkplain");
         AddOwnEntry(plain.Address, "https://shop.example/", true);
         new SmtpClientSimulator().Send("friend@example.com", plain.Address, "No link this time", CleanBody);
         string plainCopy = Pop3ClientSimulator.AssertGetFirstMessageText(plain.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", plainCopy, "A URL entry says nothing about a message with no link to it.");
      }

      [Test]
      [Description("A URL entry that allows rescues a message whose link the URL blacklists list, for the account that listed it and for no other recipient")]
      public void AUrlEntryRescuesALinkTheBlacklistsList()
      {
         Account trusting = AddAccount("linktrusting");
         Account other = AddAccount("linkunbothered");

         AddOwnEntry(trusting.Address, "http://surbl-org-permanent-test-point.com/", false);

         new SmtpClientSimulator().Send("editor@example.com", new List<string> { trusting.Address, other.Address }, "Weekly links", SpamBody);

         string otherCopy = Pop3ClientSimulator.AssertGetFirstMessageText(other.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam", otherCopy, "The blacklist marked the other recipient's copy; without that the rescue below proves nothing.");
         StringAssert.Contains("[SPAM] Weekly links", otherCopy);

         string trustingCopy = Pop3ClientSimulator.AssertGetFirstMessageText(trusting.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", trustingCopy,
            "The account listed the link's host as allowed, so its copy must arrive as though it had never been classified.");
         StringAssert.DoesNotContain("[SPAM]", trustingCopy);
      }

      [Test]
      [Description("A URL entry judges the host a link goes to: the host after the link's user name, and an address in its own order, never reversed")]
      public void AUrlEntryJudgesTheHostTheLinkGoesTo()
      {
         // Until 6.3.6 the links were read as the characters after "://" that can
         // be in a name, so http://trusted.example@shop.example/ was read as
         // trusted.example and a block of shop.example missed it; and an address
         // was handed over reversed, as a DNS list is asked about it, as well as in
         // its own order, so an entry for 61.2.0.192 blocked a link to 192.0.2.61.
         Account blocking = AddAccount("linkuserinfo");
         AddOwnEntry(blocking.Address, "https://shop.example/", true);

         new SmtpClientSimulator().Send("friend@example.com", blocking.Address, "A link with a user name",
            "A sale: http://trusted.example@shop.example/deals - and nothing else objectionable.");

         string blockingCopy = Pop3ClientSimulator.AssertGetFirstMessageText(blocking.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", blockingCopy, "The link goes to shop.example, whatever user name comes before it.");

         Account addressBlocking = AddAccount("linkaddress");
         AddOwnEntry(addressBlocking.Address, "http://192.0.2.61/", true);
         Account reversedBlocking = AddAccount("linkreversed");
         AddOwnEntry(reversedBlocking.Address, "http://61.2.0.192/", true);

         new SmtpClientSimulator().Send("friend@example.com", new List<string> { addressBlocking.Address, reversedBlocking.Address }, "A link to an address",
            "An offer: http://192.0.2.61/offer - and nothing else objectionable.");

         string addressCopy = Pop3ClientSimulator.AssertGetFirstMessageText(addressBlocking.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", addressCopy, "An entry for the address blocks a link to it.");

         string reversedCopy = Pop3ClientSimulator.AssertGetFirstMessageText(reversedBlocking.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", reversedCopy, "An entry for 61.2.0.192 is not about a link to 192.0.2.61.");
      }

      [Test]
      [Description("Every link of a message is matched against the URL entries, however many come before it, and the links are read once for all its recipients")]
      public void AUrlEntryJudgesEveryLinkReadOnceForAllRecipients()
      {
         // Until 6.3.6 the first 16 hosts of a message were
         // matched, so a link after sixteen others was never seen by any entry,
         // and the body was parsed and read again for each recipient with a URL
         // entry. Here the listed links come after twenty others, and three
         // recipients' entries are decided from one reading of the message.
         Account byName = AddAccount("linkeverybyname");
         Account byAddress = AddAccount("linkeverybyaddress");
         Account unrelated = AddAccount("linkeveryunrelated");

         AddOwnEntry(byName.Address, "https://shop.example/", true);

         // An IPv4-mapped IPv6 address is the IPv4 address a browser reaches,
         // for an entry as for a link.
         (int status, string body) mapped = AddOwnEntry(byAddress.Address, "http://[::ffff:192.0.2.62]/", true);
         StringAssert.Contains("\"value\":\"192.0.2.62/\"", mapped.body);

         AddOwnEntry(unrelated.Address, "https://elsewhere.example/", true);

         var text = new StringBuilder("Twenty links first:");
         for (int i = 1; i <= 20; i++)
            text.Append(" http://decoy" + i + ".example/");
         text.Append(" - then http://cdn.shop.example/sale and http://[::ffff:192.0.2.62]/offer - and nothing else objectionable.");

         LogHandler.DeleteCurrentDefaultLog();

         new SmtpClientSimulator().Send("friend@example.com", new List<string> { byName.Address, byAddress.Address, unrelated.Address },
            "Many links", text.ToString());

         string byNameCopy = Pop3ClientSimulator.AssertGetFirstMessageText(byName.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", byNameCopy, "The entry's link came after twenty others.");
         StringAssert.Contains("cdn.shop.example is on the account block list as shop.example/", byNameCopy,
            "The reason header names the link the entry covered.");

         string byAddressCopy = Pop3ClientSimulator.AssertGetFirstMessageText(byAddress.Address, Password);
         StringAssert.Contains("X-hMailServer-Spam: YES", byAddressCopy, "The address's entry covers the link to it as an IPv4-mapped address.");

         string unrelatedCopy = Pop3ClientSimulator.AssertGetFirstMessageText(unrelated.Address, Password);
         StringAssert.DoesNotContain("X-hMailServer-Spam", unrelatedCopy, "An entry for a host the message does not link to decided it.");

         // One reading for the three recipients.
         string log = LogHandler.ReadCurrentDefaultLog();
         int readings = Regex.Matches(log, Regex.Escape("its links read for the sender lists' URL entries")).Count;
         ClassicAssert.AreEqual(1, readings, "The message's links were read " + readings + " times for three recipients with URL entries.");
      }

      [Test]
      [Description("One account's entry is not found by another: neither changed nor removed")]
      public void AnotherAccountsEntryCannotBeChangedOrRemoved()
      {
         Account owner = AddAccount("owner");
         Account stranger = AddAccount("stranger");

         (int status, string body) created = AddOwnEntry(owner.Address, "friend.example", false);
         string id = Regex.Match(created.body, "\"id\":(\\d+)").Groups[1].Value;

         ClassicAssert.AreEqual(404, Me(stranger.Address, "PUT", "/api/v1/me/sender-list/" + id, "{\"blocked\":true}").status);
         ClassicAssert.AreEqual(404, Me(stranger.Address, "DELETE", "/api/v1/me/sender-list/" + id).status);

         (int status, string body) still = Me(owner.Address, "GET", "/api/v1/me/sender-list");
         StringAssert.Contains("friend.example", still.body);
         StringAssert.Contains("\"blocked\":false", still.body);

         ClassicAssert.AreEqual(200, Me(owner.Address, "DELETE", "/api/v1/me/sender-list/" + id).status);
         StringAssert.Contains("\"items\":[]", Me(owner.Address, "GET", "/api/v1/me/sender-list").body);
      }

      [Test]
      [Description("The server's and a domain's lists are resources over REST in the same shape, and the OpenAPI document describes all of them")]
      public void TheServerAndDomainListsAreResourcesOverRest()
      {
         string admin = Basic("Administrator", AdminPassword);

         (int status, string body) created = Http("POST", "/api/v1/sender-list", admin, "{\"value\":\"Partner.Example\",\"reason\":\"invoices\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         StringAssert.Contains("\"scope\":\"server\"", created.body);
         StringAssert.Contains("\"value\":\"partner.example\"", created.body);
         string serverId = Regex.Match(created.body, "\"id\":(\\d+)").Groups[1].Value;

         (int status, string body) replaced = Http("POST", "/api/v1/sender-list", admin, "{\"value\":\"partner.example\",\"blocked\":true}");
         ClassicAssert.AreEqual(200, replaced.status, replaced.body);
         StringAssert.Contains("\"id\":" + serverId, replaced.body, "The same value rewrites the same row.");

         (int status, string body) expiring = Http("POST", "/api/v1/sender-list", admin, "{\"value\":\"temporary.example\",\"expires\":true}");
         ClassicAssert.AreEqual(400, expiring.status, "An entry that expires needs its time. Body: " + expiring.body);

         string domainPath = "/api/v1/domains/" + _domain.Name + "/sender-list";
         (int status, string body) domainCreated = Http("POST", domainPath, admin, "{\"value\":\"supplier.example\"}");
         ClassicAssert.AreEqual(201, domainCreated.status, domainCreated.body);
         StringAssert.Contains("\"scope\":\"domain\"", domainCreated.body);
         string domainId = Regex.Match(domainCreated.body, "\"id\":(\\d+)").Groups[1].Value;

         StringAssert.DoesNotContain("supplier.example", Http("GET", "/api/v1/sender-list", admin).body, "A domain's entry is not the server's.");
         StringAssert.Contains("supplier.example", Http("GET", domainPath, admin).body);

         ClassicAssert.AreEqual(404, Http("DELETE", "/api/v1/sender-list/" + domainId, admin).status, "A domain's entry is not reached through the server's list.");
         ClassicAssert.AreEqual(200, Http("DELETE", domainPath + "/" + domainId, admin).status);
         ClassicAssert.AreEqual(200, Http("DELETE", "/api/v1/sender-list/" + serverId, admin).status);
         ClassicAssert.AreEqual(404, Http("GET", "/api/v1/domains/no-such-domain.test/sender-list", admin).status);

         // An account's own credentials reach its own list and nothing else.
         Account account = AddAccount("not-an-administrator");
         ClassicAssert.AreNotEqual(200, Http("GET", "/api/v1/sender-list", Basic(account.Address, Password)).status);

         string document = Http("GET", "/api/v1/openapi.json", admin).body;
         foreach (string path in new[] { "/api/v1/sender-list", "/api/v1/sender-list/{id}", "/api/v1/domains/{domain}/sender-list",
                                          "/api/v1/domains/{domain}/sender-list/{id}", "/api/v1/me/sender-list", "/api/v1/me/sender-list/{id}",
                                          "/api/v1/me/quarantine-digest" })
         {
            StringAssert.Contains("\"" + path + "\"", document, "The OpenAPI document must describe " + path + ".");
         }

         // Described is not enough: the Deck JSON.parses this document to build every
         // form it has. This unit's paths used to be appended without the comma that
         // joins them to the paths before and with one too many after, so the text
         // held every path and the document was not JSON. The suite has no JSON
         // parser, so the two joins are checked where they break.
         StringAssert.Contains(",\"/api/v1/sender-list\":{", document,
            "The sender-list paths must be joined to the path before them with a comma.");
         StringAssert.DoesNotContain("}\"/api/", document,
            "A path must never follow the end of the previous one without a comma; the document would not be JSON.");
         StringAssert.DoesNotContain(",,\"/api/", document,
            "A path must never follow two commas; the document would not be JSON.");
      }

      // ---- helpers ---------------------------------------------------------

      private Account AddAccount(string localPart)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, localPart + "@" + _domain.Name, Password);
      }

      private dynamic ServerList()
      {
         return ((dynamic)_settings.AntiSpam).SenderList;
      }

      private dynamic DomainList()
      {
         return ((dynamic)_domain).SenderList;
      }

      private void ClearServerList()
      {
         dynamic list = ServerList();
         list.Clear();
      }

      private static void AddEntry(dynamic list, string value, bool blocked)
      {
         dynamic entry = list.Add();
         entry.Value = value;
         entry.Blocked = blocked;
         entry.Reason = "regression suite";
         entry.Save();
      }

      private (int status, string body) AddOwnEntry(string address, string value, bool blocked)
      {
         (int status, string body) result = Me(address, "POST", "/api/v1/me/sender-list",
            "{\"value\":\"" + value + "\",\"blocked\":" + (blocked ? "true" : "false") + "}");

         ClassicAssert.IsTrue(result.status == 201 || result.status == 200, "Adding " + value + " for " + address + " failed. Body: " + result.body);
         return result;
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      private static void SendClean(string from, string to, string subject)
      {
         new SmtpClientSimulator().Send(from, to, subject, CleanBody);
      }

      private static void SendRaw(string envelopeSender, string recipient, string fromHeader, string subject)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
         socket.ReadUntil("220");

         socket.Send("HELO test\r\n");
         socket.ReadUntil("250");

         socket.Send("MAIL FROM:<" + envelopeSender + ">\r\n");
         socket.ReadUntil("250");

         socket.Send("RCPT TO:<" + recipient + ">\r\n");
         socket.ReadUntil("250");

         socket.Send("DATA\r\n");
         socket.ReadUntil("354");

         socket.Send("From: " + fromHeader + "\r\n" +
                     "To: " + recipient + "\r\n" +
                     "Subject: " + subject + "\r\n" +
                     "\r\n" +
                     CleanBody + "\r\n" +
                     ".\r\n");
         socket.ReadUntil("250");

         socket.Send("QUIT\r\n");
         socket.Disconnect();
      }

      private static void AssertMarked(string address, bool marked, string because)
      {
         string copy = Pop3ClientSimulator.AssertGetFirstMessageText(address, Password);

         if (marked)
            StringAssert.Contains("X-hMailServer-Spam", copy, "Expected " + address + "'s copy marked: " + because + ".");
         else
            StringAssert.DoesNotContain("X-hMailServer-Spam", copy, "Expected " + address + "'s copy unmarked: " + because + ".");
      }

      private (int status, string body) Me(string address, string method, string path, string body = null)
      {
         return Http(method, path, Basic(address, Password), body);
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               int status = 0;
               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
      }
   }
}
