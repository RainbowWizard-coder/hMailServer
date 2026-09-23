// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Installation
{
   /// <summary>
   ///    Every setting from the environment (docs/EnvironmentConfiguration.md):
   ///    HM_SETTING_&lt;key&gt; and HM_PROPERTY_&lt;name&gt;, their _FILE forms and the
   ///    container's four HM_REST_* names, stored by `hMailServer.exe
   ///    --apply-environment`, and the record of which settings the environment
   ///    manages - answered beside each by REST, listed by GET
   ///    /api/v1/settings/environment, and refused to every other interface.
   ///
   ///    The environment is set for a CHILD process only - the verb, started from
   ///    here with ProcessStartInfo's environment - never for the bench's service.
   ///    The verb writes the database the service runs on; the service takes what
   ///    it wrote at its next reinitialise, as a container's server takes it at its
   ///    start, so every test that reads back reinitialises first. Each has its
   ///    negative control.
   ///
   ///    Ports: the REST listener on 10020 (the row's range is 10020-10029). What
   ///    the tests change - the maximum message size, the SMTP greeting, the
   ///    submission rate key and the record itself - the teardown puts back, the
   ///    record first: while it names a setting, the COM and REST writes that put
   ///    the others back would be refused.
   /// </summary>
   [TestFixture]
   public class EnvironmentConfiguration : TestFixtureBase
   {
      private static int RestPort = 10020;
      private const string AdminPassword = "testar";

      // A [Settings] key marked now, harmless on a bench that sends no mail here.
      private const string IniKey = "MaxSubmissionsPerIPPerMinute";

      private int maxMessageSizeBefore_;
      private string welcomeBefore_;
      private bool iniKeyPresentBefore_;
      private string iniKeyBefore_;
      private int retentionBefore_;
      private readonly List<string> files_ = new List<string>();
      private readonly List<string> keys_ = new List<string>();

      [SetUp]
      public void StartListener()
      {
         // A record an earlier run left would refuse the writes below, and the one
         // at the end of the teardown.
         RunVerb(new Dictionary<string, string>());

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         maxMessageSizeBefore_ = _settings.MaxMessageSize;
         welcomeBefore_ = _settings.WelcomeSMTP;
         JsonElement ini = GetIni(IniKey);
         iniKeyPresentBefore_ = ini.GetProperty("present").GetBoolean();
         iniKeyBefore_ = ini.GetProperty("value").GetString();
         retentionBefore_ = Settings("/api/v1/settings").GetProperty("domain_report_retention_days").GetInt32();
      }

      [TearDown]
      public void PutTheServerBack()
      {
         try
         {
            // The record first, and the server told: until then the writes below are
            // refused, which is the point of the record.
            Guarded(() => RunVerb(new Dictionary<string, string>()));
            Guarded(() => _application.Reinitialize());

            Guarded(() => _settings.MaxMessageSize = maxMessageSizeBefore_);
            Guarded(() => _settings.WelcomeSMTP = welcomeBefore_);
            Guarded(() =>
            {
               if (iniKeyPresentBefore_)
                  _settings.SetIniSetting(IniKey, iniKeyBefore_);
               else
                  _settings.DeleteIniSetting(IniKey);
            });
            Guarded(() => Admin("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":" + retentionBefore_ + "}"));
            Guarded(() => { foreach (string id in keys_) Admin("DELETE", "/api/v1/apikeys/" + id, null); });
            Guarded(() => { foreach (string file in files_) File.Delete(file); });
         }
         finally
         {
            keys_.Clear();
            files_.Clear();
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---- the verb and the names ----------------------------------------------

      [Test]
      [Description("HM_PROPERTY_maxmessagesize stores the property, read back over REST and COM once the server has reinitialised; the verb prints set, and a second run prints unchanged. Negative control: the value before the run is not the one stored.")]
      public void APropertyFromTheEnvironmentIsStoredAndASecondRunChangesNothing()
      {
         int value = maxMessageSizeBefore_ == 1234 ? 1235 : 1234;
         Assert.AreNotEqual(value, _settings.MaxMessageSize, "The negative control: the server must not hold the value before the run.");

         Verb first = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_maxmessagesize", value.ToString() } });
         Assert.AreEqual(0, first.ExitCode, first.Output);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize: set", first.Output);

         _application.Reinitialize();
         Assert.AreEqual(value, _settings.MaxMessageSize, "COM does not read back the value the environment stored.");
         JsonElement group = Settings("/api/v1/settings");
         Assert.AreEqual(value, group.GetProperty("max_message_size_kb").GetInt32(), "REST does not read back the value the environment stored.");
         Assert.AreEqual("variable", group.GetProperty("max_message_size_kb_environment").GetString());

         Verb second = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_maxmessagesize", value.ToString() } });
         Assert.AreEqual(0, second.ExitCode, second.Output);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize: unchanged", second.Output);
         StringAssert.DoesNotContain(": set", second.Output);
      }

      [Test]
      [Description("A name that is not a setting is refused naming the nearest, exits 1 and changes nothing: MaxMessageSize is no [Settings] key and the sentence names HM_PROPERTY_maxmessagesize; a misspelled key names the key it was meant for. Negative control: the correctly spelled key in the same run is not stored either.")]
      public void AnUnknownNameIsRefusedNamingTheNearestAndNothingIsChanged()
      {
         string before = GetIni(IniKey).GetProperty("value").GetString();
         string wanted = before == "7" ? "8" : "7";

         Verb run = RunVerb(new Dictionary<string, string>
         {
            { "HM_SETTING_MaxMessageSize", "1234" },
            { "HM_SETTING_MaxSubmisionsPerIPPerMinute", "5" },
            { "HM_SETTING_" + IniKey, wanted },
         });

         Assert.AreEqual(1, run.ExitCode, run.Output);
         StringAssert.Contains("HM_SETTING_MaxMessageSize: refused", run.Output);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize", run.Output);
         StringAssert.Contains("HM_SETTING_MaxSubmisionsPerIPPerMinute: refused", run.Output);
         StringAssert.Contains("HM_SETTING_MaxSubmissionsPerIPPerMinute sets", run.Output);
         StringAssert.Contains("HM_SETTING_" + IniKey + ": not stored, because another variable was refused", run.Output);
         StringAssert.Contains("Nothing was changed", run.Output);

         _application.Reinitialize();
         Assert.AreEqual(before, GetIni(IniKey).GetProperty("value").GetString(), "A refused run stored a value.");
         Assert.AreEqual(maxMessageSizeBefore_, _settings.MaxMessageSize);
         Assert.AreEqual(0, Environment_().Count, "A refused run wrote the record.");
      }

      [Test]
      [Description("A value of the wrong kind is refused: a [Settings] key read as a number refuses a word, and a property is refused with the sentence its REST settings row refuses the same value with - the same words, compared. Negative control: the REST route refuses the value too, and the value in range is taken by both.")]
      public void AValueOfTheWrongKindIsRefusedWithTheRestSentence()
      {
         Verb word = RunVerb(new Dictionary<string, string> { { "HM_SETTING_" + IniKey, "many" } });
         Assert.AreEqual(1, word.ExitCode, word.Output);
         StringAssert.Contains("HM_SETTING_" + IniKey + ": refused", word.Output);
         StringAssert.Contains("whole number", word.Output);

         (int status, string body) rest = Admin("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":5000}");
         Assert.AreEqual(400, rest.status, rest.body);
         string sentence = JsonDocument.Parse(rest.body).RootElement.GetProperty("error").GetString();

         Verb range = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_DomainReportRetentionDays", "5000" } });
         Assert.AreEqual(1, range.ExitCode, range.Output);
         StringAssert.Contains("HM_PROPERTY_DomainReportRetentionDays: refused: " + sentence, range.Output);

         int inRange = retentionBefore_ == 400 ? 401 : 400;
         Verb taken = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_DomainReportRetentionDays", inRange.ToString() } });
         Assert.AreEqual(0, taken.ExitCode, taken.Output);
         StringAssert.Contains("HM_PROPERTY_DomainReportRetentionDays: set", taken.Output);
      }

      [Test]
      [Description("A _FILE form reads the value from the file, its trailing line break stripped, and the value is never printed, never answered over REST and never in the changed document; the plain form and the _FILE form set together are refused. Negative control: COM reads the value the file held, without its line break.")]
      public void AFileSuppliesTheValueWhichIsNeverShownAndBothFormsAreRefused()
      {
         string secretText = "Greeting from a Secret " + Guid.NewGuid().ToString("N").Substring(0, 8);
         string file = Path.Combine(Path.GetTempPath(), "hm-env-" + Guid.NewGuid().ToString("N") + ".txt");
         File.WriteAllText(file, secretText + "\r\n", new UTF8Encoding(false));
         files_.Add(file);

         Verb both = RunVerb(new Dictionary<string, string>
         {
            { "HM_PROPERTY_welcomesmtp", "plain" },
            { "HM_PROPERTY_welcomesmtp_FILE", file },
         });
         Assert.AreEqual(1, both.ExitCode, both.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp, HM_PROPERTY_welcomesmtp_FILE all name welcomesmtp; set one of them", both.Output);

         // A setting a domain or an account may hold its own value of is shown to
         // their administrators as what is in effect, so a file cannot keep it secret.
         Verb levels = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_maxmessagesize_FILE", file } });
         Assert.AreEqual(1, levels.ExitCode, levels.Output);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize_FILE: refused: maxmessagesize may also be set for a domain and an account", levels.Output);
         StringAssert.DoesNotContain(secretText, levels.Output);

         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", file } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE: set (welcomesmtp, from a file; the value is not shown)", run.Output);
         StringAssert.DoesNotContain(secretText, run.Output);

         _application.Reinitialize();
         Assert.AreEqual(secretText, _settings.WelcomeSMTP, "The value is the file's, less its trailing line break.");

         JsonElement group = Settings("/api/v1/settings");
         Assert.AreEqual("(set)", group.GetProperty("welcome_smtp").GetString());
         Assert.AreEqual("file", group.GetProperty("welcome_smtp_environment").GetString());

         foreach (string path in new[] { "/api/v1/settings", "/api/v1/settings/changed", "/api/v1/settings/environment" })
         {
            (int status, string body) answer = Admin("GET", path, null);
            Assert.AreEqual(200, answer.status, path + ": " + answer.body);
            StringAssert.DoesNotContain(secretText, answer.body, path + " answered the value a file supplied.");
         }

         // And the second run reads the same file and changes nothing.
         Verb again = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", file } });
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE: unchanged", again.Output);
      }

      // ---- the record, and the refusals ---------------------------------------

      [Test]
      [Description("A setting the environment set is answered as managed and refused to REST (409) and COM when a change would move it, and taken when it says what the setting holds; a run without the variable takes the record away and the change is taken. Negative control: the same writes before and after.")]
      public void AManagedSettingIsRefusedEverywhereUntilARunWithoutIt()
      {
         int size = maxMessageSizeBefore_ == 1234 ? 1235 : 1234;
         Verb run = RunVerb(new Dictionary<string, string>
         {
            { "HM_PROPERTY_maxmessagesize", size.ToString() },
            { "HM_SETTING_" + IniKey, "7" },
         });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         _application.Reinitialize();

         JsonElement ini = GetIni(IniKey);
         Assert.IsTrue(ini.GetProperty("managed").GetBoolean(), ini.ToString());
         Assert.AreEqual("variable", ini.GetProperty("environment").GetString());
         Assert.AreEqual("7", ini.GetProperty("value").GetString());

         (int status, string body) put = Admin("PUT", "/api/v1/settings/ini/" + IniKey, "{\"value\":\"9\"}");
         Assert.AreEqual(409, put.status, put.body);
         StringAssert.Contains("HM_SETTING_" + IniKey, put.body);
         StringAssert.Contains("set by this server's environment", put.body);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/ini/" + IniKey, "{\"value\":\"7\"}").status, "The value it has is taken.");
         Assert.AreEqual(409, Admin("DELETE", "/api/v1/settings/ini/" + IniKey, null).status);

         (int status, string body) group = Admin("PUT", "/api/v1/settings", "{\"max_message_size_kb\":" + (size + 10) + "}");
         Assert.AreEqual(409, group.status, group.body);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize", group.body);
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"max_message_size_kb\":" + size + "}").status, "The value it has is taken.");
         Assert.AreEqual(400, Admin("PUT", "/api/v1/settings", "{\"max_message_size_kb_environment\":\"file\"}").status, "The mark is answered and never taken.");

         COMException comIni = Assert.Throws<COMException>(() => _settings.SetIniSetting(IniKey, "9"));
         StringAssert.Contains("set by this server's environment", comIni.Message);
         COMException comProperty = Assert.Throws<COMException>(() => _settings.MaxMessageSize = size + 10);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize", comProperty.Message);
         Assert.AreEqual(size, _settings.MaxMessageSize, "A refused COM write changed the value.");

         // Without the variables: the record goes, and the same writes are taken.
         Verb without = RunVerb(new Dictionary<string, string>());
         Assert.AreEqual(0, without.ExitCode, without.Output);
         StringAssert.Contains("No longer set by the environment", without.Output);
         _application.Reinitialize();

         Assert.IsFalse(GetIni(IniKey).GetProperty("managed").GetBoolean());
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/ini/" + IniKey, "{\"value\":\"9\"}").status);
         Assert.IsFalse(Settings("/api/v1/settings").TryGetProperty("max_message_size_kb_environment", out _));
         _settings.MaxMessageSize = size + 10;
         Assert.AreEqual(size + 10, _settings.MaxMessageSize);
      }

      [Test]
      [Description("GET /api/v1/settings/environment lists each managed setting with its variable, store and source and never a value; a read-only key may read it and a key restricted to a domain may not. Negative control: nothing managed, nothing listed.")]
      public void TheEnvironmentRouteListsNamesAndSourcesToAReader()
      {
         Assert.AreEqual(0, Environment_().Count, "The negative control: nothing is managed before the run.");

         string file = Path.Combine(Path.GetTempPath(), "hm-env-" + Guid.NewGuid().ToString("N") + ".txt");
         File.WriteAllText(file, "Banner " + Guid.NewGuid().ToString("N"), new UTF8Encoding(false));
         files_.Add(file);

         Verb run = RunVerb(new Dictionary<string, string>
         {
            { "HM_PROPERTY_maxmessagesize", _settings.MaxMessageSize.ToString() },
            { "HM_PROPERTY_welcomesmtp_FILE", file },
         });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         _application.Reinitialize();

         List<JsonElement> listed = Environment_();
         Assert.AreEqual(2, listed.Count, string.Join(", ", listed));
         JsonElement size = listed.Single(e => e.GetProperty("name").GetString() == "maxmessagesize");
         Assert.AreEqual("HM_PROPERTY_maxmessagesize", size.GetProperty("variable").GetString());
         Assert.AreEqual("database", size.GetProperty("store").GetString());
         Assert.AreEqual("variable", size.GetProperty("source").GetString());
         JsonElement greeting = listed.Single(e => e.GetProperty("name").GetString() == "welcomesmtp");
         Assert.AreEqual("file", greeting.GetProperty("source").GetString());
         Assert.IsFalse(greeting.TryGetProperty("value", out _), "An entry carries a value.");

         string reader = CreateKey("environment - reader", "readonly", null);
         (int status, string body) asReader = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/settings/environment", "Bearer " + reader, null);
         Assert.AreEqual(200, asReader.status, asReader.body);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE", asReader.body);

         string confined = CreateKey("environment - confined", "full", "elsewhere.test");
         Assert.AreEqual(403, RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/settings/environment", "Bearer " + confined, null).status,
            "The environment is the server's; a key confined to a domain does not read it.");
      }

      [Test]
      [Description("The container's four REST variables are aliases of the settings they always stored: HM_REST_PORT and HM_REST_BIND_ADDRESS are recorded under their own names for RestApiPort and RestApiBindAddress, and each of the four refuses to be set beside the HM_SETTING_ name it stands for. Negative control: the run that sets them only changes nothing - the values are the ones the listener has.")]
      public void TheLegacyRestNamesAreAliasesOfTheSettings()
      {
         Verb clash = RunVerb(new Dictionary<string, string>
         {
            { "HM_REST_PORT", RestPort.ToString() }, { "HM_SETTING_RestApiPort", RestPort.ToString() },
            { "HM_REST_CERTIFICATE_FILE", @"C:\no\such.pem" }, { "HM_SETTING_RestApiCertificateFile", @"C:\no\such.pem" },
            { "HM_REST_PRIVATE_KEY_FILE", @"C:\no\such.key" }, { "HM_SETTING_RestApiPrivateKeyFile", @"C:\no\such.key" },
            { "HM_REST_BIND_ADDRESS", "127.0.0.1" }, { "HM_SETTING_RestApiBindAddress", "127.0.0.1" },
         });
         Assert.AreEqual(1, clash.ExitCode, clash.Output);
         StringAssert.Contains("HM_REST_PORT, HM_SETTING_RestApiPort all name RestApiPort", clash.Output);
         StringAssert.Contains("HM_REST_CERTIFICATE_FILE, HM_SETTING_RestApiCertificateFile all name RestApiCertificateFile", clash.Output);
         StringAssert.Contains("HM_REST_PRIVATE_KEY_FILE, HM_SETTING_RestApiPrivateKeyFile all name RestApiPrivateKeyFile", clash.Output);
         StringAssert.Contains("HM_REST_BIND_ADDRESS, HM_SETTING_RestApiBindAddress all name RestApiBindAddress", clash.Output);

         Verb run = RunVerb(new Dictionary<string, string>
         {
            { "HM_REST_PORT", RestPort.ToString() },
            { "HM_REST_BIND_ADDRESS", "127.0.0.1" },
         });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("HM_REST_PORT: unchanged", run.Output);
         StringAssert.Contains("HM_REST_BIND_ADDRESS: unchanged", run.Output);

         _application.Reinitialize();
         List<JsonElement> listed = Environment_();
         JsonElement port = listed.Single(e => e.GetProperty("name").GetString() == "RestApiPort");
         Assert.AreEqual("HM_REST_PORT", port.GetProperty("variable").GetString());
         Assert.AreEqual("ini", port.GetProperty("store").GetString());
         Assert.IsTrue(listed.Any(e => e.GetProperty("variable").GetString() == "HM_REST_BIND_ADDRESS"));
      }

      [Test]
      [Description("--apply-environment with nothing set is a no-op that exits 0 and says so. Negative control: nothing is recorded as managed after it.")]
      public void NothingSetIsANoOp()
      {
         Verb run = RunVerb(new Dictionary<string, string>());
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("No variable names a setting", run.Output);
         StringAssert.DoesNotContain(": set", run.Output);

         _application.Reinitialize();
         Assert.AreEqual(0, Environment_().Count);
         Assert.AreEqual(maxMessageSizeBefore_, _settings.MaxMessageSize);
      }

      [Test]
      [Description("build/check-environment-names.py passes, and proves its own negative control: a name that is no setting, in a copy of the document, is refused.")]
      public void TheNamesCheckerPassesAndRefusesAMadeUpName()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH, so the checker cannot run here.");

         string checker = RepositoryFile(Path.Combine("build", "check-environment-names.py"));
         var start = new ProcessStartInfo(python, "\"" + checker + "\"")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };
         start.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            ClassicAssert.IsTrue(process.WaitForExit(120 * 1000), "The checker did not finish within two minutes.");
            process.WaitForExit();

            string printed = output.Result + error.Result;
            ClassicAssert.AreEqual(0, process.ExitCode, printed);
            StringAssert.Contains("negative control", printed);
         }
      }

      // ---- helpers ------------------------------------------------------------

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      /// <summary>
      ///    hMailServer.exe --apply-environment, beside the service it administers,
      ///    with exactly these HM_ variables in its environment and none of the
      ///    runner's.
      /// </summary>
      private Verb RunVerb(Dictionary<string, string> variables)
      {
         string executable = ServerExecutable();

         var start = new ProcessStartInfo(executable, "--apply-environment")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         foreach (KeyValuePair<string, string> variable in variables)
            start.EnvironmentVariables[variable.Key] = variable.Value;

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            Assert.IsTrue(process.WaitForExit(180 * 1000), "--apply-environment did not finish within three minutes.");
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      /// <summary>The hMailServer.exe the service runs, from the program directory it reports.</summary>
      private string ServerExecutable()
      {
         string program = _settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("EnvironmentConfiguration teardown: " + e.Message);
         }
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private JsonElement GetIni(string name)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/settings/ini/" + name, null);
         Assert.AreEqual(200, answer.status, "GET " + name + ": " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      private JsonElement Settings(string path)
      {
         (int status, string body) answer = Admin("GET", path, null);
         Assert.AreEqual(200, answer.status, "GET " + path + ": " + answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      private List<JsonElement> Environment_()
         => Settings("/api/v1/settings/environment").GetProperty("settings").EnumerateArray().Select(e => e.Clone()).ToList();

      private string CreateKey(string label, string scope, string domains)
      {
         string body = "{\"label\":\"" + label + "\",\"scope\":\"" + scope + "\"" +
                       (domains == null ? "" : ",\"domains\":\"" + domains + "\"") + "}";
         (int status, string answer) = Admin("POST", "/api/v1/apikeys", body);
         Assert.AreEqual(201, status, "POST /api/v1/apikeys must create a key. Body: " + answer);

         JsonElement created = JsonDocument.Parse(answer).RootElement;
         keys_.Add(created.GetProperty("id").ToString());
         return created.GetProperty("key").GetString();
      }

      private static string FindPython()
      {
         foreach (string entry in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
         {
            string directory = entry.Trim().Trim('"');
            if (directory.Length == 0 || directory.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) >= 0)
               continue;

            try
            {
               string candidate = Path.Combine(directory, "python.exe");
               if (File.Exists(candidate))
                  return candidate;
            }
            catch (ArgumentException)
            {
            }
         }

         return null;
      }

      private static string RepositoryFile(string relativePath)
      {
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relativePath + " cannot be found.");
         return null;
      }
   }
}
