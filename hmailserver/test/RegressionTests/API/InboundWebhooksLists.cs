// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    The inbound webhooks' tests that only the Windows suite compiles: a route
   ///    beside a distribution list, which needs the list's COM shape (Managed,
   ///    Mode, RequireSenderAddress as an address), and a route removed between a
   ///    message's acceptance and its delivery, which an event script holds open.
   ///    The rest of the fixture is InboundWebhooks.cs.
   /// </summary>
   public partial class InboundWebhooks
   {
      private const string ListPassword = "list-password";

      // The language this server runs event scripts in, asked once before the
      // fixture's REST listener starts - asking restarts the server
      // (ScriptLanguages) - or null when it runs none.
      private string _scriptLanguage;

      [OneTimeSetUp]
      public void AskWhichScriptLanguage()
      {
         _scriptLanguage = ScriptLanguages.ServerRuns("JavaScript") ? "JavaScript" :
                           ScriptLanguages.ServerRuns("JScript") ? "JScript" : null;

         // Asking may have reinitialised the server; the settings object is read again.
         _settings = _application.Settings;
      }

      [Test]
      [Description("A route that posts only takes the mail of a managed list's address as it took the RCPT: the route posts it and the list sends nothing - so a sender the list's own rules would have refused never reaches its members through the route.")]
      public void ARouteThatPostsOnlyTakesAManagedListsMail()
      {
         string member = "announce-member@" + _domainName;
         string announcer = "announcer@" + _domainName;
         string list = "announce@" + _domainName;

         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), member, ListPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), announcer, ListPassword);

         DistributionList announcements = SingletonProvider<TestSetup>.Instance.AddDistributionList(CurrentDomain(), list, new List<string> { member });
         announcements.Managed = true;
         announcements.Mode = eDistributionListMode.eLMAnnouncement;
         announcements.RequireSenderAddress = announcer;
         announcements.Save();

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            // A route for the whole domain, the list's address among them.
            Create(_domainName);

            // A stranger: announcement-only refuses them, and before the route took
            // the list's address first the list distributed it anyway.
            SmtpClientSimulator.StaticSend("stranger@example.net", list, "Not an announcement", "A body.");

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count, "The route did not post the list's mail.");

            Pop3ClientSimulator.AssertMessageCount(member, ListPassword, 0);
         }
      }

      [Test]
      [Description("A route that delivers as well, on an alias of a managed list, posts the message and lets the list distribute it: the list's copy goes to the list manager, and its members receive it - it is never sent over SMTP to the domain's MX.")]
      public void ARouteThatDeliversAsWellOnAnAliasOfAManagedListLetsTheListDistributeIt()
      {
         string member = "team-member@" + _domainName;
         string list = "team@" + _domainName;
         string alias = "helpdesk@" + _domainName;

         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), member, ListPassword);

         DistributionList team = SingletonProvider<TestSetup>.Instance.AddDistributionList(CurrentDomain(), list, new List<string> { member });
         team.Managed = true;
         team.Save();

         SingletonProvider<TestSetup>.Instance.AddAlias(CurrentDomain(), alias, list);

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(alias, "\"deliver\":true");

            SmtpClientSimulator.StaticSend("customer@example.net", alias, "Help, please", "A body.");

            Assert.AreEqual(1, WaitForRequests(endpoint, 1).Count, "The route did not post it.");
            Pop3ClientSimulator.AssertMessageCount(member, ListPassword, 1);
         }
      }

      [Test]
      [Description("A managed list's address written with a plus tag, under a route that delivers as well, reaches the list: the route posts it with the tag, and the list manager distributes it.")]
      public void APlusTaggedListAddressUnderARouteReachesTheList()
      {
         Domain domain = CurrentDomain();
         domain.PlusAddressingEnabled = true;
         domain.PlusAddressingCharacter = "+";
         domain.Save();

         string member = "news-member@" + _domainName;
         string list = "news@" + _domainName;

         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), member, ListPassword);

         DistributionList news = SingletonProvider<TestSetup>.Instance.AddDistributionList(CurrentDomain(), list, new List<string> { member });
         news.Managed = true;
         news.Save();

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(list, "\"deliver\":true");

            string tagged = "news+weekly@" + _domainName;
            SmtpClientSimulator.StaticSend("reader@example.net", tagged, "For the list", "A body.");

            List<string> requests = WaitForRequests(endpoint, 1);
            Assert.AreEqual(1, requests.Count, "The route did not post it.");
            StringAssert.Contains(tagged, ParseForm(requests[0], endpoint.RequestBodies[0])["envelope"].Text);

            Pop3ClientSimulator.AssertMessageCount(member, ListPassword, 1);
         }
      }

      [Test]
      [Description("A route that delivers as well, on a moderated list, posts the message and sends the posting the list's mode refuses to the moderator, as the list would have with no route - never to the members.")]
      public void ARouteThatDeliversAsWellOnAModeratedListSendsThePostingToTheModerator()
      {
         string member = "mod-member@" + _domainName;
         string moderator = "moderator@" + _domainName;
         string poster = "poster@" + _domainName;
         string list = "moderated@" + _domainName;

         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), member, ListPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), moderator, ListPassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), poster, ListPassword);

         DistributionList moderated = SingletonProvider<TestSetup>.Instance.AddDistributionList(CurrentDomain(), list, new List<string> { member });
         moderated.Mode = eDistributionListMode.eLMMembership;
         moderated.ModeratorAddress = moderator;
         moderated.Save();

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(list, "\"deliver\":true");

            // The poster is not a member: the membership mode refuses them, and the
            // list's moderator is asked instead.
            SmtpClientSimulator.StaticSend(poster, list, "Please approve", "A body.");

            Assert.AreEqual(1, WaitForRequests(endpoint, 1).Count, "The route did not post it.");

            string forwarded = Pop3ClientSimulator.AssertGetFirstMessageText(moderator, ListPassword);
            StringAssert.Contains("Subject: Please approve", forwarded, forwarded);

            Pop3ClientSimulator.AssertMessageCount(member, ListPassword, 0);
         }
      }

      [Test]
      [Description("A route removed between a message's acceptance and its delivery leaves the address what it is without one: here nobody's, so the sender is told it is unknown (5.1.1) - the message is never posted, and never sent on over SMTP as though the address were somewhere else.")]
      public void ARouteRemovedBeforeItsMessageIsDeliveredLeavesTheAddressAsItIsWithoutOne()
      {
         if (_scriptLanguage == null)
            Assert.Ignore("This server runs no event script, and the test holds a message between its acceptance and its delivery with one: " + ScriptLanguages.Describe());

         string sender = "orphan-sender@" + _domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(CurrentDomain(), sender, AccountPassword);

         string address = "gone@" + _domainName;

         Scripting scripting = _application.Settings.Scripting;

         using (var endpoint = new FakeHttpEndpoint(IPAddress.Loopback, EndpointPort, 200, "{}", "application/json"))
         {
            Create(address);

            try
            {
               // Every delivery is held for a few seconds once it has started - past
               // the global rules and before the routes are asked - so that the
               // route can be removed in between.
               LogHandler.DeleteEventLog();
               scripting.Language = _scriptLanguage;
               File.WriteAllText(scripting.CurrentScriptFile, string.Join("\r\n",
                  "function OnDeliveryStart(oMessage) {",
                  "   EventLog.Write('inbound-webhooks: holding a delivery');",
                  "   var until = new Date().getTime() + 5000;",
                  "   while (new Date().getTime() < until) { }",
                  "}"));
               scripting.Enabled = true;
               scripting.Reload();

               SmtpClientSimulator.StaticSend(sender, address, "Held", "A body.");

               Assert.IsTrue(WaitUntil(() => EventLogContains("inbound-webhooks: holding a delivery"), 30), "The delivery was never held.");

               (int status, string body) removed = Http("DELETE", RoutesPath + "/" + Uri.EscapeDataString(address));
               Assert.AreEqual(200, removed.status, removed.body);

               string report = Pop3ClientSimulator.AssertGetFirstMessageText(sender, AccountPassword);
               StringAssert.Contains("5.1.1", report, report);
               StringAssert.Contains(address, report, report);
               StringAssert.Contains("Unknown user", report, report);

               Assert.AreEqual(0, endpoint.Requests.Count, "A route removed before the delivery posted it all the same.");
            }
            finally
            {
               scripting.Enabled = false;

               string file = scripting.CurrentScriptFile;
               if (File.Exists(file))
                  File.WriteAllText(file, string.Empty);

               scripting.Reload();
            }
         }
      }

      [Test]
      [Description("A key holding manage_inbound_webhooks and confined to a domain cannot route the server administrator's mailbox in that domain - by its address, by a plus-tagged address that reaches it, or by routing the whole domain - nor change or remove the route the administrator put on it; an ordinary account of the domain it still routes")]
      public void AConfinedKeyCannotRouteTheServerAdministratorsMailbox()
      {
         // The negative control for RestApiServer::InboundWebhookReachesAccountBeyond_.
         // Until 26 September 2026 a route was judged by its domain alone, so a key
         // or a custom role confined to a domain could route the server
         // administrator's address there to a URL of its own - the route takes the
         // mail instead of the mailbox - and read its mail, password resets included.
         Domain domain = CurrentDomain();
         domain.PlusAddressingEnabled = true;
         domain.PlusAddressingCharacter = "+";
         domain.Save();

         Account boss = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "boss@" + _domainName, AccountPassword);
         boss.AdminLevel = eAdminLevel.hAdminLevelServerAdmin;
         boss.Save();
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plain@" + _domainName, AccountPassword);

         MakeRole("inboundtest-takein", "view_configuration,manage_inbound_webhooks");
         string key = MakeKey("inbound take-in", null, "inboundtest-takein", _domainName);

         const string Refusal = "holds authority this credential does not";

         foreach (string address in new[] { "boss@" + _domainName, "boss+invoices@" + _domainName, _domainName })
         {
            (int status, string body) refused = AsKey(key, "POST", RoutesPath,
               "{\"address\":\"" + address + "\",\"url\":\"https://8.8.8.8/inbound\",\"secret\":\"" + Secret + "\"}");
            Assert.AreEqual(403, refused.status, "A confined key routed " + address + ": " + refused.body);
            StringAssert.Contains(Refusal, refused.body, address);
         }

         StringAssert.DoesNotContain("8.8.8.8", Read(RoutesPath), "A refused route was stored.");

         // The route the administrator put on the mailbox is the administrator's:
         // the key may not repoint it, nor take it away.
         Create("boss@" + _domainName);
         string bossPath = RoutesPath + "/" + Uri.EscapeDataString("boss@" + _domainName);

         (int status, string body) repointed = AsKey(key, "PUT", bossPath, "{\"url\":\"https://8.8.8.8/mine\"}");
         Assert.AreEqual(403, repointed.status, repointed.body);
         StringAssert.Contains(Refusal, repointed.body);
         StringAssert.DoesNotContain("8.8.8.8", Read(bossPath), "The refused PUT changed the route.");

         (int status, string body) removed = AsKey(key, "DELETE", bossPath, null);
         Assert.AreEqual(403, removed.status, removed.body);
         Assert.AreEqual(200, Http("GET", bossPath).status, "The refused DELETE removed the route.");

         // An ordinary account of the domain is the key's to route, as before.
         (int status, string body) plain = AsKey(key, "POST", RoutesPath,
            "{\"address\":\"plain@" + _domainName + "\",\"url\":\"https://8.8.8.8/inbound\",\"secret\":\"" + Secret + "\"}");
         Assert.AreEqual(201, plain.status, plain.body);
         Assert.AreEqual(200, AsKey(key, "DELETE", RoutesPath + "/" + Uri.EscapeDataString("plain@" + _domainName), null).status);
      }

      private static bool EventLogContains(string text)
      {
         try
         {
            return LogHandler.ReadEventLog().Contains(text);
         }
         catch (IOException)
         {
            // The server may be mid-write; asked again.
            return false;
         }
      }
   }
}
