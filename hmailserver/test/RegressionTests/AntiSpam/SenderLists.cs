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
