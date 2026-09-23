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
   ///    allocated, 9876-9885). What the set-up changes the teardown puts back, each
   ///    write guarded on its own, and it reinitialises whatever a failure left;
   ///    what a test changes beyond that, the test puts back in its own finally.
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
            Guarded_(() => Admin("DELETE", "/api/v1/server/drain", null));
            Guarded_(() => { if ((int) _application.ServerState != StateRunning) _application.Start(); });

            // Each on its own, so one that fails does not keep the others from being
            // put back.
            Guarded_(() => IniFileSetting.Write("MaxSubmissionsPerIPPerMinute", "0"));
            Guarded_(() => IniFileSetting.Write("ManageSieveServerPort", "0"));
            Guarded_(() => IniFileSetting.Write("CalDavRedirectUrl", ""));
            Guarded_(() => IniFileSetting.Delete("WebServicesBindAddress"));
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      private static void Guarded_(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("SettingsApplyOnSave teardown: " + e.Message);
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

      /// <summary>GET /api/v1/settings/ini/{name}, answered as JSON.</summary>
      private JsonElement GetIni_(string name)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/settings/ini/" + name, null);
         Assert.AreEqual(200, answer.status, "GET " + name + ": " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      /// <summary>DELETE /api/v1/settings/ini/{name}, answered as JSON.</summary>
      private JsonElement DeleteIni_(string name)
      {
         (int status, string body) answer = Admin("DELETE", "/api/v1/settings/ini/" + name, null);
         Assert.AreEqual(200, answer.status, "DELETE " + name + ": " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      /// <summary>Puts one key back as GetIni_ found it: stored with its value, or not stored.</summary>
      private void PutBack_(string name, JsonElement before)
      {
         if (before.GetProperty("present").GetBoolean())
            PutIni_(name, before.GetProperty("value").GetString());
         else
            DeleteIni_(name);
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

            // Applies is the next-to-last field since the setting-levels row added
            // Levels after it; counting from the end keeps a JSON default that
            // contains spaces from shifting it.
            marks[fields[1]] = fields[fields.Length - 2];
         }

         return marks;
      }

      // ---- the tests ------------------------------------------------------------

      [Test]
      [Description("A key marked now applies when it is saved, with no reinitialise: MaxSubmissionsPerIPPerMinute set to 2 over REST makes the third MAIL FROM on a fresh connection a 421 at once, and DELETE returns it to its default of unlimited at once - the answer says applies now and pending false, and the next MAIL FROM is accepted. The negative control is the first half: at 0, ten transactions in a row are accepted.")]
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

         // Returned to its default with DELETE, which publishes as PUT does: no
         // reinitialise, and the throttle is gone for the next connection.
         JsonElement removed = DeleteIni_("MaxSubmissionsPerIPPerMinute");
         Assert.IsFalse(removed.GetProperty("present").GetBoolean(), "Answer: " + removed);
         Assert.AreEqual("now", removed.GetProperty("applies").GetString(), "The DELETE answer should carry the key's mark: " + removed);
         Assert.IsFalse(removed.GetProperty("pending").GetBoolean(), "A key marked now is not pending after a DELETE: " + removed);

         using (TcpConnection socket = OpenSmtp_())
         {
            for (int i = 0; i < 3; i++)
               StringAssert.StartsWith("250", StartTransaction_(socket), "Deleted, the default of unlimited should apply at once - transaction " + i + ".");
            socket.Send("QUIT\r\n");
         }
      }

      [Test]
      [Description("A saved per-minute limit counts from when it was saved (the bench of 23 September 2026, SMTP.RateShaping): with MaxSubmissionsPerIPPerMinute at 2, two MAIL FROMs are accepted and the third refused; saved as 3, the window starts again and a fresh connection is accepted three times before the fourth is refused. The negative control is the first half, and the count: kept, the two events already in the window would have let one more through, not three - which is how a test that had used the limit left RateShaping's first MAIL FROM refused a few seconds later.")]
      public void ASavedRateLimitCountsFromWhenItWasSaved()
      {
         try
         {
            PutIni_("MaxSubmissionsPerIPPerMinute", "2");
            using (TcpConnection socket = OpenSmtp_())
            {
               StringAssert.StartsWith("250", StartTransaction_(socket), "The first transaction at 2.");
               StringAssert.StartsWith("250", StartTransaction_(socket), "The second transaction at 2.");
               StringAssert.StartsWith("421", StartTransaction_(socket), "The third transaction at 2 should be refused.");
               socket.Send("QUIT\r\n");
            }

            PutIni_("MaxSubmissionsPerIPPerMinute", "3");
            using (TcpConnection socket = OpenSmtp_())
            {
               for (int i = 0; i < 3; i++)
                  StringAssert.StartsWith("250", StartTransaction_(socket), "Saved as 3, transaction " + i + " of a new window should be accepted.");
               StringAssert.StartsWith("421", StartTransaction_(socket), "The fourth transaction at 3 should be refused.");
               socket.Send("QUIT\r\n");
            }
         }
         finally
         {
            DeleteIni_("MaxSubmissionsPerIPPerMinute");
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
      [Description("A check of the route against the baseline, not a proof that any mark is right: GET /api/v1/settings/ini keeps its entries as {name, value, secret} - the shape the export clients read, a name the loader does not read listed like any other - and beside them an applies map holding exactly the [Settings] keys of build/ini-settings-baseline.txt, each with the baseline's mark, and a pending array naming what GET /api/v1/server/settings/pending lists. check-ini-coverage.py already holds the header the route reads to the baseline, so what this proves is that the route answers the header; whether a mark is TRUE is what the behavioural tests here prove, for the keys they exercise.")]
      public void EveryKeyCarriesItsMark()
      {
         // A name the loader does not read, stored as a script's own key would be:
         // listed with its value, and without a mark.
         const string ScriptKey = "ZzSettingsApplyScriptKey";
         PutIni_(ScriptKey, "kept for a script");

         try
         {
            (int status, string body) answer = Admin("GET", "/api/v1/settings/ini", null);
            Assert.AreEqual(200, answer.status, answer.body);
            JsonElement root = JsonDocument.Parse(answer.body).RootElement;

            StringAssert.Contains("{\"name\":\"" + ScriptKey + "\",\"value\":\"kept for a script\",\"secret\":false}", answer.body,
               "A stored name the loader does not read is listed with its value, in the entry shape the clients read.");

            foreach (JsonElement entry in root.GetProperty("settings").EnumerateArray())
            {
               var members = entry.EnumerateObject().Select(property => property.Name).ToArray();
               CollectionAssert.AreEqual(new[] { "name", "value", "secret" }, members,
                  entry.GetProperty("name").GetString() + " is listed with members other than name, value and secret.");
            }

            CollectionAssert.AreEquivalent(Pending_().Keys.ToArray(),
               root.GetProperty("pending").EnumerateArray().Select(name => name.GetString()).ToArray(),
               "The list's pending array and the pending route disagree.");

            Assert.IsFalse(root.GetProperty("applies").TryGetProperty(ScriptKey, out _), "A name the loader does not read has a mark in the applies map.");
         }
         finally
         {
            DeleteIni_(ScriptKey);
         }

         JsonElement listed = JsonDocument.Parse(Admin("GET", "/api/v1/settings/ini", null).body).RootElement;
         Dictionary<string, string> baseline = BaselineMarks_();

         var served = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (JsonProperty property in listed.GetProperty("applies").EnumerateObject())
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

         // Every mark in one call - what the Control Panel reads once per page - and
         // the same map the REST list answers.
         var overComAll = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (JsonProperty property in JsonDocument.Parse(settings.IniSettingAppliesAll).RootElement.EnumerateObject())
            overComAll[property.Name] = property.Value.GetString();
         var overRest = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (JsonProperty property in JsonDocument.Parse(Admin("GET", "/api/v1/settings/ini", null).body).RootElement.GetProperty("applies").EnumerateObject())
            overRest[property.Name] = property.Value.GetString();
         CollectionAssert.AreEquivalent(overRest, overComAll, "IniSettingAppliesAll and the REST applies map disagree.");
         Assert.AreEqual("reinitialize", overComAll["ManageSieveServerPort"]);
      }

      [Test]
      [Description("REST and COM agree about a name the loader does not read: COM's IniSettingApplies refuses it, and GET /api/v1/settings/ini/{name} answers applies null with the refusal's own sentence as applies_refused - not now, which made a misspelled key look like a working setting. The route still reads the name, which it stores and reads on purpose for a script's own key. The negative control is a key the loader does read, which answers its mark and no applies_refused. COM also takes the name as spelled: with a space round it, it is no key.")]
      public void ANameTheLoaderDoesNotReadHasNoMarkOnEitherSurface()
      {
         const string Unknown = "NoSuchSettingTheLoaderReads";

         string refusedOverCom = null;
         try
         {
            _settings.IniSettingApplies(Unknown);
         }
         catch (COMException e)
         {
            refusedOverCom = e.Message;
         }
         Assert.IsNotNull(refusedOverCom, "COM answered a mark for a name the loader does not read.");

         JsonElement overRest = GetIni_(Unknown);
         Assert.AreEqual(JsonValueKind.Null, overRest.GetProperty("applies").ValueKind, "REST answered a mark for a name the loader does not read: " + overRest);
         Assert.AreEqual(refusedOverCom.Trim(), overRest.GetProperty("applies_refused").GetString(), "REST and COM refuse the name in different words.");
         Assert.IsFalse(overRest.GetProperty("pending").GetBoolean());

         JsonElement known = GetIni_("MaxSubmissionsPerIPPerMinute");
         Assert.AreEqual("now", known.GetProperty("applies").GetString());
         Assert.IsFalse(known.TryGetProperty("applies_refused", out _), "A key the loader reads carries applies_refused: " + known);

         Assert.Throws<COMException>(() => _settings.IniSettingApplies(" MaxSubmissionsPerIPPerMinute "),
            "A name with a space round it is not a key, as GetIniSetting refuses it.");
      }

      [Test]
      [Description("Saves racing saves (the review of 22 September 2026, finding 1): two clients each make twenty PUT/DELETE pairs on a key of their own at once - one ending on a DELETE, one on a PUT - and afterwards each key is stored exactly as its own client's last acknowledged request left it. Before the save path took the reload lock for its store write, and before a save's reload was made read-only, one save's reload reconciling the file against the table could write a line back between another save's DELETE of the line and of the row - which that save's reload then adopted, so the setting came back after a DELETE that answered 200 - or put an older value back over a PUT. The race needs two saves inside a few milliseconds of each other, so this cannot fail on demand against that code; it holds the end state, which that code could leave wrong. The internal IniFileSettingsTester runs the same storm on eight threads, and checks the running values as well.")]
      public void SavesRacingSavesLeaveWhatWasLastAcknowledged()
      {
         const int Pairs = 20;
         const string EndsDeleted = "DmarcRptOrganizationName";
         const string EndsWritten = "TlsRptOrganizationName";

         JsonElement deletedBefore = GetIni_(EndsDeleted);
         JsonElement writtenBefore = GetIni_(EndsWritten);

         var failures = new List<string>();
         string lastWritten = null;

         try
         {
            var first = new Thread(() =>
            {
               for (int i = 0; i < Pairs; i++)
               {
                  (int status, string body) put = Admin("PUT", "/api/v1/settings/ini/" + EndsDeleted, "{\"value\":\"racing " + i + "\"}");
                  (int status, string body) delete = Admin("DELETE", "/api/v1/settings/ini/" + EndsDeleted, null);
                  if (put.status != 200 || delete.status != 200)
                     lock (failures) failures.Add(EndsDeleted + " pair " + i + ": " + put.status + "/" + delete.status);
               }
            });

            var second = new Thread(() =>
            {
               for (int i = 0; i < Pairs; i++)
               {
                  string value = "racing " + i;
                  (int status, string body) delete = Admin("DELETE", "/api/v1/settings/ini/" + EndsWritten, null);
                  (int status, string body) put = Admin("PUT", "/api/v1/settings/ini/" + EndsWritten, "{\"value\":\"" + value + "\"}");
                  if (put.status != 200 || delete.status != 200)
                     lock (failures) failures.Add(EndsWritten + " pair " + i + ": " + delete.status + "/" + put.status);
                  else
                     lastWritten = value;
               }
            });

            first.Start();
            second.Start();
            first.Join();
            second.Join();

            CollectionAssert.IsEmpty(failures, "Some of the racing requests were refused.");

            JsonElement deleted = GetIni_(EndsDeleted);
            Assert.IsFalse(deleted.GetProperty("present").GetBoolean(),
               EndsDeleted + "'s last acknowledged request was a DELETE, and the setting is back: " + deleted);

            JsonElement written = GetIni_(EndsWritten);
            Assert.IsTrue(written.GetProperty("present").GetBoolean(), EndsWritten + "'s last acknowledged request was a PUT, and nothing is stored: " + written);
            Assert.AreEqual(lastWritten, written.GetProperty("value").GetString(),
               EndsWritten + " holds a value other than the last one acknowledged - another save put an older one back.");

            // And what the store itself holds, over COM, which reads the same rows.
            string[] names = _settings.IniSettingNames.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);
            CollectionAssert.DoesNotContain(names, EndsDeleted);
            CollectionAssert.Contains(names, EndsWritten);
         }
         finally
         {
            PutBack_(EndsDeleted, deletedBefore);
            PutBack_(EndsWritten, writtenBefore);
         }
      }

      [Test]
      [Description("A key marked restart is held across a reinitialise: ServiceAccountName saved answers pending with applies restart, and after POST /api/v1/server/reinitialize it is still pending with the same running value - the reinitialise takes the keys marked reinitialize and not this one. Put back, it is not pending. The negative control is the reinitialize key beside it in AReinitializeKeyWaitsForTheReinitialize, which the same reinitialise does take.")]
      public void ARestartKeyIsHeldAcrossAReinitialize()
      {
         const string Key = "ServiceAccountName";
         JsonElement before = GetIni_(Key);
         string running = before.GetProperty("value").GetString();

         try
         {
            JsonElement saved = PutIni_(Key, @".\hmail-settings-apply-test");
            Assert.AreEqual("restart", saved.GetProperty("applies").GetString(), "Answer: " + saved);
            Assert.IsTrue(saved.GetProperty("pending").GetBoolean(), "A saved restart key should answer pending: " + saved);

            ReinitializeOverRest_();

            Dictionary<string, JsonElement> pending = Pending_();
            Assert.IsTrue(pending.ContainsKey(Key), "The reinitialise took a key marked restart.");
            Assert.AreEqual(running, pending[Key].GetProperty("running").GetString(), "The running value moved across the reinitialise.");
            Assert.AreEqual(@".\hmail-settings-apply-test", pending[Key].GetProperty("stored").GetString());
            Assert.AreEqual("restart", pending[Key].GetProperty("applies").GetString());
         }
         finally
         {
            PutBack_(Key, before);
         }

         Assert.IsFalse(Pending_().ContainsKey(Key), "Put back, the key is still pending.");
      }

      [Test]
      [Description("A secret in the pending list is named and never shown: MetricsServerAuthPassword saved answers pending, and GET /api/v1/server/settings/pending lists it with secret true, its stored value as (set) and its running value as (set) or empty - and the password's text appears nowhere in the answer. The negative control is ManageSieveServerPort, which is listed with its two values as they are.")]
      public void ASecretIsNamedInThePendingListAndNeverShown()
      {
         const string Key = "MetricsServerAuthPassword";
         const string Password = "pending-list-secret-9876";
         JsonElement before = GetIni_(Key);
         bool hadOne = before.GetProperty("value").GetString().Length > 0;

         try
         {
            PutIni_(Key, Password);
            PutIni_("ManageSieveServerPort", SievePortA.ToString());

            (int status, string body) answer = Admin("GET", "/api/v1/server/settings/pending", null);
            Assert.AreEqual(200, answer.status, answer.body);
            StringAssert.DoesNotContain(Password, answer.body, "The pending list shows the password.");

            Dictionary<string, JsonElement> pending = Pending_();
            Assert.IsTrue(pending.ContainsKey(Key), "The saved secret is not listed as pending: " + answer.body);
            Assert.IsTrue(pending[Key].GetProperty("secret").GetBoolean());
            Assert.AreEqual("(set)", pending[Key].GetProperty("stored").GetString());
            Assert.AreEqual(hadOne ? "(set)" : "", pending[Key].GetProperty("running").GetString());

            Assert.IsFalse(pending["ManageSieveServerPort"].GetProperty("secret").GetBoolean());
            Assert.AreEqual(SievePortA.ToString(), pending["ManageSieveServerPort"].GetProperty("stored").GetString());
         }
         finally
         {
            PutIni_("ManageSieveServerPort", "0");
            PutBack_(Key, before);
         }
      }

      [Test]
      [Description("The database fault injector says so the moment it is set, with no reinitialise: SimulateDatabaseFailureFor saved over REST - a fragment no statement contains, so nothing fails - puts HM6119 in the error log from the save's own subscriber. The negative control is the set-up, which starts every test with no error log at all, and the clear at the end, which announces nothing.")]
      public void TheFaultInjectorAnnouncesItselfWhenItIsSaved()
      {
         const string Key = "SimulateDatabaseFailureFor";
         JsonElement before = GetIni_(Key);
         Assert.AreEqual("", before.GetProperty("value").GetString(), "The fault injector is already set on this bench.");

         try
         {
            JsonElement saved = PutIni_(Key, "no-statement-contains-this-9876");
            Assert.AreEqual("now", saved.GetProperty("applies").GetString(), "Answer: " + saved);

            CustomAsserts.AssertReportedError("HM6119", "no-statement-contains-this-9876");
         }
         finally
         {
            PutBack_(Key, before);
            LogHandler.DeleteErrorLog();
         }
      }
   }
}
