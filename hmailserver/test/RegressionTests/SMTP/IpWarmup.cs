// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    The IP warm-up (docs/IpWarmup.md): a schedule for an outbound address holds
   ///    it to a daily ceiling of recipients per large provider, enforced by the
   ///    queue - each part with its negative control, against the suite's SMTP
   ///    simulator through routes.
   ///
   ///    The provider of a delivery is decided from the mail hosts it connects to,
   ///    never from the recipient's domain, so the "provider" here is a simulator on
   ///    127.0.0.1 reached through a route whose target host is
   ///    alt1.aspmx.l.google.com - one of Google's MX host names, which the suite's
   ///    zone (SuiteDns) answers with 127.0.0.1 for the length of the fixture. The
   ///    other provider is a second route whose host, mx.warmup-other.test, the
   ///    zone answers the same way. SMTPDeliveryBindToIP is 127.0.0.1, so every
   ///    delivery leaves from the address the schedule is for.
   ///
   ///    The listener is the fixture's own, on the first port of the range this row
   ///    was allocated (9926-9935). The plans' arithmetic on sample days - gentle
   ///    and fast, the day a schedule is on, the providers table - is held by
   ///    WarmupPlanTester in the internal suite, which MainOperations.RunTestSuite
   ///    runs; the day arithmetic is also proved here through the route.
   ///
   ///    Not proved here, and said so in docs/IpWarmup.md: room held by
   ///    deliveries in flight (it needs two deliveries racing), a host of the
   ///    other family skipped for reaching from another warming address (it needs
   ///    IPv6 to a simulator), what a database failure does (7210 to 7213), the
   ///    audit trail's entry, the configuration backup's round trip, and an MX
   ///    lookup's own host names reaching the classification (every delivery here
   ///    goes through a route).
   /// </summary>
   [TestFixture]
   public class IpWarmup : TestFixtureBase
   {
      private static int RestPort = 9926;
      private const string AdminPassword = "testar";

      private const string BindAddress = "127.0.0.1";

      // The password of the accounts the permissions test gives roles to.
      private const string RolePassword = "Warmup-secret-1234";

      private const string GoogleDomain = "warmup-google.test";
      private const string GoogleHost = "alt1.aspmx.l.google.com";
      private const string OtherDomain = "warmup-other.test";
      private const string OtherHost = "mx.warmup-other.test";

      private Account _account;

      [SetUp]
      public void StartRestApiAndClearSchedules()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "warmup-sender@example.test", "test");

         SuiteDns.Zone.WithA(GoogleHost, "127.0.0.1").WithA(OtherHost, "127.0.0.1");

         ClearSchedules_();

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         AwayFromMidnight_();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // A schedule left behind would hold the next fixture's deliveries
            // from 127.0.0.1 or ::1, so the schedules go first, and each step
            // runs whatever the one before it did.
            ClearSchedules_();
         }
         finally
         {
            try
            {
               SuiteDns.Reset();

               // A message this fixture left in the queue - the one held until
               // tomorrow, above all - would be attempted again by the next
               // test's server with the route gone.
               TestSetup.DeleteMessagesInQueue();
            }
            finally
            {
               RestListener.Stop();
               _application.Reinitialize();
            }
         }
      }

      // ---- helpers ------------------------------------------------------------

      // Straight at the tables, so that a schedule left by a test that failed
      // half-way is gone whether or not the listener is up; the Reinitialize
      // that follows in SetUp and TearDown makes the server forget its cache.
      private void ClearSchedules_()
      {
         _application.Database.ExecuteSQL("delete from hm_warmupcounts");
         _application.Database.ExecuteSQL("delete from hm_warmup");
      }

      // A test that straddled midnight UTC would count on two days, and one
      // whose hold to midnight were shorter than a route's five-minute retry
      // interval would not tell the two paths apart. Nothing here takes ten
      // minutes, so a start in the last ten waits them out.
      private static void AwayFromMidnight_()
      {
         DateTime now = DateTime.UtcNow;
         DateTime midnight = now.Date.AddDays(1);

         if (midnight - now < TimeSpan.FromMinutes(10))
            Thread.Sleep(midnight - now + TimeSpan.FromSeconds(5));
      }

      private Route AddRoute_(string domainName, string host, int port, int tries)
      {
         Route route = _settings.Routes.Add();
         route.DomainName = domainName;
         route.TargetSMTPHost = host;
         route.TargetSMTPPort = port;
         route.NumberOfTries = tries;
         route.MinutesBetweenTry = 5;
         route.TreatRecipientAsLocalDomain = false;
         route.TreatSenderAsLocalDomain = false;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
         return route;
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private JsonElement CreateSchedule_(string json)
      {
         (int status, string body) created = Admin("POST", "/api/v1/warmup", json);
         Assert.AreEqual(201, created.status, "POST /api/v1/warmup: " + created.body);
         return JsonDocument.Parse(created.body).RootElement;
      }

      private JsonElement Schedule_(string address)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/warmup", null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/warmup: " + answer.body);

         foreach (JsonElement schedule in JsonDocument.Parse(answer.body).RootElement.GetProperty("schedules").EnumerateArray())
         {
            if (schedule.GetProperty("address").GetString() == address)
               return schedule;
         }

         Assert.Fail("No warm-up schedule for " + address + " in " + answer.body);
         return default(JsonElement);
      }

      private JsonElement Provider_(string address, string provider)
      {
         foreach (JsonElement entry in Schedule_(address).GetProperty("providers").EnumerateArray())
         {
            if (entry.GetProperty("provider").GetString() == provider)
               return entry;
         }

         Assert.Fail("The schedule for " + address + " has no entry for " + provider + ".");
         return default(JsonElement);
      }

      private int Sent_(string provider) => Provider_(BindAddress, provider).GetProperty("sent").GetInt32();

      private int ScheduleCount_()
         => JsonDocument.Parse(Admin("GET", "/api/v1/warmup", null).body).RootElement.GetProperty("schedules").GetArrayLength();

      /// <summary>The days GET /api/v1/warmup/{id}/history answers.</summary>
      private JsonElement History_(long id)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/warmup/" + id + "/history", null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/warmup/" + id + "/history: " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.GetProperty("days").Clone();
      }

      /// <summary>Today's UTC day number, as hm_warmupcounts.wcday holds it.</summary>
      private static long TodayNumber_() => DateTimeOffset.UtcNow.ToUnixTimeSeconds() / 86400;

      private static string Today_() => DateTime.UtcNow.Date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);

      private (int status, string body) AsAccount_(string address, string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic(address, RolePassword), requestBody);

      private (int status, string body) Bearer_(string token, string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + token, requestBody);

      private void Assign_(string address, string role, string domains)
      {
         (int status, string body) assigned = Admin("PUT", "/api/v1/role-assignments/" + address, "{\"role\":\"" + role + "\",\"domains\":\"" + domains + "\"}");
         Assert.AreEqual(200, assigned.status, "The administrator could not give " + address + " the role " + role + ": " + assigned.body);
      }

      private static DateTime NextTryUtc_(string nextTry)
         => DateTime.SpecifyKind(DateTime.ParseExact(nextTry, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture), DateTimeKind.Local).ToUniversalTime();

      private int Deferred_(string provider) => Provider_(BindAddress, provider).GetProperty("deferred").GetInt32();

      private static bool WaitFor_(Func<bool> condition, int seconds)
      {
         DateTime deadline = DateTime.UtcNow.AddSeconds(seconds);

         while (DateTime.UtcNow < deadline)
         {
            if (condition())
               return true;

            Thread.Sleep(250);
         }

         return condition();
      }

      /// <summary>The queue's rows for recipients at a domain: (id, tries, locked, recipients, next try).</summary>
      private List<(long id, int tries, bool locked, string recipients, string nextTry)> Queued_(string domain)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/queue", null);
         Assert.AreEqual(200, answer.status, answer.body);

         var rows = new List<(long, int, bool, string, string)>();
         foreach (JsonElement message in JsonDocument.Parse(answer.body).RootElement.GetProperty("messages").EnumerateArray())
         {
            string recipients = message.GetProperty("recipients").GetString() ?? "";
            if (recipients.IndexOf("@" + domain, StringComparison.OrdinalIgnoreCase) >= 0)
            {
               rows.Add((message.GetProperty("id").GetInt64(), message.GetProperty("tries").GetInt32(), message.GetProperty("locked").GetBoolean(),
                  recipients, message.GetProperty("next_try").GetString()));
            }
         }

         return rows;
      }

      private static List<string> Recipients_(string domain, int count)
      {
         var list = new List<string>();
         for (int i = 1; i <= count; i++)
            list.Add("r" + i.ToString("00", CultureInfo.InvariantCulture) + "@" + domain);
         return list;
      }

      private static Dictionary<string, int> AcceptAll_(IEnumerable<string> addresses)
      {
         var accepted = new Dictionary<string, int>();
         foreach (string address in addresses)
            accepted[address] = 250;
         return accepted;
      }

      /// <summary>
      ///    Sends one message to count recipients at the Google route and waits for
      ///    the simulator's one session to end; answers how many RCPT TO it saw.
      /// </summary>
      private int SendToGoogle_(int port, int count, string subject)
      {
         List<string> recipients = Recipients_(GoogleDomain, count);

         using (var google = new SmtpServerSimulator(1, port))
         {
            google.AddRecipientResult(AcceptAll_(recipients));
            google.StartListen();

            SmtpClientSimulator.StaticSend(_account.Address, recipients, subject, "Warm-up");

            google.WaitForCompletion();
            return google.RcptTosReceived;
         }
      }

      // ---- the ceiling --------------------------------------------------------

      [Test]
      [Description("Day 1 of the standard plan allows 50 recipients a day at Google: fifty are delivered, the fifty-first is deferred without a connection until after midnight UTC with its tries unchanged, the counts say 50 sent and 1 deferred, and a message to another provider still goes.")]
      public void FiftyGoToGoogleOnDayOneAndTheFiftyFirstWaitsForTomorrow()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;

         JsonElement schedule = CreateSchedule_("{\"address\":\"" + BindAddress + "\"}");
         Assert.AreEqual("standard", schedule.GetProperty("plan").GetString());
         Assert.AreEqual(1, schedule.GetProperty("day").GetInt32(), "A schedule begun today is not on day 1.");
         Assert.IsTrue(schedule.GetProperty("in_bind_setting").GetBoolean(), "SMTPDeliveryBindToIP names the address, and the entry says it does not.");

         int port = TestSetup.GetNextFreePort();
         AddRoute_(GoogleDomain, GoogleHost, port, 3);

         int offered = SendToGoogle_(port, 51, "Fifty-one");

         Assert.AreEqual(50, offered, "The simulator was offered a number of recipients other than the day's ceiling.");

         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => !r.locked), 20), "The recipient over the ceiling is not in the queue.");

         var held = Queued_(GoogleDomain);
         Assert.AreEqual(1, held.Count, "One message should be left in the queue.");
         Assert.AreEqual(1, held[0].recipients.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries).Length,
            "Exactly one recipient should be left: " + held[0].recipients);
         StringAssert.Contains("r51@" + GoogleDomain, held[0].recipients, "The recipient held is not the last of the batch.");
         Assert.AreEqual(0, held[0].tries, "The warm-up's deferral was counted as an attempt.");

         // The queue writes the next try on the server's clock; this machine's.
         DateTime nextTry = NextTryUtc_(held[0].nextTry);
         DateTime midnight = DateTime.UtcNow.Date.AddDays(1);
         ClassicAssert.GreaterOrEqual(nextTry, midnight, "The held recipient's next try is before midnight UTC.");
         ClassicAssert.Less(nextTry, midnight.AddMinutes(10), "The held recipient's next try is not in the first minutes of the next UTC day.");

         Assert.IsTrue(LogHandler.DefaultLogContains("1 recipient(s) held by the IP warm-up of " + BindAddress + ": day 1 of its standard schedule allows 50 recipient(s) a day at Google, and today's have been sent. Next attempt after midnight UTC, in "),
            "The log does not say which address, provider, day and ceiling held the recipient, and that it waits for the next day.");
         Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"), "The deferral was not taken on the uncounted path.");

         JsonElement google = Provider_(BindAddress, "google");
         Assert.AreEqual(50, google.GetProperty("sent").GetInt32(), "Google's count is not the fifty delivered.");
         Assert.AreEqual(1, google.GetProperty("deferred").GetInt32(), "Google's deferred count is not the one held.");
         Assert.AreEqual(50, google.GetProperty("ceiling").GetInt32(), "Day 1's ceiling for Google is not 50.");

         // The history says the same of today, with the day and the ceiling
         // stored with it. A row for yesterday, put in behind the server, comes
         // after it - newest first - with the day it was stored with (0), not
         // the 1 the schedule's start would give if it were worked out again.
         long id = schedule.GetProperty("id").GetInt64();
         JsonElement today = History_(id);
         Assert.AreEqual(1, today.GetArrayLength(), "The history is not today's one entry: " + today);
         Assert.AreEqual(Today_(), today[0].GetProperty("date").GetString());
         Assert.AreEqual("google", today[0].GetProperty("provider").GetString());
         Assert.AreEqual(1, today[0].GetProperty("day").GetInt32());
         Assert.AreEqual(50, today[0].GetProperty("sent").GetInt32());
         Assert.AreEqual(1, today[0].GetProperty("deferred").GetInt32());
         Assert.AreEqual(50, today[0].GetProperty("ceiling").GetInt32());

         _application.Database.ExecuteSQL("insert into hm_warmupcounts (wcwarmupid, wcday, wcprovider, wcsent, wcdeferred, wcceiling, wcplanday) values (" +
            id + ", " + (TodayNumber_() - 1) + ", 'yahoo', 7, 0, 25, 0)");

         JsonElement days = History_(id);
         Assert.AreEqual(2, days.GetArrayLength(), "The history does not hold the two days: " + days);
         Assert.AreEqual(Today_(), days[0].GetProperty("date").GetString(), "The history is not newest first: " + days);
         Assert.AreEqual(DateTime.UtcNow.Date.AddDays(-1).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture), days[1].GetProperty("date").GetString());
         Assert.AreEqual(0, days[1].GetProperty("day").GetInt32(), "The history worked the day out again rather than reporting the one stored.");
         Assert.AreEqual(25, days[1].GetProperty("ceiling").GetInt32(), "The history worked the ceiling out again rather than reporting the one stored.");
         Assert.AreEqual(7, days[1].GetProperty("sent").GetInt32());

         // A message sent once the ceiling is reached is held with no
         // connection made for it at all.
         using (var late = new SmtpServerSimulator(1, port))
         {
            late.AddRecipientResult(AcceptAll_(new[] { "late@" + GoogleDomain }));
            late.StartListen();

            SmtpClientSimulator.StaticSend(_account.Address, "late@" + GoogleDomain, "Late", "Held for tomorrow");

            Assert.IsTrue(WaitFor_(() => Deferred_("google") == 2, 20), "The message sent after the ceiling was reached was not held.");
            Assert.AreEqual(0, late.NumberOfConnectedClients, "A connection was made for a message the day's ceiling holds.");
         }

         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).FindAll(r => !r.locked && r.tries == 0).Count == 2, 20),
            "The late message is not in the queue with its tries unchanged.");

         // The control: another provider is under its own ceiling (five times
         // the named providers', 250 on day 1) and goes at once.
         int otherPort = TestSetup.GetNextFreePort();
         using (var other = new SmtpServerSimulator(1, otherPort))
         {
            other.AddRecipientResult(AcceptAll_(new[] { "someone@" + OtherDomain }));
            other.StartListen();

            AddRoute_(OtherDomain, OtherHost, otherPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "someone@" + OtherDomain, "Other provider", "Goes at once");

            other.WaitForCompletion();
            StringAssert.Contains("Goes at once", other.MessageData, "A message to another provider was held by Google's ceiling.");
         }

         Assert.IsTrue(WaitFor_(() => Sent_("other") == 1, 20), "The other provider's delivery was not counted as one recipient.");
         Assert.AreEqual(250, Provider_(BindAddress, "other").GetProperty("ceiling").GetInt32(), "The other providers' ceiling is not five times Google's.");
         Assert.AreEqual(50, Sent_("google"), "A delivery to another provider moved Google's count.");
      }

      [Test]
      [Description("A recipient held past its route's retry span is counted, and its counted try still lands after midnight UTC - not at the retry interval, inside a day whose ceiling cannot reopen.")]
      public void AHoldPastItsRetrySpanIsCountedAndStillWaitsForTheNextDay()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;

         long id = CreateSchedule_("{\"address\":\"" + BindAddress + "\"}").GetProperty("id").GetInt64();

         // Today's allowance at Google already sent.
         _application.Database.ExecuteSQL("insert into hm_warmupcounts (wcwarmupid, wcday, wcprovider, wcsent, wcdeferred, wcceiling, wcplanday) values (" +
            id + ", " + TodayNumber_() + ", 'google', 50, 0, 50, 1)");

         // Two tries five minutes apart: a retry schedule that spans ten minutes.
         int port = TestSetup.GetNextFreePort();
         AddRoute_(GoogleDomain, GoogleHost, port, 2);

         using (var google = new SmtpServerSimulator(1, port))
         {
            google.AddRecipientResult(AcceptAll_(new[] { "late@" + GoogleDomain }));
            google.StartListen();

            SmtpClientSimulator.StaticSend(_account.Address, "late@" + GoogleDomain, "Past its span", "Held for tomorrow");

            Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => !r.locked && r.tries == 0), 20),
               "The recipient over the ceiling was not held on the uncounted path.");

            long messageId = Queued_(GoogleDomain)[0].id;

            // Queued long before the span, as a message held since yesterday
            // would be: the next pass is counted.
            _application.Database.ExecuteSQL("update hm_messages set messagecreatetime = '2001-01-01 00:00:00' where messageid = " + messageId);

            (int status, string body) retried = Admin("POST", "/api/v1/queue/" + messageId + "/retry", null);
            Assert.AreEqual(200, retried.status, retried.body);

            Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => !r.locked && r.tries == 1), 30),
               "The pass past the retry span was not counted.");

            DateTime nextTry = NextTryUtc_(Queued_(GoogleDomain)[0].nextTry);
            ClassicAssert.GreaterOrEqual(nextTry, DateTime.UtcNow.Date.AddDays(1),
               "The counted try was put at the retry interval, inside a day whose ceiling cannot reopen.");

            Assert.IsTrue(LogHandler.DefaultLogContains("This deferral is counted, so that the message is not held for ever."), "The span was not crossed.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the deferral's own length, since the IP warm-up's hold is longer than the retry interval"),
               "The counted try was not kept at the deferral's own length.");
            Assert.IsTrue(LogHandler.DefaultLogContains("Next attempt after midnight UTC, in ") && LogHandler.DefaultLogContains("; the attempt is counted against the retry limit."),
               "The warm-up's line does not say the next attempt is after midnight and counted.");
            Assert.AreEqual(0, google.NumberOfConnectedClients, "A connection was made for a recipient the day's ceiling holds.");
         }
      }

      [Test]
      [Description("With no schedule, or with the schedule disabled, a delivery from the address is not held: sixty recipients to Google on what would be day 1 all go, and nothing is counted.")]
      public void NoScheduleOrADisabledOneHoldsNothing()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;

         int port = TestSetup.GetNextFreePort();
         Route route = AddRoute_(GoogleDomain, GoogleHost, port, 3);

         Assert.AreEqual(60, SendToGoogle_(port, 60, "No schedule"), "A delivery from an address with no schedule was held.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Count == 0, 20), "A recipient was left in the queue with no schedule.");

         long id = CreateSchedule_("{\"address\":\"" + BindAddress + "\",\"enabled\":false}").GetProperty("id").GetInt64();

         int secondPort = TestSetup.GetNextFreePort();
         route.TargetSMTPPort = secondPort;
         route.Save();

         Assert.AreEqual(60, SendToGoogle_(secondPort, 60, "Disabled schedule"), "A delivery was held by a disabled schedule.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Count == 0, 20), "A recipient was left in the queue by a disabled schedule.");

         Assert.AreEqual(0, Sent_("google"), "A disabled schedule counted.");
         Assert.AreEqual(0, Deferred_("google"), "A disabled schedule deferred.");

         // The same schedule switched on while the server's cache holds it off:
         // in force at the next delivery, which only a write that makes the
         // server read the schedules again can give.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/warmup/" + id, "{\"enabled\":true}").status);

         int thirdPort = TestSetup.GetNextFreePort();
         route.TargetSMTPPort = thirdPort;
         route.Save();

         Assert.AreEqual(50, SendToGoogle_(thirdPort, 51, "Enabled again"), "A schedule switched on while the cache held it off was not in force at the next delivery.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => !r.locked && r.tries == 0), 20), "The recipient over the ceiling is not held.");
      }

      [Test]
      [Description("A delivery that binds no address is untouched by a schedule for 127.0.0.1: sixty recipients go and nothing is counted, and the entry says SMTPDeliveryBindToIP does not name the address.")]
      public void ADeliveryWithNoBindAddressIsUntouched()
      {
         // PerformBasicSetup has emptied SMTPDeliveryBindToIP.
         JsonElement schedule = CreateSchedule_("{\"address\":\"" + BindAddress + "\"}");
         Assert.IsFalse(schedule.GetProperty("in_bind_setting").GetBoolean(), "The entry says a setting that is empty names the address.");

         int port = TestSetup.GetNextFreePort();
         AddRoute_(GoogleDomain, GoogleHost, port, 3);

         Assert.AreEqual(60, SendToGoogle_(port, 60, "No bind"), "A delivery that binds no address was held by a schedule for one.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Count == 0, 20), "A recipient was left in the queue.");

         Assert.AreEqual(0, Sent_("google"), "A delivery that bound no address was counted against a schedule.");
      }

      [Test]
      [Description("A schedule past its plan's last day holds nothing and counts nothing: sixty recipients to Google from a schedule begun forty days ago all go, and its history stays empty.")]
      public void ACompletedScheduleHoldsAndCountsNothing()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;

         string longAgo = DateTime.UtcNow.Date.AddDays(-40).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
         JsonElement schedule = CreateSchedule_("{\"address\":\"" + BindAddress + "\",\"started\":\"" + longAgo + "\"}");
         Assert.IsTrue(schedule.GetProperty("complete").GetBoolean(), "A schedule begun forty days ago is not complete.");

         int port = TestSetup.GetNextFreePort();
         AddRoute_(GoogleDomain, GoogleHost, port, 3);

         Assert.AreEqual(60, SendToGoogle_(port, 60, "Complete"), "A complete schedule held a delivery.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Count == 0, 20), "A recipient was left in the queue.");

         Assert.AreEqual(0, Sent_("google"), "A complete schedule counted.");
         Assert.AreEqual(0, History_(schedule.GetProperty("id").GetInt64()).GetArrayLength(), "A complete schedule has days in its history.");
      }

      [Test]
      [Description("A day's count survives a restart and is read again from the table: three recipients sent, the count changed to ten behind the server, the server reinitialised - and the day resumes at ten, so the next forty-eight are forty delivered and eight deferred.")]
      public void ACountSurvivesARestart()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;
         CreateSchedule_("{\"address\":\"" + BindAddress + "\"}");

         int port = TestSetup.GetNextFreePort();
         Route route = AddRoute_(GoogleDomain, GoogleHost, port, 3);

         Assert.AreEqual(3, SendToGoogle_(port, 3, "Before the restart"));
         Assert.IsTrue(WaitFor_(() => Sent_("google") == 3, 20), "The three recipients were not counted.");

         // Changed behind the server's cache, which still holds three: only a
         // server that reads the table again after the reinitialise sees ten.
         _application.Database.ExecuteSQL("update hm_warmupcounts set wcsent = 10 where wcprovider = 'google'");

         _application.Reinitialize();

         Assert.AreEqual(10, Sent_("google"), "The day's count in the table is not the one listed.");

         int secondPort = TestSetup.GetNextFreePort();
         route.TargetSMTPPort = secondPort;
         route.Save();

         Assert.AreEqual(40, SendToGoogle_(secondPort, 48, "After the restart"), "The reinitialised server did not read the day's count again.");
         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => !r.locked && r.tries == 0), 20), "The recipients over the ceiling are not held.");

         Assert.AreEqual(50, Sent_("google"));
         Assert.AreEqual(8, Deferred_("google"));
      }

      [Test]
      [Description("A connection that fails gives the reservation back, so the count does not move; the control, a remote that refuses two of five recipients, counts the three it accepted.")]
      public void AFailedConnectionGivesTheReservationBack()
      {
         _settings.SMTPDeliveryBindToIP = BindAddress;
         long id = CreateSchedule_("{\"address\":\"" + BindAddress + "\"}").GetProperty("id").GetInt64();

         // Nothing listens here: the connection is refused. Two tries, so the
         // message stays queued with the one it spent.
         int deadPort = TestSetup.GetNextFreePort();
         Route route = AddRoute_(GoogleDomain, GoogleHost, deadPort, 2);

         SmtpClientSimulator.StaticSend(_account.Address, Recipients_(GoogleDomain, 5), "Refused", "Nobody answers");

         Assert.IsTrue(WaitFor_(() => Queued_(GoogleDomain).Exists(r => r.tries == 1), 30), "The refused connection did not count its attempt.");
         Assert.AreEqual(0, Sent_("google"), "A connection that was refused kept its reservation.");
         Assert.AreEqual(0, Deferred_("google"), "A refused connection was counted as a deferral.");
         Assert.AreEqual(0, History_(id).GetArrayLength(), "A day on which nothing was delivered or held has an entry in the history.");

         TestSetup.DeleteMessagesInQueue();

         // The control: a remote that refuses two of five at RCPT TO.
         int port = TestSetup.GetNextFreePort();
         route.TargetSMTPPort = port;
         route.Save();

         List<string> recipients = Recipients_(GoogleDomain, 5);
         using (var google = new SmtpServerSimulator(1, port))
         {
            Dictionary<string, int> results = AcceptAll_(recipients);
            results[recipients[1]] = 550;
            results[recipients[3]] = 550;
            google.AddRecipientResult(results);
            google.StartListen();

            SmtpClientSimulator.StaticSend(_account.Address, recipients, "Two refused", "Three accepted");

            google.WaitForCompletion();
         }

         Assert.IsTrue(WaitFor_(() => Sent_("google") == 3, 20), "The count is not the three recipients the remote accepted.");

         JsonElement days = History_(id);
         Assert.AreEqual(1, days.GetArrayLength(), "The history is not the one day something was delivered: " + days);
         Assert.AreEqual(3, days[0].GetProperty("sent").GetInt32());
      }

      // ---- the day arithmetic -------------------------------------------------

      [Test]
      [Description("A schedule begun eight days ago is on day 9: 10,000 a day at a named provider and 50,000 at the others on the standard plan, 500 on gentle and 100,000 on fast; a start later than today is refused.")]
      public void AScheduleBegunEightDaysAgoIsOnDayNine()
      {
         string eightDaysAgo = DateTime.UtcNow.Date.AddDays(-8).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);

         JsonElement schedule = CreateSchedule_("{\"address\":\"192.0.2.10\",\"started\":\"" + eightDaysAgo + "\"}");
         long id = schedule.GetProperty("id").GetInt64();

         Assert.AreEqual(eightDaysAgo, schedule.GetProperty("started").GetString());
         Assert.AreEqual(9, schedule.GetProperty("day").GetInt32(), "A schedule begun eight days ago is not on day 9.");
         Assert.AreEqual(10000, Provider_("192.0.2.10", "microsoft").GetProperty("ceiling").GetInt32(), "Day 9 of the standard plan does not allow 10,000.");
         Assert.AreEqual(50000, Provider_("192.0.2.10", "other").GetProperty("ceiling").GetInt32(), "The other providers do not get five times it.");

         (int status, string body) gentle = Admin("PUT", "/api/v1/warmup/" + id, "{\"plan\":\"gentle\"}");
         Assert.AreEqual(200, gentle.status, gentle.body);
         Assert.AreEqual(500, Provider_("192.0.2.10", "google").GetProperty("ceiling").GetInt32(), "Day 9 of the gentle plan does not allow 500.");

         (int status, string body) fast = Admin("PUT", "/api/v1/warmup/" + id, "{\"plan\":2}");
         Assert.AreEqual(200, fast.status, fast.body);
         Assert.AreEqual(100000, Provider_("192.0.2.10", "yahoo").GetProperty("ceiling").GetInt32(), "Day 9 of the fast plan does not allow 100,000.");
         Assert.AreEqual(15, Schedule_("192.0.2.10").GetProperty("last_day").GetInt32(), "The fast plan's last day is not 15.");

         // Past the plan: no ceiling, and the entry says the warm-up is over.
         string longAgo = DateTime.UtcNow.Date.AddDays(-40).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/warmup/" + id, "{\"started\":\"" + longAgo + "\"}").status);
         Assert.IsTrue(Schedule_("192.0.2.10").GetProperty("complete").GetBoolean(), "A schedule past its plan's last day is not complete.");
         Assert.AreEqual(JsonValueKind.Null, Provider_("192.0.2.10", "google").GetProperty("ceiling").ValueKind, "A complete schedule still has a ceiling.");

         string tomorrow = DateTime.UtcNow.Date.AddDays(1).ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
         (int status, string body) future = Admin("PUT", "/api/v1/warmup/" + id, "{\"started\":\"" + tomorrow + "\"}");
         Assert.AreEqual(400, future.status, "A start later than today was accepted: " + future.body);
         Assert.AreEqual(longAgo, Schedule_("192.0.2.10").GetProperty("started").GetString(), "A refused start changed the schedule.");
      }

      // ---- the routes ---------------------------------------------------------

      [Test]
      [Description("The routes round-trip a schedule - created, listed, changed, its history read, deleted - and refuse a bad address, a plan of 3, an unknown field and a second schedule for the same address, however it is written.")]
      public void TheRoutesRoundTripAndRefuse()
      {
         JsonElement created = CreateSchedule_("{\"address\":\"0:0:0:0:0:0:0:1\",\"plan\":\"gentle\"}");
         long id = created.GetProperty("id").GetInt64();

         Assert.AreEqual("::1", created.GetProperty("address").GetString(), "The address is not written the way a delivery names it.");
         Assert.AreEqual("gentle", created.GetProperty("plan").GetString());
         Assert.IsTrue(created.GetProperty("enabled").GetBoolean(), "A new schedule is not enabled.");
         Assert.AreEqual(DateTime.UtcNow.Date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture), created.GetProperty("started").GetString(), "A new schedule does not begin today.");
         Assert.AreEqual(6, created.GetProperty("providers").GetArrayLength(), "The entry does not name the six providers.");
         Assert.AreEqual(25, Provider_("::1", "apple").GetProperty("ceiling").GetInt32(), "Day 1 of the gentle plan does not allow 25.");

         (int status, string body) changed = Admin("PUT", "/api/v1/warmup/" + id, "{\"enabled\":false,\"plan\":\"fast\"}");
         Assert.AreEqual(200, changed.status, changed.body);
         JsonElement after = Schedule_("::1");
         Assert.IsFalse(after.GetProperty("enabled").GetBoolean(), "enabled was not changed.");
         Assert.AreEqual("fast", after.GetProperty("plan").GetString(), "plan was not changed.");
         Assert.AreEqual("::1", after.GetProperty("address").GetString(), "A field left out of the PUT did not keep its value.");

         (int status, string body) history = Admin("GET", "/api/v1/warmup/" + id + "/history", null);
         Assert.AreEqual(200, history.status, history.body);
         JsonElement days = JsonDocument.Parse(history.body).RootElement;
         Assert.AreEqual("::1", days.GetProperty("address").GetString());
         Assert.AreEqual(0, days.GetProperty("days").GetArrayLength(), "A schedule nothing was sent under has days in its history.");

         // The refusals, each with nothing changed.
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"192.0.2.300\"}").status, "An address that is not one was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"mail.example.com\"}").status, "A host name was accepted as an address.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"0.0.0.0\"}").status, "The unspecified address was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"::\"}").status, "The unspecified IPv6 address was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"plan\":\"standard\"}").status, "A schedule with no address was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"192.0.2.20\",\"plan\":3}").status, "A plan of 3 was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"192.0.2.20\",\"plan\":\"quick\"}").status, "A plan that is no plan was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"192.0.2.20\",\"pool\":\"a\"}").status, "An unknown field was accepted.");
         Assert.AreEqual(400, Admin("POST", "/api/v1/warmup", "{\"address\":\"192.0.2.20\",\"started\":\"24/09/2026\"}").status, "A date not written YYYY-MM-DD was accepted.");
         Assert.AreEqual(400, Admin("PUT", "/api/v1/warmup/" + id, "{\"plan\":3}").status, "A plan of 3 was accepted by the PUT.");
         Assert.AreEqual(400, Admin("PUT", "/api/v1/warmup/" + id, "{\"pool\":\"a\"}").status, "An unknown field was accepted by the PUT.");

         (int status, string body) moved = Admin("PUT", "/api/v1/warmup/" + id, "{\"address\":\"192.0.2.30\",\"plan\":\"gentle\"}");
         Assert.AreEqual(400, moved.status, "A schedule's address was changed: " + moved.body);
         StringAssert.Contains("cannot be changed", moved.body);
         Assert.AreEqual("fast", Schedule_("::1").GetProperty("plan").GetString(), "A refused PUT changed the schedule.");

         // The same address in another spelling is not a change.
         Assert.AreEqual(200, Admin("PUT", "/api/v1/warmup/" + id, "{\"address\":\"0::1\"}").status, "The schedule's own address, written otherwise, was refused.");

         (int status, string body) duplicate = Admin("POST", "/api/v1/warmup", "{\"address\":\"::1\"}");
         Assert.AreEqual(409, duplicate.status, "A second schedule for the same address was accepted: " + duplicate.body);
         Assert.AreEqual(409, Admin("POST", "/api/v1/warmup", "{\"address\":\"0::1\"}").status, "A second schedule for the same address, written otherwise, was accepted.");

         (int status, string body) all = Admin("GET", "/api/v1/warmup", null);
         Assert.AreEqual(1, JsonDocument.Parse(all.body).RootElement.GetProperty("schedules").GetArrayLength(), "A refused write left a schedule behind: " + all.body);

         Assert.AreEqual(404, Admin("PUT", "/api/v1/warmup/999999", "{\"enabled\":true}").status, "A PUT of an unknown id was not 404.");
         Assert.AreEqual(404, Admin("GET", "/api/v1/warmup/999999/history", null).status, "The history of an unknown id was not 404.");

         (int status, string body) deleted = Admin("DELETE", "/api/v1/warmup/" + id, null);
         Assert.AreEqual(200, deleted.status, deleted.body);
         Assert.AreEqual(404, Admin("DELETE", "/api/v1/warmup/" + id, null).status, "A second delete was not 404.");
         Assert.AreEqual(0, JsonDocument.Parse(Admin("GET", "/api/v1/warmup", null).body).RootElement.GetProperty("schedules").GetArrayLength(), "The deleted schedule is still listed.");
      }

      [Test]
      [Description("The routes are the queue's: view_queue reads and is refused every write, manage_queue writes, a credential confined to a domain is refused with the queue's sentence, and a read-only key reads and does not write - each refusal changing nothing.")]
      public void TheRoutesAnswerToTheQueuesPermissions()
      {
         long id = CreateSchedule_("{\"address\":\"192.0.2.40\"}").GetProperty("id").GetInt64();

         string domainName = _domain.Name;
         string viewer = "warmup-viewer@" + domainName;
         string manager = "warmup-manager@" + domainName;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, viewer, RolePassword);
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, manager, RolePassword);

         string keyId = null;

         try
         {
            Assert.AreEqual(201, Admin("POST", "/api/v1/roles", "{\"name\":\"warmuptest-viewer\",\"permissions\":\"view_queue\"}").status);
            Assert.AreEqual(201, Admin("POST", "/api/v1/roles", "{\"name\":\"warmuptest-manager\",\"permissions\":\"view_queue,manage_queue\"}").status);
            Assign_(viewer, "warmuptest-viewer", "");
            Assign_(manager, "warmuptest-manager", "");

            // view_queue: the reads, and not one write.
            Assert.AreEqual(200, AsAccount_(viewer, "GET", "/api/v1/warmup", null).status, "view_queue was refused the listing.");
            Assert.AreEqual(200, AsAccount_(viewer, "GET", "/api/v1/warmup/" + id + "/history", null).status, "view_queue was refused the history.");
            Assert.AreEqual(403, AsAccount_(viewer, "POST", "/api/v1/warmup", "{\"address\":\"192.0.2.41\"}").status, "view_queue added a schedule.");
            Assert.AreEqual(403, AsAccount_(viewer, "PUT", "/api/v1/warmup/" + id, "{\"enabled\":false}").status, "view_queue changed a schedule.");
            Assert.AreEqual(403, AsAccount_(viewer, "DELETE", "/api/v1/warmup/" + id, null).status, "view_queue removed a schedule.");
            Assert.AreEqual(1, ScheduleCount_(), "A refused write added or removed a schedule.");
            Assert.IsTrue(Schedule_("192.0.2.40").GetProperty("enabled").GetBoolean(), "A refused PUT changed the schedule.");

            // The control: manage_queue writes.
            Assert.AreEqual(200, AsAccount_(manager, "PUT", "/api/v1/warmup/" + id, "{\"enabled\":false}").status, "manage_queue was refused a change.");
            Assert.IsFalse(Schedule_("192.0.2.40").GetProperty("enabled").GetBoolean(), "manage_queue's change was not made.");

            // The same role confined to a domain: the queue's refusal, on a read
            // and on a write, and nothing changed.
            Assign_(manager, "warmuptest-manager", domainName);

            (int status, string body) confinedRead = AsAccount_(manager, "GET", "/api/v1/warmup", null);
            Assert.AreEqual(403, confinedRead.status, "A credential confined to a domain read the warm-up.");
            StringAssert.Contains("the delivery queue is server-wide", confinedRead.body);

            (int status, string body) confinedWrite = AsAccount_(manager, "POST", "/api/v1/warmup", "{\"address\":\"192.0.2.41\"}");
            Assert.AreEqual(403, confinedWrite.status, "A credential confined to a domain added a schedule.");
            StringAssert.Contains("the delivery queue is server-wide", confinedWrite.body);
            Assert.AreEqual(403, AsAccount_(manager, "GET", "/api/v1/warmup/" + id + "/history", null).status, "A credential confined to a domain read a history.");
            Assert.AreEqual(1, ScheduleCount_(), "A confined credential's refused write added a schedule.");

            // A read-only key: the listing, and not a write.
            (int status, string body) key = Admin("POST", "/api/v1/apikeys", "{\"label\":\"warmuptest readonly\",\"scope\":\"readonly\"}");
            Assert.AreEqual(201, key.status, key.body);
            JsonElement created = JsonDocument.Parse(key.body).RootElement;
            keyId = created.GetProperty("id").GetString();
            string token = created.GetProperty("key").GetString();

            Assert.AreEqual(200, Bearer_(token, "GET", "/api/v1/warmup", null).status, "A read-only key was refused the listing.");

            (int status, string body) keyWrite = Bearer_(token, "POST", "/api/v1/warmup", "{\"address\":\"192.0.2.42\"}");
            Assert.AreEqual(403, keyWrite.status, "A read-only key added a schedule.");
            StringAssert.Contains("read-only", keyWrite.body);
            Assert.AreEqual(1, ScheduleCount_(), "A read-only key's refused write added a schedule.");
         }
         finally
         {
            if (keyId != null)
               Admin("DELETE", "/api/v1/apikeys/" + keyId, null);

            Admin("DELETE", "/api/v1/role-assignments/" + viewer, null);
            Admin("DELETE", "/api/v1/role-assignments/" + manager, null);
            Admin("DELETE", "/api/v1/roles/warmuptest-viewer", null);
            Admin("DELETE", "/api/v1/roles/warmuptest-manager", null);
         }
      }
   }
}
