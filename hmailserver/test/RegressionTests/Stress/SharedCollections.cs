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
   ///    The collections other threads change, raced from outside for thirty seconds each
   ///    (ARCHITECTURE.md, "A collection another thread can change is iterated as a locked
   ///    copy"). Collection::GetVector() hands out the live vector without the collection's
   ///    lock, and a loop over it while another thread refreshes the collection reads freed
   ///    memory - which the server may well survive, because /EHa lets catch (...) swallow
   ///    the access violation. So what is asserted is that the process is the one that
   ///    started, the crash oracle recorded nothing, the error log is empty, and every
   ///    message the race submitted was delivered.
   ///
   ///    Through REST and COM only. Nothing here speaks SMTP or IMAP from this machine,
   ///    where a proxying virus scanner has been known to sit on the loopback ports: the
   ///    messages go in through the webmail's send route and are counted over COM.
   ///
   ///    A race proves little in one green run; these are here so that every run of the
   ///    suite takes another thirty seconds' chance at each.
   /// </summary>
   [TestFixture]
   public class SharedCollections : TestFixtureBase
   {
      private static int RestPort = 10120;
      private const int ChurnPortNumber = 10125;
      private const string AdminPassword = "testar";
      private const string Password = "Shared-Collections-Passw0rd!";

      private static readonly TimeSpan RaceDuration = TimeSpan.FromSeconds(30);
      private static readonly TimeSpan DeliveryCeiling = TimeSpan.FromMinutes(4);

      // The REST listener allows one credential 200 requests in ten seconds. Every loop
      // below is paced well under that, and a 429 all the same is waited out and counted,
      // never taken for a fault: it is the listener doing its job.
      private static readonly TimeSpan EditorSpacing = TimeSpan.FromMilliseconds(150);
      private static readonly TimeSpan SenderSpacing = TimeSpan.FromMilliseconds(250);
      private static readonly TimeSpan ReaderSpacing = TimeSpan.FromMilliseconds(330);

      private const int SenderCount = 3;

      // Standing rules on the domain, so that the walk the send route makes over the
      // cached domain rules - RuleApplier::HasRejectAction and WouldReject - is long
      // enough for a refresh on another thread to land inside it.
      private const int StandingRuleCount = 60;

      private class Race
      {
         private readonly List<string> failures_ = new List<string>();
         public int Edits;
         public int Reads;
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
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // The port the churn makes, should the race have ended between its create and
            // its delete. PerformBasicSetup puts the default ports back anyway; this keeps a
            // listener on 10125 from starting if the service is restarted before then.
            DeleteChurnPorts();
            Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"watch\"}");
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
      [Description("A domain's and an account's rules are created and deleted over REST for thirty seconds while three accounts submit through the send route - which walks the domain's cached rules, refreshed in place after every edit, for a reject action - and every message is delivered, the process is the one that started and the crash oracle is clean.")]
      public void RulesEditedWhileMessagesAreSubmittedAndDeliveredLoseNothing()
      {
         // Three accounts sending a hundred messages each in half a minute is the shape of
         // an account takeover, and watch mode would raise the alert for it. The detector
         // is not what is under test.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"off\"}").status);

         var senders = new List<Account>();
         for (int i = 0; i < SenderCount; i++)
            senders.Add(SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "shared-rules-" + i + "@" + _domain.Name, Password));

         string domainRules = "/api/v1/domains/" + Uri.EscapeDataString(_domain.Name) + "/rules";
         string accountRules = "/api/v1/accounts/" + Uri.EscapeDataString(senders[0].Address) + "/rules";

         var setupPacer = new Pacer(TimeSpan.FromMilliseconds(60));
         for (int i = 0; i < StandingRuleCount; i++)
         {
            setupPacer.Wait();
            (int status, string body) created = AdminPatiently("POST", domainRules, NeverMatchingRule("standing-" + i, "reject"), null);
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

         foreach (Thread thread in threads)
            thread.Start();
         foreach (Thread thread in threads)
            thread.Join();

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
      [Description("A TCP/IP port is created, changed and deleted over REST for thirty seconds while the routes that list the ports - the SRV records, the port listing and the setup checklist - are asked in a loop, and the process is the one that started and the crash oracle is clean.")]
      public void PortsChangedWhileTheyAreListedCrashNothing()
      {
         // Each route answers before the race, so that a failure inside it is the race's.
         foreach (string path in ListingPaths)
         {
            (int status, string body) probe = Admin("GET", path, null);
            Assert.AreEqual(200, probe.status, path + " did not answer before the race: " + probe.body);
         }

         var race = new Race();
         DateTime deadline = DateTime.UtcNow + RaceDuration;
         var threads = new List<Thread>();

         threads.Add(new Thread(() => ChurnPort(deadline, race)));
         foreach (string path in ListingPaths)
         {
            string listed = path;
            threads.Add(new Thread(() => ReadInALoop(listed, deadline, race)));
         }

         foreach (Thread thread in threads)
            thread.Start();
         foreach (Thread thread in threads)
            thread.Join();

         TestContext.WriteLine("Ports race: {0} port create-change-delete rounds, {1} listings read, {2} requests throttled, over {3}.",
            race.Edits, race.Reads, race.Throttled, RaceDuration);

         AssertTheServerCameThrough(race);

         Assert.Greater(race.Edits, 0, "The port was never changed, so nothing was raced.");
         Assert.Greater(race.Reads, 0, "Nothing was listed, so nothing was raced.");

         StringAssert.DoesNotContain("\"port\":" + ChurnPortNumber + ",", Admin("GET", "/api/v1/ports", null).body,
            "Every port the race created was deleted again.");
      }

      private static readonly string[] ListingPaths =
      {
         "/api/v1/srv",
         "/api/v1/ports",
         "/api/v1/setup/checklist?dns=0",
      };

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
               // whichever thread asks for it next.
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

      private static void ChurnPort(DateTime deadline, Race race)
      {
         var pacer = new Pacer(EditorSpacing);
         try
         {
            while (DateTime.UtcNow < deadline)
            {
               pacer.Wait();
               (int status, string body) created = AdminPatiently("POST", "/api/v1/ports", PortBody("0.0.0.0"), race);
               if (created.status != 201)
               {
                  race.Fail("POST /api/v1/ports answered " + created.status + ": " + created.body);
                  return;
               }

               long id = long.Parse(Regex.Match(created.body, "\"id\":(\\d+)").Groups[1].Value);

               pacer.Wait();
               (int status, string body) changed = AdminPatiently("PUT", "/api/v1/ports/" + id, PortBody("127.0.0.1"), race);
               if (changed.status != 200)
               {
                  race.Fail("PUT /api/v1/ports/" + id + " answered " + changed.status + ": " + changed.body);
                  return;
               }

               pacer.Wait();
               (int status, string body) deleted = AdminPatiently("DELETE", "/api/v1/ports/" + id, null, race);
               if (deleted.status != 200)
               {
                  race.Fail("DELETE /api/v1/ports/" + id + " answered " + deleted.status + ": " + deleted.body);
                  return;
               }

               Interlocked.Increment(ref race.Edits);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The port editor stopped: " + ex.Message);
         }
      }

      private static void ReadInALoop(string path, DateTime deadline, Race race)
      {
         var pacer = new Pacer(ReaderSpacing);
         try
         {
            while (DateTime.UtcNow < deadline)
            {
               pacer.Wait();
               (int status, string body) read = Admin("GET", path, null);

               if (read.status == 429)
               {
                  Interlocked.Increment(ref race.Throttled);
                  Thread.Sleep(1000);
                  continue;
               }

               if (read.status != 200)
               {
                  race.Fail("GET " + path + " answered " + read.status + ": " + read.body);
                  return;
               }

               Interlocked.Increment(ref race.Reads);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The reader of " + path + " stopped: " + ex.Message);
         }
      }

      private void DeleteChurnPorts()
      {
         (int status, string body) listed = Admin("GET", "/api/v1/ports", null);
         if (listed.status != 200)
            return;

         foreach (Match port in Regex.Matches(listed.body, "\\{\"id\":(\\d+),[^{}]*\"port\":" + ChurnPortNumber + ","))
            Admin("DELETE", "/api/v1/ports/" + port.Groups[1].Value, null);
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

      private static string PortBody(string address)
      {
         return "{\"protocol\":\"smtp\",\"address\":\"" + address + "\",\"port\":" + ChurnPortNumber + ",\"connection_security\":\"none\"}";
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
