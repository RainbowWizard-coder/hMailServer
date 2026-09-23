// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    A setting that takes effect when it is saved (docs/SettingsApply.md): every
   ///    [Settings] key carries a mark in build/ini-settings-baseline.txt - now,
   ///    reinitialize or restart - a save of a key marked now is in effect when the
   ///    save returns, and one marked reinitialize is stored, NOT applied, and listed
   ///    as pending until POST /api/v1/server/reinitialize takes it. Each with its
   ///    negative control.
   ///
   ///    Not one test here calls Application.Reinitialize to make a now key apply:
   ///    that is the point. The ones that reinitialise do it over REST, to take a
   ///    reinitialize key, which is the other half of the point.
   ///
   ///    The swap itself - a getter read ten thousand times while a thousand blocks
   ///    are published under it - is the internal IniFileSettingsTester, which
   ///    MainOperations.RunTestSuite runs with the rest of ClassTester.
   ///
   ///    Ports: the REST listener on 9876, the web services listener on 9877, the
   ///    ManageSieve listener moved between 9878 and 9879 (the range this row was
   ///    allocated, 9876-9885). Everything is put back in the teardown, which
   ///    reinitialises whatever a failure left.
   /// </summary>
   [TestFixture]
   public class SettingsApplyOnSave : TestFixtureBase
   {
      private static int RestPort = 9876;
      private const int WebServicesPort = 9877;
      private const int SievePortA = 9878;
      private const int SievePortB = 9879;
      private const string AdminPassword = "testar";

      private const string CalDavTargetA = "https://calendar-a.example.test/dav/";
      private const string CalDavTargetB = "https://calendar-b.example.test/dav/";

      private const int StateRunning = 3;

      [SetUp]
      public void StartListeners()
      {
         IniFileSetting.Write("MaxSubmissionsPerIPPerMinute", "0");
         IniFileSetting.Write("ManageSieveServerPort", "0");
         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
         // Only a restart key can be pending after a reinitialise - a value some
         // earlier fixture stored and the process has not started with - and none of
         // the tests below touches one.
         Assert.AreEqual(0, Pending_().Values.Count(entry => entry.GetProperty("applies").GetString() != "restart"),
            "A reinitialize key was pending straight after a reinitialise: " + Admin("GET", "/api/v1/server/settings/pending", null).body);
      }

      [TearDown]
      public void PutTheServerBack()
      {
         try
         {
            try { Admin("DELETE", "/api/v1/server/drain", null); } catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { }
            try { if ((int) _application.ServerState != StateRunning) _application.Start(); } catch (Exception e) when (!ExceptionPolicy.IsFatal(e)) { }

            IniFileSetting.Write("MaxSubmissionsPerIPPerMinute", "0");
            IniFileSetting.Write("ManageSieveServerPort", "0");
            IniFileSetting.Write("CalDavRedirectUrl", "");
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---- helpers ------------------------------------------------------------

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      /// <summary>PUT /api/v1/settings/ini/{name}, answered as JSON.</summary>
      private JsonElement PutIni_(string name, string value)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/settings/ini/" + name,
            "{\"value\":" + JsonSerializer.Serialize(value) + "}");
         Assert.AreEqual(200, answer.status, "PUT " + name + ": " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      /// <summary>GET /api/v1/server/settings/pending, by name.</summary>
      private Dictionary<string, JsonElement> Pending_()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/server/settings/pending", null);
         Assert.AreEqual(200, answer.status, "GET /api/v1/server/settings/pending: " + answer.body);

         var byName = new Dictionary<string, JsonElement>(StringComparer.OrdinalIgnoreCase);
         foreach (JsonElement entry in JsonDocument.Parse(answer.body).RootElement.Clone().EnumerateArray())
            byName[entry.GetProperty("name").GetString()] = entry;
         return byName;
      }

      /// <summary>
      ///    POST /api/v1/server/reinitialize, then GET /api/v1/status until the
      ///    listener answers again - what the Deck's and the Control Panel's "apply
      ///    now" do. Returns the reinitialise's own answer.
      /// </summary>
      private JsonElement ReinitializeOverRest_()
      {
         (int status, string body) answer = Admin("POST", "/api/v1/server/reinitialize", null);
         Assert.AreEqual(202, answer.status, "POST /api/v1/server/reinitialize: " + answer.body);
         JsonElement document = JsonDocument.Parse(answer.body).RootElement.Clone();

         // The answer leaves before the listener that carried it stops, so the
         // first polls may reach the old listener, the gap, or the new one.
         Thread.Sleep(1500);
         DateTime deadline = DateTime.Now.AddSeconds(90);
         while (true)
         {
            try
            {
               (int status, string body) probe = Admin("GET", "/api/v1/status", null);
               if (probe.status == 200 && (int) _application.ServerState == StateRunning)
                  break;
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               // The listener is between its two lives.
            }

            Assert.IsTrue(DateTime.Now < deadline, "The server did not answer GET /api/v1/status within 90 seconds of the reinitialise.");
            Thread.Sleep(500);
         }

         return document;
      }

      /// <summary>A connection that must be refused at the socket - not accepted and then dropped.</summary>
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

      private static void AssertListening_(int port, string what)
      {
         SocketException refused = null;
         for (int attempt = 0; attempt < 25; attempt++)
         {
            refused = AttemptConnection_(port);
            if (refused == null)
               return;
            Thread.Sleep(200);
         }

         Assert.Fail(what + ": nothing is listening on port " + port + ": " + refused.Message);
      }

      private static void AssertNotListening_(int port, string what)
      {
         SocketException refused = AttemptConnection_(port);
         Assert.IsNotNull(refused, what + ": port " + port + " accepted a connection.");
         Assert.AreEqual(SocketError.ConnectionRefused, refused.SocketErrorCode, what + ": the connection to port " + port + " failed, but not as a refusal: " + refused.Message);
      }

      /// <summary>One MAIL FROM, then RSET so the next one starts a new transaction.</summary>
      private static string StartTransaction_(TcpConnection socket)
      {
         string reply = socket.SendAndReceive("MAIL FROM:<sender@example.com>\r\n");
         if (reply.StartsWith("250"))
            StringAssert.StartsWith("250", socket.SendAndReceive("RSET\r\n"));
         return reply;
      }

      private static TcpConnection OpenSmtp_()
      {
         var socket = new TcpConnection();
         Assert.IsTrue(socket.Connect(25));
         StringAssert.StartsWith("220", socket.Receive());
         StringAssert.StartsWith("250", socket.SendAndReceive("HELO example.com\r\n"));
         return socket;
      }

      private static (int status, string headers) WebServicesGet_(string path)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", WebServicesPort);
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] request = Encoding.ASCII.GetBytes("GET " + path + " HTTP/1.0\r\nHost: example.test\r\nConnection: close\r\n\r\n");
               stream.Write(request, 0, request.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int status = 0;
               string[] parts = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (parts.Length >= 2)
                  int.TryParse(parts[1], out status);
               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(0, separator) : raw);
            }
         }
      }

      private static string Location_(string headers)
      {
         foreach (string line in headers.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.StartsWith("Location:", StringComparison.OrdinalIgnoreCase))
               return line.Substring("Location:".Length).Trim();
         }
         return null;
      }

      /// <summary>
      ///    build/ini-settings-baseline.txt, found by walking up from the test
      ///    assembly to the repository it was built from.
      /// </summary>
      private static Dictionary<string, string> BaselineMarks_()
      {
         DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory);
         string path = null;
         while (directory != null)
         {
            string candidate = Path.Combine(directory.FullName, "build", "ini-settings-baseline.txt");
            if (File.Exists(candidate))
            {
               path = candidate;
               break;
            }
            directory = directory.Parent;
         }

         Assert.IsNotNull(path, "build/ini-settings-baseline.txt was not found above " + TestContext.CurrentContext.TestDirectory);

         var marks = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string raw in File.ReadAllLines(path))
         {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith("#"))
               continue;

            string[] fields = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (fields[0] != "Settings")
               continue;

            marks[fields[1]] = fields[fields.Length - 1];
         }

         return marks;
      }

      // ---- the tests ------------------------------------------------------------

      [Test]
      [Description("A key marked now applies when it is saved, with no reinitialise: MaxSubmissionsPerIPPerMinute set to 2 over REST makes the third MAIL FROM on a fresh connection a 421 at once, and set back to 0 the next MAIL FROM is accepted at once. The negative control is the first half: at 0, ten transactions in a row are accepted.")]
      public void ANowKeyAppliesWhenItIsSaved()
      {
         // At 0 - the control - nothing is throttled.
         using (TcpConnection socket = OpenSmtp_())
         {
            for (int i = 0; i < 10; i++)
               StringAssert.StartsWith("250", StartTransaction_(socket), "Transaction " + i + " at 0 should be accepted.");
            socket.Send("QUIT\r\n");
         }

         JsonElement saved = PutIni_("MaxSubmissionsPerIPPerMinute", "2");
         Assert.AreEqual("now", saved.GetProperty("applies").GetString(), "The answer should carry the key's mark: " + saved);
         Assert.IsFalse(saved.GetProperty("pending").GetBoolean(), "A key marked now is never pending: " + saved);

         // No reinitialise: a fresh connection, and the third transaction is refused.
         using (TcpConnection socket = OpenSmtp_())
         {
            StartTransaction_(socket);
            StartTransaction_(socket);
            string third = StartTransaction_(socket);
            StringAssert.StartsWith("421", third, "The third MAIL FROM after MaxSubmissionsPerIPPerMinute=2 was saved should be throttled at once.");
            socket.Send("QUIT\r\n");
         }

         PutIni_("MaxSubmissionsPerIPPerMinute", "0");

         using (TcpConnection socket = OpenSmtp_())
         {
            StringAssert.StartsWith("250", StartTransaction_(socket), "Set back to 0, a MAIL FROM should be accepted at once.");
            socket.Send("QUIT\r\n");
         }
      }

      [Test]
      [Description("A key marked now whose reader copied it when it started is re-latched by a subscriber when it is saved: /.well-known/caldav, whose redirect target the web services listener caches for a minute, answers the new CalDavRedirectUrl straight after the save. The negative control is the order: the first request primes the cache with the old target, which is what it would go on answering for up to a minute without the subscriber.")]
      public void ASubscribedKeyIsRelatchedWhenItIsSaved()
      {
         PutIni_("CalDavRedirectUrl", CalDavTargetA);

         (int status, string headers) first = WebServicesGet_("/.well-known/caldav");
         Assert.AreEqual(301, first.status, "Headers: " + first.headers);
         Assert.AreEqual(CalDavTargetA, Location_(first.headers), "Headers: " + first.headers);

         JsonElement saved = PutIni_("CalDavRedirectUrl", CalDavTargetB);
         Assert.AreEqual("now", saved.GetProperty("applies").GetString(), "Answer: " + saved);

         (int status, string headers) second = WebServicesGet_("/.well-known/caldav");
         Assert.AreEqual(301, second.status, "Headers: " + second.headers);
         Assert.AreEqual(CalDavTargetB, Location_(second.headers),
            "The redirect still names the target the cache was primed with, so the save did not reach the listener. Headers: " + second.headers);
      }

      [Test]
      [Description("A key marked reinitialize is stored and NOT applied: ManageSieveServerPort saved over REST answers pending true, the listener stays where it is and the new port refuses, and GET /api/v1/server/settings/pending lists it with the stored and the running value. Putting the stored value back empties the list; saved again, POST /api/v1/server/reinitialize takes it - the new port answers, the old one refuses - and the list is empty.")]
      public void AReinitializeKeyWaitsForTheReinitialize()
      {
         // Off to on: stored, not applied.
         JsonElement saved = PutIni_("ManageSieveServerPort", SievePortA.ToString());
         Assert.AreEqual("reinitialize", saved.GetProperty("applies").GetString(), "Answer: " + saved);
         Assert.IsTrue(saved.GetProperty("pending").GetBoolean(), "A saved reinitialize key should answer pending: " + saved);
         AssertNotListening_(SievePortA, "Saved but not reinitialised");

         Dictionary<string, JsonElement> pending = Pending_();
         Assert.IsTrue(pending.ContainsKey("ManageSieveServerPort"), "The key is not listed as pending.");
         Assert.AreEqual(SievePortA.ToString(), pending["ManageSieveServerPort"].GetProperty("stored").GetString());
         Assert.AreEqual("0", pending["ManageSieveServerPort"].GetProperty("running").GetString());
         Assert.AreEqual("reinitialize", pending["ManageSieveServerPort"].GetProperty("applies").GetString());

         // Put back before anything applied it: not pending, because the list is
         // recomputed rather than remembered.
         JsonElement putBack = PutIni_("ManageSieveServerPort", "0");
         Assert.IsFalse(putBack.GetProperty("pending").GetBoolean(), "Answer: " + putBack);
         Assert.IsFalse(Pending_().ContainsKey("ManageSieveServerPort"), "A value put back is still listed as pending.");

         // Saved again, and taken by the reinitialise.
         PutIni_("ManageSieveServerPort", SievePortA.ToString());
         JsonElement reinitialised = ReinitializeOverRest_();
         Assert.IsFalse(reinitialised.GetProperty("drained").GetBoolean(), "A reinitialise of a server that was not draining said it ended a drain: " + reinitialised);
         AssertListening_(SievePortA, "After the reinitialise");
         Assert.IsFalse(Pending_().ContainsKey("ManageSieveServerPort"), "The reinitialise did not take the pending key.");

         // A listener moved: the old port still answers and the new one does not,
         // until the reinitialise.
         PutIni_("ManageSieveServerPort", SievePortB.ToString());
         AssertListening_(SievePortA, "The running listener, with a new port saved");
         AssertNotListening_(SievePortB, "The saved port, before the reinitialise");
         pending = Pending_();
         Assert.AreEqual(SievePortB.ToString(), pending["ManageSieveServerPort"].GetProperty("stored").GetString());
         Assert.AreEqual(SievePortA.ToString(), pending["ManageSieveServerPort"].GetProperty("running").GetString());

         ReinitializeOverRest_();
         AssertListening_(SievePortB, "The saved port, after the reinitialise");
         AssertNotListening_(SievePortA, "The old port, after the reinitialise");
         Assert.IsFalse(Pending_().ContainsKey("ManageSieveServerPort"), "The reinitialise did not take the pending key.");
      }

      [Test]
      [Description("A reinitialise ends maintenance mode, and the REST answer says so: drained is true when the server was draining when it was asked, and after it the server is running with its listeners open.")]
      public void AReinitializeThatEndsADrainSaysSo()
      {
         (int status, string body) entered = Admin("POST", "/api/v1/server/drain", null);
         Assert.AreEqual(200, entered.status, "POST /api/v1/server/drain: " + entered.body);

         JsonElement answer = ReinitializeOverRest_();
         Assert.IsTrue(answer.GetProperty("drained").GetBoolean(), "The reinitialise of a draining server did not say it ended the drain: " + answer);

         (int status, string body) state = Admin("GET", "/api/v1/server/drain", null);
         Assert.AreEqual(200, state.status, state.body);
         Assert.IsFalse(JsonDocument.Parse(state.body).RootElement.GetProperty("draining").GetBoolean(), "Still draining after the reinitialise: " + state.body);
         AssertListening_(25, "SMTP after the reinitialise");
      }

      [Test]
      [Description("Every key GET /api/v1/settings/ini lists carries an applies of now, reinitialize or restart, and its applies map holds exactly the [Settings] keys of build/ini-settings-baseline.txt, each with the baseline's mark. The negative control is the comparison itself: a key missing, added or marked differently fails it by name.")]
      public void EveryKeyCarriesItsMark()
      {
         (int status, string body) answer = Admin("GET", "/api/v1/settings/ini", null);
         Assert.AreEqual(200, answer.status, answer.body);
         JsonElement root = JsonDocument.Parse(answer.body).RootElement;

         var words = new HashSet<string> { "now", "reinitialize", "restart" };
         foreach (JsonElement entry in root.GetProperty("settings").EnumerateArray())
         {
            string applies = entry.GetProperty("applies").GetString();
            Assert.IsTrue(words.Contains(applies), entry.GetProperty("name").GetString() + " carries applies '" + applies + "'.");
            JsonValueKind pendingKind = entry.GetProperty("pending").ValueKind;
            Assert.IsTrue(pendingKind == JsonValueKind.True || pendingKind == JsonValueKind.False,
               entry.GetProperty("name").GetString() + " carries no pending flag.");
         }

         Dictionary<string, string> baseline = BaselineMarks_();
         var served = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (JsonProperty property in root.GetProperty("applies").EnumerateObject())
            served[property.Name] = property.Value.GetString();

         Assert.AreEqual(baseline.Count, served.Count, "The server marks " + served.Count + " keys and the baseline lists " + baseline.Count + ".");
         foreach (KeyValuePair<string, string> key in baseline)
         {
            Assert.IsTrue(served.ContainsKey(key.Key), key.Key + " is in the baseline and not in the server's applies map.");
            Assert.AreEqual(key.Value, served[key.Key], key.Key + " is marked differently by the server and the baseline.");
         }
      }

      [Test]
      [Description("Over COM: IniSettingApplies answers now for MaxSubmissionsPerIPPerMinute and reinitialize for ManageSieveServerPort, and refuses a name the loader does not read - TCPIPThreads, which is an hm_settings property and not a [Settings] key, and a name that is nothing at all. PendingIniSettings lists what GET /api/v1/server/settings/pending lists, and is empty again when the value is put back.")]
      public void TheMarkAndThePendingListOverCom()
      {
         Settings settings = _settings;

         Assert.AreEqual("now", settings.IniSettingApplies("MaxSubmissionsPerIPPerMinute"));
         Assert.AreEqual("reinitialize", settings.IniSettingApplies("ManageSieveServerPort"));
         Assert.AreEqual("restart", settings.IniSettingApplies("ServiceAccountName"));

         Assert.Throws<COMException>(() => settings.IniSettingApplies("TCPIPThreads"),
            "TCPIPThreads is an hm_settings property; the [Settings] loader does not read it, so it has no mark.");
         Assert.Throws<COMException>(() => settings.IniSettingApplies("NoSuchSettingAnywhere"));

         Assert.IsFalse(settings.PendingIniSettings.Contains("ManageSieveServerPort"), "ManageSieveServerPort should not be pending at the start.");

         settings.SetIniSetting("ManageSieveServerPort", SievePortB.ToString());
         string[] overCom = settings.PendingIniSettings.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);
         CollectionAssert.AreEquivalent(Pending_().Keys.ToArray(), overCom, "COM and REST disagree about what is pending.");
         CollectionAssert.Contains(overCom, "ManageSieveServerPort");

         settings.SetIniSetting("ManageSieveServerPort", "0");
         Assert.IsFalse(settings.PendingIniSettings.Contains("ManageSieveServerPort"), "A value put back is still listed over COM.");
         CollectionAssert.AreEquivalent(Pending_().Keys.ToArray(),
            settings.PendingIniSettings.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries),
            "COM and REST disagree about what is pending once the value is put back.");
      }
   }
}
