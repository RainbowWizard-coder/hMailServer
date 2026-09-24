// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Installation
{
   /// <summary>
   ///    Every setting from the environment (docs/EnvironmentConfiguration.md):
   ///    HM_SETTING_&lt;key&gt; and HM_PROPERTY_&lt;name&gt;, their _FILE forms and the
   ///    container's four HM_REST_* names, stored by `hMailServer.exe
   ///    --apply-environment`, and the two records the server keeps of them: which
   ///    settings the environment manages - answered beside each by REST, listed by
   ///    GET /api/v1/settings/environment, and refused to an administrator's change
   ///    over REST and COM - and which values a file supplied, withheld from every
   ///    answer until an administrator writes another.
   ///
   ///    The environment is set for a CHILD process only - the verb, started from
   ///    here with ProcessStartInfo's environment - never for the bench's service.
   ///    The verb writes the database the service runs on; the service takes what
   ///    it wrote at its next reinitialise, as a container's server takes it at its
   ///    start, so every test that reads back reinitialises first. Each has its
   ///    negative control.
   ///
   ///    Ports: the REST listener on 10020 and a stand-in identity provider on 10021
   ///    (the row's range is 10020-10029). What the tests change the teardown puts
   ///    back, the records first: while they name a setting, the COM and REST writes
   ///    that put the others back would be refused, or recorded as secrets.
   /// </summary>
   [TestFixture]
   public class EnvironmentConfiguration : TestFixtureBase
   {
      private static int RestPort = 10020;
      private const int IssuerPort = 10021;
      private const string AdminPassword = "testar";

      // A [Settings] key marked now, harmless on a bench that sends no mail here.
      private const string IniKey = "MaxSubmissionsPerIPPerMinute";

      // A [Settings] key marked restart, which /Register alone reads: pending from the
      // moment it is stored until the service starts again, and put back before then.
      private const string RestartKey = "ServiceAccountName";

      private const string RecordsSql =
         "delete from hm_settings where settingname in ('EnvironmentManagedSettings', 'EnvironmentSecretSettings')";

      private int maxMessageSizeBefore_;
      private string welcomeBefore_;
      private bool iniKeyPresentBefore_;
      private string iniKeyBefore_;
      private int retentionBefore_;
      private readonly List<string> files_ = new List<string>();
      private readonly List<string> directories_ = new List<string>();
      private readonly List<string> keys_ = new List<string>();
      private readonly List<Action> putBack_ = new List<Action>();

      /// <summary>
      ///    A run that was killed between the verb and the teardown leaves the records
      ///    behind, and the base SetUp's own writes - the SMTP greeting, the maximum
      ///    message size - would then be refused before this fixture's SetUp could
      ///    clear them. This runs before any SetUp.
      /// </summary>
      [OneTimeSetUp]
      public void ClearAStaleRecord()
      {
         _application.Database.ExecuteSQL(RecordsSql);
         _application.Reinitialize();
      }

      [SetUp]
      public void StartListener()
      {
         // A record an earlier test left would refuse the writes below, and the one
         // at the end of the teardown.
         RunVerb(new Dictionary<string, string>());
         _application.Database.ExecuteSQL(RecordsSql);

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
            // The record of the managed settings first, and the server told: until
            // then the writes below are refused, which is the point of it. The record
            // of the values a file supplied goes last: while it names a setting, the
            // write that puts the setting back is recorded as a secret's and the value
            // before it is never in the audit trail.
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
            foreach (Action action in putBack_)
               Guarded(action);
            Guarded(() => _application.Database.ExecuteSQL(RecordsSql));
            Guarded(() => { foreach (string id in keys_) Admin("DELETE", "/api/v1/apikeys/" + id, null); });
            Guarded(() => { foreach (string file in files_) File.Delete(file); });
            Guarded(() => { foreach (string directory in directories_) Directory.Delete(directory, true); });
         }
         finally
         {
            putBack_.Clear();
            keys_.Clear();
            files_.Clear();
            directories_.Clear();
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
      [Description("A prefix that looks like one of the two and is not - HM_SETTINGS_, HM_PROPERTIES_, a prefix in small letters - is refused naming the right one, where it used to be passed over without a word and the setting left unset. Negative control: a variable the entrypoint reads itself (HM_DB_HOST) is not named at all, and a correctly spelled one in the same run is not stored.")]
      public void AMisspeltPrefixIsRefusedNamingTheRightOne()
      {
         int value = maxMessageSizeBefore_ == 1234 ? 1235 : 1234;
         Verb run = RunVerb(new Dictionary<string, string>
         {
            { "HM_SETTINGS_" + IniKey, "5" },
            { "HM_PROPERTIES_welcomesmtp", "Hello" },
            { "hm_property_welcomesmtp", "Hello" },
            { "HM_DB_HOST", "db" },
            { "HM_PROPERTY_maxmessagesize", value.ToString() },
         });

         Assert.AreEqual(1, run.ExitCode, run.Output);
         StringAssert.Contains("HM_SETTINGS_" + IniKey + ": refused", run.Output);
         StringAssert.Contains("did you mean HM_SETTING_" + IniKey + "?", run.Output);
         StringAssert.Contains("HM_PROPERTIES_welcomesmtp: refused", run.Output);
         StringAssert.Contains("did you mean HM_PROPERTY_welcomesmtp?", run.Output);
         StringAssert.Contains("hm_property_welcomesmtp: refused", run.Output);
         StringAssert.DoesNotContain("HM_DB_HOST", run.Output, "A variable the entrypoint reads is nobody's misspelling.");
         StringAssert.Contains("HM_PROPERTY_maxmessagesize: not stored, because another variable was refused", run.Output);

         _application.Reinitialize();
         Assert.AreEqual(maxMessageSizeBefore_, _settings.MaxMessageSize, "A refused run stored a value.");
      }

      [Test]
      [Description("The two records and a row nothing reads are not names a variable sets, and an empty alias is an unset one, as the entrypoint always read it. Negative control: the same alias with a value is stored under its own name.")]
      public void TheRecordsAndARowNothingReadsAreRefusedAndAnEmptyAliasIsUnset()
      {
         Verb records = RunVerb(new Dictionary<string, string>
         {
            { "HM_PROPERTY_EnvironmentManagedSettings", "HM_PROPERTY_maxmessagesize" },
            { "HM_PROPERTY_EnvironmentSecretSettings", "HM_PROPERTY_welcomesmtp" },
            { "HM_PROPERTY_relaymode", "1" },
         });
         Assert.AreEqual(1, records.ExitCode, records.Output);
         StringAssert.Contains("HM_PROPERTY_EnvironmentManagedSettings: refused: EnvironmentManagedSettings is where the server records", records.Output);
         StringAssert.Contains("HM_PROPERTY_EnvironmentSecretSettings: refused: EnvironmentSecretSettings is where the server records", records.Output);
         StringAssert.Contains("HM_PROPERTY_relaymode: refused: relaymode is an hm_settings row that nothing in this build reads", records.Output);

         Verb empty = RunVerb(new Dictionary<string, string> { { "HM_REST_PORT", "" } });
         Assert.AreEqual(0, empty.ExitCode, empty.Output);
         StringAssert.Contains("No variable names a setting", empty.Output);

         Verb given = RunVerb(new Dictionary<string, string> { { "HM_REST_PORT", RestPort.ToString() } });
         Assert.AreEqual(0, given.ExitCode, given.Output);
         StringAssert.Contains("HM_REST_PORT: unchanged", given.Output);
      }

      [Test]
      [Description("A value of the wrong kind is refused: a [Settings] key read as a number refuses a word, a number refuses a switch's word, and a property or a key behind a settings row is refused with the sentence that row refuses the same value with - the same words, compared. Negative control: the REST route refuses the value too, and the value in range is taken by both.")]
      public void AValueOfTheWrongKindIsRefusedWithTheRestSentence()
      {
         Verb word = RunVerb(new Dictionary<string, string> { { "HM_SETTING_" + IniKey, "many" } });
         Assert.AreEqual(1, word.ExitCode, word.Output);
         StringAssert.Contains("HM_SETTING_" + IniKey + ": refused", word.Output);
         StringAssert.Contains("whole number", word.Output);

         // A switch's word is a switch's: off is not the 0 that means "no limit".
         Verb off = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_maxmessagesize", "off" } });
         Assert.AreEqual(1, off.ExitCode, off.Output);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize: refused: maxmessagesize holds a whole number", off.Output);

         (int status, string body) rest = Admin("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":5000}");
         Assert.AreEqual(400, rest.status, rest.body);
         string sentence = JsonDocument.Parse(rest.body).RootElement.GetProperty("error").GetString();

         Verb range = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_DomainReportRetentionDays", "5000" } });
         Assert.AreEqual(1, range.ExitCode, range.Output);
         StringAssert.Contains("HM_PROPERTY_DomainReportRetentionDays: refused: " + sentence, range.Output);

         // A [Settings] key a settings row keeps is held to that row's check.
         (int status, string body) tarpit = Admin("PUT", "/api/v1/settings/antispam", "{\"tarpit_delay\":500}");
         Assert.AreEqual(400, tarpit.status, tarpit.body);
         string tarpitSentence = JsonDocument.Parse(tarpit.body).RootElement.GetProperty("error").GetString();
         Verb delay = RunVerb(new Dictionary<string, string> { { "HM_SETTING_SmtpTarpitDelaySeconds", "500" } });
         Assert.AreEqual(1, delay.ExitCode, delay.Output);
         StringAssert.Contains("HM_SETTING_SmtpTarpitDelaySeconds: refused: " + tarpitSentence, delay.Output);

         Verb rewrite = RunVerb(new Dictionary<string, string> { { "HM_SETTING_RewriteEnvelopeFromWhenForwarding", "7" } });
         Assert.AreEqual(1, rewrite.ExitCode, rewrite.Output);
         StringAssert.Contains("HM_SETTING_RewriteEnvelopeFromWhenForwarding: refused", rewrite.Output);
         StringAssert.Contains("must be true or false", rewrite.Output);

         // The value in range: taken over REST, and taken by the verb.
         int inRange = retentionBefore_ == 400 ? 401 : 400;
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":" + inRange + "}").status,
            "REST takes the value in range.");
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"domain_report_retention_days\":" + retentionBefore_ + "}").status);
         Verb taken = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_DomainReportRetentionDays", inRange.ToString() } });
         Assert.AreEqual(0, taken.ExitCode, taken.Output);
         StringAssert.Contains("HM_PROPERTY_DomainReportRetentionDays: set", taken.Output);
      }

      [Test]
      [Description("A value is written through its REST settings row's setter, so it is stored as REST stores it: the script language given in small letters is stored as JavaScript or VBScript, which is what every reader compares with - JavaScript accepted here although this build runs VBScript and JScript, because a configuration made for a Linux server has to apply. Negative control: the language before the run is the other one.")]
      public void AValueIsStoredAsItsRestRowStoresIt()
      {
         string before = _settings.Scripting.Language;
         putBack_.Add(() => _settings.Scripting.Language = before);

         bool toJavaScript = !string.Equals(before, "JavaScript", StringComparison.Ordinal);
         string typed = toJavaScript ? "javascript" : "vbscript";
         string canonical = toJavaScript ? "JavaScript" : "VBScript";
         Assert.AreNotEqual(canonical, before, "The negative control: the server must not hold the language before the run.");

         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_scriptlanguage", typed } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("HM_PROPERTY_scriptlanguage: set", run.Output);

         _application.Reinitialize();
         Assert.AreEqual(canonical, _settings.Scripting.Language, "The value was stored as typed, which no reader of it recognises.");
         Assert.AreEqual(canonical, Settings("/api/v1/settings/scripting").GetProperty("language").GetString());
      }

      [Test]
      [Description("The VAPID private key is stored through the setter that derives its public half, and checked as REST checks it; the public half is refused as derived. Negative control: the public key the server answers is the one this fixture derives from the key it generated.")]
      public void TheVapidKeyIsCheckedAndItsPublicHalfDerived()
      {
         string publicBefore = Settings("/api/v1/settings/push").GetProperty("vapid_public_key").GetString();
         putBack_.Add(() =>
         {
            if (string.IsNullOrEmpty(publicBefore))
               Admin("PUT", "/api/v1/settings/push", "{\"vapid_private_key\":\"\"}");
            else
               Admin("POST", "/api/v1/settings/push/vapid", "{}");
         });

         Verb bad = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_WebPushVapidPrivateKey", "not-a-key" } });
         Assert.AreEqual(1, bad.ExitCode, bad.Output);
         StringAssert.Contains("HM_PROPERTY_WebPushVapidPrivateKey: refused", bad.Output);

         Verb derived = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_WebPushVapidPublicKey", "anything" } });
         Assert.AreEqual(1, derived.ExitCode, derived.Output);
         StringAssert.Contains("HM_PROPERTY_WebPushVapidPublicKey: refused: WebPushVapidPublicKey is derived from the private key", derived.Output);

         string privateKey, publicKey;
         using (ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256))
         {
            ECParameters parameters = key.ExportParameters(true);
            byte[] point = new byte[65];
            point[0] = 4;
            Buffer.BlockCopy(parameters.Q.X, 0, point, 1, 32);
            Buffer.BlockCopy(parameters.Q.Y, 0, point, 33, 32);
            privateKey = Base64Url(parameters.D);
            publicKey = Base64Url(point);
         }
         Assert.AreNotEqual(publicKey, publicBefore, "The negative control: the server must not hold this key before the run.");

         string file = TempFile(privateKey + "\n");
         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_WebPushVapidPrivateKey_FILE", file } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("HM_PROPERTY_WebPushVapidPrivateKey_FILE: set (WebPushVapidPrivateKey, from a file; the value is not shown)", run.Output);
         StringAssert.DoesNotContain(privateKey, run.Output);

         _application.Reinitialize();
         JsonElement push = Settings("/api/v1/settings/push");
         Assert.AreEqual(publicKey, push.GetProperty("vapid_public_key").GetString(), "The public half was not derived from the private key the file held.");
         Assert.AreEqual("file", push.GetProperty("vapid_private_key_environment").GetString(), "A write-only key the environment sets is marked.");
         Assert.IsFalse(push.TryGetProperty("vapid_private_key", out _), "A write-only key is answered.");
         Assert.AreEqual(409, Admin("POST", "/api/v1/settings/push/vapid", "{}").status, "A new pair would replace the key the environment sets.");
      }

      [Test]
      [Description("A _FILE form reads the value from the file, its trailing line break stripped, and the value is never printed, never answered over REST and never in the changed document, which marks the setting as set from a file; the plain form and the _FILE form set together are refused. Negative control: COM reads the value the file held, without its line break.")]
      public void AFileSuppliesTheValueWhichIsNeverShownAndBothFormsAreRefused()
      {
         string secretText = "Greeting from a Secret " + Guid.NewGuid().ToString("N").Substring(0, 8);
         string file = TempFile(secretText + "\r\n");

         Verb both = RunVerb(new Dictionary<string, string>
         {
            { "HM_PROPERTY_welcomesmtp", "plain" },
            { "HM_PROPERTY_welcomesmtp_FILE", file },
         });
         Assert.AreEqual(1, both.ExitCode, both.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp, HM_PROPERTY_welcomesmtp_FILE all name welcomesmtp; set one of them", both.Output);

         // A setting a domain or an account may hold its own value of is shown to
         // their administrators as what is in effect, so a file cannot keep it secret.
         // The refusal is decided before the file is read, so the last line guards
         // that order rather than a sentence that could quote the file.
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

         // The changed document, which the Control Panel reads: the entry marked as
         // the environment's, from a file, and named among the values withheld.
         JsonElement changed = Settings("/api/v1/settings/changed");
         JsonElement entry = changed.GetProperty("settings").EnumerateArray().Single(e => e.GetProperty("name").GetString() == "welcomesmtp");
         Assert.AreEqual("file", entry.GetProperty("environment").GetString());
         Assert.AreEqual("(set)", entry.GetProperty("value").GetString());
         Assert.IsTrue(entry.GetProperty("secret").GetBoolean());
         Assert.IsTrue(changed.GetProperty("from_file").EnumerateArray().Any(e => e.GetProperty("name").GetString() == "welcomesmtp"),
            "The changed document does not list the value a file supplied among those withheld: " + changed);

         // And the second run reads the same file and changes nothing.
         Verb again = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", file } });
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE: unchanged", again.Output);
      }

      [Test]
      [Description("A _FILE that names no file - a missing path, a directory (the folder a Secret is mounted in, a mount whose source is missing), a file with a NUL in it - is refused by name, and nothing is stored. Negative control: the same variable naming a readable file is stored.")]
      public void AFileThatIsNoFileIsRefused()
      {
         string missing = Path.Combine(Path.GetTempPath(), "hm-env-missing-" + Guid.NewGuid().ToString("N") + ".txt");
         Verb absent = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", missing } });
         Assert.AreEqual(1, absent.ExitCode, absent.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE: refused: HM_PROPERTY_welcomesmtp_FILE names " + missing + ", which cannot be read", absent.Output);

         // Refused by the check that it is a regular file, which the sentence names -
         // not by the open failing, as it does on Windows and does not on Linux.
         string directory = Path.Combine(Path.GetTempPath(), "hm-env-dir-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(directory);
         directories_.Add(directory);
         Verb folder = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", directory } });
         Assert.AreEqual(1, folder.ExitCode, folder.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE names " + directory + ", which is not a file", folder.Output);

         string withNul = Path.Combine(Path.GetTempPath(), "hm-env-" + Guid.NewGuid().ToString("N") + ".txt");
         File.WriteAllBytes(withNul, Encoding.ASCII.GetBytes("abc\0def"));
         files_.Add(withNul);
         Verb nul = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", withNul } });
         Assert.AreEqual(1, nul.ExitCode, nul.Output);
         StringAssert.Contains("which is not UTF-8 text", nul.Output);

         _application.Reinitialize();
         Assert.AreEqual(welcomeBefore_, _settings.WelcomeSMTP, "A refused run stored a value.");

         string good = TempFile("A greeting " + Guid.NewGuid().ToString("N").Substring(0, 6));
         Verb taken = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", good } });
         Assert.AreEqual(0, taken.ExitCode, taken.Output);
      }

      [Test]
      [Description("A settings row's check that quotes the value it refuses is not printed for a value from a file: a fixed sentence names the variable and the check instead. Negative control: the same text in a plain variable is refused with the row's own sentence, which quotes it.")]
      public void ARefusalNeverQuotesAValueFromAFile()
      {
         string marker = "hunter2-" + Guid.NewGuid().ToString("N").Substring(0, 8);
         string file = TempFile(marker);

         Verb fromFile = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_scriptlanguage_FILE", file } });
         Assert.AreEqual(1, fromFile.ExitCode, fromFile.Output);
         StringAssert.Contains("HM_PROPERTY_scriptlanguage_FILE: refused: scriptlanguage does not take the value HM_PROPERTY_scriptlanguage_FILE gives it", fromFile.Output);
         StringAssert.DoesNotContain(marker, fromFile.Output, "The refusal printed the value the file held.");

         Verb plain = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_scriptlanguage", marker } });
         Assert.AreEqual(1, plain.ExitCode, plain.Output);
         StringAssert.Contains(marker, plain.Output, "The negative control: the row's sentence quotes the value, which is why a file's is replaced.");
      }

      [Test]
      [Description("A value a file supplied stays withheld after the variable is gone - answered as (set), listed as withheld, never printed as the value a later run replaced - until an administrator writes another, which the audit trail records as changed; the data directory holds the entrypoint's marker file only while the environment manages anything. Negative control: the administrator's own value is answered in clear.")]
      public void AValueFromAFileStaysWithheldUntilAnAdministratorWritesAnother()
      {
         string secretText = "Secret greeting " + Guid.NewGuid().ToString("N").Substring(0, 8);
         string file = TempFile(secretText);
         string marker = Path.Combine(_settings.Directories.DataDirectory, ".hmailserver-environment");

         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", file } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         Assert.IsTrue(File.Exists(marker), "No marker file at " + marker + " while the environment manages a setting.");

         Verb released = RunVerb(new Dictionary<string, string>());
         Assert.AreEqual(0, released.ExitCode, released.Output);
         StringAssert.Contains("No longer set by the environment, and the administrator's to change again: welcomesmtp", released.Output);
         Assert.IsFalse(File.Exists(marker), "The marker file outlived the last managed setting.");
         _application.Reinitialize();

         JsonElement group = Settings("/api/v1/settings");
         Assert.AreEqual("(set)", group.GetProperty("welcome_smtp").GetString(), "A value a file supplied was answered once the variable was gone.");
         Assert.IsFalse(group.TryGetProperty("welcome_smtp_environment", out _), "A released setting is still marked as managed.");
         (int status, string body) changed = Admin("GET", "/api/v1/settings/changed", null);
         StringAssert.DoesNotContain(secretText, changed.body);
         StringAssert.Contains("\"from_file\":[{\"store\":\"database\",\"name\":\"welcomesmtp\"}]", changed.body);

         // A later run that sets it plainly does not print the value it replaces.
         Verb plain = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp", "A plain greeting" } });
         Assert.AreEqual(0, plain.ExitCode, plain.Output);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp: set (welcomesmtp; the value is not shown)", plain.Output);
         StringAssert.DoesNotContain(secretText, plain.Output);
         _application.Reinitialize();
         Assert.AreEqual("A plain greeting", Settings("/api/v1/settings").GetProperty("welcome_smtp").GetString(),
            "A value a variable supplied is the variable's, and answered.");

         // From a file again, released again, and then an administrator's own value.
         Assert.AreEqual(0, RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_welcomesmtp_FILE", file } }).ExitCode);
         Assert.AreEqual(0, RunVerb(new Dictionary<string, string>()).ExitCode);
         _application.Reinitialize();
         Assert.AreEqual("(set)", Settings("/api/v1/settings").GetProperty("welcome_smtp").GetString());

         string mine = "The administrator's greeting " + Guid.NewGuid().ToString("N").Substring(0, 6);
         (int status, string body) written = Admin("PUT", "/api/v1/settings", "{\"welcome_smtp\":\"" + mine + "\"}");
         Assert.AreEqual(200, written.status, written.body);
         Assert.AreEqual(mine, Settings("/api/v1/settings").GetProperty("welcome_smtp").GetString(),
            "The administrator's own value is withheld as the file's was.");

         string trail = Admin("GET", "/api/v1/audit?object_type=setting&limit=50", null).body;
         StringAssert.Contains("welcomesmtp: (changed)", trail, "The change from a value a file supplied is recorded with its values.");
         StringAssert.DoesNotContain(secretText, trail, "The audit trail recorded the value a file supplied.");
      }

      [Test]
      [Description("A setting whose name says it is a secret, set from a plain variable, is never printed; a write-only key the environment sets is marked, and a group PUT that names it is refused with 409 before anything in it is applied. Negative control: the other key in the same body is unchanged.")]
      public void ASecretNamedVariableIsNeverPrintedAndItsWriteOnlyKeyIsRefused()
      {
         string password = "relay-" + Guid.NewGuid().ToString("N");
         putBack_.Add(() => Admin("PUT", "/api/v1/settings", "{\"smtp_relayer_password\":\"\"}"));

         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_smtprelayerpassword", password } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.Contains("HM_PROPERTY_smtprelayerpassword: ", run.Output);
         StringAssert.DoesNotContain(password, run.Output, "The verb printed a secret's value.");
         StringAssert.DoesNotContain("smtprelayerpassword was", run.Output, "The verb printed the secret the variable replaced.");

         _application.Reinitialize();
         JsonElement group = Settings("/api/v1/settings");
         Assert.AreEqual("variable", group.GetProperty("smtp_relayer_password_environment").GetString());
         Assert.IsFalse(group.TryGetProperty("smtp_relayer_password", out _), "A write-only key is answered.");

         string greeting = group.GetProperty("welcome_smtp").GetString();
         (int status, string body) put = Admin("PUT", "/api/v1/settings",
            "{\"welcome_smtp\":\"Changed " + Guid.NewGuid().ToString("N").Substring(0, 6) + "\",\"smtp_relayer_password\":\"other\"}");
         Assert.AreEqual(409, put.status, put.body);
         StringAssert.Contains("HM_PROPERTY_smtprelayerpassword", put.body);
         Assert.AreEqual(greeting, Settings("/api/v1/settings").GetProperty("welcome_smtp").GetString(),
            "A key before the refused one was applied.");
      }

      [Test]
      [Description("A [Settings] key a file supplied is withheld on every route that answers it - the key's own, the list, whose environment map names it, the pending list, COM's GetIniSetting. Negative control: the list answers a key the environment does not set with its value.")]
      public void AnIniKeyFromAFileIsWithheldOnEveryRoute()
      {
         // The negative control's key, stored with a value nothing withholds.
         _settings.SetIniSetting(IniKey, "7");

         JsonElement before = GetIni(RestartKey);
         bool presentBefore = before.GetProperty("present").GetBoolean();
         string valueBefore = before.GetProperty("value").GetString();
         putBack_.Add(() =>
         {
            if (presentBefore)
               _settings.SetIniSetting(RestartKey, valueBefore);
            else
               _settings.DeleteIniSetting(RestartKey);
         });

         string account = "svc-" + Guid.NewGuid().ToString("N").Substring(0, 10);
         string file = TempFile(account + "\n");
         Verb run = RunVerb(new Dictionary<string, string> { { "HM_SETTING_" + RestartKey + "_FILE", file } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         StringAssert.DoesNotContain(account, run.Output);
         _application.Reinitialize();

         JsonElement key = GetIni(RestartKey);
         Assert.AreEqual("(set)", key.GetProperty("value").GetString());
         Assert.IsTrue(key.GetProperty("managed").GetBoolean());
         Assert.AreEqual("file", key.GetProperty("environment").GetString());

         (int status, string body) list = Admin("GET", "/api/v1/settings/ini", null);
         Assert.AreEqual(200, list.status, list.body);
         StringAssert.DoesNotContain(account, list.body);
         JsonElement listed = JsonDocument.Parse(list.body).RootElement;
         Assert.AreEqual("file", listed.GetProperty("environment").GetProperty(RestartKey).GetString());
         JsonElement row = listed.GetProperty("settings").EnumerateArray().Single(r => r.GetProperty("name").GetString() == RestartKey);
         Assert.IsTrue(row.GetProperty("secret").GetBoolean());
         Assert.AreEqual("(set)", row.GetProperty("value").GetString());
         JsonElement other = listed.GetProperty("settings").EnumerateArray().Single(r => r.GetProperty("name").GetString() == IniKey);
         Assert.AreEqual("7", other.GetProperty("value").GetString(), "The negative control: a key nothing withholds is listed with its value.");
         Assert.IsFalse(other.GetProperty("secret").GetBoolean());

         (int status, string body) pending = Admin("GET", "/api/v1/server/settings/pending", null);
         Assert.AreEqual(200, pending.status, pending.body);
         StringAssert.DoesNotContain(account, pending.body);
         JsonElement waiting = JsonDocument.Parse(pending.body).RootElement.EnumerateArray().Single(p => p.GetProperty("name").GetString() == RestartKey);
         Assert.AreEqual("(set)", waiting.GetProperty("stored").GetString());
         Assert.IsTrue(waiting.GetProperty("secret").GetBoolean());

         Assert.AreEqual("(set)", _settings.GetIniSetting(RestartKey), "COM answered the value a file supplied.");
      }

      [Test]
      [Description("HM_PROPERTY_OidcClientSecret_FILE - the Kubernetes Secret the _FILE form was made for - stores the single sign-on client secret, answered as set and never as its value, and the sign-in sends it to the identity provider. Negative control: the identity provider was sent no other secret.")]
      public void TheSingleSignOnSecretComesFromAFile()
      {
         putBack_.Add(() => Admin("PUT", "/api/v1/settings",
            "{\"oidc_issuer\":\"\",\"oidc_client_id\":\"\",\"oidc_client_secret\":\"\",\"oidc_redirect_base_url\":\"\"}"));

         string issuer = "http://127.0.0.1:" + IssuerPort;
         string secret = "oidc" + Guid.NewGuid().ToString("N");
         string file = TempFile(secret + "\n");

         using (var endpoint = new FakeHttpEndpoint(IssuerPort, 404, "{}", "application/json"))
         {
            endpoint.SetResponse("/.well-known/openid-configuration", 200, Encoding.UTF8.GetBytes(
               "{\"issuer\":\"" + issuer + "\",\"authorization_endpoint\":\"" + issuer + "/authorize\"," +
               "\"token_endpoint\":\"" + issuer + "/token\",\"jwks_uri\":\"" + issuer + "/jwks\"}"), "application/json");
            endpoint.SetResponse("/jwks", 200, Encoding.UTF8.GetBytes("{\"keys\":[]}"), "application/json");

            (int status, string body) configured = Admin("PUT", "/api/v1/settings",
               "{\"oidc_issuer\":\"" + issuer + "\",\"oidc_client_id\":\"hmailserver-environment\"," +
               "\"oidc_redirect_base_url\":\"http://127.0.0.1:" + RestPort + "\"}");
            Assert.AreEqual(200, configured.status, configured.body);

            Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_OidcClientSecret_FILE", file } });
            Assert.AreEqual(0, run.ExitCode, run.Output);
            StringAssert.Contains("HM_PROPERTY_OidcClientSecret_FILE: set (OidcClientSecret, from a file; the value is not shown)", run.Output);
            StringAssert.DoesNotContain(secret, run.Output);
            _application.Reinitialize();

            (int status, string body) group = Admin("GET", "/api/v1/settings", null);
            StringAssert.Contains("\"oidc_client_secret_environment\":\"file\"", group.body);
            StringAssert.DoesNotContain(secret, group.body);
            (int status, string body) changed = Admin("GET", "/api/v1/settings/changed", null);
            StringAssert.DoesNotContain(secret, changed.body);
            JsonElement entry = JsonDocument.Parse(changed.body).RootElement.GetProperty("settings").EnumerateArray()
               .Single(e => e.GetProperty("name").GetString() == "OidcClientSecret");
            Assert.AreEqual("(set)", entry.GetProperty("value").GetString());

            // The sign-in: its start, and the callback that exchanges the code - with
            // the secret, at the identity provider's token endpoint.
            (int status, string body, string headers) started = Raw("GET", "/portal/oidc/start", null);
            Assert.AreEqual(302, started.status, started.headers);
            string location = Regex.Match(started.headers, @"Location: (\S+)").Groups[1].Value;
            string cookie = Regex.Match(started.headers, @"Set-Cookie: (hmoidc=[^;]+);").Groups[1].Value;
            string state = Regex.Match(location, @"[?&]state=([^&]+)").Groups[1].Value;
            Assert.IsNotEmpty(state, started.headers);

            (int status, string body, string headers) callback = Raw("GET", "/portal/oidc/callback?state=" + state + "&code=a-code", "Cookie: " + cookie);
            Assert.AreEqual(302, callback.status, callback.headers);

            List<string> bodies = endpoint.RequestBodies.Select(b => Encoding.ASCII.GetString(b)).ToList();
            Assert.IsTrue(bodies.Any(b => b.Contains("client_secret=" + secret)),
               "The token request did not carry the secret the file held: " + string.Join(" | ", bodies));
            Assert.IsFalse(bodies.Any(b => b.Contains("client_secret=") && !b.Contains("client_secret=" + secret)));
         }
      }

      // ---- the record, and the refusals ---------------------------------------

      [Test]
      [Description("A setting the environment set is answered as managed and refused to REST (409) and COM when a change would move it, and taken when it says what the setting holds; a run without the variable takes the record away and every one of the refused writes is then taken. Negative control: the same writes before and after, and the refused writes changed nothing.")]
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
         Assert.AreEqual("7", GetIni(IniKey).GetProperty("value").GetString(), "A refused COM write changed the value.");
         COMException comProperty = Assert.Throws<COMException>(() => _settings.MaxMessageSize = size + 10);
         StringAssert.Contains("HM_PROPERTY_maxmessagesize", comProperty.Message);
         Assert.AreEqual(size, _settings.MaxMessageSize, "A refused COM write changed the value.");

         // Without the variables: the record goes, and every write refused above is taken.
         Verb without = RunVerb(new Dictionary<string, string>());
         Assert.AreEqual(0, without.ExitCode, without.Output);
         StringAssert.Contains("No longer set by the environment", without.Output);
         _application.Reinitialize();

         Assert.IsFalse(GetIni(IniKey).GetProperty("managed").GetBoolean());
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings/ini/" + IniKey, "{\"value\":\"9\"}").status);
         Assert.AreEqual(200, Admin("DELETE", "/api/v1/settings/ini/" + IniKey, null).status);
         _settings.SetIniSetting(IniKey, "9");
         Assert.AreEqual("9", GetIni(IniKey).GetProperty("value").GetString());
         Assert.IsFalse(Settings("/api/v1/settings").TryGetProperty("max_message_size_kb_environment", out _));
         Assert.AreEqual(200, Admin("PUT", "/api/v1/settings", "{\"max_message_size_kb\":" + (size + 10) + "}").status);
         _settings.MaxMessageSize = size + 20;
         Assert.AreEqual(size + 20, _settings.MaxMessageSize);
      }

      [Test]
      [Description("GET /api/v1/settings/environment lists each managed setting with its variable, store and source and never a value; a read-only key may read it and a key restricted to a domain may not; the changed document lists the same names; the start writes one line naming them in the application log. Negative control: nothing managed, nothing listed.")]
      public void TheEnvironmentRouteListsNamesAndSourcesToAReader()
      {
         Assert.AreEqual(0, Environment_().Count, "The negative control: nothing is managed before the run.");

         string file = TempFile("Banner " + Guid.NewGuid().ToString("N"));

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

         // The changed document's own list, which the Control Panel reads.
         List<JsonElement> changed = Settings("/api/v1/settings/changed").GetProperty("environment").EnumerateArray().Select(e => e.Clone()).ToList();
         Assert.IsTrue(changed.Any(e => e.GetProperty("variable").GetString() == "HM_PROPERTY_maxmessagesize" && e.GetProperty("source").GetString() == "variable"),
            string.Join(", ", changed));
         Assert.IsTrue(changed.Any(e => e.GetProperty("variable").GetString() == "HM_PROPERTY_welcomesmtp_FILE" && e.GetProperty("source").GetString() == "file"),
            string.Join(", ", changed));

         // What the start said.
         string log = LogHandler.ReadCurrentDefaultLog();
         StringAssert.Contains("Settings set by this server's environment", log);
         StringAssert.Contains("maxmessagesize (HM_PROPERTY_maxmessagesize)", log);

         string reader = CreateKey("environment - reader", "readonly", null);
         (int status, string body) asReader = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/settings/environment", "Bearer " + reader, null);
         Assert.AreEqual(200, asReader.status, asReader.body);
         StringAssert.Contains("HM_PROPERTY_welcomesmtp_FILE", asReader.body);

         string confined = CreateKey("environment - confined", "full", "elsewhere.test");
         Assert.AreEqual(403, RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/settings/environment", "Bearer " + confined, null).status,
            "The environment is the server's; a key confined to a domain does not read it.");
      }

      [Test]
      [Description("A settings backup taken while the environment manages settings does not carry the records, so a restore brings no refusal back. Negative control: before the run without the variables the setting is managed.")]
      public void ABackupDoesNotCarryTheRecords()
      {
         string directory = Path.Combine(Path.GetTempPath(), "hm-env-backup-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(directory);
         directories_.Add(directory);

         Verb run = RunVerb(new Dictionary<string, string> { { "HM_PROPERTY_maxmessagesize", _settings.MaxMessageSize.ToString() } });
         Assert.AreEqual(0, run.ExitCode, run.Output);
         _application.Reinitialize();
         Assert.AreEqual(1, Environment_().Count, "The negative control: the setting is managed when the backup is taken.");

         RunSettingsBackup(directory);

         Assert.AreEqual(0, RunVerb(new Dictionary<string, string>()).ExitCode);
         _application.Reinitialize();
         Assert.AreEqual(0, Environment_().Count);

         RestoreSettings(directory);
         WaitForRest();
         Assert.AreEqual(0, Environment_().Count, "The restore brought the record of the managed settings back.");
         _settings.MaxMessageSize = maxMessageSizeBefore_ + 1;
         Assert.AreEqual(maxMessageSizeBefore_ + 1, _settings.MaxMessageSize, "A change to the setting was refused after the restore.");
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
            if (!process.WaitForExit(120 * 1000))
            {
               process.Kill();
               Assert.Fail("The checker did not finish within two minutes, and was stopped.");
            }
            process.WaitForExit();

            string printed = output.Result + error.Result;
            ClassicAssert.AreEqual(0, process.ExitCode, printed);
            StringAssert.Contains("negative controls", printed);
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
      ///    runner's. One that has not finished in three minutes is stopped, so that
      ///    it does not go on writing the bench's database under the teardown.
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
            if (!process.WaitForExit(180 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail("--apply-environment did not finish within three minutes, and was stopped.");
            }
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

      private string TempFile(string content)
      {
         string file = Path.Combine(Path.GetTempPath(), "hm-env-" + Guid.NewGuid().ToString("N") + ".txt");
         File.WriteAllText(file, content, new UTF8Encoding(false));
         files_.Add(file);
         return file;
      }

      private static string Base64Url(byte[] data)
         => Convert.ToBase64String(data).TrimEnd('=').Replace('+', '-').Replace('/', '_');

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

      /// <summary>
      ///    One HTTP/1.0 GET with the response headers as well as its body: the single
      ///    sign-on test reads Location and Set-Cookie, which RestRequest drops.
      /// </summary>
      private static (int status, string body, string headers) Raw(string method, string path, string extraHeader)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               string request = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n" +
                                (string.IsNullOrEmpty(extraHeader) ? "" : extraHeader + "\r\n") +
                                "Connection: close\r\n\r\n";
               byte[] bytes = Encoding.ASCII.GetBytes(request);
               stream.Write(bytes, 0, bytes.Length);

               byte[] buffer = new byte[4096];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               int status = 0;
               string[] first = raw.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (first.Length >= 2)
                  int.TryParse(first[1], out status);

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               return (status, separator >= 0 ? raw.Substring(separator + 4) : "", separator >= 0 ? raw.Substring(0, separator) : raw);
            }
         }
      }

      /// <summary>The REST listener answering again, after a restore reinitialised the server.</summary>
      private void WaitForRest()
      {
         for (int attempt = 0; attempt < 50; attempt++)
         {
            try
            {
               if (Admin("GET", "/api/v1/status", null).status == 200)
                  return;
            }
            catch (Exception notYet) when (!ExceptionPolicy.IsFatal(notYet))
            {
               // The listener is restarting.
            }

            Thread.Sleep(200);
         }
      }

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

      // ---- a settings backup and restore (SettingLevels' way) -------------------

      private void RunSettingsBackup(string directory)
      {
         var backup = _application.Settings.Backup;

         bool domainsBefore = backup.BackupDomains;
         bool messagesBefore = backup.BackupMessages;
         bool settingsBefore = backup.BackupSettings;
         bool compressBefore = backup.CompressDestinationFiles;
         string destinationBefore = backup.Destination;
         putBack_.Add(() =>
         {
            var settings = _application.Settings.Backup;
            settings.BackupDomains = domainsBefore;
            settings.BackupMessages = messagesBefore;
            settings.BackupSettings = settingsBefore;
            settings.CompressDestinationFiles = compressBefore;
            settings.Destination = destinationBefore;
         });

         backup.BackupDomains = false;
         backup.BackupMessages = false;
         backup.BackupSettings = true;
         backup.CompressDestinationFiles = true;
         backup.Destination = directory;

         CustomAsserts.AssertDeleteFile(backup.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(backup.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within 30 seconds.");
      }

      private void RestoreSettings(string directory)
      {
         string startTime = _application.Status.StartTime;

         // Status.StartTime has one-second resolution; step past the second.
         Thread.Sleep(1100);

         FileInfo[] archives = new DirectoryInfo(directory).GetFiles("*.7z");
         ClassicAssert.AreEqual(1, archives.Length, "Expected exactly one archive in " + directory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = false;
         backup.RestoreMessages = false;
         backup.RestoreSettings = true;
         backup.StartRestore();

         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string now = _application.Status.StartTime;
               if (now.Length > 0 && now != startTime)
               {
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
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
