// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    An upgrade nobody has to book a window for (docs/Upgrading.md, "An upgrade
   ///    without a window"): maintenance mode - a drain without a stop - over
   ///    POST, GET and DELETE /api/v1/server/drain; the stop that closes the
   ///    doors before it waits; and the delivery pause that lets a session in
   ///    flight finish and starts no other. Each with its negative control.
   ///
   ///    The message a drain must hold is queued over the webmail's send route
   ///    (POST /api/v1/me/messages), which writes into the delivery queue in
   ///    process: the one way to hand the queue a message while the SMTP listener
   ///    is closed, the path a person in the webmail takes during a drain, and the
   ///    same call on the Windows bench and the Linux one.
   ///
   ///    What is deliberately NOT here: [Database] UpgradeOnStart end to end. It
   ///    would need a second database at an older schema, and on this bench the
   ///    only way to make one in-process - Database.CreateInternalDatabase - writes
   ///    the bench's own hMailServer.sdf. The walk it runs is SchemaUpgrader, which
   ///    the internal suite (MainOperations.RunTestSuite -> SchemaUpgraderTester)
   ///    proves on a Compact database of its own with a chain of its own: N-1 to N
   ///    one step at a time, N untouched, a failing statement leaving the version
   ///    where it was and naming the step.
   ///
   ///    The listener is the fixture's own, on the first port of the range this
   ///    row was allocated (9866-9875); the metrics listener takes the second and
   ///    the ManageSieve listener, for the one test that opens it, the third. The
   ///    service is the bench's, so every test that drains or stops it puts it back
   ///    Running in its own finally as well as in the teardown, which reinitialises
   ///    whatever a failure left.
   /// </summary>
   [TestFixture]
   public class DrainAndUpgrade : TestFixtureBase
   {
      private static int RestPort = 9866;
      private const int MetricsPort = 9867;
      private const int ManageSievePort = 9868;
      private const string AdminPassword = "testar";
      private const string UserPassword = "test";

      // hStateDraining in hMailServer.idl. Compared as the number so that the
      // fixture builds against an interop generated before the enum gained it.
      private const int StateDraining = 5;
      private const int StateRunning = 3;

      private const string RemoteDomain = "dummy-example.com";

      private Account _account;

      [SetUp]
      public void StartListeners()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "drain-user@example.test", UserPassword);

         IniFileSetting.Write("MetricsServerPort", MetricsPort.ToString());
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void PutTheServerBack()
      {
         try
         {
            // Whatever a failed test left: a drain, a stopped server, a drain
            // window, a delivery-thread ceiling, a ManageSieve listener, a route,
            // queued messages.
            try { Admin("DELETE", "/api/v1/server/drain", null); } catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { }
            try { if ((int) _application.ServerState != StateRunning) _application.Start(); } catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { }

            IniFileSetting.Write("ShutdownDrainSeconds", "0");
            IniFileSetting.Write("ManageSieveServerPort", "0");
            _settings.MaxDeliveryThreads = 50;
            DeleteRoutes_();
            TestSetup.DeleteMessagesInQueue();
         }
         finally
         {
            IniFileSetting.Write("MetricsServerPort", "0");
            RestListener.Stop();
            // Stops whatever is left, starts everything afresh, unpaused, with the
            // settings above read: the one call that ends Running whatever happened.
            _application.Reinitialize();
         }
      }

      // ---- helpers ------------------------------------------------------------

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private (int status, string body) Bearer(string method, string path, string key, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, "Bearer " + key, requestBody);

      private JsonElement Drain_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/server/drain", null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/server/drain: " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement;
      }

      /// <summary>A connection to a port that must be refused at the socket - not accepted and then dropped.</summary>
      private static SocketException AttemptConnection_(int port)
      {
         using (var client = new TcpClient())
         {
            try
            {
               client.Connect("127.0.0.1", port);
            }
            catch (SocketException e)
            {
               return e;
            }

            return null;
         }
      }

      private static void AssertRefused_(int port, string what)
      {
         SocketException refused = AttemptConnection_(port);
         Assert.IsNotNull(refused, what + ": the connection to port " + port + " was accepted; the listener is open.");
         Assert.AreEqual(SocketError.ConnectionRefused, refused.SocketErrorCode, what + ": the connection to port " + port + " failed, but not as a refusal: " + refused.Message);
      }

      private static void AssertAccepted_(int port, string what)
      {
         SocketException refused = AttemptConnection_(port);
         Assert.IsNull(refused, what + ": the connection to port " + port + " was refused: " + (refused == null ? "" : refused.Message));
      }

      // A minimal HTTP/1.0 GET against the metrics listener; the same shape
      // HealthProbes uses, on this fixture's port.
      private static (int status, string body) Probe(string path)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", MetricsPort);
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
               byte[] request = Encoding.ASCII.GetBytes("GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
               stream.Write(request, 0, request.Length);

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
               return (statusCode, separator >= 0 ? raw.Substring(separator + 4) : "");
            }
         }
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

      private Route AddRoute_(string host, int port)
      {
         Route route = _settings.Routes.Add();
         route.DomainName = RemoteDomain;
         route.TargetSMTPHost = host;
         route.TargetSMTPPort = port;
         route.NumberOfTries = 3;
         route.MinutesBetweenTry = 5;
         route.TreatRecipientAsLocalDomain = false;
         route.TreatSenderAsLocalDomain = false;
         route.ConnectionSecurity = eConnectionSecurity.eCSNone;
         route.Save();
         return route;
      }

      private void DeleteRoutes_()
      {
         Routes routes = _settings.Routes;
         for (int i = routes.Count - 1; i >= 0; i--)
         {
            if (string.Equals(routes[i].DomainName, RemoteDomain, StringComparison.OrdinalIgnoreCase))
               routes[i].Delete();
         }
      }

      /// <summary>
      ///    Queues one message for the remote domain the way the webmail sends:
      ///    POST /api/v1/me/messages with the account's own credential, which
      ///    writes the message into the delivery queue in process. The REST
      ///    listener stays up through a drain, so this is the one way to hand the
      ///    queue a message while the SMTP listener is closed - and what a person
      ///    in the webmail does during one.
      /// </summary>
      private void QueueMessage_(string subject)
      {
         string json = "{\"to\":\"test@" + RemoteDomain + "\",\"subject\":\"" + subject + "\",\"text\":\"" + subject + "\"}";
         (int status, string body) sent = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/me/messages", Basic(_account.Address, UserPassword), json);
         Assert.AreEqual(201, sent.status, "The webmail's send was refused. Body: " + sent.body);
      }

      private int QueuedForTheRemote_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/queue", null);
         Assert.AreEqual(200, answer.status, answer.body);

         return JsonDocument.Parse(answer.body).RootElement.GetProperty("messages").EnumerateArray()
            .Count(m => (m.GetProperty("recipients").GetString() ?? "").IndexOf("@" + RemoteDomain, StringComparison.OrdinalIgnoreCase) >= 0);
      }

      private static Dictionary<string, int> Accepts_(string address) => new Dictionary<string, int> { [address] = 250 };

      private static string Extract_(string json, string key)
      {
         Match match = Regex.Match(json, "\"" + key + "\"\\s*:\\s*\"?([^\",}]*)\"?");
         Assert.IsTrue(match.Success, "No '" + key + "' in: " + json);
         return match.Groups[1].Value;
      }

      /// <summary>The audit rows for the drain, oldest first by their id - the route answers newest first.</summary>
      private List<JsonElement> DrainAuditRows_()
      {
         (int status, string body) trail = Admin("GET", "/api/v1/audit?limit=200", null);
         Assert.AreEqual(200, trail.status, trail.body);

         using (JsonDocument document = JsonDocument.Parse(trail.body))
         {
            return document.RootElement.GetProperty("entries").EnumerateArray()
               .Where(e => e.GetProperty("object_type").GetString() == "server" && e.GetProperty("object").GetString() == "drain")
               .Select(e => e.Clone())
               .OrderBy(e => e.GetProperty("id").GetInt64())
               .ToList();
         }
      }

      /// <summary>
      ///    A raw ManageSieve session: the greeting read to its OK line. The
      ///    listener is a blocking accept loop of its own, which is why the drain
      ///    has to close it separately and why this fixture opens it.
      /// </summary>
      private sealed class ManageSieveSession : IDisposable
      {
         private readonly TcpClient _client = new TcpClient();
         private NetworkStream _stream;
         private readonly StringBuilder _received = new StringBuilder();

         public string Connect()
         {
            _client.Connect("127.0.0.1", ManageSievePort);
            _client.ReceiveTimeout = 5000;
            _stream = _client.GetStream();
            return ReadUntilOk_();
         }

         public string Send(string line)
         {
            byte[] bytes = Encoding.ASCII.GetBytes(line + "\r\n");
            _stream.Write(bytes, 0, bytes.Length);
            return ReadUntilOk_();
         }

         private string ReadUntilOk_()
         {
            _received.Clear();
            byte[] buffer = new byte[4096];
            DateTime deadline = DateTime.UtcNow.AddSeconds(5);
            while (DateTime.UtcNow < deadline)
            {
               int read = _stream.Read(buffer, 0, buffer.Length);
               if (read <= 0)
                  break;
               _received.Append(Encoding.ASCII.GetString(buffer, 0, read));
               string text = _received.ToString();
               if (text.Split(new[] { "\r\n" }, StringSplitOptions.None).Any(l => l.StartsWith("OK", StringComparison.Ordinal)))
                  return text;
            }
            return _received.ToString();
         }

         public void Dispose()
         {
            try { _stream?.Dispose(); } catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { }
            _client.Dispose();
         }
      }

      // ---- maintenance mode ----------------------------------------------------

      [Test]
      [Description("Entering the drain closes the three listeners to new connections while the session already open keeps answering, /readyz says draining, the state is named everywhere, a second POST is 409; leaving reopens all three, /readyz is 200 again, a second DELETE is 409; both are on the audit trail.")]
      public void MaintenanceModeClosesTheDoorsAndOpensThemAgain()
      {
         // The negative control: before the drain, every door is open.
         AssertAccepted_(TestPorts.Imap, "Before the drain");
         AssertAccepted_(TestPorts.Smtp, "Before the drain");
         AssertAccepted_(TestPorts.Pop3, "Before the drain");
         Assert.AreEqual(409, Admin("DELETE", "/api/v1/server/drain", null).status, "DELETE when nothing is draining must be 409.");
         Assert.IsFalse(Drain_().GetProperty("draining").GetBoolean());
         Assert.AreEqual(200, Probe("/readyz").status, "The bench's /readyz is not 200 before the drain.");

         var imap = new ImapClientSimulator();
         string banner = imap.Connect();
         StringAssert.StartsWith("* OK", banner);

         try
         {
            (int status, string body) entered = Admin("POST", "/api/v1/server/drain", "{}");
            Assert.AreEqual(200, entered.status, "POST /api/v1/server/drain: " + entered.body);
            JsonElement answer = JsonDocument.Parse(entered.body).RootElement;
            Assert.IsTrue(answer.GetProperty("draining").GetBoolean(), entered.body);
            Assert.AreEqual(1, answer.GetProperty("sessions").GetInt32(), "One IMAP session is open: " + entered.body);
            Assert.IsFalse(answer.GetProperty("drained").GetBoolean(), "A session is still open, so the drain is not complete: " + entered.body);
            StringAssert.IsMatch(@"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$", answer.GetProperty("since").GetString());

            // A NEW connection on each protocol is refused at the socket.
            AssertRefused_(TestPorts.Imap, "During the drain");
            AssertRefused_(TestPorts.Smtp, "During the drain");
            AssertRefused_(TestPorts.Pop3, "During the drain");

            // The session already open still answers.
            StringAssert.Contains("A1 OK", imap.Send("A1 NOOP"));

            (int status, string body) ready = Probe("/readyz");
            Assert.AreEqual(503, ready.status, ready.body);
            StringAssert.Contains("draining", ready.body);
            StringAssert.Contains("\"state\":\"draining\"", Probe("/healthz").body);

            JsonElement read = Drain_();
            Assert.IsTrue(read.GetProperty("draining").GetBoolean());
            Assert.AreEqual(1, read.GetProperty("sessions").GetInt32(), "GET shows the one session.");

            (int status, string body) twice = Admin("POST", "/api/v1/server/drain", "{}");
            Assert.AreEqual(409, twice.status, twice.body);
            StringAssert.Contains("already draining", twice.body);

            Assert.AreEqual(StateDraining, (int) _application.ServerState, "Application.ServerState does not read as draining.");
            StringAssert.Contains("\"state\":5", Admin("GET", "/api/v1/status", null).body);

            // The open session ends, and the drain becomes complete.
            imap.Disconnect();
            Assert.IsTrue(WaitFor_(() => Drain_().GetProperty("drained").GetBoolean(), 10), "The drain did not become complete once the session closed: " + Admin("GET", "/api/v1/server/drain", null).body);
         }
         finally
         {
            imap.Disconnect();
         }

         (int status, string body) left = Admin("DELETE", "/api/v1/server/drain", null);
         Assert.AreEqual(200, left.status, "DELETE /api/v1/server/drain: " + left.body);
         JsonElement after = JsonDocument.Parse(left.body).RootElement;
         Assert.IsFalse(after.GetProperty("draining").GetBoolean(), left.body);
         Assert.IsFalse(after.TryGetProperty("not_reopened", out JsonElement _), "Every port was free to reopen: " + left.body);

         AssertAccepted_(TestPorts.Imap, "After the drain");
         AssertAccepted_(TestPorts.Smtp, "After the drain");
         AssertAccepted_(TestPorts.Pop3, "After the drain");
         Assert.IsTrue(WaitFor_(() => Probe("/readyz").status == 200, 10), "/readyz did not return to 200: " + Probe("/readyz").body);
         Assert.AreEqual(StateRunning, (int) _application.ServerState);
         Assert.AreEqual(409, Admin("DELETE", "/api/v1/server/drain", null).status, "A second DELETE must be 409.");

         // A session works end to end again, which a refused port could not show.
         var again = new ImapClientSimulator();
         Assert.IsTrue(again.ConnectAndLogon(_account.Address, UserPassword), "An IMAP logon after the drain failed.");
         again.Disconnect();

         // Both halves are on the audit trail, in the actor's name.
         List<JsonElement> rows = DrainAuditRows_();
         Assert.IsTrue(rows.Any(e => e.GetProperty("action").GetString() == "entered"), "No 'entered' event on the audit trail.");
         Assert.IsTrue(rows.Any(e => e.GetProperty("action").GetString() == "left"), "No 'left' event on the audit trail.");
         JsonElement enteredRow = rows.Last(e => e.GetProperty("action").GetString() == "entered");
         StringAssert.Contains("1 session(s)", enteredRow.GetProperty("detail").GetString());
         Assert.AreEqual("administrator", enteredRow.GetProperty("actor").GetString(), "The operator who entered the drain is not the actor.");
      }

      [Test]
      [Description("The ManageSieve listener - an accept loop of its own, not one of the shared acceptors - is closed by the drain too: a new connection is refused while the session already open is counted and answers NOOP, and it accepts again once the drain is left.")]
      public void MaintenanceModeClosesManageSieveToo()
      {
         IniFileSetting.Write("ManageSieveServerPort", ManageSievePort.ToString());
         _application.Reinitialize();

         try
         {
            // The negative control: open, and a session's greeting ends in OK.
            AssertAccepted_(ManageSievePort, "Before the drain");

            using (var sieve = new ManageSieveSession())
            {
               StringAssert.Contains("OK", sieve.Connect());

               (int status, string body) entered = Admin("POST", "/api/v1/server/drain", "{}");
               Assert.AreEqual(200, entered.status, entered.body);
               JsonElement answer = JsonDocument.Parse(entered.body).RootElement;
               Assert.AreEqual(1, answer.GetProperty("sessions").GetInt32(), "The open ManageSieve session is counted: " + entered.body);

               try
               {
                  AssertRefused_(ManageSievePort, "During the drain");
                  StringAssert.StartsWith("OK", sieve.Send("NOOP").TrimStart(), "The open ManageSieve session stopped answering during the drain.");
                  Assert.IsFalse(Drain_().GetProperty("drained").GetBoolean(), "The ManageSieve session is open, so the drain is not complete.");
               }
               finally
               {
                  (int status, string body) left = Admin("DELETE", "/api/v1/server/drain", null);
                  Assert.AreEqual(200, left.status, left.body);
                  Assert.IsFalse(JsonDocument.Parse(left.body).RootElement.TryGetProperty("not_reopened", out JsonElement _), "The ManageSieve port did not reopen: " + left.body);
               }
            }

            AssertAccepted_(ManageSievePort, "After the drain");
            using (var again = new ManageSieveSession())
               StringAssert.Contains("OK", again.Connect());

            // And the count goes back down once the session has ended.
            Assert.IsTrue(WaitFor_(() => Drain_().GetProperty("sessions").GetInt32() == 0, 10), "The ended ManageSieve session is still counted: " + Admin("GET", "/api/v1/server/drain", null).body);
         }
         finally
         {
            IniFileSetting.Write("ManageSieveServerPort", "0");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A read-only key may read the drain's state and may neither enter nor leave it; the administrator may.")]
      public void AReadOnlyKeyMayReadTheDrainAndNotChangeIt()
      {
         (int status, string created) = Admin("POST", "/api/v1/apikeys", "{\"label\":\"drain - readonly\",\"scope\":\"readonly\"}");
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + created);
         string keyId = Extract_(created, "id");
         string key = Extract_(created, "key");

         try
         {
            Assert.AreEqual(200, Bearer("GET", "/api/v1/server/drain", key, null).status, "A read-only key reads the state.");
            Assert.AreEqual(403, Bearer("POST", "/api/v1/server/drain", key, "{}").status, "A read-only key must not enter the drain.");
            Assert.IsFalse(Drain_().GetProperty("draining").GetBoolean(), "The refused POST entered the drain.");
            AssertAccepted_(TestPorts.Imap, "After the refused POST");

            // The negative control: the administrator enters, and the key may not leave.
            Assert.AreEqual(200, Admin("POST", "/api/v1/server/drain", "{}").status);
            try
            {
               Assert.AreEqual(403, Bearer("DELETE", "/api/v1/server/drain", key, null).status, "A read-only key must not leave the drain.");
               Assert.IsTrue(Drain_().GetProperty("draining").GetBoolean(), "The refused DELETE left the drain.");
            }
            finally
            {
               Assert.AreEqual(200, Admin("DELETE", "/api/v1/server/drain", null).status);
            }
         }
         finally
         {
            Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
         }
      }

      [Test]
      [Description("A port taken by something else while the listeners were closed is named in not_reopened when the drain is left - the rest reopen and delivery resumes - and a restart takes it back.")]
      public void APortTakenDuringTheDrainIsNamedWhenTheDrainIsLeft()
      {
         Assert.AreEqual(200, Admin("POST", "/api/v1/server/drain", "{}").status);

         // The IMAP port is free now, so something else can bind it - as a
         // program started during the maintenance would. On the wildcard address
         // the bench's acceptors bind: Windows lets a wildcard bind coexist with
         // a loopback one, so a squatter on 127.0.0.1 alone would not be in the
         // server's way.
         var squatter = new TcpListener(IPAddress.Any, TestPorts.Imap);
         squatter.Start();

         try
         {
            (int status, string body) left = Admin("DELETE", "/api/v1/server/drain", null);
            Assert.AreEqual(200, left.status, "The other ports reopen, so the drain is left: " + left.body);
            JsonElement answer = JsonDocument.Parse(left.body).RootElement;
            Assert.IsFalse(answer.GetProperty("draining").GetBoolean(), left.body);
            Assert.IsTrue(answer.TryGetProperty("not_reopened", out JsonElement notReopened), "The taken port is not named: " + left.body);
            Assert.IsTrue(notReopened.EnumerateArray().Any(p => (p.GetString() ?? "").EndsWith(":" + TestPorts.Imap)), "The IMAP port is not the one named: " + left.body);

            // The rest are open, and the failure is on the error log by name.
            AssertAccepted_(TestPorts.Smtp, "After the drain with one port taken");
            AssertAccepted_(TestPorts.Pop3, "After the drain with one port taken");
            Assert.AreEqual(409, Admin("DELETE", "/api/v1/server/drain", null).status, "The drain was left, so a second DELETE is 409.");
            Assert.IsTrue(WaitFor_(() => LogHandler.ReadErrorLog().Contains("7150"), 10), "HM7150 is not on the error log.");
         }
         finally
         {
            squatter.Stop();

            // The bind failures this test provoked (HM4316 for the acceptor, HM7150
            // for the drain) are this test's, not a fault: cleared, or the next
            // test's setup would refuse to run.
            LogHandler.ReadAndDeleteErrorLog();

            // A restart tries every port again - the negative control.
            _application.Reinitialize();
         }

         AssertAccepted_(TestPorts.Imap, "After the restart");
      }

      [Test]
      [Description("A message queued from the webmail while draining is not delivered until the drain is left, and is delivered within seconds of that.")]
      public void AQueuedMessageWaitsForTheDrainToEnd()
      {
         int port = TestSetup.GetNextFreePort();
         using (var simulator = new SmtpServerSimulator(1, port))
         {
            simulator.AddRecipientResult(Accepts_("test@" + RemoteDomain));
            simulator.StartListen();
            AddRoute_("127.0.0.1", port);

            Assert.AreEqual(200, Admin("POST", "/api/v1/server/drain", "{}").status);
            try
            {
               // The webmail's send is accepted during the drain: its listener is up.
               QueueMessage_("Held for the drain");

               // Not started: no session at the simulator, the message in the
               // queue, nothing in flight - the manager handed nothing out.
               Thread.Sleep(5000);
               Assert.AreEqual(0, simulator.NumberOfConnectedClients, "The message was delivered during the drain.");
               Assert.AreEqual(1, QueuedForTheRemote_(), "The message is not in the queue.");
               JsonElement state = Drain_();
               Assert.AreEqual(0, state.GetProperty("deliveries").GetInt32(), "A delivery was started during the drain: " + state);
               Assert.IsTrue(state.GetProperty("drained").GetBoolean(), "Nothing is in flight, so the drain is complete: " + state);
            }
            finally
            {
               Assert.AreEqual(200, Admin("DELETE", "/api/v1/server/drain", null).status);
            }

            // The negative control: within seconds of leaving, it goes.
            simulator.WaitForCompletion();
            StringAssert.Contains("Held for the drain", simulator.MessageData);
            Assert.IsTrue(WaitFor_(() => QueuedForTheRemote_() == 0, 15), "The delivered message is still in the queue.");
         }
      }

      // ---- the stop --------------------------------------------------------------

      [Test]
      [Description("With a drain window and an IMAP session open, a stop waits about the window - and a connection attempted one second into it is refused at the socket, not accepted and cut. ShutdownDrain.cs proves the window's timing without a session; this proves the doors.")]
      public void AStopDrainsWithTheDoorsClosed()
      {
         const int drainSeconds = 3;
         IniFileSetting.Write("ShutdownDrainSeconds", drainSeconds.ToString());
         _application.Reinitialize();

         // The negative control: a moment before the stop, the door is open.
         AssertAccepted_(TestPorts.Imap, "Before the stop");

         var imap = new ImapClientSimulator();
         imap.Connect();

         bool accepted = false;
         SocketException refused = null;
         Exception other = null;
         var attempt = new Thread(() =>
         {
            Thread.Sleep(1000);
            try
            {
               using (var client = new TcpClient())
               {
                  client.Connect("127.0.0.1", TestPorts.Imap);
                  accepted = true;
               }
            }
            catch (SocketException e) { refused = e; }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { other = e; }
         });

         try
         {
            attempt.Start();
            Stopwatch timer = Stopwatch.StartNew();
            _application.Stop();
            timer.Stop();
            attempt.Join();

            Assert.GreaterOrEqual(timer.ElapsedMilliseconds, (drainSeconds * 1000) - 700, "Stop() did not wait for the drain window while a session was open.");
            Assert.IsNull(other, "The connection attempt failed for a reason that is not a socket error: " + other);
            Assert.IsFalse(accepted, "A connection one second into the stop's drain was accepted: the listeners were still open.");
            Assert.IsNotNull(refused, "The connection attempt neither succeeded nor failed.");
            Assert.AreEqual(SocketError.ConnectionRefused, refused.SocketErrorCode, "The connection failed, but not as a refusal: " + refused.Message);

            string log = LogHandler.ReadCurrentDefaultLog();
            StringAssert.Contains("Drain: listeners closed; 1 session(s) and 0 delivery(ies) in flight; up to " + drainSeconds + " seconds.", log);
         }
         finally
         {
            imap.Disconnect();
            _application.Start();
            IniFileSetting.Write("ShutdownDrainSeconds", "0");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A stop after a completed drain does not wait again: with a drain window set and nothing in flight, it returns at once - and the stop records the 'left' the operator's 'entered' was waiting for, as the server.")]
      public void AStopAfterACompletedDrainDoesNotWaitAgain()
      {
         IniFileSetting.Write("ShutdownDrainSeconds", "5");
         _application.Reinitialize();

         try
         {
            int leftBefore = DrainAuditRows_().Count(e => e.GetProperty("action").GetString() == "left");

            Assert.AreEqual(200, Admin("POST", "/api/v1/server/drain", "{}").status);
            Assert.IsTrue(WaitFor_(() => Drain_().GetProperty("drained").GetBoolean(), 10), "The drain did not complete with nothing open.");

            Stopwatch timer = Stopwatch.StartNew();
            _application.Stop();
            timer.Stop();

            Assert.Less(timer.ElapsedMilliseconds, 2500, "A stop after a completed drain waited for the window again.");
            string log = LogHandler.ReadCurrentDefaultLog();
            StringAssert.Contains("Graceful shutdown: already drained; stopping now.", log);
            StringAssert.Contains("Drain: ended by a stop of the servers", log);

            // The 'left' a stop records, as the server: the operator's 'entered'
            // is not left without its other half by a restart.
            _application.Start();
            List<JsonElement> lefts = DrainAuditRows_().Where(e => e.GetProperty("action").GetString() == "left").ToList();
            Assert.AreEqual(leftBefore + 1, lefts.Count, "The stop did not record a 'left'.");
            JsonElement byTheStop = lefts.Last();
            Assert.AreEqual("server", byTheStop.GetProperty("actor").GetString(), "The 'left' a stop records is not the server's own.");
            Assert.AreEqual("system", byTheStop.GetProperty("actor_type").GetString());
            StringAssert.Contains("Ended by a stop", byTheStop.GetProperty("detail").GetString());
         }
         finally
         {
            if ((int) _application.ServerState != StateRunning)
               _application.Start();
            IniFileSetting.Write("ShutdownDrainSeconds", "0");
            _application.Reinitialize();
         }
      }

      // ---- the delivery pause ----------------------------------------------------

      [Test]
      [Description("A stop while a delivery is talking to a remote lets that session finish - the message arrives - and does not start the message queued behind it, which goes after the restart.")]
      public void TheDeliveryPauseFinishesTheSessionInFlightAndStartsNoOther()
      {
         // One delivery thread, so that the second message waits behind the first
         // rather than opening its own session; the thread count is read when the
         // servers start, hence the Reinitialize. And a window long enough for the
         // held session to finish inside it.
         _settings.MaxDeliveryThreads = 1;
         IniFileSetting.Write("ShutdownDrainSeconds", "20");
         _application.Reinitialize();

         int port = TestSetup.GetNextFreePort();
         using (var simulator = new SmtpServerSimulator(2, port))
         {
            simulator.HoldMailFromReplyFor = TimeSpan.FromSeconds(5);
            simulator.AddRecipientResult(Accepts_("test@" + RemoteDomain));
            simulator.AddRecipientResult(Accepts_("test@" + RemoteDomain));
            simulator.StartListen();
            AddRoute_("127.0.0.1", port);

            try
            {
               QueueMessage_("First, in flight");
               Assert.IsTrue(WaitFor_(() => simulator.NumberOfConnectedClients == 1, 15), "The first delivery did not open its session.");

               // Handed to the queue behind the first, on the one thread.
               QueueMessage_("Second, waiting");
               Thread.Sleep(1000);

               Stopwatch timer = Stopwatch.StartNew();
               _application.Stop();
               timer.Stop();

               // The session in flight finished inside the window rather than the
               // window elapsing; the message arrived; the second was not started.
               Assert.Less(timer.ElapsedMilliseconds, 19000, "The drain ran to its window: something in flight did not finish.");
               StringAssert.Contains("First, in flight", simulator.MessageData);
               Assert.AreEqual(1, simulator.NumberOfConnectedClients, "The second queued message was started during the stop.");
               StringAssert.Contains("Graceful shutdown: all sessions and deliveries drained.", LogHandler.ReadCurrentDefaultLog());

               // The negative control: after the restart it goes.
               _application.Start();
               Assert.IsTrue(WaitFor_(() => simulator.NumberOfConnectedClients == 2, 30), "The second message was not delivered after the restart.");
               simulator.WaitForCompletion();
               StringAssert.Contains("Second, waiting", simulator.MessageData);
            }
            finally
            {
               if ((int) _application.ServerState != StateRunning)
                  _application.Start();
            }
         }
      }
   }
}
