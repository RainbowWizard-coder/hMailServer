// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The own-reputation watch, over REST: GET /api/v1/reputation,
   ///    POST /api/v1/reputation/check, and the reputation.listed alert the rows
   ///    raise.
   ///
   ///    Every list here is a zone in the suite's own DNS (SuiteDns), so the whole
   ///    walk is the server's: the test entries RFC 5782 requires, the address's own
   ///    name, and the TXT the list answers with. Nothing resolves anything real.
   ///
   ///    The fixture is linked into the Linux suite, and everything it does it does
   ///    over REST - including emptying what the watch has stored, which is a check
   ///    against no lists rather than a DELETE. Application.Database is a shim there
   ///    whose ExecuteSQL calls Assert.Ignore, and one of those in [SetUp] would
   ///    ignore every test in the fixture on every Linux run while still counting as
   ///    a linked fixture.
   ///
   ///    The tests that matter are the ones about NOT saying clean. A list that
   ///    refuses the query, one that does not answer this resolver, and one that
   ///    answers for every address each have their own status, none of them is
   ///    "not listed", and a listing an earlier check found survives a check that
   ///    could not get an answer - because an administrator who is told their
   ///    server is clean stops looking.
   /// </summary>
   [TestFixture]
   public class RestApiReputationWatch : TestFixtureBase
   {
      private static int RestPort = 9590;
      private const string AdminPassword = "testar";

      private const string ReputationPath = "/api/v1/reputation";
      private const string CheckPath = "/api/v1/reputation/check";
      private const string RulePath = "/api/v1/alerts/rules/reputation.listed";

      // TEST-NET-1 and TEST-NET-3 (RFC 5737) and the documentation prefix
      // (RFC 3849): addresses that are routable in shape and belong to nobody, so a
      // query about one leaves this machine no more than the zone does.
      private const string Listed = "192.0.2.25";
      private const string Other = "203.0.113.9";
      private const string Sixth = "2001:db8::25";

      private string savedEnabled_;
      private string savedLists_;
      private string savedAddresses_;

      // The newest alert event when this test started. Every assertion about
      // reputation.listed counts from here rather than from an empty table,
      // because emptying the table needs SQL and this fixture is deliberately
      // reachable over REST alone - see the class comment.
      private static long baseline_;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Http("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // What the bench had, put back in the teardown: this fixture points the
         // watch at zones that exist only while it runs.
         Dictionary<string, object> settings = MiniJson.Object(Read("/api/v1/settings"));
         savedEnabled_ = Convert.ToBoolean(settings["reputation_watch_enabled"]) ? "true" : "false";
         savedLists_ = (string) settings["reputation_watch_lists"];
         savedAddresses_ = (string) settings["reputation_watch_addresses"];

         // The rule is shipped on; it is written here so that a bench where somebody
         // turned it off still runs these, and put back as it ships in the teardown.
         Http("PUT", RulePath, "{\"enabled\":true,\"digest\":false,\"cooldown_minutes\":0,\"actions\":1}");

         Forget();

         // After the reset, so that a condition the reset ended is behind the line
         // and this test starts with reputation.listed neither raised nor recently
         // cleared as far as anything here can see.
         baseline_ = NewestEventId();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            SuiteDns.Reset();

            // Before the settings are put back, because this is what empties the
            // rows: a check against no lists at all writes nothing and deletes
            // every row the last one left.
            Forget();

            Http("PUT", "/api/v1/settings",
               "{\"reputation_watch_enabled\":" + savedEnabled_ +
               ",\"reputation_watch_lists\":\"" + savedLists_ + "\"" +
               ",\"reputation_watch_addresses\":\"" + savedAddresses_ + "\"}");

            Http("PUT", RulePath, "{\"enabled\":true,\"digest\":false,\"cooldown_minutes\":1440,\"actions\":1}");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ------------------------------------------------------------------ helpers

      /// <summary>
      ///    Everything the watch has stored, forgotten - over REST, with no SQL.
      ///    A check against no lists and no addresses produces no results, and
      ///    WriteRows_ deletes every row the previous check left; the alert pass
      ///    that ends the check then finds nothing stored and ends the condition.
      ///
      ///    It is written this way on purpose. The obvious version is two DELETE
      ///    statements through Application.Database, and on Linux that object is a
      ///    shim whose ExecuteSQL calls Assert.Ignore - from [SetUp], which ignores
      ///    every test in the fixture. A fixture that reports as skipped for ever
      ///    is worse than one that is not linked at all, because the skip looks
      ///    like a gap in the server rather than a mistake here.
      /// </summary>
      private static void Forget()
      {
         (int status, string body) = Http("PUT", "/api/v1/settings",
            "{\"reputation_watch_enabled\":true,\"reputation_watch_lists\":\"\",\"reputation_watch_addresses\":\"\"}");

         Assert.AreEqual(200, status, body);

         // 409 is the answer when the daily task is mid-check; it wakes every hour
         // and the bench may have the watch on, so this waits rather than failing a
         // test over a collision that has nothing to do with what it is testing.
         for (int attempt = 0; attempt < 10; attempt++)
         {
            (int checkStatus, string checkBody) = Http("POST", CheckPath, "{}");

            if (checkStatus == 200)
               return;

            if (checkStatus != 409)
               Assert.Fail("POST " + CheckPath + " answered " + checkStatus + ": " + checkBody);

            Thread.Sleep(500);
         }

         Assert.Fail("A reputation check was running for five seconds, so the fixture could not empty what it stored.");
      }

      /// <summary>The id of the newest alert event of any condition, or 0.</summary>
      private static long NewestEventId()
      {
         (int status, string body) = Http("GET", "/api/v1/alerts/events?limit=1");
         Assert.AreEqual(200, status, body);

         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>())
            return Convert.ToInt64(entry["id"]);

         return 0;
      }

      /// <summary>What the watch is to check, as the three settings.</summary>
      private static void Watch(string lists, string addresses)
      {
         (int status, string body) = Http("PUT", "/api/v1/settings",
            "{\"reputation_watch_enabled\":true,\"reputation_watch_lists\":\"" + lists +
            "\",\"reputation_watch_addresses\":\"" + addresses + "\"}");

         Assert.AreEqual(200, status, body);
      }

      /// <summary>
      ///    The two entries RFC 5782 section 5 requires of every list: 127.0.0.2
      ///    listed, 127.0.0.1 not. Without them the watch refuses to believe the
      ///    zone, which is the point of three of these tests.
      /// </summary>
      private static void Answering(string zone)
      {
         SuiteDns.Zone.WithA("2.0.0.127." + zone, "127.0.0.2");
      }

      private static void ListAddress(string zone, string reversed, string answer, string reason)
      {
         SuiteDns.Zone.WithA(reversed + "." + zone, answer);

         if (reason != null)
            SuiteDns.Zone.WithTxt(reversed + "." + zone, reason);
      }

      private static Dictionary<string, object> Check()
      {
         (int status, string body) = Http("POST", CheckPath, "{}");
         Assert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      private static Dictionary<string, object> Report()
      {
         (int status, string body) = Http("GET", ReputationPath);
         Assert.AreEqual(200, status, body);
         return MiniJson.Object(body);
      }

      /// <summary>The answer one list gave about one address, or null.</summary>
      private static Dictionary<string, object> Result(Dictionary<string, object> report, string address, string zone)
      {
         foreach (Dictionary<string, object> entry in MiniJson.List(report, "addresses").Cast<Dictionary<string, object>>())
         {
            if (!string.Equals((string) entry["address"], address, StringComparison.OrdinalIgnoreCase))
               continue;

            foreach (Dictionary<string, object> result in ((List<object>) entry["results"]).Cast<Dictionary<string, object>>())
            {
               if (string.Equals((string) result["list"], zone, StringComparison.OrdinalIgnoreCase))
                  return result;
            }
         }

         return null;
      }

      private static string StatusOf(Dictionary<string, object> report, string address, string zone)
      {
         Dictionary<string, object> result = Result(report, address, zone);
         Assert.IsNotNull(result, "No answer for " + address + " on " + zone + ".");
         return (string) result["status"];
      }

      /// <summary>
      ///    The newest reputation.listed event this test caused, or null when it
      ///    caused none. Events older than the baseline are another test's.
      /// </summary>
      private static Dictionary<string, object> NewestAlert()
      {
         (int status, string body) = Http("GET", "/api/v1/alerts/events?limit=50");
         Assert.AreEqual(200, status, body);

         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>())
         {
            if ((string) entry["condition"] == "reputation.listed" && Convert.ToInt64(entry["id"]) > baseline_)
               return entry;
         }

         return null;
      }

      private static string Read(string path)
      {
         (int status, string body) = Http("GET", path);
         Assert.AreEqual(200, status, body);
         return body;
      }

      // ------------------------------------------------------------------ the tests

      [Test]
      [Description("An address on a list is reported with the list's own answer and reason, raises the alert, and the alert clears when the list stops listing it.")]
      public void AListingIsReportedAndAlertedAndClearsWhenItGoesAway()
      {
         const string zone = "bl.repwatch.test";

         Answering(zone);
         ListAddress(zone, "25.2.0.192", "127.0.0.2", "Listed for sending unsolicited mail");

         Watch(zone, Listed);

         Dictionary<string, object> report = Check();
         Dictionary<string, object> result = Result(report, Listed, zone);

         Assert.IsNotNull(result, "The address was not checked at all: " + MiniJson.Text(report, "counts.addresses"));
         Assert.AreEqual("listed", (string) result["status"]);
         Assert.AreEqual(true, result["listed"], "listed must be true, not merely present.");
         Assert.AreEqual("127.0.0.2", (string) result["code"]);
         StringAssert.Contains("unsolicited", (string) result["reason"], "The list's own TXT is the reason.");
         Assert.Greater(Convert.ToInt64(result["listed_since"]), 0, "A listing records when it started.");

         Dictionary<string, object> raised = NewestAlert();
         Assert.IsNotNull(raised, "A listing must raise reputation.listed.");
         Assert.AreEqual("raised", (string) raised["state"]);
         StringAssert.Contains(Listed, (string) raised["summary"], "The alert names the address.");
         StringAssert.Contains(zone, (string) raised["summary"], "The alert names the list.");

         // The same answer again is the same condition, not a second message.
         Check();
         Assert.AreEqual(1, Count("reputation.listed", "raised"), "A condition that is still true does not raise again.");

         // Delisted: the zone keeps its test entries and forgets the address.
         SuiteDns.Reset();
         Answering(zone);

         report = Check();
         result = Result(report, Listed, zone);

         Assert.AreEqual("not_listed", (string) result["status"]);
         Assert.AreEqual(false, result["listed"]);
         Assert.AreEqual(0, Convert.ToInt64(result["listed_since"]), "A definite answer clears the listing's start.");

         Dictionary<string, object> cleared = NewestAlert();
         Assert.AreEqual("cleared", (string) cleared["state"], "A delisting clears the alert.");

         // Every configured pair gave a definite answer, so this is the one case in
         // which the clear may say that nothing is on any list.
         StringAssert.Contains("No outbound address of this server is on any of the block lists it checks",
            (string) cleared["summary"],
            "A clear with a definite answer from every list says so plainly.");

         // And the stored report says the same as the check did.
         Assert.AreEqual("not_listed", StatusOf(Report(), Listed, zone));
      }

      [Test]
      [Description("A list that refuses the query is reported as refused with its own error code, is not a listing, and is not clean either.")]
      public void ARefusalIsNeitherAListingNorClean()
      {
         const string zone = "refusing.repwatch.test";

         // 127.255.255.254 is what a list answers when the query reached it through
         // a public resolver. It is an error code, not a verdict.
         SuiteDns.Zone.WithA("2.0.0.127." + zone, "127.255.255.254");
         SuiteDns.Zone.WithTxt("2.0.0.127." + zone, "Error: open resolver");

         Watch(zone, Listed);

         Dictionary<string, object> result = Result(Check(), Listed, zone);

         Assert.AreEqual("refused", (string) result["status"]);
         Assert.IsNull(result["listed"], "A refusal says nothing about the address, so listed is null.");
         Assert.AreEqual("127.255.255.254", (string) result["code"]);
         StringAssert.Contains("resolver", (string) result["reason"]);

         Assert.IsNull(NewestAlert(), "A refusal must not raise a listing alert.");
      }

      [Test]
      [Description("A list that does not list its own RFC 5782 test entry is not answering this resolver, so its silence about an address is not reported as clean.")]
      public void AListThatDoesNotAnswerIsNotBelieved()
      {
         const string zone = "silent.repwatch.test";

         // Nothing published at all: the zone answers NODATA for everything, which
         // for a real list would read exactly like "not listed".
         Watch(zone, Listed);

         Dictionary<string, object> result = Result(Check(), Listed, zone);

         Assert.AreEqual("list_not_answering", (string) result["status"]);
         Assert.IsNull(result["listed"]);
         StringAssert.Contains("127.0.0.2", (string) result["reason"], "The reason names the test entry the list owes.");

         Assert.IsNull(NewestAlert(), "A list that is not answering must not raise a listing alert.");
      }

      [Test]
      [Description("A list that lists 127.0.0.1 is answering for every address, so a listing from it is not believed.")]
      public void AListThatListsEverythingIsNotBelieved()
      {
         const string zone = "everything.repwatch.test";

         Answering(zone);
         SuiteDns.Zone.WithA("1.0.0.127." + zone, "127.0.0.2");
         ListAddress(zone, "25.2.0.192", "127.0.0.2", null);

         Watch(zone, Listed);

         Dictionary<string, object> result = Result(Check(), Listed, zone);

         Assert.AreEqual("list_lists_everything", (string) result["status"]);
         Assert.IsNull(result["listed"]);
         Assert.IsNull(NewestAlert(), "A zone that answers for everything must not raise a listing alert.");
      }

      [Test]
      [Description("A check that cannot get an answer keeps the listing the last one found, and the alert with it.")]
      public void AnUnansweredCheckKeepsTheListingItFound()
      {
         const string zone = "flaky.repwatch.test";

         Answering(zone);
         ListAddress(zone, "25.2.0.192", "127.0.0.2", "Listed");

         Watch(zone, Listed);

         Assert.AreEqual("listed", StatusOf(Check(), Listed, zone));
         Assert.AreEqual(1, Count("reputation.listed", "raised"));

         long since = Convert.ToInt64(Result(Report(), Listed, zone)["listed_since"]);

         // The list stops answering this resolver altogether: its test entry is gone,
         // which is the shape of a zone that has been retired under the server's feet.
         SuiteDns.Reset();

         Dictionary<string, object> result = Result(Check(), Listed, zone);

         Assert.AreEqual("list_not_answering", (string) result["status"]);
         Assert.AreEqual(since, Convert.ToInt64(result["listed_since"]),
            "An answer nobody gave must not delist an address.");
         Assert.AreEqual(0, Count("reputation.listed", "cleared"),
            "A check that got no answer must not clear the alert.");
      }

      [Test]
      [Description("A list marked ipv4: is not asked about an IPv6 address, and the pair is reported as not covered rather than as clean.")]
      public void AnIpv4ListIsNotAskedAboutAnIpv6Address()
      {
         const string zone = "fourth.repwatch.test";

         Answering(zone);
         SuiteDns.Zone.ClearQueries();

         Watch("ipv4:" + zone, Sixth);

         Dictionary<string, object> result = Result(Check(), Sixth, zone);

         Assert.AreEqual("not_applicable", (string) result["status"]);
         Assert.IsNull(result["listed"]);

         // 2001:db8::25 reversed is a name of nibbles ending 8.b.d.0.1.0.0.2; no
         // query for it was made at all.
         foreach (string query in SuiteDns.Zone.Queries)
            StringAssert.DoesNotContain("8.b.d.0.1.0.0.2." + zone, query, "An IPv4-only list was asked about an IPv6 address.");
      }

      [Test]
      [Description("A private address is never looked up: it is not what another server sees, and the query would tell the list about this network.")]
      public void APrivateAddressIsNeverLookedUp()
      {
         const string zone = "private.repwatch.test";

         Answering(zone);
         SuiteDns.Zone.ClearQueries();

         Watch(zone, "10.1.2.3 " + Other);

         Dictionary<string, object> report = Check();

         Assert.IsNull(Result(report, "10.1.2.3", zone), "A private address must not be checked.");
         Assert.IsNotNull(Result(report, Other, zone), "The public address beside it is still checked.");

         bool named = false;
         foreach (Dictionary<string, object> skipped in MiniJson.List(report, "skipped").Cast<Dictionary<string, object>>())
         {
            if ((string) skipped["address"] != "10.1.2.3")
               continue;

            named = true;
            Assert.AreEqual("private", (string) skipped["reason"]);
            StringAssert.Contains("private", (string) skipped["detail"]);
         }

         Assert.IsTrue(named, "The report must say which addresses it will not look up, and why.");

         foreach (string query in SuiteDns.Zone.Queries)
            StringAssert.DoesNotContain("3.2.1.10." + zone, query, "A private address was looked up.");
      }

      [Test]
      [Description("The listing codes decide what counts as a listing: an answer outside them is recorded and is not a listing.")]
      public void ListingCodesDecideWhatCountsAsListed()
      {
         const string zone = "coded.repwatch.test";

         Answering(zone);
         ListAddress(zone, "25.2.0.192", "127.0.0.1", null);

         // A list whose 127.0.0.1 means something else entirely - a welcome list -
         // must not have that read as a block.
         Watch(zone + "=127.0.0.2|127.0.0.4-7", Listed);

         Dictionary<string, object> result = Result(Check(), Listed, zone);

         Assert.AreEqual("not_listed", (string) result["status"]);
         Assert.AreEqual("127.0.0.1", (string) result["code"], "What the list answered is recorded either way.");
         Assert.IsNull(NewestAlert(), "An answer outside the list's listing codes is not a listing.");
      }

      [Test]
      [Description("A list added since the last check has no answer yet, and the report names the pair as not_checked rather than leaving it out.")]
      public void APairNoCheckHasCoveredIsNamedRatherThanOmitted()
      {
         const string zone = "asked.repwatch.test";
         const string added = "unasked.repwatch.test";

         Answering(zone);

         Watch(zone, Listed);
         Assert.AreEqual("not_listed", StatusOf(Check(), Listed, zone));

         // The second zone is named AFTER the check, so nothing has ever asked it
         // about this address. Nothing publishes it either: the point is the pair
         // with no stored row, not what the zone would answer. The daily check is
         // not brought forward by the edit, so this is what a report says - and
         // what a monitoring job reads - for up to a day.
         Watch(zone + " " + added, Listed);

         Dictionary<string, object> report = Report();
         Dictionary<string, object> result = Result(report, Listed, added);

         Assert.IsNotNull(result, "A configured list with no stored answer must still be in the report.");
         Assert.AreEqual("not_checked", (string) result["status"]);
         Assert.IsNull(result["listed"], "Never asked is not the same as not listed.");
         Assert.AreEqual(0, Convert.ToInt64(result["checked"]), "Nothing has checked it, so there is no time to show.");

         // The count is what hmctl's exit code reads, and it is why that code
         // treats not_checked as neither listed nor clean.
         var counts = (Dictionary<string, object>) report["counts"];
         Assert.AreEqual(1, Convert.ToInt64(counts["not_checked"]), "The report counts the pair nothing has covered.");

         // And the address's own verdict is null rather than false: one of its
         // lists has never been asked.
         Dictionary<string, object> entry = null;
         foreach (Dictionary<string, object> candidate in MiniJson.List(report, "addresses").Cast<Dictionary<string, object>>())
         {
            if (string.Equals((string) candidate["address"], Listed, StringComparison.OrdinalIgnoreCase))
               entry = candidate;
         }

         Assert.IsNotNull(entry);
         Assert.IsNull(entry["listed"], "An address with a list nobody has asked is not clean.");
      }

      [Test]
      [Description("The alert's clear says why nothing is listed: when a list could not be asked, it does not claim that nothing is on any list.")]
      public void TheClearDoesNotClaimAnAllClearItHasNoAnswersFor()
      {
         const string answering = "clearing.repwatch.test";
         const string mute = "mute.repwatch.test";

         Answering(answering);
         ListAddress(answering, "25.2.0.192", "127.0.0.2", "Listed");

         Watch(answering, Listed);

         Assert.AreEqual("listed", StatusOf(Check(), Listed, answering));
         Assert.AreEqual("raised", (string) NewestAlert()["state"]);

         // Delisted by the list that answers, and a second list named that answers
         // nothing at all. Nothing is listed - but "nothing is on any list" is not
         // what is known, because one of the two lists never said.
         SuiteDns.Reset();
         Answering(answering);

         Watch(answering + " " + mute, Listed);

         Dictionary<string, object> report = Check();
         Assert.AreEqual("not_listed", StatusOf(report, Listed, answering));
         Assert.AreEqual("list_not_answering", StatusOf(report, Listed, mute));

         Dictionary<string, object> cleared = NewestAlert();
         Assert.AreEqual("cleared", (string) cleared["state"], "Nothing is listed, so the alert clears.");

         string summary = (string) cleared["summary"];

         StringAssert.DoesNotContain("on any of the block lists it checks", summary,
            "The clear must not claim an all-clear while a list has answered nothing.");
         StringAssert.Contains("came back with no answer", summary,
            "The clear says how many checks did not come back.");
         StringAssert.Contains("not an all-clear", summary,
            "And says in so many words that this is not one.");
      }

      [Test]
      [Description("Switching the watch off ends the condition rather than leaving it raised for ever, and the clear says that is what happened.")]
      public void SwitchingTheWatchOffEndsTheConditionAndSaysSo()
      {
         const string zone = "switchoff.repwatch.test";

         Answering(zone);
         ListAddress(zone, "25.2.0.192", "127.0.0.2", "Listed");

         Watch(zone, Listed);

         Assert.AreEqual("listed", StatusOf(Check(), Listed, zone));
         Assert.AreEqual("raised", (string) NewestAlert()["state"]);

         // The administrator's first reaction. Every path that could clear the
         // condition is behind the enabled check, so without this it would stay
         // raised until the watch was switched back on and a check came back clean.
         (int status, string body) = Http("PUT", "/api/v1/settings", "{\"reputation_watch_enabled\":false}");
         Assert.AreEqual(200, status, body);

         (int runStatus, string runBody) = Http("POST", "/api/v1/alerts/run", "{}");
         Assert.AreEqual(200, runStatus, runBody);

         Dictionary<string, object> cleared = NewestAlert();
         Assert.AreEqual("cleared", (string) cleared["state"], "A watch that is off leaves nothing raised.");

         string summary = (string) cleared["summary"];

         StringAssert.Contains("switched off", summary, "The clear says why it cleared.");
         StringAssert.DoesNotContain("on any of the block lists it checks", summary,
            "Switching the watch off is not an all-clear.");
      }

      [Test]
      [Description("A list or an address the watch could not use is refused where it is written, in the words the server uses everywhere else.")]
      public void TheSettingsRefuseWhatTheWatchCannotUse()
      {
         (int status, string body) lists = Http("PUT", "/api/v1/settings", "{\"reputation_watch_lists\":\"not a zone!\"}");
         Assert.AreEqual(400, lists.status, lists.body);
         StringAssert.Contains("bl.spamcop.net", lists.body, "The refusal shows what a list looks like.");

         (int status, string body) codes = Http("PUT", "/api/v1/settings", "{\"reputation_watch_lists\":\"bl.example.test=127.0.0\"}");
         Assert.AreEqual(400, codes.status, codes.body);

         (int status, string body) addresses = Http("PUT", "/api/v1/settings", "{\"reputation_watch_addresses\":\"mail.example.com\"}");
         Assert.AreEqual(400, addresses.status, addresses.body);
         StringAssert.Contains("IP address", addresses.body);

         (int status, string body) good = Http("PUT", "/api/v1/settings",
            "{\"reputation_watch_lists\":\"ipv6:bl.example.test=127.0.0.2|127.0.0.4-7\",\"reputation_watch_addresses\":\"" + Listed + "\"}");
         Assert.AreEqual(200, good.status, good.body);

         Dictionary<string, object> settings = MiniJson.Object(Read("/api/v1/settings"));
         Assert.AreEqual("ipv6:bl.example.test=127.0.0.2|127.0.0.4-7", (string) settings["reputation_watch_lists"]);
      }

      [Test]
      [Description("The report and the check are the server's own: a read-only key is refused the check, and a key issued for one domain is refused both.")]
      public void TheCheckIsTheServersOwnRatherThanADomains()
      {
         (string id, string key) readOnly = CreateKey("repwatch-read", "read", null);
         (string id, string key) confined = CreateKey("repwatch-domain", "readwrite", _domain.Name);

         try
         {
            Assert.AreEqual(200, Bearer("GET", ReputationPath, readOnly.key).status,
               "A read-only key may read what the last check found.");

            Assert.AreEqual(403, Bearer("POST", CheckPath, readOnly.key, "{}").status,
               "A read-only key may not make this server send queries.");

            Assert.AreEqual(403, Bearer("GET", ReputationPath, confined.key).status,
               "The addresses this server sends from are not one domain's business.");

            Assert.AreEqual(403, Bearer("POST", CheckPath, confined.key, "{}").status);
         }
         finally
         {
            Http("DELETE", "/api/v1/apikeys/" + readOnly.id);
            Http("DELETE", "/api/v1/apikeys/" + confined.id);
         }
      }

      // ------------------------------------------------------------------ plumbing

      private static int Count(string condition, string state)
      {
         (int status, string body) = Http("GET", "/api/v1/alerts/events?limit=50");
         Assert.AreEqual(200, status, body);

         int count = 0;

         foreach (Dictionary<string, object> entry in MiniJson.List(MiniJson.Object(body), "events").Cast<Dictionary<string, object>>())
         {
            if ((string) entry["condition"] == condition && (string) entry["state"] == state &&
                Convert.ToInt64(entry["id"]) > baseline_)
            {
               count++;
            }
         }

         return count;
      }

      private static (string id, string key) CreateKey(string name, string scope, string domains)
      {
         string body = "{\"name\":\"" + name + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";

         (int status, string created) = Http("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);

         return (Extract(created, "id"), Extract(created, "key"));
      }

      private static string Extract(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      private static (int status, string body) Http(string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return Http(method, path, "Basic " + credentials, requestBody);
      }

      private static (int status, string body) Bearer(string method, string path, string token, string requestBody = null)
      {
         return Http(method, path, "Bearer " + token, requestBody);
      }

      private static (int status, string body) Http(string method, string path, string authorization, string requestBody)
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
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());

               int statusCode = 0;
               string[] lines = raw.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out statusCode);
               }

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, body);
            }
         }
      }
   }
}
