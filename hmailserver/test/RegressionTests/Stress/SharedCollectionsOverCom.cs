// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Stress
{
   /// <summary>
   ///    The items of the running server's shared configuration collections, changed over COM
   ///    and saved again and again while other threads of the server read them (COM/COMCollection.h,
   ///    "A change to an item of a shared collection is written onto a copy"). Until 24 September
   ///    2026 COM's put_* wrote the fields of the very object a session was reading: a string
   ///    assigned while a session copied it freed the buffer being copied - the heap-corruption
   ///    abort's use-after-free, one level below the vector - and a record written a field at a
   ///    time handed a reader half of each.
   ///
   ///    Three races of thirty seconds. Each item is flipped over COM between two whole states,
   ///    every field of one state put on an object read from the collection, with a pause after
   ///    each put, and then saved. In the route race the webmail's send route reads the routes as
   ///    a session does, resolving an address past every one of them (RecipientParser's walk), and
   ///    the REST listing of the routes reads every field of the running route on a listener
   ///    thread; in the other two the REST listings are the only readers - the sessions' own
   ///    readers of those lists (the DNS black list and SURBL tests, the relay check, delivery's
   ///    policy lookup, the virus scanner, the notices, IMAP's ACL commands) are not raced here.
   ///    Every listing must show each item in one state or the other, never a mix, and the
   ///    process must be the one that started, the crash oracle clean and the error log empty.
   ///
   ///    The mix is the part a race can see. Against the old code, where each put wrote the
   ///    running item, a listing taken during a write shows the fields already put beside the
   ///    ones not yet put, and the pause after each put makes that most of every write, so the
   ///    route, the two lists, the relay, the policy and the blocked attachment each fail it
   ///    within a few listings - by reasoning: not run against the old code. On this code the
   ///    puts go to the object's own copy and the pause changes nothing a reader sees. The group
   ///    and the server message have one field each, a mix of two states cannot exist for them,
   ///    and their races are survival tests only, as the use-after-free is for all of them: it is
   ///    caught only when a read and a write meet in the same few instructions, so, like
   ///    Stress.SharedCollections, every run of the Windows suite takes another chance at it.
   ///
   ///    Through REST and COM only, nothing over SMTP or IMAP from this machine, where a proxying
   ///    virus scanner has been known to sit on the loopback ports.
   /// </summary>
   [TestFixture]
   public class SharedCollectionsOverCom : TestFixtureBase
   {
      private static int RestPort = 10170;
      private const string AdminPassword = "testar";
      private const string Password = "Shared-Collections-Over-Com-Passw0rd!";

      private static readonly TimeSpan RaceDuration = TimeSpan.FromSeconds(30);

      // The REST listener allows one credential 200 requests in ten seconds, and every
      // listing below is the administrator's: the listers together stay well under it. A 429
      // all the same is waited out and counted, never taken for a fault.
      private static readonly TimeSpan ListerSpacing = TimeSpan.FromMilliseconds(300);
      private static readonly TimeSpan SenderSpacing = TimeSpan.FromMilliseconds(250);

      // Between two COM saves: each one reads its whole collection again from the database.
      private static readonly TimeSpan WriterSpacing = TimeSpan.FromMilliseconds(100);

      // After each put_* of a multi-field state, so that on the old code, where the put
      // wrote the running item, a listing during a write shows a mix (the summary above).
      private static readonly TimeSpan PutSpacing = TimeSpan.FromMilliseconds(20);

      private const int SenderCount = 3;
      private const int StandingRouteCount = 30;
      private const string Suffix = ".copies-over-com.test";
      private const string TargetDomain = "zz-target" + Suffix;

      private string compromiseModeBefore_;

      // A server message a race changed, and its text before the race: put back in the
      // TearDown over REST, which answers on the same port after a restart the service
      // manager made, when the fixture's COM objects belong to the process that died.
      private string messageToRestore_;
      private string messageTextBefore_;

      private sealed class Race
      {
         private readonly List<string> failures_ = new List<string>();
         public int Rounds;
         public int Refused;
         public int Throttled;
         public volatile bool Stopped;

         public void Fail(string failure)
         {
            lock (failures_)
               failures_.Add(failure);
            Stopped = true;
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

      /// <summary>
      ///    One item raced: where the REST listing shows it, which of its members are
      ///    compared, the two states it is flipped between, and how COM writes one of them.
      /// </summary>
      private sealed class Subject
      {
         public string Noun;
         public string ListPath;
         public long Id;
         public string[] Fields;
         public string[][] States;
         public Action<int> WriteOverCom;
         public int Reads;
         public int LastWritten;
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         // Read again after the reinitialise: the one read before belongs to the
         // configuration it replaced.
         _settings = _application.Settings;

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         // Three accounts sending over and over is the shape of an account takeover, and
         // watch mode would raise the alert for it. Off for the race, and put back.
         (int status, string body) settings = Admin("GET", "/api/v1/settings", null);
         Assert.AreEqual(200, settings.status, "GET /api/v1/settings: " + settings.body);
         compromiseModeBefore_ = MiniJson.Text(MiniJson.Object(settings.body), "compromise_detection_mode");
         Assert.IsNotNull(compromiseModeBefore_, "The settings carry no compromise_detection_mode: " + settings.body);

         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"off\"}").status);
      }

      [TearDown]
      public void StopRestApi()
      {
         if (messageToRestore_ != null)
         {
            try
            {
               (int status, string body) restored = Admin("PUT", "/api/v1/settings/messages/" + messageToRestore_,
                  "{\"text\":" + JsonText(messageTextBefore_) + "}");
               if (restored.status == 200)
                  messageToRestore_ = null;
               else
                  Console.WriteLine("The server message " + messageToRestore_ + " could not be put back over REST: " + restored.status + " " + restored.body);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               Console.WriteLine("The server message " + messageToRestore_ + " could not be put back over REST: " + ex.Message);
            }
         }

         try
         {
            if (compromiseModeBefore_ != null)
            {
               (int status, string body) restored = Admin("PUT", "/api/v1/settings", "{\"compromise_detection_mode\":\"" + compromiseModeBefore_ + "\"}");
               if (restored.status != 200)
                  Console.WriteLine("compromise_detection_mode could not be put back to " + compromiseModeBefore_ + ": " + restored.status + " " + restored.body);
            }

            // PerformBasicSetup removes the routes, the relays, the lists and the groups
            // before the next test; this is for the run whose last test this was.
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
            _settings = _application.Settings;
         }
      }

      [Test]
      [Description("A route is renamed and re-pointed over COM, every field put on a copy and saved, for thirty seconds while three accounts resolve an unlisted address through the send route past thirty-one others and the REST listing reads every field of it; each resolution is refused as not in the route list, every listing shows the route whole in one state or the other, and the process, the crash oracle and the error log come through clean.")]
      public void ARouteWrittenOverComWhileSessionsReadItStaysWhole()
      {
         var senders = new List<Account>();
         for (int i = 0; i < SenderCount; i++)
            senders.Add(SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "over-com-" + i + "@" + _domain.Name, Password));

         for (int i = 0; i < StandingRouteCount; i++)
            AddRoute(string.Format(CultureInfo.InvariantCulture, "standing-{0:00}{1}", i, Suffix), 2525, "", 1);

         // Last by name, and taking only the address it lists: an address it does not list is
         // refused at resolution, after RecipientParser has walked every other route.
         Route target = AddRoute(TargetDomain, 2525, "", 1);
         RouteAddress listed = target.Addresses.Add();
         listed.Address = "listed@" + TargetDomain;
         listed.Save();

         string[][] states =
         {
            new[] { "churn-a" + Suffix, "127.0.0.1", "2525", "state a", "1" },
            new[] { "churn-b" + Suffix, "127.0.0.2", "2526", "state b", "2" },
         };

         Route churn = AddRoute(states[0][0], 2525, "state a", 1);
         long churnId = churn.ID;

         var subject = new Subject
         {
            Noun = "The route",
            ListPath = "/api/v1/routes",
            Id = churnId,
            Fields = new[] { "domain_name", "target_smtp_host", "target_smtp_port", "description", "number_of_tries" },
            States = states,
            WriteOverCom = state =>
            {
               Route route = _settings.Routes.get_ItemByDBID((int) churnId);
               route.DomainName = states[state][0];
               BetweenPuts();
               route.TargetSMTPHost = states[state][1];
               BetweenPuts();
               route.TargetSMTPPort = int.Parse(states[state][2], CultureInfo.InvariantCulture);
               BetweenPuts();
               route.Description = states[state][3];
               BetweenPuts();
               route.NumberOfTries = int.Parse(states[state][4], CultureInfo.InvariantCulture);
               BetweenPuts();
               route.Save();
            },
         };

         // Before the race, so that a refusal inside it is the race's.
         (int status, string body) probe = Send("POST", "/api/v1/me/messages", Basic(senders[0].Address, Password), UnlistedMessage(0, 0));
         Assert.AreEqual(400, probe.status, "The send route should refuse an address the route does not list: " + probe.body);
         StringAssert.Contains("not in route list", probe.body);

         var race = new Race();
         DateTime deadline = DateTime.UtcNow + RaceDuration;
         var readers = new List<Thread> { new Thread(() => ReadWhole(subject, deadline, race)) };
         for (int i = 0; i < SenderCount; i++)
         {
            int index = i;
            string address = senders[i].Address;
            readers.Add(new Thread(() => ResolveUnlisted(index, address, deadline, race)));
         }

         Churn(new List<Subject> { subject }, readers, deadline, race);

         TestContext.WriteLine("Route race: {0} COM saves, {1} listings read whole, {2} resolutions refused as they should be, {3} requests throttled, over {4} with {5} standing routes.",
            race.Rounds, subject.Reads, race.Refused, race.Throttled, RaceDuration, StandingRouteCount);

         AssertTheServerCameThrough(race);

         Assert.Greater(race.Rounds, 0, "The route was never written, so nothing was raced.");
         Assert.Greater(subject.Reads, 0, "The route was never listed, so nothing was raced.");
         Assert.Greater(race.Refused, 0, "Nothing was resolved, so nothing was raced.");
         Assert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "The race queued nothing.");

         AssertLastWritten(subject);
      }

      [Test]
      [Description("A DNS black list, a SURBL server, an incoming relay and a remote domain policy are flipped over COM between two whole states, every field put on a copy and saved, for thirty seconds while the REST listings read them; every listing shows each item whole in one state or the other, and the process, the crash oracle and the error log come through clean.")]
      public void AntiSpamListsRelaysAndPoliciesWrittenOverComStayWhole()
      {
         string[][] dnsblStates =
         {
            new[] { "dnsbl-a" + Suffix, "Listed by a", "127.0.0.2", "3" },
            new[] { "dnsbl-b" + Suffix, "Listed by b", "127.0.0.4", "7" },
         };
         DNSBlackList dnsbl = _settings.AntiSpam.DNSBlackLists.Add();
         dnsbl.Active = false;
         WriteDnsbl(dnsbl, dnsblStates[0]);
         long dnsblId = dnsbl.ID;

         string[][] surblStates =
         {
            new[] { "surbl-a" + Suffix, "Listed by a", "127.0.0.2", "4" },
            new[] { "surbl-b" + Suffix, "Listed by b", "127.0.0.8", "9" },
         };
         SURBLServer surbl = _settings.AntiSpam.SURBLServers.Add();
         surbl.Active = false;
         WriteSurbl(surbl, surblStates[0]);
         long surblId = surbl.ID;

         string[][] relayStates =
         {
            new[] { "copies-relay-a", "10.81.0.1", "10.81.0.9" },
            new[] { "copies-relay-b", "10.82.0.1", "10.82.0.9" },
         };
         IncomingRelay relay = _settings.IncomingRelays.Add();
         WriteRelay(relay, relayStates[0]);
         long relayId = relay.ID;

         string[][] policyStates =
         {
            new[] { "policy-a" + Suffix, "state a", "2", "30" },
            new[] { "policy-b" + Suffix, "state b", "5", "60" },
         };
         RemoteDomainPolicy policy = _settings.RemoteDomainPolicies.Add();
         policy.Active = true;
         WritePolicy(policy, policyStates[0]);
         long policyId = policy.ID;

         try
         {
            var subjects = new List<Subject>
            {
               new Subject
               {
                  Noun = "The DNS black list", ListPath = "/api/v1/dns-blacklists", Id = dnsblId,
                  Fields = new[] { "dns_host", "reject_message", "expected_result", "score" }, States = dnsblStates,
                  WriteOverCom = state => WriteDnsbl(_settings.AntiSpam.DNSBlackLists.get_ItemByDBID((int) dnsblId), dnsblStates[state]),
               },
               new Subject
               {
                  Noun = "The SURBL server", ListPath = "/api/v1/surbl-servers", Id = surblId,
                  Fields = new[] { "dns_host", "reject_message", "expected_result", "score" }, States = surblStates,
                  WriteOverCom = state => WriteSurbl(_settings.AntiSpam.SURBLServers.get_ItemByDBID((int) surblId), surblStates[state]),
               },
               new Subject
               {
                  Noun = "The incoming relay", ListPath = "/api/v1/incoming-relays", Id = relayId,
                  Fields = new[] { "name", "lower_ip", "upper_ip" }, States = relayStates,
                  WriteOverCom = state => WriteRelay(_settings.IncomingRelays.get_ItemByDBID((int) relayId), relayStates[state]),
               },
               new Subject
               {
                  Noun = "The remote domain policy", ListPath = "/api/v1/remote-domains", Id = policyId,
                  Fields = new[] { "domain_name", "description", "max_connections", "max_messages_per_minute" }, States = policyStates,
                  WriteOverCom = state => WritePolicy(_settings.RemoteDomainPolicies.get_ItemByDBID((int) policyId), policyStates[state]),
               },
            };

            RaceAll(subjects, "Lists, relay and policy");
         }
         finally
         {
            // PerformBasicSetup removes the lists and the relays; nothing removes a policy.
            // Guarded: after a crash the service manager restarted from, these COM objects
            // belong to the process that died, and an exception here would replace the
            // crash oracle's account of the crash with "The RPC server is unavailable".
            Tidy("the remote domain policy", () => _settings.RemoteDomainPolicies.DeleteByDBID((int) policyId));
            Tidy("the SURBL server", () => _settings.AntiSpam.SURBLServers.DeleteByDBID((int) surblId));
         }
      }

      [Test]
      [Description("An IMAP group, a blocked attachment and a server message - the collections the sweep of the COM classes added to the row - are flipped over COM between two whole states for thirty seconds while the REST listings read them; every listing shows each item whole in one state or the other, and the process, the crash oracle and the error log come through clean.")]
      public void GroupsAttachmentsAndMessagesWrittenOverComStayWhole()
      {
         string[][] groupStates = { new[] { "CopiesOverComA" }, new[] { "CopiesOverComB" } };
         hMailServer.Group group = _settings.Groups.Add();
         group.Name = groupStates[0][0];
         group.Save();
         long groupId = group.ID;

         string[][] attachmentStates =
         {
            new[] { "*.copies-over-com-a", "state a" },
            new[] { "*.copies-over-com-b", "state b" },
         };
         BlockedAttachment attachment = _settings.AntiVirus.BlockedAttachments.Add();
         WriteAttachment(attachment, attachmentStates[0]);
         long attachmentId = attachment.ID;

         const string messageName = "VIRUS_ATTACHMENT_REMOVED";
         ServerMessage message = _settings.ServerMessages.get_ItemByName(messageName);
         string originalText = message.Text;
         long messageId = message.ID;

         // Put back by the TearDown, over REST, whatever happens here.
         messageToRestore_ = messageName;
         messageTextBefore_ = originalText;
         string[][] messageStates =
         {
            new[] { "Removed, as state a says." },
            new[] { "Removed, as state b says, at rather more length than state a does." },
         };
         message.Text = messageStates[0][0];
         message.Save();

         try
         {
            var subjects = new List<Subject>
            {
               new Subject
               {
                  Noun = "The group", ListPath = "/api/v1/groups", Id = groupId,
                  Fields = new[] { "name" }, States = groupStates,
                  WriteOverCom = state =>
                  {
                     hMailServer.Group written = _settings.Groups.get_ItemByDBID((int) groupId);
                     written.Name = groupStates[state][0];
                     written.Save();
                  },
               },
               new Subject
               {
                  Noun = "The blocked attachment", ListPath = "/api/v1/blocked-attachments", Id = attachmentId,
                  Fields = new[] { "wildcard", "description" }, States = attachmentStates,
                  WriteOverCom = state => WriteAttachment(_settings.AntiVirus.BlockedAttachments.get_ItemByDBID((int) attachmentId), attachmentStates[state]),
               },
               new Subject
               {
                  Noun = "The server message", ListPath = "/api/v1/settings/messages", Id = messageId,
                  Fields = new[] { "text" }, States = messageStates,
                  WriteOverCom = state =>
                  {
                     ServerMessage written = _settings.ServerMessages.get_ItemByDBID((int) messageId);
                     written.Text = messageStates[state][0];
                     written.Save();
                  },
               },
            };

            RaceAll(subjects, "Group, attachment and message");
         }
         finally
         {
            // Guarded, for the reason the policy's delete is; the message text is put back
            // by the TearDown in any case, over REST, which a restarted service answers.
            Tidy("the blocked attachment", () => _settings.AntiVirus.BlockedAttachments.DeleteByDBID((int) attachmentId));
         }
      }

      private void RaceAll(List<Subject> subjects, string name)
      {
         var race = new Race();
         DateTime deadline = DateTime.UtcNow + RaceDuration;
         var readers = new List<Thread>();
         foreach (Subject subject in subjects)
         {
            Subject read = subject;
            readers.Add(new Thread(() => ReadWhole(read, deadline, race)));
         }

         Churn(subjects, readers, deadline, race);

         var reads = new StringBuilder();
         foreach (Subject subject in subjects)
            reads.AppendFormat(CultureInfo.InvariantCulture, "{0}{1} {2}", reads.Length > 0 ? ", " : "", subject.Reads, subject.ListPath);

         TestContext.WriteLine("{0} race: {1} rounds of COM saves ({2} items each), listings read whole: {3}; {4} requests throttled, over {5}.",
            name, race.Rounds, subjects.Count, reads, race.Throttled, RaceDuration);

         AssertTheServerCameThrough(race);

         Assert.Greater(race.Rounds, 0, "Nothing was written, so nothing was raced.");
         foreach (Subject subject in subjects)
         {
            Assert.Greater(subject.Reads, 0, subject.Noun + " was never listed, so nothing was raced.");
            AssertLastWritten(subject);
         }
      }

      /// <summary>
      ///    The readers on threads of their own, the writer on this one - the COM objects
      ///    belong to it - flipping every subject to the other state, one save at a time,
      ///    until the deadline or the first failure.
      /// </summary>
      private void Churn(List<Subject> subjects, List<Thread> readers, DateTime deadline, Race race)
      {
         foreach (Thread reader in readers)
         {
            reader.IsBackground = true;
            reader.Start();
         }

         var pacer = new Pacer(WriterSpacing);
         int state = 1;

         try
         {
            while (DateTime.UtcNow < deadline && !race.Stopped)
            {
               foreach (Subject subject in subjects)
               {
                  pacer.Wait();
                  subject.WriteOverCom(state);
                  subject.LastWritten = state;
               }

               state = 1 - state;
               Interlocked.Increment(ref race.Rounds);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The COM writer stopped: " + ex.Message);
         }
         finally
         {
            foreach (Thread reader in readers)
               reader.Join();
         }
      }

      private static void ReadWhole(Subject subject, DateTime deadline, Race race)
      {
         var pacer = new Pacer(ListerSpacing);
         try
         {
            while (DateTime.UtcNow < deadline && !race.Stopped)
            {
               pacer.Wait();
               (int status, string body) listed = Admin("GET", subject.ListPath, null);

               if (listed.status == 429)
               {
                  Interlocked.Increment(ref race.Throttled);
                  Thread.Sleep(1000);
                  continue;
               }

               if (listed.status != 200)
               {
                  race.Fail("GET " + subject.ListPath + " answered " + listed.status + ": " + listed.body);
                  return;
               }

               string[] seen = FieldsOf(listed.body, subject.Id, subject.Fields);
               if (seen == null)
               {
                  race.Fail(subject.Noun + " (id " + subject.Id + ") was not in the listing: " + listed.body);
                  return;
               }

               if (!Same(seen, subject.States[0]) && !Same(seen, subject.States[1]))
               {
                  race.Fail(subject.Noun + " was listed half-written: " + string.Join(" | ", seen) +
                     " is neither " + string.Join(" | ", subject.States[0]) + " nor " + string.Join(" | ", subject.States[1]) + ".");
                  return;
               }

               Interlocked.Increment(ref subject.Reads);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("The reader of " + subject.ListPath + " stopped: " + ex.Message);
         }
      }

      private static void ResolveUnlisted(int index, string address, DateTime deadline, Race race)
      {
         var pacer = new Pacer(SenderSpacing);
         string me = Basic(address, Password);
         int attempt = 1;

         try
         {
            while (DateTime.UtcNow < deadline && !race.Stopped)
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
                  return;
               }

               Interlocked.Increment(ref race.Refused);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            race.Fail("Sender " + address + " stopped: " + ex.Message);
         }
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

      // What the listing shows once the race is over is the last state COM saved.
      private static void AssertLastWritten(Subject subject)
      {
         (int status, string body) listed = Admin("GET", subject.ListPath, null);
         Assert.AreEqual(200, listed.status, listed.body);

         string[] seen = FieldsOf(listed.body, subject.Id, subject.Fields);
         Assert.IsNotNull(seen, subject.Noun + " is not in the listing: " + listed.body);
         Assert.IsTrue(Same(seen, subject.States[subject.LastWritten]),
            subject.Noun + " does not show the state saved last: " + string.Join(" | ", seen));
      }

      private Route AddRoute(string domainName, int port, string description, int tries)
      {
         // This machine: nothing a race does is delivered through a route, and a route
         // pointing anywhere else would be a bad thing to leave behind.
         Route route = _settings.Routes.Add();
         route.DomainName = domainName;
         route.Description = description;
         route.TargetSMTPHost = "127.0.0.1";
         route.TargetSMTPPort = port;
         route.NumberOfTries = tries;
         route.MinutesBetweenTry = 5;
         route.AllAddresses = false;
         route.TreatRecipientAsLocalDomain = false;
         route.TreatSenderAsLocalDomain = false;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
         return route;
      }

      private static void WriteDnsbl(DNSBlackList list, string[] state)
      {
         list.DNSHost = state[0];
         BetweenPuts();
         list.RejectMessage = state[1];
         BetweenPuts();
         list.ExpectedResult = state[2];
         BetweenPuts();
         list.Score = int.Parse(state[3], CultureInfo.InvariantCulture);
         BetweenPuts();
         list.Save();
      }

      private static void WriteSurbl(SURBLServer server, string[] state)
      {
         server.DNSHost = state[0];
         BetweenPuts();
         server.RejectMessage = state[1];
         BetweenPuts();
         server.ExpectedResult = state[2];
         BetweenPuts();
         server.Score = int.Parse(state[3], CultureInfo.InvariantCulture);
         BetweenPuts();
         server.Save();
      }

      private static void WriteRelay(IncomingRelay relay, string[] state)
      {
         relay.Name = state[0];
         BetweenPuts();
         relay.LowerIP = state[1];
         BetweenPuts();
         relay.UpperIP = state[2];
         BetweenPuts();
         relay.Save();
      }

      private static void WritePolicy(RemoteDomainPolicy policy, string[] state)
      {
         policy.DomainName = state[0];
         BetweenPuts();
         policy.Description = state[1];
         BetweenPuts();
         policy.MaxConnections = int.Parse(state[2], CultureInfo.InvariantCulture);
         BetweenPuts();
         policy.MaxMessagesPerMinute = int.Parse(state[3], CultureInfo.InvariantCulture);
         BetweenPuts();
         policy.Save();
      }

      private static void WriteAttachment(BlockedAttachment attachment, string[] state)
      {
         attachment.Wildcard = state[0];
         BetweenPuts();
         attachment.Description = state[1];
         BetweenPuts();
         attachment.Save();
      }

      // The pause after each put of a multi-field state (PutSpacing).
      private static void BetweenPuts()
      {
         Thread.Sleep(PutSpacing);
      }

      // A clean-up that must not replace the failure a test is reporting.
      private static void Tidy(string what, Action action)
      {
         try
         {
            action();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            Console.WriteLine("Could not remove " + what + " after the race: " + ex.Message);
         }
      }

      // A string as a JSON string literal, for the one body here that carries text the
      // server supplied.
      private static string JsonText(string value)
      {
         var json = new StringBuilder("\"");
         foreach (char c in value ?? "")
         {
            switch (c)
            {
               case '"': json.Append("\\\""); break;
               case '\\': json.Append("\\\\"); break;
               case '\n': json.Append("\\n"); break;
               case '\r': json.Append("\\r"); break;
               case '\t': json.Append("\\t"); break;
               default:
                  if (c < 0x20)
                     json.AppendFormat(CultureInfo.InvariantCulture, "\\u{0:x4}", (int) c);
                  else
                     json.Append(c);
                  break;
            }
         }
         return json.Append('"').ToString();
      }

      // The listed item's members, as text, in the order asked for; null when the listing
      // has no item of that id.
      private static string[] FieldsOf(string listing, long id, string[] fields)
      {
         var entries = MiniJson.Parse(listing) as List<object>;
         if (entries == null)
            return null;

         foreach (object entry in entries)
         {
            var item = entry as Dictionary<string, object>;
            if (item == null)
               continue;

            object itemId;
            if (!item.TryGetValue("id", out itemId) || !(itemId is double) || (long) (double) itemId != id)
               continue;

            var seen = new string[fields.Length];
            for (int i = 0; i < fields.Length; i++)
            {
               object value;
               seen[i] = TextOf(item.TryGetValue(fields[i], out value) ? value : null);
            }

            return seen;
         }

         return null;
      }

      private static string TextOf(object value)
      {
         if (value == null)
            return "(absent)";
         if (value is double)
            return ((long) (double) value).ToString(CultureInfo.InvariantCulture);
         if (value is bool)
            return (bool) value ? "true" : "false";
         return value.ToString();
      }

      private static bool Same(string[] seen, string[] state)
      {
         if (seen.Length != state.Length)
            return false;

         for (int i = 0; i < seen.Length; i++)
         {
            if (!string.Equals(seen[i], state[i], StringComparison.Ordinal))
               return false;
         }

         return true;
      }

      private static string UnlistedMessage(int index, int attempt)
      {
         return "{\"to\":\"unlisted@" + TargetDomain + "\",\"subject\":\"Over COM " + index + "." + attempt + "\",\"text\":\"Never queued.\"}";
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
