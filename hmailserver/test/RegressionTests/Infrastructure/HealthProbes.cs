// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Linq;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Exercises the oper/observability health probes served on the metrics
   ///    listener (MetricsServerPort, in the settings store): the Kubernetes-style
   ///    /livez (liveness), /readyz (readiness) and /healthz (JSON health) endpoints,
   ///    alongside the existing /metrics endpoint.
   /// </summary>
   [TestFixture]
   public class HealthProbes : TestFixtureBase
   {
      private const int MetricsPort = 9099;

      private void WriteSetting(string key, string value)
      {
         IniFileSetting.Write(key, value);
      }

      // Parses a Prometheus counter/gauge value from a /metrics body. Returns -1 if absent.
      private static int ParseCounter(string body, string name)
      {
         // The sample's value is the last token of the first line for the name that parses.
         return body.Split('\n')
            .Select(raw => raw.Trim())
            .Where(line => line.StartsWith(name + " "))
            .Select(line => int.TryParse(line.Substring(line.LastIndexOf(' ') + 1), out int value) ? value : (int?)null)
            .FirstOrDefault(value => value.HasValue) ?? -1;
      }

      // Issues a minimal HTTP/1.0 GET against the metrics listener and returns the
      // parsed status code and body. Retries the initial connect briefly to absorb
      // the listener bind race right after a server restart. readTimeoutMilliseconds
      // bounds each read of the answer (an IOException when it passes); by default
      // a read waits as long as the listener takes.
      private static (int status, string body) HttpGet(string path, int readTimeoutMilliseconds = System.Threading.Timeout.Infinite)
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
                  System.Threading.Thread.Sleep(200);
               }
            }
            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               stream.ReadTimeout = readTimeoutMilliseconds;

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
               string body = separator >= 0 ? raw.Substring(separator + 4) : "";

               return (statusCode, body);
            }
         }
      }

      [Test]
      [Description("RFC-agnostic operability: /livez, /readyz, /healthz and /metrics on the metrics listener.")]
      public void TestHealthProbes()
      {
         WriteSetting("MetricsServerBindAddress", "127.0.0.1");
         WriteSetting("MetricsServerPort", MetricsPort.ToString());

         // Reinitialize (not just Stop/Start) so the metrics listener picks up the
         // freshly written MetricsServerPort: the port is cached in IniFileSettings,
         // which is only re-read by InitInstance() during a full reinitialize.
         _application.Reinitialize();

         try
         {
            // Liveness: always 200 once the listener is up (no dependency checks).
            (int status, string body) live = HttpGet("/livez");
            Assert.AreEqual(200, live.status, "Liveness should be 200. Body: " + live.body);
            Assert.IsTrue(live.body.Contains("alive"), "Liveness body: " + live.body);

            // Readiness: server running and database connected -> 200.
            (int status, string body) ready = HttpGet("/readyz");
            Assert.AreEqual(200, ready.status, "Readiness should be 200 when running + DB up. Body: " + ready.body);
            Assert.IsTrue(ready.body.Contains("ready"), "Readiness body: " + ready.body);

            // Health JSON: ok + database up + running.
            (int status, string body) health = HttpGet("/healthz");
            Assert.AreEqual(200, health.status, "Health should be 200. Body: " + health.body);
            Assert.IsTrue(health.body.Contains("\"status\":\"ok\""), "Health body: " + health.body);
            Assert.IsTrue(health.body.Contains("\"database\":\"up\""), "Health body: " + health.body);
            Assert.IsTrue(health.body.Contains("\"state\":\"running\""), "Health body: " + health.body);

            // The existing metrics endpoint still works.
            (int status, string body) metrics = HttpGet("/metrics");
            Assert.AreEqual(200, metrics.status, "Metrics should be 200. Body: " + metrics.body);
            // Renamed to follow Prometheus convention: a start timestamp instead of an
            // uptime counter, and "connected" instead of "up" (which collided with
            // Prometheus's own synthetic up). See PrometheusConventions.cs.
            Assert.IsTrue(metrics.body.Contains("hmailserver_start_time_seconds"), "Metrics body did not contain expected gauge.");
            Assert.IsTrue(metrics.body.Contains("hmailserver_database_connected 1"), "Metrics body should report the database connected. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_db_connections{state=\"busy\"}"), "Metrics body missing busy pool gauge. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_db_connections{state=\"available\"}"), "Metrics body missing available pool gauge. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_tls_handshakes_total"), "Metrics body missing TLS handshake counter. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_tls_handshake_failures_total"), "Metrics body missing TLS handshake failure counter. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_auth_success_total"), "Metrics body missing auth success counter. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_auth_failures_total"), "Metrics body missing auth failure counter. Body: " + metrics.body);
            Assert.IsTrue(metrics.body.Contains("hmailserver_delivery_queue_messages"), "Metrics body missing delivery queue gauge. Body: " + metrics.body);

            // Functional check: a failed authentication increments the auth-failure counter,
            // proving the gauge is wired to the AccountLogon path (not merely emitted).
            int beforeFailures = ParseCounter(metrics.body, "hmailserver_auth_failures_total");
            Assert.GreaterOrEqual(beforeFailures, 0, "auth failure counter should be present and numeric.");

            // The command-processing summary must be present and is wired to the TCP
            // command loop; capture the baseline command count to confirm it advances.
            Assert.IsTrue(metrics.body.Contains("hmailserver_command_processing_seconds_sum"), "Metrics body missing command latency sum. Body: " + metrics.body);
            int beforeCommands = ParseCounter(metrics.body, "hmailserver_command_processing_seconds_count");
            Assert.GreaterOrEqual(beforeCommands, 0, "command count should be present and numeric.");

            Pop3ClientSimulator pop3 = new Pop3ClientSimulator();
            bool loggedOn = pop3.ConnectAndLogon("no-such-user@example.test", "definitely-wrong-password");
            Assert.IsFalse(loggedOn, "A bad login must not succeed.");

            (int status, string body) afterMetrics = HttpGet("/metrics");
            int afterFailures = ParseCounter(afterMetrics.body, "hmailserver_auth_failures_total");
            Assert.Greater(afterFailures, beforeFailures, "auth failure counter should increase after a bad login. Body: " + afterMetrics.body);

            // The bad login issued protocol command lines (USER/PASS/QUIT), so the
            // command-processing counter must have advanced too.
            int afterCommands = ParseCounter(afterMetrics.body, "hmailserver_command_processing_seconds_count");
            Assert.Greater(afterCommands, beforeCommands, "command counter should increase after protocol activity. Body: " + afterMetrics.body);

            // Unknown paths return 404.
            (int status, string body) notFound = HttpGet("/does-not-exist");
            Assert.AreEqual(404, notFound.status, "Unknown path should be 404.");
         }
         finally
         {
            WriteSetting("MetricsServerPort", "0");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    The listener serves one client at a time, so a client that connects and
      ///    sends nothing - a port scanner, a probe that died half way - must be let go
      ///    after the five-second socket timeout for the probes behind it to be
      ///    answered. Until 6.3.6 that timeout was set in the Windows form, which Linux
      ///    refuses, so on Linux the first read waited for as long as the idle client
      ///    stayed connected and the load balancer's /livez behind it was never
      ///    answered: against that code this test fails on Linux, at the read timeout
      ///    below. Windows honoured the timeout all along, and there it guards it.
      /// </summary>
      [Test]
      [Description("A client that connects to the metrics listener and sends nothing does not stop /livez from being answered.")]
      public void AnIdleClientIsLetGoAndTheProbeBehindItAnswered()
      {
         WriteSetting("MetricsServerBindAddress", "127.0.0.1");
         WriteSetting("MetricsServerPort", MetricsPort.ToString());
         _application.Reinitialize();

         try
         {
            // The listener is up once a probe is answered.
            Assert.AreEqual(200, HttpGet("/livez").status, "The metrics listener did not answer /livez at all.");

            using (var idle = new TcpClient())
            {
               idle.Connect("127.0.0.1", MetricsPort);

               // Behind the idle client, and given twice the listener's socket timeout
               // plus a margin to be answered.
               var watch = System.Diagnostics.Stopwatch.StartNew();
               (int status, string body) live;
               try
               {
                  live = HttpGet("/livez", 15000);
               }
               catch (IOException e)
               {
                  Assert.Fail("/livez behind a client that sent nothing was not answered in " + watch.Elapsed.TotalSeconds.ToString("0.0") +
                     " s: the listener is still waiting on the idle client. " + e.Message);
                  return;
               }

               Assert.AreEqual(200, live.status, "/livez behind an idle client. Body: " + live.body);
               Assert.Less(watch.Elapsed.TotalSeconds, 15, "/livez behind an idle client was answered only after " + watch.Elapsed.TotalSeconds.ToString("0.0") + " s.");
            }
         }
         finally
         {
            WriteSetting("MetricsServerPort", "0");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    Until 6.3.6 the metrics listener set SO_REUSEADDR on Windows too, where it
      ///    lets a socket bind a port that another socket with the option is already
      ///    listening on: the listener reported itself listening while the connections
      ///    went to the other program (measured on Windows 11, 20 connections of 20).
      ///    Without the option the bind fails, and the application log says so. Here
      ///    the port is held by a listener with the option, as a program ported from
      ///    POSIX holds one; against the old code the bind succeeds, "Listening on" is
      ///    written for the port and "Failed to bind" is not, so this fails on Windows.
      ///    Linux keeps the option, where it never lets two listeners share a port, and
      ///    there the test passes either way.
      /// </summary>
      [Test]
      [Description("A metrics port that another program listens on with SO_REUSEADDR is refused and reported, not shared.")]
      public void APortAnotherProgramListensOnIsRefusedNotShared()
      {
         int port = TestSetup.GetNextFreePort();
         var holder = new TcpListener(IPAddress.Loopback, port);
         holder.ExclusiveAddressUse = false;
         holder.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
         holder.Start();

         try
         {
            // Only what this test's reinitialise writes is read: the log is the day's.
            int logBefore = LogHandler.ReadCurrentDefaultLog().Length;

            WriteSetting("MetricsServerBindAddress", "127.0.0.1");
            WriteSetting("MetricsServerPort", port.ToString());
            _application.Reinitialize();

            Assert.IsTrue(LogHandler.DefaultLogContains("MetricsServer: Failed to bind to 127.0.0.1:" + port + "."),
               "The metrics listener did not report that it could not bind a port another program listens on.");

            string log = LogHandler.ReadCurrentDefaultLog();
            string written = log.Length >= logBefore ? log.Substring(logBefore) : log;
            Assert.IsFalse(written.Contains("MetricsServer: Listening on 127.0.0.1:" + port + " "),
               "The metrics listener bound a port another program listens on, and reported itself listening.");
         }
         finally
         {
            WriteSetting("MetricsServerPort", "0");
            _application.Reinitialize();
            holder.Stop();
         }
      }

      /// <summary>
      ///    On Windows a socket of the same account could bind 127.0.0.1 on a port the
      ///    metrics listener held on 0.0.0.0 - with no option at all, which Windows
      ///    allows for the more specific address - and from then on take every
      ///    connection made to 127.0.0.1 (measured on Windows 11: 20 connections of
      ///    20). Since 6.3.6 the listener claims its port with SO_EXCLUSIVEADDRUSE, and
      ///    that bind is refused. Against the code before, the bind below succeeds and
      ///    this fails on Windows. Linux refuses that bind whatever the listener sets,
      ///    and there the test passes either way.
      /// </summary>
      [Test]
      [Description("A socket cannot bind 127.0.0.1 on the port the metrics listener holds on 0.0.0.0 and take its connections.")]
      public void AMoreSpecificBindCannotTakeTheListenersPort()
      {
         WriteSetting("MetricsServerBindAddress", "0.0.0.0");
         WriteSetting("MetricsServerPort", MetricsPort.ToString());
         _application.Reinitialize();

         try
         {
            // The listener is up once a probe is answered.
            Assert.AreEqual(200, HttpGet("/livez").status, "The metrics listener did not answer /livez at all.");

            var taker = new TcpListener(IPAddress.Loopback, MetricsPort);
            taker.ExclusiveAddressUse = false;
            bool bound;
            try
            {
               taker.Start();
               bound = true;
            }
            catch (SocketException)
            {
               bound = false;
            }

            try
            {
               Assert.IsFalse(bound, "A socket bound 127.0.0.1:" + MetricsPort + " under the metrics listener on 0.0.0.0, and would take its connections there.");

               // And the listener still answers on 127.0.0.1.
               Assert.AreEqual(200, HttpGet("/livez", 15000).status, "/livez on 127.0.0.1 after the refused bind.");
            }
            finally
            {
               if (bound)
                  taker.Stop();
            }
         }
         finally
         {
            WriteSetting("MetricsServerBindAddress", "127.0.0.1");
            WriteSetting("MetricsServerPort", "0");
            _application.Reinitialize();
         }
      }
   }
}
