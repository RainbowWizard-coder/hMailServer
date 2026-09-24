// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Stress
{
   /// <summary>
   ///    Two collections that other threads change in place, raced from outside for thirty
   ///    seconds each (ARCHITECTURE.md, "A collection another thread can change is iterated
   ///    as a locked copy"): ObjectCache's domain and account rules, refreshed in place after
   ///    every save while the webmail's send route and delivery walk them, and the SMTP
   ///    configuration's routes, added to, refreshed and erased from while the send route
   ///    resolves an address against every one of them. A walk over the live vector while
   ///    another thread swaps or shifts it reads freed memory, which the server may well
   ///    survive - /EHa lets catch (...) swallow the access violation - so what is asserted
   ///    is that the process is the one that started, the crash oracle recorded nothing, the
   ///    error log is empty, and every message the rules race submitted was delivered.
   ///
   ///    Both are survival smoke tests of the fixed server, not seen to catch the old one.
   ///    Against a build with RuleApplier's HasRejectAction and WouldReject put back to
   ///    GetVector() and ApplyRules to its index walk, the rules race passed three times in
   ///    three on 24 September 2026 (forty edit rounds and some 360 messages each); the
   ///    routes race has not been run against the old code. A race proves little in one run
   ///    either way: they are in the Windows suite so that every run of it takes another
   ///    thirty seconds' chance at each.
   ///
   ///    Through REST and COM only. Nothing here speaks SMTP or IMAP from this machine,
   ///    where a proxying virus scanner has been known to sit on the loopback ports: the
   ///    messages go in through the webmail's send route and are counted over COM.
   /// </summary>
   [TestFixture]
   public class SharedCollections : TestFixtureBase
   {
      private static int RestPort = 10120;
      private const string AdminPassword = "testar";
      private const string Password = "Shared-Collections-Passw0rd!";

      private static readonly TimeSpan RaceDuration = TimeSpan.FromSeconds(30);
      private static readonly TimeSpan DeliveryCeiling = TimeSpan.FromMinutes(4);

      // The REST listener allows one credential 200 requests in ten seconds. Every loop
      // below is paced well under that, and a 429 all the same is waited out and counted,
      // never taken for a fault: it is the listener doing its job.
      private static readonly TimeSpan EditorSpacing = TimeSpan.FromMilliseconds(150);
      private static readonly TimeSpan SenderSpacing = TimeSpan.FromMilliseconds(250);

      private const int SenderCount = 3;

      // Standing rules on the domain, so that the walks the send route makes over the
      // cached domain rules are long: fifty-nine that never match and do not reject,
      // then one that never matches and rejects. RuleApplier::HasRejectAction walks all
      // sixty before it finds a reject action, and WouldReject walks all sixty.
      private const int StandingRuleCount = 60;

      // Standing routes, and one more for the domain the race writes to, last by name
      // so that RecipientParser's GetItemByNameWithWildcardMatch walks every other route
      // before it finds it.
      private const int StandingRouteCount = 40;
      private const string RouteDomainSuffix = ".shared-collections.test";
      private const string TargetDomain = "zz-target" + RouteDomainSuffix;

      private string compromiseModeBefore_;

      private class Race
      {
         private readonly List<string> failures_ = new List<string>();
         public int Edits;
         public int Refused;
         public int Throttled;
         public readonly int[] Sent = new int[SenderCount];

         public void Fail(string failure)
         {
            lock (failures_)
               failures_.Add(failure);
         }

         public string[] Failures
         {
            get
            {
               lock (failures_)
                  return failures_.ToArray();
            }
         }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // Three accounts sending a hundred messages each in half a minute is the shape
         // of an account takeover, and watch mode would raise the alert for it. The
         // detector is not what is under test, so it is off for the race and put back
         // to whatever it was.
         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, settings.status, "GET /api/v1/settings: " + settings.body);
         compromiseModeBefore_ = MiniJson.Text(MiniJson.Object(settings.body), "compromise_detection_mode");
         Assert.IsNotNull(compromiseModeBefore_, "The settings carry no compromise_detection_mode: " + settings.body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"off\"}").status);
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            if (compromiseModeBefore_ != null)
            {
               (int status, string body) restored = Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"" + compromiseModeBefore_ + "\"}");
               if (restored.status != 200)
                  Console.WriteLine("compromise_detection_mode could not be put back to " + compromiseModeBefore_ + ": " + restored.status + " " + restored.body);
            }

            // PerformBasicSetup removes them before the next test; this is for the
            // run whose last test this was.
            SingletonProvider<TestSetup>.Instance.RemoveAllRoutes();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Console.WriteLine("Could not tidy up after the race: " + ex.Message);
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A domain's and an account's rules are created and deleted over REST for thirty seconds while three accounts submit through the send route - which walks the domain's cached rules, refreshed in place after every edit, for a reject action - and delivery walks the domain's and the account's; every message is delivered, the process is the one that started and the crash oracle is clean.")]
      public void RulesEditedWhileMessagesAreSubmittedAndDeliveredLoseNothing()
      {
         var senders = CreateSenders("shared-rules-");

         string domainRules = "/api/v1/domains/" + Uri.EscapeDataString(_domain.Name) + "/rules";
         string accountRules = "/api/v1/accounts/" + Uri.EscapeDataString(senders[0].Address) + "/rules";

         var setupPacer = new Pacer(TimeSpan.FromMilliseconds(60));
         for (int i = 0; i < StandingRuleCount; i++)
         {
            setupPacer.Wait();
            string action = i == StandingRuleCount - 1 ? "reject" : "delete";
            (int status, string body) created = AdminPatiently("POST", domainRules, NeverMatchingRule("standing-" + i, action), null);
            Assert.AreEqual(201, created.status, "A standing domain rule was not created: " + created.body);
         }

         var race = new Race();
         DateTime deadline = DateTime.UtcNow + RaceDuration;
         var threads = new List<Thread>();

         threads.Add(new Thread(() => ChurnRules(domainRules, accountRules, deadline, race)));
         for (int i = 0; i < SenderCount; i++)
         {
            int index = i;
            string address = senders[i].Address;
            threads.Add(new Thread(() => SubmitToSelf(index, address, deadline, race)));
         }

         RunToTheEnd(threads);

         int total = 0;
         foreach (int sent in race.Sent)
            total += sent;

         TestContext.WriteLine("Rules race: {0} rule edits (a domain rule and an account rule created and deleted each), {1} messages submitted, {2} requests throttled, over {3} with {4} standing domain rules.",
            race.Edits, total, race.Throttled, RaceDuration, StandingRuleCount);

         AssertTheServerCameThrough(race);

         Assert.Greater(race.Edits, 0, "The rules were never edited, so nothing was raced.");
         Assert.Greater(total, 0, "Nothing was submitted, so nothing was raced.");

         // Every message delivered: the rules the race added never match, so each one
         // reaches the inbox of the account that sent it to itself.
         Stopwatch waited = Stopwatch.StartNew();
         var delivered = new int[SenderCount];
         while (true)
         {
            bool all = true;
            for (int i = 0; i < SenderCount; i++)
            {
               delivered[i] = InboxCount(senders[i]);
               if (delivered[i] != race.Sent[i])
                  all = false;
            }

            if (all)
               break;

            if (waited.Elapsed > DeliveryCeiling)
            {
               Assert.Fail(string.Format("Not every message was delivered within {0}: sent {1}, in the inboxes {2}, {3} still queued.",
                  DeliveryCeiling, string.Join("/", race.Sent), string.Join("/", delivered), TestSetup.GetNumberOfMessagesInDeliveryQueue()));
            }

            Thread.Sleep(500);
         }

         TestContext.WriteLine("Every message delivered in {0:0.0} s after the race ended.", waited.Elapsed.TotalSeconds);

         AssertTheServerCameThrough(race);
      }

      [Test]
      [Description("A route is created, changed and deleted over REST for thirty seconds - an append, a refresh that swaps the whole list, and an erase from the front of it - while three accounts send through the send route to an unhosted domain whose route sits after forty others, so every resolution walks every route; each is refused as not in the route list, nothing is queued, the process is the one that started and the crash oracle is clean.")]
      public void RoutesChangedWhileTheSendRouteWalksThemCrashNothing()
      {
         var senders = CreateSenders("shared-routes-");

         var setupPacer = new Pacer(TimeSpan.FromMilliseconds(60));
         for (int i = 0; i < StandingRouteCount; i++)
         {
            setupPacer.Wait();
            (int status, string body) created = AdminPatiently("POST", "/api/v1/routes", RouteBody(string.Format("standing-{0:00}{1}", i, RouteDomainSuffix), 2525, ""), null);
            Assert.AreEqual(201, created.status, "A standing route was not created: " + created.body);
         }

         // The domain the senders write to: a route that takes only the one address it
         // lists, so an address it does not list is refused at resolution and nothing is
         // queued or delivered anywhere.
         (int status, string body) target = AdminPatiently("POST", "/api/v1/routes",
            RouteBody(TargetDomain, 2525, ",\"all_addresses\":false,\"addresses\":[\"listed@" + TargetDomain + "\"]"), null);
         Assert.AreEqual(201, target.status, "The target route was not created: " + target.body);

         // Before the race, so that a refusal inside it is the race's.
         (int status, string body) probe = Send("POST", "/api/v1/me/messages", Basic(senders[0].Address, Password), UnlistedMessage(0, 0));
         Assert.AreEqual(400, probe.status, "The send route should refuse an address the route does not list: " + probe.body);
         StringAssert.Contains("not in route list", probe.body);

         var race = new Race();
         DateTime deadline = DateTime.UtcNow + RaceDuration;
         var threads = new List<Thread>();

         threads.Add(new Thread(() => ChurnRoute(deadline, race)));
         for (int i = 0; i < SenderCount; i++)
         {
            int index = i;
            string address = senders[i].Address;
            threads.Add(new Thread(() => ResolveUnlisted(index, address, deadline, race)));
         }

         RunToTheEnd(threads);

         TestContext.WriteLine("Routes race: {0} route create-change-delete rounds, {1} resolutions refused as they should be, {2} requests throttled, over {3} with {4} standing routes.",
            race.Edits, race.Refused, race.Throttled, RaceDuration, StandingRouteCount);

         AssertTheServerCameThrough(race);

         Assert.Greater(race.Edits, 0, "The routes were never changed, so nothing was raced.");
         Assert.Greater(race.Refused, 0, "Nothing was resolved, so nothing was raced.");
         Assert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "The race queued nothing.");
      }

      private List<Account> CreateSenders(string prefix)
      {
         var senders = new List<Account>();
         for (int i = 0; i < SenderCount; i++)
            senders.Add(SingletonProvider<TestSetup>.Instance.AddAccount(_domain, prefix + i + "@" + _domain.Name, Password));
         return senders;
      }

      private static void RunToTheEnd(List<Thread> threads)
      {
         foreach (Thread thread in threads)
            thread.Start();
         foreach (Thread thread in threads)
            thread.Join();
      }

      private void AssertTheServerCameThrough(Race race)
      {
         // The same process: a crash the service manager restarted from is a crash.
         ServiceRestartDetector.ValidateProcessId();

         string[] faults = CrashOracleAsserts.GetMemorySafetyEvents();
         Assert.IsEmpty(faults, faults.Length == 0 ? "" : CrashOracleAsserts.DescribeMemorySafetyEvents(faults));

         Assert.IsEmpty(race.Failures, string.Join(Environment.NewLine, race.Failures));

         (int status, string body) alive = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, alive.status, "The REST API stopped answering: " + alive.body);

         CustomAsserts.AssertNoReportedError();
      }

      private static void ChurnRules(string domainRules, string accountRules, DateTime deadline, Race race)
      {
         var pacer = new Pacer(EditorSpacing);
         try
         {
            while (DateTime.UtcNow < deadline)
            {
               // A domain rule and an account rule, each saved and deleted: four reloads
               // flagged, and each one a Refresh() of the cached collection in place, on
               // whichever thread asks for it next. The domain's is walked by the send
               // route (HasRejectAction, WouldReject) and by delivery (ApplyRules); the
               // account's by delivery alone.
               if (!CreateAndDelete(domainRules, NeverMatchingRule("churn", "reject"), pacer, race))
                  return;
               if (!CreateAndDelete(accountRules, NeverMatchingRule("churn", "delete"), pacer, race))
                  return;

               Interlocked.Increment(ref race.Edits);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The rule editor stopped: " + ex.Message);
         }
      }

      private static bool CreateAndDelete(string collection, string body, Pacer pacer, Race race)
      {
         pacer.Wait();
         (int status, string body) created = AdminPatiently("POST", collection, body, race);
         if (created.status != 201)
         {
            race.Fail("POST " + collection + " answered " + created.status + ": " + created.body);
            return false;
         }

         long id = long.Parse(Regex.Match(created.body, "\"id\":(\\d+)").Groups[1].Value);

         pacer.Wait();
         (int status, string body) deleted = AdminPatiently("DELETE", collection + "/" + id, null, race);
         if (deleted.status != 200)
         {
            race.Fail("DELETE " + collection + "/" + id + " answered " + deleted.status + ": " + deleted.body);
            return false;
         }

         return true;
      }

      private static void SubmitToSelf(int index, string address, DateTime deadline, Race race)
      {
         var pacer = new Pacer(SenderSpacing);
         string me = Basic(address, Password);
         int sent = 0;

         try
         {
            while (DateTime.UtcNow < deadline)
            {
               pacer.Wait();
               (int status, string body) result = Send("POST", "/api/v1/me/messages", me,
                  "{\"to\":\"" + address + "\",\"subject\":\"Shared collections " + index + "." + sent + "\",\"text\":\"One of the race's messages.\"}");

               if (result.status == 429)
               {
                  Interlocked.Increment(ref race.Throttled);
                  Thread.Sleep(1000);
                  continue;
               }

               if (result.status != 201 || !result.body.Contains("\"held\":false"))
               {
                  race.Fail("The send route answered " + address + " with " + result.status + ": " + result.body);
                  break;
               }

               sent++;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("Sender " + address + " stopped: " + ex.Message);
         }
         finally
         {
            race.Sent[index] = sent;
         }
      }

      private static void ChurnRoute(DateTime deadline, Race race)
      {
         var pacer = new Pacer(EditorSpacing);
         string churn = "churn" + RouteDomainSuffix;
         try
         {
            while (DateTime.UtcNow < deadline)
            {
               // POST appends the route to the shared collection; PUT saves a changed
               // copy and refreshes the collection, which swaps in a new vector sorted by
               // name - the churn route first; DELETE then erases it from the front, so
               // every route after it moves down a slot.
               pacer.Wait();
               (int status, string body) created = AdminPatiently("POST", "/api/v1/routes", RouteBody(churn, 2525, ""), race);
               if (created.status != 201)
               {
                  race.Fail("POST /api/v1/routes answered " + created.status + ": " + created.body);
                  return;
               }

               long id = long.Parse(Regex.Match(created.body, "\"id\":(\\d+)").Groups[1].Value);

               pacer.Wait();
               (int status, string body) changed = AdminPatiently("PUT", "/api/v1/routes/" + id, RouteBody(churn, 2526, ""), race);
               if (changed.status != 200)
               {
                  race.Fail("PUT /api/v1/routes/" + id + " answered " + changed.status + ": " + changed.body);
                  return;
               }

               pacer.Wait();
               (int status, string body) deleted = AdminPatiently("DELETE", "/api/v1/routes/" + id, null, race);
               if (deleted.status != 200)
               {
                  race.Fail("DELETE /api/v1/routes/" + id + " answered " + deleted.status + ": " + deleted.body);
                  return;
               }

               Interlocked.Increment(ref race.Edits);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The route editor stopped: " + ex.Message);
         }
      }

      private static void ResolveUnlisted(int index, string address, DateTime deadline, Race race)
      {
         var pacer = new Pacer(SenderSpacing);
         string me = Basic(address, Password);
         int attempt = 1;

         try
         {
            while (DateTime.UtcNow < deadline)
            {
               pacer.Wait();
               (int status, string body) result = Send("POST", "/api/v1/me/messages", me, UnlistedMessage(index, attempt++));

               if (result.status == 429)
               {
                  Interlocked.Increment(ref race.Throttled);
                  Thread.Sleep(1000);
                  continue;
               }

               if (result.status != 400 || !result.body.Contains("not in route list"))
               {
                  race.Fail("The send route answered " + address + " with " + result.status + ", not the refusal: " + result.body);
                  break;
               }

               Interlocked.Increment(ref race.Refused);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("Sender " + address + " stopped: " + ex.Message);
         }
      }

      private static string UnlistedMessage(int index, int attempt)
      {
         return "{\"to\":\"unlisted@" + TargetDomain + "\",\"subject\":\"Shared collections route " + index + "." + attempt + "\",\"text\":\"Never queued.\"}";
      }

      private static int InboxCount(Account account)
      {
         try
         {
            return account.IMAPFolders.get_ItemByName("Inbox").Messages.Count;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // No inbox until the first delivery made one.
            return 0;
         }
      }

      private static string NeverMatchingRule(string name, string action)
      {
         // A subject no message of the race carries, so the rule is walked and never fires.
         return "{\"name\":\"" + name + "\",\"active\":true,\"all_criteria\":true," +
                "\"criteria\":[{\"field\":\"subject\",\"match\":\"contains\",\"value\":\"no message has this subject 5f1d\"}]," +
                "\"actions\":[" + (action == "reject" ? "{\"type\":\"reject\",\"value\":\"Refused by a rule nothing matches.\"}" : "{\"type\":\"" + action + "\"}") + "]}";
      }

      private static string RouteBody(string domainName, int port, string extra)
      {
         // This machine: nothing the race does is delivered through a route, and a
         // route that pointed anywhere else would be a bad thing to leave behind.
         return "{\"domain_name\":\"" + domainName + "\",\"target_smtp_host\":\"127.0.0.1\",\"target_smtp_port\":" + port + extra + "}";
      }

      // A 429 waited out, a few times, for the requests whose answer the race needs.
      private static (int status, string body) AdminPatiently(string method, string path, string requestBody, Race race)
      {
         (int status, string body) result = Admin(method, path, requestBody);
         for (int attempt = 0; attempt < 12 && result.status == 429; attempt++)
         {
            if (race != null)
               Interlocked.Increment(ref race.Throttled);
            Thread.Sleep(1000);
            result = Admin(method, path, requestBody);
         }

         return result;
      }

      private sealed class Pacer
      {
         private readonly TimeSpan spacing_;
         private readonly Stopwatch clock_ = Stopwatch.StartNew();
         private TimeSpan next_ = TimeSpan.Zero;

         public Pacer(TimeSpan spacing)
         {
            spacing_ = spacing;
         }

         public void Wait()
         {
            TimeSpan now = clock_.Elapsed;
            if (now < next_)
               Thread.Sleep(next_ - now);
            next_ = (now > next_ ? now : next_) + spacing_;
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static (int status, string body) Send(string method, string path, string authorization, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, authorization, requestBody);
      }
   }
}
