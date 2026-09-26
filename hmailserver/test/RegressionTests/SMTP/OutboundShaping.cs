// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
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
   ///    Outbound shaping (docs/OutboundShaping.md): the back-off a destination's
   ///    own slow-down reply opens, the connection ceilings and the tenant share,
   ///    and the rule that a deferral this server chose does not count as an
   ///    attempt - each with its negative control, against the suite's SMTP
   ///    simulator through routes.
   ///
   ///    Two destinations, because the point of most of this is that one is held
   ///    and the other is not: dummy-example.com is routed to a simulator on
   ///    127.0.0.1 and other-example.com to one on 127.0.0.2. A route's target
   ///    host is what the back-off and the ceilings key by, so the two addresses
   ///    are what makes them two destinations; both are the loopback interface.
   ///
   ///    The listener is the fixture's own, on the first port of the range this
   ///    row was allocated (9856-9865).
   /// </summary>
   [TestFixture]
   public class OutboundShaping : TestFixtureBase
   {
      private static int RestPort = 9856;
      private const string AdminPassword = "testar";

      private const string FirstDomain = "dummy-example.com";
      private const string FirstHost = "127.0.0.1";
      private const string SecondDomain = "other-example.com";
      private const string SecondHost = "127.0.0.2";

      // A third address for the second of the two tests that set the fixed
      // per-destination rate: the rate's bucket is keyed by the target and
      // slides over a minute, so two tests a few seconds apart on one address
      // would find the first one's delivery still in the bucket.
      private const string ThirdHost = "127.0.0.3";
      private const string FourthHost = "127.0.0.4";

      private const string SlowDownAtMailFrom = "451 4.7.650 The mail server has been temporarily rate limited";
      private const string SlowDownAtBanner = "421 4.7.0 [TSS04] Messages temporarily deferred due to unexpected volume";

      private Account _account;

      [SetUp]
      public void StartRestApiAndResetShaping()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "shaping-sender@example.test", "test");

         ResetSettings_();
         ClearPolicies_();

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         ClearBackoffs_();
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            // A message this fixture left in the queue would be attempted again
            // by the next test's server, with the route gone; and a window left
            // open would hold the next test's deliveries.
            ClearBackoffs_();
            TestSetup.DeleteMessagesInQueue();
            ClearPolicies_();
            ResetSettings_();
            IniFileSetting.Write("MaxOutboundPerDestinationPerMinute", "0");
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---- helpers ------------------------------------------------------------

      private void ResetSettings_()
      {
         // The ceiling first: each write is refused when it would cross the
         // other, and no test here leaves the base above sixty.
         _settings.OutboundBackoffMaxMinutes = 60;
         _settings.OutboundBackoffMinutes = 2;
         _settings.MaxOutboundConnectionsPerDestination = 0;
         _settings.MaxOutboundConnectionsPerTenant = 0;
      }

      private void ClearPolicies_()
      {
         hMailServer.RemoteDomainPolicies policies = _settings.RemoteDomainPolicies;

         for (int i = policies.Count - 1; i >= 0; i--)
            policies[i].Delete();
      }

      private RemoteDomainPolicy AddPolicy_(string domainName, Action<RemoteDomainPolicy> configure)
      {
         RemoteDomainPolicy policy = _settings.RemoteDomainPolicies.Add();
         policy.DomainName = domainName;
         policy.Active = true;
         configure(policy);
         policy.Save();
         return policy;
      }

      private Route AddRoute_(string domainName, string host, int port, int tries, bool treatAsLocal = false, int minutesBetween = 5)
      {
         Route route = _settings.Routes.Add();
         route.DomainName = domainName;
         route.TargetSMTPHost = host;
         route.TargetSMTPPort = port;
         route.NumberOfTries = tries;
         route.MinutesBetweenTry = minutesBetween;
         route.TreatRecipientAsLocalDomain = treatAsLocal;
         route.TreatSenderAsLocalDomain = treatAsLocal;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
         return route;
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private JsonElement Shaping_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/queue/shaping", null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/queue/shaping: " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement;
      }

      private JsonElement? FindBackoff_(string destination)
      {
         foreach (JsonElement entry in Shaping_().GetProperty("backoffs").EnumerateArray())
         {
            if (entry.GetProperty("destination").GetString() == destination)
               return entry;
         }

         return null;
      }

      private int ConnectionsInUse_(string map, string name)
      {
         JsonElement counts = Shaping_().GetProperty("connections").GetProperty(map);
         return counts.TryGetProperty(name, out JsonElement value) ? value.GetInt32() : 0;
      }

      private void ClearBackoffs_()
      {
         foreach (JsonElement entry in Shaping_().GetProperty("backoffs").EnumerateArray())
            Admin("DELETE", "/api/v1/queue/shaping/" + Uri.EscapeDataString(entry.GetProperty("destination").GetString()), null);
      }

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

      private bool WaitForBackoff_(string destination)
         => WaitFor_(() => FindBackoff_(destination).HasValue, 20);

      /// <summary>The queue's rows for recipients at a domain: (id, tries, locked).</summary>
      private List<(long id, int tries, bool locked)> Queued_(string domain)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/queue", null);
         Assert.AreEqual(200, answer.status, answer.body);

         var rows = new List<(long, int, bool)>();
         foreach (JsonElement message in JsonDocument.Parse(answer.body).RootElement.GetProperty("messages").EnumerateArray())
         {
            if ((message.GetProperty("recipients").GetString() ?? "").IndexOf("@" + domain, StringComparison.OrdinalIgnoreCase) >= 0)
               rows.Add((message.GetProperty("id").GetInt64(), message.GetProperty("tries").GetInt32(), message.GetProperty("locked").GetBoolean()));
         }

         return rows;
      }

      /// <summary>The next try the queue lists for the one message at a domain.</summary>
      private string NextTry_(string domain)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/queue", null);
         Assert.AreEqual(200, answer.status, answer.body);

         foreach (JsonElement message in JsonDocument.Parse(answer.body).RootElement.GetProperty("messages").EnumerateArray())
         {
            if ((message.GetProperty("recipients").GetString() ?? "").IndexOf("@" + domain, StringComparison.OrdinalIgnoreCase) >= 0)
               return message.GetProperty("next_try").GetString();
         }

         Assert.Fail("No queued message for " + domain + ".");
         return "";
      }

      /// <summary>
      ///    The one queued message with no attempt counted, for the retry. Not a
      ///    locked one: a message whose session is being held open is in the
      ///    queue with zero tries too, and it is not the held one.
      /// </summary>
      private long HeldMessageId_(string domain)
      {
         List<(long id, int tries, bool locked)> rows = Queued_(domain);
         (long id, int tries, bool locked) held = rows.Find(r => r.tries == 0 && !r.locked);
         Assert.AreNotEqual(0, held.id, "No unlocked queued message with zero tries for " + domain + "; the queue holds " + rows.Count + ".");
         return held.id;
      }

      private static Dictionary<string, int> Accepts_(string address) => new Dictionary<string, int> { [address] = 250 };

      private static DateTime ServerTime_(string text)
         => DateTime.ParseExact(text, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);

      // ---- the back-off -------------------------------------------------------

      [Test]
      [Description("A slow-down reply at MAIL FROM opens a back-off: the next message to that destination is held without a connection, with its tries unchanged, the log line and the listing say so, and a message to another destination goes at once.")]
      public void ASlowDownReplyAtMailFromHoldsTheDestinationAndNobodyElse()
      {
         int slowPort = TestSetup.GetNextFreePort();
         int otherPort = TestSetup.GetNextFreePort();

         using (var slow = new SmtpServerSimulator(2, slowPort))
         using (var other = new SmtpServerSimulator(1, otherPort))
         {
            slow.MailFromReply = SlowDownAtMailFrom;
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.StartListen();

            other.AddRecipientResult(Accepts_("test@" + SecondDomain));
            other.StartListen();

            AddRoute_(FirstDomain, FirstHost, slowPort, 3);
            AddRoute_(SecondDomain, SecondHost, otherPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Opens the window");

            Assert.IsTrue(WaitForBackoff_(FirstHost), "The slow-down reply at MAIL FROM did not open a back-off for " + FirstHost + ".");
            Assert.IsTrue(LogHandler.DefaultLogContains(FirstHost + " asked this server to slow down ("), "The log does not say the destination asked to slow down.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Held by the window");

            Assert.IsTrue(LogHandler.DefaultLogContains("Delivery to " + FirstHost + " deferred: the destination asked this server to slow down at"),
               "The second message was not deferred by the window, or the log does not say so.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"),
               "The deferral was counted as an attempt.");
            Assert.AreEqual(1, slow.NumberOfConnectedClients, "The second message was offered over a connection the back-off should have prevented.");

            JsonElement backoff = FindBackoff_(FirstHost).Value;
            StringAssert.Contains("4.7.650", backoff.GetProperty("reply").GetString(), "The listing does not carry the remote's reply.");
            Assert.AreEqual(2, backoff.GetProperty("window_minutes").GetInt32(), "The first window is not the base of two minutes.");
            Assert.AreEqual(1, backoff.GetProperty("deferred").GetInt32(), "The window did not count the delivery it held.");
            Assert.IsFalse(backoff.GetProperty("probing").GetBoolean(), "Nothing should be probing while the window is open.");
            Assert.AreEqual(FirstHost, backoff.GetProperty("host").GetString(), "The listing does not name the host that answered.");
            Assert.Less(ServerTime_(backoff.GetProperty("since").GetString()), ServerTime_(backoff.GetProperty("until").GetString()), "until is not after since.");

            // The refused one counted an attempt; the held one did not.
            List<(long id, int tries, bool locked)> queued = Queued_(FirstDomain);
            Assert.AreEqual(2, queued.Count, "Both messages should be in the queue.");
            Assert.AreEqual(1, queued.FindAll(r => r.tries == 1).Count, "The message the remote refused should have one try counted.");
            Assert.AreEqual(1, queued.FindAll(r => r.tries == 0).Count, "The message the window held should have no try counted.");

            // The control: another destination is untouched.
            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "Third", "Goes at once");

            other.WaitForCompletion();
            StringAssert.Contains("Goes at once", other.MessageData, "A message to another destination was held by the first one's window.");
            Assert.IsFalse(FindBackoff_(SecondHost).HasValue, "A window was opened for a destination that never asked.");
         }
      }

      [Test]
      [Description("DELETE /api/v1/queue/shaping/{destination} clears the window, so a retry of the held message is delivered at once; the listing is then empty, a second clear is 404, and the clear is logged.")]
      public void ClearingTheBackoffLetsARetryDeliver()
      {
         int slowPort = TestSetup.GetNextFreePort();

         using (var slow = new SmtpServerSimulator(2, slowPort))
         {
            slow.MailFromReplies.Enqueue(SlowDownAtMailFrom);
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.StartListen();

            AddRoute_(FirstDomain, FirstHost, slowPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Opens the window");
            Assert.IsTrue(WaitForBackoff_(FirstHost), "The slow-down reply did not open a back-off.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Delivered after the clear");
            Assert.IsTrue(LogHandler.DefaultLogContains("Delivery to " + FirstHost + " deferred: the destination asked"), "The second message was not held.");
            Assert.AreEqual(1, slow.NumberOfConnectedClients, "The held message was offered anyway.");

            long held = HeldMessageId_(FirstDomain);

            (int status, string body) cleared = Admin("DELETE", "/api/v1/queue/shaping/" + FirstHost, null);
            Assert.AreEqual(200, cleared.status, cleared.body);
            StringAssert.Contains("\"cleared\":true", cleared.body);

            Assert.AreEqual(0, Shaping_().GetProperty("backoffs").GetArrayLength(), "The window is still listed after the clear.");
            Assert.IsTrue(LogHandler.DefaultLogContains("The back-off for " + FirstHost + " was cleared"), "The clear was not logged.");

            (int status, string body) again = Admin("DELETE", "/api/v1/queue/shaping/" + FirstHost, null);
            Assert.AreEqual(404, again.status, "Clearing a window that is not there should be 404: " + again.body);

            (int status, string body) retried = Admin("POST", "/api/v1/queue/" + held + "/retry", null);
            Assert.AreEqual(200, retried.status, retried.body);

            Assert.IsTrue(WaitFor_(() => slow.NumberOfConnectedClients == 2, 30), "The retried message was not offered after the clear.");
            slow.WaitForCompletion();
            StringAssert.Contains("Delivered after the clear", slow.MessageData, "The retried message was not delivered.");
         }
      }

      [Test]
      [Description("A 421 at the banner opens a back-off too: the next message is held without a connection and the listing carries the banner.")]
      public void A421AtTheBannerOpensABackoff()
      {
         int slowPort = TestSetup.GetNextFreePort();

         using (var slow = new SmtpServerSimulator(2, slowPort))
         {
            slow.ConnectReply = SlowDownAtBanner;
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.AddRecipientResult(Accepts_("test@" + FirstDomain));
            slow.StartListen();

            AddRoute_(FirstDomain, FirstHost, slowPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Meets the banner");
            Assert.IsTrue(WaitForBackoff_(FirstHost), "The 421 banner did not open a back-off.");

            JsonElement backoff = FindBackoff_(FirstHost).Value;
            StringAssert.StartsWith("421 4.7.0", backoff.GetProperty("reply").GetString(), "The listing does not carry the banner.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Held by the window");
            Assert.IsTrue(LogHandler.DefaultLogContains("Delivery to " + FirstHost + " deferred: the destination asked"), "The second message was not held.");
            Assert.AreEqual(1, slow.NumberOfConnectedClients, "The held message was offered anyway.");
         }
      }

      [Test]
      [Description("Not a signal: a 450 4.2.1 for one mailbox, a 451 4.7.1 greylisting, a 550 refusal and a refused connection open no window - the messages take the paths they always took.")]
      public void RepliesThatAreNotASignalOpenNothing()
      {
         int port = TestSetup.GetNextFreePort();
         int deadPort = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(3, port))
         {
            remote.MailFromReplies.Enqueue("451 4.7.1 Greylisted, please try again later");
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.AddRecipientResult(new Dictionary<string, int> { ["busy@" + FirstDomain] = 450 });
            remote.AddRecipientResult(new Dictionary<string, int> { ["dead@" + FirstDomain] = 550 });
            remote.RecipientReplyTexts["busy@" + FirstDomain] = "4.2.1 Mailbox busy, too many messages arriving";
            remote.RecipientReplyTexts["dead@" + FirstDomain] = "5.1.1 <dead@" + FirstDomain + ">: User unknown";
            remote.StartListen();

            AddRoute_(FirstDomain, FirstHost, port, 3);
            // Nothing listens here: the connection is refused.
            AddRoute_(SecondDomain, SecondHost, deadPort, 1);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Greylisted", "4.7.1 is this message's, not the destination's");
            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients >= 1, 20), "The greylisted message was not offered.");

            // The counted path's own sentence, and the try it counted: the
            // uncounted path's line also says "Scheduling it for later delivery".
            Assert.IsTrue(LogHandler.DefaultLogContains("Message could not be delivered. Scheduling it for later delivery"), "The greylisted message was not rescheduled on the counted path.");
            Assert.IsTrue(WaitFor_(() => Queued_(FirstDomain).Exists(r => r.tries == 1), 20), "The greylisting reply did not count an attempt.");

            SmtpClientSimulator.StaticSend(_account.Address, "busy@" + FirstDomain, "Busy mailbox", "4.2.1 is one mailbox");
            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients >= 2, 20), "The busy-mailbox message was not offered: the greylisting reply opened a window.");

            SmtpClientSimulator.StaticSend(_account.Address, "dead@" + FirstDomain, "Unknown", "A 550 is a refusal");
            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients >= 3, 20), "The refused message was not offered: a 4.2.x reply opened a window.");
            remote.WaitForCompletion();

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "Refused connection", "Nothing was said");
            Assert.IsTrue(WaitFor_(() => Queued_(SecondDomain).Count == 0, 40), "The message to the port nobody listens on was not bounced at its one try.");

            Assert.AreEqual(0, Shaping_().GetProperty("backoffs").GetArrayLength(),
               "A window was opened by a reply that is not a request to slow down.");
         }
      }

      [Test]
      [Description("With a one-minute window, the first delivery after it connects as the probe: answered with the same slow-down reply the window doubles; delivered, the entry is gone.")]
      public void TheProbeDoublesOnTheSameAnswerAndClearsOnADelivery()
      {
         _settings.OutboundBackoffMinutes = 1;

         int doublesPort = TestSetup.GetNextFreePort();
         int clearsPort = TestSetup.GetNextFreePort();

         using (var doubles = new SmtpServerSimulator(2, doublesPort))
         using (var clears = new SmtpServerSimulator(2, clearsPort))
         {
            doubles.MailFromReplies.Enqueue(SlowDownAtMailFrom);
            doubles.MailFromReplies.Enqueue(SlowDownAtMailFrom);
            doubles.AddRecipientResult(Accepts_("test@" + FirstDomain));
            doubles.AddRecipientResult(Accepts_("test@" + FirstDomain));
            doubles.StartListen();

            clears.MailFromReplies.Enqueue(SlowDownAtMailFrom);
            clears.AddRecipientResult(Accepts_("test@" + SecondDomain));
            clears.AddRecipientResult(Accepts_("test@" + SecondDomain));
            clears.StartListen();

            AddRoute_(FirstDomain, FirstHost, doublesPort, 3);
            AddRoute_(SecondDomain, SecondHost, clearsPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Opens the first window");
            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "First", "Opens the second window");

            Assert.IsTrue(WaitForBackoff_(FirstHost) && WaitForBackoff_(SecondHost), "Both windows should be open.");
            Assert.AreEqual(1, FindBackoff_(FirstHost).Value.GetProperty("window_minutes").GetInt32(), "The window is not the one-minute base.");

            DateTime untilFirst = ServerTime_(FindBackoff_(FirstHost).Value.GetProperty("until").GetString());
            DateTime untilSecond = ServerTime_(FindBackoff_(SecondHost).Value.GetProperty("until").GetString());
            DateTime passed = (untilFirst > untilSecond ? untilFirst : untilSecond).AddSeconds(2);

            // The one wait in this fixture: the window is a minute long.
            while (DateTime.Now < passed)
               Thread.Sleep(1000);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Probes and is refused again");
            Assert.IsTrue(LogHandler.DefaultLogContains("The back-off window for " + FirstHost + " has passed; this delivery probes it."), "The first delivery after the window did not probe.");
            Assert.IsTrue(WaitFor_(() => FindBackoff_(FirstHost).HasValue && FindBackoff_(FirstHost).Value.GetProperty("window_minutes").GetInt32() == 2, 20),
               "The same slow-down reply to the probe did not double the window.");
            Assert.AreEqual(2, doubles.NumberOfConnectedClients, "The probe did not connect.");
            Assert.IsFalse(FindBackoff_(FirstHost).Value.GetProperty("probing").GetBoolean(), "The entry is still marked probing after the probe was answered.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "Second", "Probe succeeds");
            clears.WaitForCompletion();
            StringAssert.Contains("Probe succeeds", clears.MessageData, "The probe's delivery did not arrive.");
            Assert.IsTrue(WaitFor_(() => !FindBackoff_(SecondHost).HasValue, 20), "A probe that delivered did not clear the window.");
         }
      }

      // ---- not counted --------------------------------------------------------

      [Test]
      [Description("A deferral by MaxOutboundPerDestinationPerMinute at one try is not a bounce: no report in the sender's mailbox, the message queued with zero tries for one to five minutes, and once the rate's window has drained a retry delivers it.")]
      public void AShapingDeferralDoesNotSpendTheRetryBudget()
      {
         IniFileSetting.Write("MaxOutboundPerDestinationPerMinute", "1");
         _application.Reinitialize();

         int port = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(2, port))
         {
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.SecondsToWaitBeforeTerminate = 120;
            remote.StartListen();

            // One try: before this, the second message was bounced without ever
            // having been offered to anybody.
            AddRoute_(FirstDomain, FirstHost, port, 1);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Within the rate");
            Assert.IsTrue(WaitFor_(() => remote.MessageData.Contains("Within the rate"), 20), "The first message was not delivered.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Over the rate");
            Assert.IsTrue(LogHandler.DefaultLogContains("deferred due to the per-destination rate limit"), "The second message was not deferred by the rate.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"), "The deferral was counted.");

            List<(long id, int tries, bool locked)> queued = Queued_(FirstDomain);
            Assert.AreEqual(1, queued.Count, "The deferred message is not in the queue.");
            Assert.AreEqual(0, queued[0].tries, "The deferral counted an attempt.");

            var pop3 = new Pop3ClientSimulator();
            Assert.AreEqual(0, pop3.GetMessageCount(_account.Address, "test"), "A delivery report reached the sender for a message that was never offered.");

            // The deferral is one to five minutes, spread by the message's id.
            TimeSpan wait = ServerTime_(NextTry_(FirstDomain)) - DateTime.Now;
            Assert.That(wait, Is.GreaterThan(TimeSpan.FromSeconds(45)).And.LessThan(TimeSpan.FromSeconds(320)), "The deferral is not one to five minutes: " + wait);

            // Retried every few seconds: each retry only makes the queue look
            // again, and while the rate's window has not drained each look is
            // deferred again, uncounted. Once it has, the message arrives.
            long held = queued[0].id;
            DateTime deadline = DateTime.UtcNow.AddSeconds(100);
            while (DateTime.UtcNow < deadline && !remote.MessageData.Contains("Over the rate"))
            {
               Admin("POST", "/api/v1/queue/" + held + "/retry", null);
               Thread.Sleep(5000);
            }

            remote.WaitForCompletion();
            StringAssert.Contains("Over the rate", remote.MessageData, "The deferred message did not arrive once the rate's window had drained.");
            Assert.AreEqual(0, pop3.GetMessageCount(_account.Address, "test"), "A delivery report reached the sender.");
            Assert.AreEqual(0, Queued_(FirstDomain).Count, "The message is still queued after its delivery.");
         }
      }

      [Test]
      [Description("An uncounted pass does not repeat the first pass's side effects: with a mirror address set and the second message deferred by the rate through several passes, the mirror mailbox holds one copy per message, not one per pass.")]
      public void AnUncountedPassDoesNotMirrorAgain()
      {
         Account mirror = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "mirror-shaping@example.test", "test");
         _settings.MirrorEMailAddress = mirror.Address;
         IniFileSetting.Write("MaxOutboundPerDestinationPerMinute", "1");
         _application.Reinitialize();

         try
         {
            int port = TestSetup.GetNextFreePort();

            using (var remote = new SmtpServerSimulator(2, port))
            {
               remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
               remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
               remote.SecondsToWaitBeforeTerminate = 120;
               remote.StartListen();

               // The third address: the other rate test ran on the first a few
               // seconds ago, and its bucket has not drained yet.
               AddRoute_(FirstDomain, ThirdHost, port, 3);

               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Within the rate");
               Assert.IsTrue(WaitFor_(() => remote.MessageData.Contains("Within the rate"), 20), "The first message was not delivered.");

               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Over the rate");
               Assert.IsTrue(LogHandler.DefaultLogContains("Delivery to " + ThirdHost + " deferred due to the per-destination rate limit"), "The second message was not deferred by the rate.");

               long held = HeldMessageId_(FirstDomain);

               // Several uncounted passes before the rate's window drains: each
               // retry makes the queue look again, and each look is deferred.
               for (int pass = 0; pass < 3; pass++)
               {
                  Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + held + "/retry", null).status);
                  Thread.Sleep(3000);
               }

               Assert.IsTrue(Queued_(FirstDomain).Exists(r => r.id == held && r.tries == 0), "The held message counted a try, or left the queue, during the uncounted passes.");

               DateTime deadline = DateTime.UtcNow.AddSeconds(100);
               while (DateTime.UtcNow < deadline && !remote.MessageData.Contains("Over the rate"))
               {
                  Admin("POST", "/api/v1/queue/" + held + "/retry", null);
                  Thread.Sleep(5000);
               }

               remote.WaitForCompletion();
               StringAssert.Contains("Over the rate", remote.MessageData, "The deferred message did not arrive once the rate's window had drained.");

               // One mirror copy per message: two, and still two a moment later.
               var pop3 = new Pop3ClientSimulator();
               Assert.IsTrue(WaitFor_(() => pop3.GetMessageCount(mirror.Address, "test") >= 2, 20), "The two messages were not mirrored.");
               Thread.Sleep(5000);
               Assert.AreEqual(2, pop3.GetMessageCount(mirror.Address, "test"), "An uncounted pass mirrored the held message again.");
            }
         }
         finally
         {
            _settings.MirrorEMailAddress = "";
         }
      }

      [Test]
      [Description("An uncounted pass does not repeat a rule's forward or its script function: with a server rule forwarding the second message and calling a script function, and that message deferred by the rate through several passes, the forward arrives once and the function runs once, not once per pass.")]
      public void AnUncountedPassDoesNotForwardAgain()
      {
         Account forwarded = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "forward-shaping@example.test", "test");

         Rule rule = _application.Rules.Add();
         rule.Name = "Forward the held message";
         rule.Active = true;
         rule.UseAND = true;

         RuleCriteria criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTSubject;
         criteria.MatchType = eRuleMatchType.eMTContains;
         criteria.MatchValue = "Held and forwarded";
         criteria.Save();

         // And addressed to the remote domain: the forwarded copy keeps the subject,
         // and without this the rule would forward the forward, to the rule loop limit.
         RuleCriteria outbound = rule.Criterias.Add();
         outbound.UsePredefined = true;
         outbound.PredefinedField = eRulePredefinedField.eFTRecipientList;
         outbound.MatchType = eRuleMatchType.eMTContains;
         outbound.MatchValue = FirstDomain;
         outbound.Save();

         RuleAction action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAForwardEmail;
         action.To = forwarded.Address;
         action.Save();

         // A script function too: what one does cannot be known - it may send
         // mail as the forward does - so it is gated as the forward is.
         RuleAction script = rule.Actions.Add();
         script.Type = eRuleActionType.eRARunScriptFunction;
         script.ScriptFunction = "MarkTheHeldPass";
         script.Save();

         rule.Save();

         const string ScriptMark = "held-pass-script-ran";
         File.WriteAllText(_settings.Scripting.CurrentScriptFile,
            "Sub MarkTheHeldPass(message)" + Environment.NewLine +
            " Call EventLog.Write(\"" + ScriptMark + "\")" + Environment.NewLine +
            "End Sub");
         _settings.Scripting.Enabled = true;
         _settings.Scripting.Reload();
         if (File.Exists(LogHandler.GetEventLogFileName()))
            File.Delete(LogHandler.GetEventLogFileName());

         IniFileSetting.Write("MaxOutboundPerDestinationPerMinute", "1");
         _application.Reinitialize();

         try
         {
            int port = TestSetup.GetNextFreePort();

            using (var remote = new SmtpServerSimulator(2, port))
            {
               remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
               remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
               remote.SecondsToWaitBeforeTerminate = 120;
               remote.StartListen();

               // A fourth address, whose rate bucket no other test in this fixture
               // has touched.
               AddRoute_(FirstDomain, FourthHost, port, 3);

               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Within the rate");
               Assert.IsTrue(WaitFor_(() => remote.MessageData.Contains("Within the rate"), 20), "The first message was not delivered.");

               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Held and forwarded", "Over the rate");
               Assert.IsTrue(LogHandler.DefaultLogContains("Delivery to " + FourthHost + " deferred due to the per-destination rate limit"), "The second message was not deferred by the rate.");

               long held = HeldMessageId_(FirstDomain);

               // Several uncounted passes before the rate's window drains: each
               // retry makes the queue look again, runs the rules again, and is
               // deferred again. Until 26 September 2026 each one forwarded.
               for (int pass = 0; pass < 3; pass++)
               {
                  Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + held + "/retry", null).status);
                  Thread.Sleep(3000);
               }

               Assert.IsTrue(Queued_(FirstDomain).Exists(r => r.id == held && r.tries == 0), "The held message counted a try, or left the queue, during the uncounted passes.");

               DateTime deadline = DateTime.UtcNow.AddSeconds(100);
               while (DateTime.UtcNow < deadline && !remote.MessageData.Contains("Over the rate"))
               {
                  Admin("POST", "/api/v1/queue/" + held + "/retry", null);
                  Thread.Sleep(5000);
               }

               remote.WaitForCompletion();
               StringAssert.Contains("Over the rate", remote.MessageData, "The deferred message did not arrive once the rate's window had drained.");

               // One forward for the one attempt: the first pass made it, and the
               // passes that repeated that attempt did not make it again.
               var pop3 = new Pop3ClientSimulator();
               Assert.IsTrue(WaitFor_(() => pop3.GetMessageCount(forwarded.Address, "test") >= 1, 20), "The rule did not forward the message at all.");
               Thread.Sleep(5000);
               Assert.AreEqual(1, pop3.GetMessageCount(forwarded.Address, "test"), "An uncounted pass forwarded the held message again.");

               // And one call of the function. Until 26 September 2026 the
               // forward was gated and the function was not: it ran on every pass.
               string eventLog = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
               int calls = eventLog.Split(new[] { ScriptMark }, StringSplitOptions.None).Length - 1;
               Assert.AreEqual(1, calls, "An uncounted pass ran the rule's script function again. Event log: " + eventLog);
            }
         }
         finally
         {
            rule.Delete();
         }
      }

      [Test]
      [Description("A hold is bounded by the retry schedule's span: with a one-minute window, one try a minute apart and a destination that answers 421 to every session, every held message is bounced within the schedule rather than held for as long as the windows keep doubling.")]
      public void AnUncountedHoldIsBoundedByTheRetrySchedule()
      {
         _settings.OutboundBackoffMinutes = 1;

         int port = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(6, port))
         {
            remote.ConnectReply = SlowDownAtBanner;
            for (int session = 0; session < 6; session++)
               remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.StartListen();

            // One try, a minute between tries: the schedule spans one minute.
            AddRoute_(FirstDomain, FirstHost, port, 1, false, 1);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Opens the window and is bounced at its one try");
            Assert.IsTrue(WaitForBackoff_(FirstHost), "The 421 banner did not open a window.");

            // The report the bounce writes is a new queue row, and the queue
            // looks for new rows on its own minute timer unless it is woken -
            // which is what SubmitEMail does, and what the suite's other
            // mailbox waits do while they poll.
            var pop3 = new Pop3ClientSimulator();
            DateTime firstBounce = DateTime.UtcNow.AddSeconds(30);
            while (DateTime.UtcNow < firstBounce && pop3.GetMessageCount(_account.Address, "test") < 1)
            {
               _application.SubmitEMail();
               Thread.Sleep(1000);
            }
            Assert.AreEqual(1, pop3.GetMessageCount(_account.Address, "test"), "The first message's one try was a 421, so it should have bounced at once.");

            for (int held = 0; held < 3; held++)
               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Held " + held, "Held by the window");

            Assert.IsTrue(WaitFor_(() => Queued_(FirstDomain).FindAll(r => r.tries == 0).Count == 3, 20), "The three messages were not held uncounted.");
            Assert.AreEqual(1, remote.NumberOfConnectedClients, "A held message was offered while the window was open.");

            // Only one of the three can probe when the window ends; the other
            // two would wait for a doubled window, and the next, and the next.
            // The bound is what brings them back: once queued longer than the
            // schedule's one minute, a shaping deferral is counted, and at one
            // try that is the bounce.
            DateTime deadline = DateTime.UtcNow.AddSeconds(170);
            while (DateTime.UtcNow < deadline && pop3.GetMessageCount(_account.Address, "test") < 4)
            {
               _application.SubmitEMail();
               Thread.Sleep(5000);
            }

            Assert.AreEqual(4, pop3.GetMessageCount(_account.Address, "test"), "The held messages were not all bounced within the schedule's span.");
            Assert.IsTrue(LogHandler.DefaultLogContains("longer than the 1 minute(s) its retry schedule spans"), "The bound did not say why it counted the deferral.");
            Assert.AreEqual(0, Queued_(FirstDomain).Count, "A message is still held.");
         }
      }

      // ---- the remote domain policy's two limits, which had no test ---------------

      [Test]
      [Description("The policy's messages a minute: the second message is deferred, uncounted, and the reason names the policy; another destination is untouched.")]
      public void ThePolicyPerMinuteCeilingDefersTheSecondMessageNamingThePolicy()
      {
         AddPolicy_(FirstDomain, p => p.MaxMessagesPerMinute = 1);

         int port = TestSetup.GetNextFreePort();
         int otherPort = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(1, port))
         using (var other = new SmtpServerSimulator(1, otherPort))
         {
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.StartListen();
            other.AddRecipientResult(Accepts_("test@" + SecondDomain));
            other.StartListen();

            AddRoute_(FirstDomain, FirstHost, port, 3);
            AddRoute_(SecondDomain, SecondHost, otherPort, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Within the policy");
            remote.WaitForCompletion();
            StringAssert.Contains("Within the policy", remote.MessageData);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Over the policy");
            Assert.IsTrue(LogHandler.DefaultLogContains("The remote domain policy for " + FirstDomain + " allows 1 message(s) a minute."), "The deferral does not name the policy.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"), "The deferral was counted.");
            HeldMessageId_(FirstDomain);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "Elsewhere", "Not under the policy");
            other.WaitForCompletion();
            StringAssert.Contains("Not under the policy", other.MessageData, "The policy's rate held a domain it does not name.");
         }
      }

      [Test]
      [Description("The policy's connection ceiling: with the first session held open, the second message is deferred without a second connection, the listing shows the policy's one session, and both arrive once the hold ends.")]
      public void ThePolicyConnectionCeilingHoldsTheSecondSessionWithoutConnecting()
      {
         AddPolicy_(FirstDomain, p => p.MaxConnections = 1);

         int port = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(2, port))
         {
            remote.HoldMailFromReplyFor = TimeSpan.FromSeconds(20);
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.SecondsToWaitBeforeTerminate = 60;
            remote.StartListen();

            AddRoute_(FirstDomain, FirstHost, port, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Holds the session");
            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients == 1 && ConnectionsInUse_("destinations", FirstDomain) == 1, 20),
               "The first session is not counted under the policy's name.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Waits for the slot");
            Assert.IsTrue(LogHandler.DefaultLogContains("allows 1 simultaneous connection(s), and they are all in use"), "The second message was not deferred by the ceiling.");
            Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"), "The deferral was counted.");
            Assert.AreEqual(1, remote.NumberOfConnectedClients, "A second session was opened past the ceiling.");

            long held = HeldMessageId_(FirstDomain);

            Assert.IsTrue(WaitFor_(() => remote.MessageData.Contains("Holds the session"), 40), "The held session did not complete.");
            Assert.IsTrue(WaitFor_(() => ConnectionsInUse_("destinations", FirstDomain) == 0, 10), "The slot was not released with the session.");

            Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + held + "/retry", null).status);
            remote.WaitForCompletion();
            StringAssert.Contains("Waits for the slot", remote.MessageData, "The deferred message did not arrive once the slot was free.");
         }
      }

      // ---- the server-wide ceiling and the tenant share ---------------------------

      [Test]
      [Description("MaxOutboundConnectionsPerDestination holds a destination no policy names: the same shape as the policy's ceiling, keyed by the target.")]
      public void TheServerWideConnectionCeilingAppliesWhereNoPolicyDoes()
      {
         _settings.MaxOutboundConnectionsPerDestination = 1;

         int port = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(2, port))
         {
            remote.HoldMailFromReplyFor = TimeSpan.FromSeconds(20);
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.SecondsToWaitBeforeTerminate = 60;
            remote.StartListen();

            AddRoute_(FirstDomain, FirstHost, port, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Holds the session");
            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients == 1 && ConnectionsInUse_("destinations", FirstHost) == 1, 20),
               "The first session is not counted under the destination.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Waits for the slot");
            Assert.IsTrue(LogHandler.DefaultLogContains("This server allows 1 simultaneous connection(s) to one destination, and they are all in use"), "The second message was not deferred by the server-wide ceiling.");
            Assert.AreEqual(1, remote.NumberOfConnectedClients, "A second session was opened past the ceiling.");

            long held = HeldMessageId_(FirstDomain);

            Assert.IsTrue(WaitFor_(() => remote.MessageData.Contains("Holds the session"), 40), "The held session did not complete.");
            Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + held + "/retry", null).status);
            remote.WaitForCompletion();
            StringAssert.Contains("Waits for the slot", remote.MessageData, "The deferred message did not arrive once the slot was free.");
         }
      }

      [Test]
      [Description("A policy's connection value wins over the server-wide ceiling: with the server at 1 and the policy at 2, two sessions are open at once.")]
      public void APolicyConnectionValueWinsOverTheServerWideCeiling()
      {
         _settings.MaxOutboundConnectionsPerDestination = 1;
         AddPolicy_(FirstDomain, p => p.MaxConnections = 2);

         int port = TestSetup.GetNextFreePort();

         using (var remote = new SmtpServerSimulator(2, port))
         {
            remote.HoldMailFromReplyFor = TimeSpan.FromSeconds(10);
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.AddRecipientResult(Accepts_("test@" + FirstDomain));
            remote.SecondsToWaitBeforeTerminate = 60;
            remote.StartListen();

            AddRoute_(FirstDomain, FirstHost, port, 3);

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "One of two");
            SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "Second", "Two of two");

            Assert.IsTrue(WaitFor_(() => remote.NumberOfConnectedClients == 2, 20), "The policy's two connections were not both allowed; the server-wide ceiling of one applied.");
            remote.WaitForCompletion();
            Assert.AreEqual(0, Queued_(FirstDomain).Count, "A message was deferred although the policy allows two sessions.");
         }
      }

      [Test]
      [Description("MaxOutboundConnectionsPerTenant: with one delivery of a hosted domain held open, its second is deferred while another hosted domain's goes at once; the held one arrives after the hold.")]
      public void TheTenantShareHoldsOneSendingDomainAndNotAnother()
      {
         _settings.MaxOutboundConnectionsPerTenant = 1;

         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain("second-tenant.test");

         try
         {
            Account colleague = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "colleague@example.test", "test");
            Account outsider = SingletonProvider<TestSetup>.Instance.AddAccount(second, "sender@second-tenant.test", "test");

            int heldPort = TestSetup.GetNextFreePort();
            int freePort = TestSetup.GetNextFreePort();

            using (var held = new SmtpServerSimulator(1, heldPort))
            using (var free = new SmtpServerSimulator(2, freePort))
            {
               held.HoldMailFromReplyFor = TimeSpan.FromSeconds(20);
               held.AddRecipientResult(Accepts_("test@" + FirstDomain));
               held.SecondsToWaitBeforeTerminate = 60;
               held.StartListen();

               free.AddRecipientResult(Accepts_("test@" + SecondDomain));
               free.AddRecipientResult(Accepts_("test@" + SecondDomain));
               free.SecondsToWaitBeforeTerminate = 60;
               free.StartListen();

               AddRoute_(FirstDomain, FirstHost, heldPort, 3);
               AddRoute_(SecondDomain, SecondHost, freePort, 3);

               SmtpClientSimulator.StaticSend(_account.Address, "test@" + FirstDomain, "First", "Holds the tenant's one slot");
               Assert.IsTrue(WaitFor_(() => held.NumberOfConnectedClients == 1 && ConnectionsInUse_("tenants", _domain.Name.ToLowerInvariant()) == 1, 20),
                  "The first delivery is not counted under its tenant.");

               SmtpClientSimulator.StaticSend(colleague.Address, "test@" + SecondDomain, "Colleague", "Same tenant, other destination");
               Assert.IsTrue(LogHandler.DefaultLogContains("The tenant " + _domain.Name.ToLowerInvariant() + " may have 1 deliver(y/ies) in flight"), "The second delivery of the tenant was not deferred.");
               Assert.IsTrue(LogHandler.DefaultLogContains("the attempt is not counted against the retry limit"), "The deferral was counted.");
               Assert.AreEqual(0, free.NumberOfConnectedClients, "The tenant's second delivery was made past its share.");

               SmtpClientSimulator.StaticSend(outsider.Address, "test@" + SecondDomain, "Other tenant", "Another hosted domain goes at once");
               Assert.IsTrue(WaitFor_(() => free.MessageData.Contains("Another hosted domain goes at once"), 20), "Another hosted domain was held by the first one's share.");

               long deferred = HeldMessageId_(SecondDomain);

               Assert.IsTrue(WaitFor_(() => held.MessageData.Contains("Holds the tenant's one slot"), 40), "The held session did not complete.");
               Assert.IsTrue(WaitFor_(() => ConnectionsInUse_("tenants", _domain.Name.ToLowerInvariant()) == 0, 10), "The tenant slot was not released with the session.");

               Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + deferred + "/retry", null).status);
               free.WaitForCompletion();
               StringAssert.Contains("Same tenant, other destination", free.MessageData, "The deferred delivery did not arrive once the tenant's slot was free.");
            }
         }
         finally
         {
            second.Delete();
         }
      }

      [Test]
      [Description("Mail relayed for senders this server does not host shares the one tenant (external): the second relayed message is deferred while a hosted sender's goes at once.")]
      public void RelayedMailFromOutsideSharesOneTenant()
      {
         _settings.MaxOutboundConnectionsPerTenant = 1;

         int heldPort = TestSetup.GetNextFreePort();
         int freePort = TestSetup.GetNextFreePort();

         using (var held = new SmtpServerSimulator(1, heldPort))
         using (var free = new SmtpServerSimulator(2, freePort))
         {
            held.HoldMailFromReplyFor = TimeSpan.FromSeconds(20);
            held.AddRecipientResult(Accepts_("test@" + FirstDomain));
            held.SecondsToWaitBeforeTerminate = 60;
            held.StartListen();

            free.AddRecipientResult(Accepts_("test@" + SecondDomain));
            free.AddRecipientResult(Accepts_("test@" + SecondDomain));
            free.SecondsToWaitBeforeTerminate = 60;
            free.StartListen();

            // Treated as local, so that a sender from outside may deliver to them
            // without authenticating: the backup-MX shape.
            AddRoute_(FirstDomain, FirstHost, heldPort, 3, true);
            AddRoute_(SecondDomain, SecondHost, freePort, 3, true);

            SmtpClientSimulator.StaticSend("someone@outside.example", "test@" + FirstDomain, "Relayed", "Holds the outsiders' one slot");
            Assert.IsTrue(WaitFor_(() => held.NumberOfConnectedClients == 1 && ConnectionsInUse_("tenants", "(external)") == 1, 20),
               "A relayed delivery is not counted under (external).");

            SmtpClientSimulator.StaticSend("else@elsewhere.example", "test@" + SecondDomain, "Relayed too", "Shares the one slot");
            Assert.IsTrue(LogHandler.DefaultLogContains("The tenant (external) may have 1 deliver(y/ies) in flight"), "The second relayed delivery was not deferred.");
            Assert.AreEqual(0, free.NumberOfConnectedClients, "The second relayed delivery was made past the outsiders' share.");

            SmtpClientSimulator.StaticSend(_account.Address, "test@" + SecondDomain, "Hosted", "A hosted sender is its own tenant");
            Assert.IsTrue(WaitFor_(() => free.MessageData.Contains("A hosted sender is its own tenant"), 20), "A hosted sender was held by the outsiders' share.");

            long deferred = HeldMessageId_(SecondDomain);

            Assert.IsTrue(WaitFor_(() => held.MessageData.Contains("Holds the outsiders' one slot"), 40), "The held session did not complete.");
            Assert.AreEqual(200, Admin("POST", "/api/v1/queue/" + deferred + "/retry", null).status);
            free.WaitForCompletion();
            StringAssert.Contains("Shares the one slot", free.MessageData, "The deferred relayed delivery did not arrive once the slot was free.");
         }
      }

      // ---- the settings -------------------------------------------------------

      [Test]
      [Description("The four settings round-trip over REST and COM, and refuse a ceiling below the base, a negative, a value past the range and a non-number with a sentence, changing nothing.")]
      public void TheFourSettingsRoundTripAndRefuseBadValues()
      {
         (int status, string body) put = Admin("PUT", "/api/v1/settings",
            "{\"outbound_backoff_max_minutes\":90,\"outbound_backoff_minutes\":5,\"max_outbound_connections_per_destination\":3,\"max_outbound_connections_per_tenant\":4}");
         Assert.AreEqual(200, put.status, put.body);

         (int status, string body) get = Admin("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, get.status, get.body);
         JsonElement settings = JsonDocument.Parse(get.body).RootElement;
         Assert.AreEqual(5, settings.GetProperty("outbound_backoff_minutes").GetInt32());
         Assert.AreEqual(90, settings.GetProperty("outbound_backoff_max_minutes").GetInt32());
         Assert.AreEqual(3, settings.GetProperty("max_outbound_connections_per_destination").GetInt32());
         Assert.AreEqual(4, settings.GetProperty("max_outbound_connections_per_tenant").GetInt32());

         // COM reads what REST wrote.
         Assert.AreEqual(5, _settings.OutboundBackoffMinutes);
         Assert.AreEqual(90, _settings.OutboundBackoffMaxMinutes);
         Assert.AreEqual(3, _settings.MaxOutboundConnectionsPerDestination);
         Assert.AreEqual(4, _settings.MaxOutboundConnectionsPerTenant);

         (int status, string body) belowBase = Admin("PUT", "/api/v1/settings", "{\"outbound_backoff_max_minutes\":4}");
         Assert.AreEqual(400, belowBase.status, belowBase.body);
         StringAssert.Contains("must not be below outbound_backoff_minutes", belowBase.body);

         // And the mirror image: a base above the ceiling, refused the same
         // way, with GET answering the ceiling the row holds and no other.
         (int status, string body) aboveCeiling = Admin("PUT", "/api/v1/settings", "{\"outbound_backoff_minutes\":91}");
         Assert.AreEqual(400, aboveCeiling.status, aboveCeiling.body);
         StringAssert.Contains("must not be above outbound_backoff_max_minutes", aboveCeiling.body);
         Assert.AreEqual(90, JsonDocument.Parse(Admin("GET", "/api/v1/settings", null).body).RootElement.GetProperty("outbound_backoff_max_minutes").GetInt32(),
            "GET does not answer the stored ceiling.");

         (int status, string body) negative = Admin("PUT", "/api/v1/settings", "{\"outbound_backoff_minutes\":-1}");
         Assert.AreEqual(400, negative.status, negative.body);
         StringAssert.Contains("between 0 and 10080", negative.body);

         (int status, string body) past = Admin("PUT", "/api/v1/settings", "{\"max_outbound_connections_per_destination\":1001}");
         Assert.AreEqual(400, past.status, past.body);
         StringAssert.Contains("between 0 and 1000", past.body);

         (int status, string body) notANumber = Admin("PUT", "/api/v1/settings", "{\"max_outbound_connections_per_tenant\":\"many\"}");
         Assert.AreEqual(400, notANumber.status, notANumber.body);

         // Nothing changed.
         Assert.AreEqual(90, _settings.OutboundBackoffMaxMinutes);
         Assert.AreEqual(5, _settings.OutboundBackoffMinutes);
         Assert.AreEqual(3, _settings.MaxOutboundConnectionsPerDestination);
         Assert.AreEqual(4, _settings.MaxOutboundConnectionsPerTenant);

         // COM refuses the same values with a sentence.
         var ceiling = Assert.Throws<System.Runtime.InteropServices.COMException>(() => _settings.OutboundBackoffMaxMinutes = 2);
         StringAssert.Contains("must not be below OutboundBackoffMinutes", ceiling.Message);
         var baseAbove = Assert.Throws<System.Runtime.InteropServices.COMException>(() => _settings.OutboundBackoffMinutes = 91);
         StringAssert.Contains("must not be above OutboundBackoffMaxMinutes", baseAbove.Message);
         var tenant = Assert.Throws<System.Runtime.InteropServices.COMException>(() => _settings.MaxOutboundConnectionsPerTenant = -1);
         StringAssert.Contains("between 0 and 1000", tenant.Message);
         Assert.Throws<System.Runtime.InteropServices.COMException>(() => _settings.OutboundBackoffMinutes = 10081);

         _settings.MaxOutboundConnectionsPerDestination = 7;
         Assert.AreEqual(7, _settings.MaxOutboundConnectionsPerDestination);
         Assert.AreEqual(7, JsonDocument.Parse(Admin("GET", "/api/v1/settings", null).body).RootElement.GetProperty("max_outbound_connections_per_destination").GetInt32(),
            "REST does not read what COM wrote.");
      }
   }
}
