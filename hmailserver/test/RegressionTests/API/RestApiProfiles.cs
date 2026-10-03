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
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Profiles, so a new server starts in the right shape (docs/Profiles.md): the
   ///    four setting profiles the server ships, each one's diff against this server,
   ///    an apply that saves the way back before it writes, the way back itself, and
   ///    the refusals that keep a listener, a trust or data from being taken
   ///    silently. Over REST (/api/v1/settings/profiles), COM (Settings.GetProfile and
   ///    the rest) and the verbs (hMailServer.exe --apply-profile and
   ///    --revert-profile), with the setup checklist's first item and the
   ///    changed-from-stock document's source beside them.
   ///
   ///    The teardown reinitialises first - a verb writes the records from another
   ///    process - then reverts any way back the server holds, puts back what a test
   ///    changed by hand, and empties the profile's two records; the bench's
   ///    changed-from-stock document is compared before and after each test, so the
   ///    fixture leaves the settings as it found them. Each test has its negative
   ///    control. Ports: the REST listener on 10030 (the row's range is 10030-10039).
   ///    No IMAP or SMTP client is used: an effect is read over REST or COM.
   /// </summary>
   [TestFixture]
   public class RestApiProfiles : TestFixtureBase
   {
      private static int RestPort = 10030;
      private const string AdminPassword = "testar";
      private const string AccountPassword = "Profiles-Passw0rd!2026";

      private const string RecordsSql =
         "update hm_settings set settingstring = '' where settingname in ('ProfileSnapshot', 'ProfileApplied')";
      private const string EnvironmentRecordsSql =
         "delete from hm_settings where settingname in ('EnvironmentManagedSettings', 'EnvironmentSecretSettings')";

      private static readonly string[] Names = { "office", "gateway", "relay", "sender" };

      private string settingsBefore_;
      private bool environmentUsed_;
      private readonly List<Action> putBack_ = new List<Action>();

      /// <summary>
      ///    A run that was killed between an apply and the teardown leaves a way back
      ///    behind, which would refuse every apply of this fixture with 409 - and is
      ///    the one record that can put the bench back. So it is reverted first, and
      ///    the records emptied only after. This runs before any SetUp.
      /// </summary>
      [OneTimeSetUp]
      public void PutBackAStaleWayBack()
      {
         _application.Reinitialize();
         try
         {
            _application.Settings.RevertProfile();
         }
         catch (COMException)
         {
            // Nothing to revert, which is the usual case.
         }
         _application.Database.ExecuteSQL(RecordsSql);
         _application.Reinitialize();
      }

      [SetUp]
      public void StartListener()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         settingsBefore_ = ChangedSettings();
      }

      [TearDown]
      public void PutTheServerBack()
      {
         string settingsAfter = null;
         try
         {
            // A service stopped by a test that failed part way is started again, and
            // the COM objects taken anew.
            if (!ServiceRunning())
               Guarded(() => RestartServerAndReacquireCom());

            // First the server reads what the database holds: a verb run beside it
            // wrote there, and a revert decided from the server's own copy would miss
            // a way back only the database has.
            Guarded(() => _application.Reinitialize());

            // A way back a test left is put back, so the settings are the ones the
            // test found; then the environment's record if a test made one - while it
            // names a setting, the writes below that put it back would be refused -
            // then whatever the test changed by hand, and the profile's two records.
            Guarded(() =>
            {
               if (SnapshotHeld())
                  Admin("POST", "/api/v1/settings/profiles/revert", "{}");
            });
            if (environmentUsed_)
            {
               Guarded(() => RunVerb("--apply-environment", new Dictionary<string, string>()));
               Guarded(() => _application.Database.ExecuteSQL(EnvironmentRecordsSql));
               Guarded(() => _application.Reinitialize());
            }
            foreach (Action action in putBack_)
               Guarded(action);
            Guarded(() => _application.Database.ExecuteSQL(RecordsSql));
            Guarded(() => _application.Reinitialize());
            Guarded(() => settingsAfter = ChangedSettings());
         }
         finally
         {
            putBack_.Clear();
            environmentUsed_ = false;
            RestListener.Stop();
            _application.Reinitialize();
         }

         ClassicAssert.AreEqual(settingsBefore_, settingsAfter, "The test left the bench's settings other than it found them.");
      }

      // ---- the list and the diff ------------------------------------------------

      [Test]
      [Description("The four profiles are listed, in their order, each with a title, a paragraph and its entries. Negative control: a name that is no profile is 404.")]
      public void TheFourProfilesAreListed()
      {
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         List<JsonElement> profiles = list.GetProperty("profiles").EnumerateArray().ToList();

         CollectionAssert.AreEqual(Names, profiles.Select(p => p.GetProperty("name").GetString()).ToArray());
         foreach (JsonElement profile in profiles)
         {
            ClassicAssert.IsNotEmpty(profile.GetProperty("title").GetString());
            ClassicAssert.IsNotEmpty(profile.GetProperty("summary").GetString());
            ClassicAssert.Greater(profile.GetProperty("entries").GetInt32(), 10, profile.GetProperty("name").GetString());
            ClassicAssert.AreEqual(0, profile.GetProperty("refused").GetInt32(), "A shipped profile has an entry its setting refuses: " + profile);
            ClassicAssert.AreEqual(JsonValueKind.Number, profile.GetProperty("confirmations").ValueKind, "The list counts the confirmations as a number: " + profile);
         }
         ClassicAssert.AreEqual(JsonValueKind.Null, list.GetProperty("snapshot").ValueKind, "No way back is held on a bench no profile was applied to.");

         (int status, string body) missing = Admin("GET", "/api/v1/settings/profiles/nonesuch", null);
         ClassicAssert.AreEqual(404, missing.status, missing.body);
         StringAssert.Contains("no profile named nonesuch", missing.body);
      }

      [Test]
      [Description("Every entry of every shipped profile is answered as written through a REST settings route - a property through the row that keeps it, a [Settings] key through its row or the ini route - and none is refused by its setting. Negative control: the diff of a property names a settings group's route, never the ini route. The writes themselves are TheRelayAndTheSenderApplyEveryEntryAndTakeEffect's.")]
      public void EveryEntryIsWrittenThroughASettingsRouteAndNoneIsRefused()
      {
         foreach (string name in Names)
         {
            JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/" + name, null), 200);
            foreach (JsonElement entry in diff.GetProperty("entries").EnumerateArray())
            {
               string setting = name + "/" + entry.GetProperty("name").GetString();
               ClassicAssert.AreEqual(JsonValueKind.Null, entry.GetProperty("refused").ValueKind, setting + " is refused: " + entry);
               string route = entry.GetProperty("route").GetString();
               StringAssert.StartsWith("PUT /api/v1/settings", route, setting);
               if (entry.GetProperty("store").GetString() == "database")
                  StringAssert.DoesNotStartWith("PUT /api/v1/settings/ini/", route, setting + " is a property and is not written through the ini route.");
               StringAssert.IsMatch("^(now|reinitialize|restart)$", entry.GetProperty("applies").GetString(), setting);
            }
            ClassicAssert.AreEqual(16, diff.GetProperty("diff_token").GetString().Length, name + " answers a diff token: " + diff.GetProperty("diff_token"));
         }
      }

      [Test]
      [Description("The office diff answers every entry with the value this server holds now and whether applying would change it, and its token moves with it. Negative control: the maximum message size set to the profile's value answers changes false, and set to another answers the value, changes true and another token.")]
      public void TheDiffAnswersTheCurrentValueAndWhetherItWouldChange()
      {
         int sizeBefore = _settings.MaxMessageSize;
         putBack_.Add(() => _settings.MaxMessageSize = sizeBefore);

         _settings.MaxMessageSize = 25600;
         JsonElement entry = Entry("office", "maxmessagesize");
         ClassicAssert.AreEqual("25600", entry.GetProperty("current").GetString());
         ClassicAssert.IsFalse(entry.GetProperty("changes").GetBoolean());
         ClassicAssert.AreEqual("limit", entry.GetProperty("levels").GetString(), "The maximum message size is a limit, and the diff says so.");
         string tokenAtProfileValue = DiffToken("office");

         _settings.MaxMessageSize = 20000;
         entry = Entry("office", "maxmessagesize");
         ClassicAssert.AreEqual("20000", entry.GetProperty("current").GetString());
         ClassicAssert.AreEqual("25600", entry.GetProperty("value").GetString());
         ClassicAssert.IsTrue(entry.GetProperty("changes").GetBoolean());
         ClassicAssert.AreNotEqual(tokenAtProfileValue, DiffToken("office"), "The token did not move with the diff.");
         ClassicAssert.AreEqual(DiffToken("office"), DiffToken("office"), "The token moved with nothing changed.");

         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/office", null), 200);
         int changing = diff.GetProperty("entries").EnumerateArray().Count(e => e.GetProperty("changes").GetBoolean());
         ClassicAssert.AreEqual(changing, diff.GetProperty("changes").GetInt32(), "changes counts the entries that would change.");
         ClassicAssert.AreEqual(changing, list.GetProperty("profiles")[0].GetProperty("changes").GetInt32(), "The list counts what the diff counts.");
      }

      // ---- apply, and the way back ------------------------------------------------

      [Test]
      [Description("An apply changes exactly the profile's entries, reads each back, is one audit event with the counts, and a second apply is refused while the way back is held. Negative control: a setting the profile does not name is not changed - neither one already changed from stock nor one at its default.")]
      public void ApplyChangesExactlyTheEntriesAndIsRecordedOnce()
      {
         Dictionary<string, string> before = ChangedValues();
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/office", null), 200);
         var named = new HashSet<string>(diff.GetProperty("entries").EnumerateArray().Select(e => e.GetProperty("name").GetString()), StringComparer.OrdinalIgnoreCase);
         int changing = diff.GetProperty("changes").GetInt32();
         ClassicAssert.Greater(changing, 0, "The negative control needs the profile to change something on this bench.");

         long since = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
         JsonElement applied = Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{\"diff_token\":\"" + diff.GetProperty("diff_token").GetString() + "\"}"), 200);
         ClassicAssert.AreEqual(changing, applied.GetProperty("set").GetInt32(), applied.ToString());
         ClassicAssert.AreEqual(0, applied.GetProperty("failed").GetInt32());

         JsonElement after = Json(Admin("GET", "/api/v1/settings/profiles/office", null), 200);
         foreach (JsonElement entry in after.GetProperty("entries").EnumerateArray())
            ClassicAssert.IsFalse(entry.GetProperty("changes").GetBoolean(), entry.GetProperty("name").GetString() + " was not set: " + entry);
         ClassicAssert.IsFalse(after.GetProperty("applicable").GetBoolean(), "With the way back held another apply is not offered.");

         Dictionary<string, string> now = ChangedValues();
         foreach (KeyValuePair<string, string> setting in before)
         {
            if (!named.Contains(setting.Key))
               ClassicAssert.IsTrue(now.ContainsKey(setting.Key) && now[setting.Key] == setting.Value, setting.Key + " is not the profile's and was changed.");
         }
         CollectionAssert.IsEmpty(now.Keys.Where(k => !before.ContainsKey(k) && !named.Contains(k)).ToList(),
            "A setting the profile does not name moved away from its default.");

         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=applied&since=" + since, null), 200);
         ClassicAssert.AreEqual(1, audit.GetProperty("total").GetInt32(), "One apply, one event: " + audit);
         JsonElement row = audit.GetProperty("entries")[0];
         ClassicAssert.AreEqual("office", row.GetProperty("object").GetString());
         StringAssert.Contains(named.Count + " entries: " + changing + " set", row.GetProperty("detail").GetString());

         (int status, string body) second = Admin("POST", "/api/v1/settings/profiles/gateway/apply", ConfirmBody("gateway"));
         ClassicAssert.AreEqual(409, second.status, second.body);
         StringAssert.Contains("is held as the way back", second.body);
      }

      [Test]
      [Description("The revert puts every value back - the changed-from-stock document the same before the apply and after the revert, and every [Settings] key the profile sets stored or absent, with the same text, as it was - clears the way back and says what waits; a revert with nothing to revert is refused. Negative control: between the apply and the revert the document differs.")]
      public void TheRevertPutsEveryValueBackAndClearsTheWayBack()
      {
         string before = ChangedSettings();
         Dictionary<string, (bool present, string value)> iniBefore = IniKeysOf("office");

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         ClassicAssert.AreNotEqual(before, ChangedSettings(), "The negative control: an applied profile changes the document.");
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);

         long since = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
         JsonElement reverted = Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         ClassicAssert.AreEqual(0, reverted.GetProperty("failed").GetInt32(), reverted.ToString());
         ClassicAssert.AreEqual(0, reverted.GetProperty("skipped").GetInt32(), reverted.ToString());
         ClassicAssert.AreEqual(JsonValueKind.Object, reverted.GetProperty("waiting").ValueKind, "The revert says what waits: " + reverted);
         foreach (JsonElement entry in reverted.GetProperty("entries").EnumerateArray())
            StringAssert.IsMatch("^(now|reinitialize|restart)$", entry.GetProperty("applies").GetString(), "Each value put back says when it takes effect: " + entry);

         ClassicAssert.AreEqual(before, ChangedSettings(), "The revert did not put every value back as it was.");
         Dictionary<string, (bool present, string value)> iniAfter = IniKeysOf("office");
         foreach (KeyValuePair<string, (bool present, string value)> key in iniBefore)
         {
            ClassicAssert.AreEqual(key.Value.present, iniAfter[key.Key].present, key.Key + " was " + (key.Value.present ? "stored" : "absent") + " and is not now.");
            if (key.Value.present)
               ClassicAssert.AreEqual(key.Value.value, iniAfter[key.Key].value, key.Key + " did not go back byte for byte.");
         }

         ClassicAssert.AreEqual("", _settings.AppliedProfile, "A reverted profile is in effect no longer.");
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         ClassicAssert.AreEqual(JsonValueKind.Null, list.GetProperty("snapshot").ValueKind);
         ClassicAssert.AreEqual("reverted", list.GetProperty("applied").GetProperty("state").GetString());

         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=reverted&since=" + since, null), 200);
         ClassicAssert.AreEqual(1, audit.GetProperty("total").GetInt32(), audit.ToString());
         ClassicAssert.AreEqual("office", audit.GetProperty("entries")[0].GetProperty("object").GetString());

         (int status, string body) again = Admin("POST", "/api/v1/settings/profiles/revert", "{}");
         ClassicAssert.AreEqual(409, again.status, again.body);
         StringAssert.Contains("nothing to revert", again.body);
      }

      [Test]
      [Description("A setting an administrator changed after the apply is left by the revert, and the answer says so. Negative control: the profile's other settings are put back.")]
      public void ASettingChangedSinceIsLeftByTheRevert()
      {
         int sizeBefore = _settings.MaxMessageSize;
         putBack_.Add(() => _settings.MaxMessageSize = sizeBefore);
         if (sizeBefore == 25600)
            _settings.MaxMessageSize = 20480;
         bool spfBefore = _settings.AntiSpam.UseSPF;
         putBack_.Add(() => _settings.AntiSpam.UseSPF = spfBefore);
         _settings.AntiSpam.UseSPF = false;

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         ClassicAssert.IsTrue(_settings.AntiSpam.UseSPF);

         _settings.MaxMessageSize = 30000;

         JsonElement reverted = Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         JsonElement left = reverted.GetProperty("entries").EnumerateArray().First(e => e.GetProperty("name").GetString() == "maxmessagesize");
         ClassicAssert.AreEqual("left", left.GetProperty("result").GetString(), left.ToString());
         StringAssert.Contains("changed since the profile set it", left.GetProperty("reason").GetString());
         ClassicAssert.AreEqual(30000, _settings.MaxMessageSize, "The administrator's own value was undone.");

         ClassicAssert.IsFalse(_settings.AntiSpam.UseSPF, "The profile's other settings were not put back.");
      }

      [Test]
      [Description("The relay and then the sender are applied with their confirmation, every entry written and none failed, every entry answering changes false afterwards, and an entry of each shown to take effect through what reads it rather than through its stored value: the directory no longer answered, the message trace switched on and kept ninety days, then a hundred and eighty. Negative control: after each revert the effects are gone.")]
      public void TheRelayAndTheSenderApplyEveryEntryAndTakeEffect()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "reader@" + _domain.Name, AccountPassword);

         bool directoryBefore = DirectoryAnswered(reader.Address);
         bool traceBefore = TraceSource().GetProperty("enabled").GetBoolean();
         if (!directoryBefore)
            Assert.Ignore("The directory is off on this bench, so the relay's switching it off shows nothing here.");

         foreach (string name in new[] { "relay", "sender" })
         {
            JsonElement applied = Json(Admin("POST", "/api/v1/settings/profiles/" + name + "/apply", ConfirmBody(name)), 200);
            ClassicAssert.AreEqual(0, applied.GetProperty("failed").GetInt32(), name + ": " + applied);
            ClassicAssert.Greater(applied.GetProperty("set").GetInt32(), 0, name + ": " + applied);
            foreach (JsonElement written in applied.GetProperty("entries").EnumerateArray())
               ClassicAssert.AreNotEqual("failed", written.GetProperty("result").GetString(), name + ": " + written);

            JsonElement after = Json(Admin("GET", "/api/v1/settings/profiles/" + name, null), 200);
            foreach (JsonElement entry in after.GetProperty("entries").EnumerateArray())
               ClassicAssert.IsFalse(entry.GetProperty("changes").GetBoolean(), name + ": " + entry.GetProperty("name").GetString() + " was not set: " + entry);

            // The effects, as what reads each setting answers them.
            ClassicAssert.IsFalse(DirectoryAnswered(reader.Address), name + ": the directory is still answered.");
            JsonElement trace = TraceSource();
            ClassicAssert.IsTrue(trace.GetProperty("enabled").GetBoolean(), name + ": the message trace is not on: " + trace);
            ClassicAssert.AreEqual(name == "relay" ? 90 : 180, trace.GetProperty("retention_days").GetInt32(), name + ": " + trace);

            Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
            ClassicAssert.AreEqual(directoryBefore, DirectoryAnswered(reader.Address), name + ": the revert did not answer the directory again.");
            ClassicAssert.AreEqual(traceBefore, TraceSource().GetProperty("enabled").GetBoolean(), name + ": the revert did not put the trace back.");
         }
      }

      [Test]
      [Description("The office profile's password policy takes effect where a password is chosen: a new account's short password is refused. Negative control: after the revert the same password is taken.")]
      public void TheOfficePasswordPolicyTakesEffect()
      {
         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);

         COMException refused = Assert.Throws<COMException>(() =>
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "short@" + _domain.Name, "Short1!"));
         StringAssert.Contains("10", refused.Message, "The refusal names the minimum length: " + refused.Message);

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         Assert.DoesNotThrow(() => SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "short@" + _domain.Name, "Short1!"));
      }

      [Test]
      [Description("The gateway profile's quarantine takes effect: an account's quarantine answers enabled. Negative control: after the revert it answers what it did before.")]
      public void TheGatewayQuarantineTakesEffect()
      {
         Account reader = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "held@" + _domain.Name, AccountPassword);
         bool before = QuarantineEnabled(reader.Address);

         Json(Admin("POST", "/api/v1/settings/profiles/gateway/apply", ConfirmBody("gateway")), 200);
         ClassicAssert.IsTrue(QuarantineEnabled(reader.Address), "The quarantine is not on for the account.");

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         ClassicAssert.AreEqual(before, QuarantineEnabled(reader.Address));
      }

      // ---- a listener, a trust or data is never taken silently ------------------------

      [Test]
      [Description("An entry that turns a filter off asks for a confirmation where it would change something: the apply without it is refused, names the entry and changes nothing; with it the profile applies. Negative control: with greylisting already off, the greylisting entry asks for nothing.")]
      public void AnEntryThatTurnsAFilterOffNeedsTheConfirmation()
      {
         bool greylistingBefore = _settings.AntiSpam.GreyListingEnabled;
         putBack_.Add(() => _settings.AntiSpam.GreyListingEnabled = greylistingBefore);

         _settings.AntiSpam.GreyListingEnabled = false;
         JsonElement quiet = Entry("gateway", "usegreylisting");
         ClassicAssert.IsFalse(quiet.GetProperty("confirm").GetBoolean(), "Greylisting already off asks for no confirmation.");
         ClassicAssert.AreEqual("turns_filter_off", quiet.GetProperty("caution").GetString());

         _settings.AntiSpam.GreyListingEnabled = true;
         JsonElement loud = Entry("gateway", "usegreylisting");
         ClassicAssert.IsTrue(loud.GetProperty("confirm").GetBoolean(), loud.ToString());

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/gateway/apply", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         JsonElement answer = JsonDocument.Parse(refused.body).RootElement;
         ClassicAssert.IsTrue(answer.GetProperty("confirmation").GetBoolean());
         JsonElement needed = answer.GetProperty("confirmation_needed").EnumerateArray().First(e => e.GetProperty("name").GetString() == "usegreylisting");
         ClassicAssert.AreEqual("turns_filter_off", needed.GetProperty("caution").GetString(), needed.ToString());
         ClassicAssert.IsTrue(_settings.AntiSpam.GreyListingEnabled, "A refused apply changed a setting.");
         ClassicAssert.AreEqual(JsonValueKind.Null, Json(Admin("GET", "/api/v1/settings/profiles", null), 200).GetProperty("snapshot").ValueKind, "A refused apply saved a way back.");

         Json(Admin("POST", "/api/v1/settings/profiles/gateway/apply", ConfirmBody("gateway")), 200);
         ClassicAssert.IsFalse(_settings.AntiSpam.GreyListingEnabled, "The confirmed apply did not turn greylisting off.");
      }

      [Test]
      [Description("An entry that trusts an address range asks for a confirmation where it would change something: the gateway's XCLIENT, off on this bench, is listed in the diff and the refusal as trusts_addresses. Negative control: with XCLIENT already on it asks for nothing.")]
      public void AnEntryThatTrustsAnAddressRangeNeedsTheConfirmation()
      {
         const string key = "SMTPXClientEnabled";
         (bool present, string value) before = IniKey(key);
         putBack_.Add(() => PutIniKeyBack(key, before));

         _settings.SetIniSetting(key, "1");
         _application.Reinitialize();
         ClassicAssert.IsFalse(Entry("gateway", key).GetProperty("confirm").GetBoolean(), "XCLIENT already on asks for no confirmation.");

         _settings.SetIniSetting(key, "0");
         _application.Reinitialize();
         JsonElement entry = Entry("gateway", key);
         ClassicAssert.AreEqual("trusts_addresses", entry.GetProperty("caution").GetString());
         ClassicAssert.IsTrue(entry.GetProperty("confirm").GetBoolean(), entry.ToString());

         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/gateway", null), 200);
         CollectionAssert.Contains(diff.GetProperty("confirmation_needed").EnumerateArray().Select(e => e.GetProperty("name").GetString()).ToList(), key);

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/gateway/apply", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains(key + " (trusts an address range", refused.body);
      }

      [Test]
      [Description("A window of days the profile would shorten asks for a confirmation, with the note that a revert does not bring back what the sweep deletes: the relay's ninety days of trace under a year kept. Negative control: under the thirty days shipped, a longer window asks for nothing.")]
      public void AWindowThatWouldShortenNeedsTheConfirmation()
      {
         const string key = "MessageTraceRetentionDays";
         (bool present, string value) before = IniKey(key);
         putBack_.Add(() => PutIniKeyBack(key, before));

         _settings.SetIniSetting(key, "30");
         _application.Reinitialize();
         JsonElement longer = Entry("relay", key);
         ClassicAssert.AreEqual("purges", longer.GetProperty("caution").GetString());
         ClassicAssert.IsFalse(longer.GetProperty("confirm").GetBoolean(), "A longer window deletes nothing and asks for nothing: " + longer);
         ClassicAssert.AreEqual(JsonValueKind.Null, longer.GetProperty("note").ValueKind);

         _settings.SetIniSetting(key, "365");
         _application.Reinitialize();
         JsonElement shorter = Entry("relay", key);
         ClassicAssert.IsTrue(shorter.GetProperty("confirm").GetBoolean(), shorter.ToString());
         StringAssert.Contains("a revert puts the number back, not what was deleted", shorter.GetProperty("note").GetString());

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains(key + " (shortens how long something is kept", refused.body);
         ClassicAssert.AreEqual("365", IniKey(key).value, "A refused apply shortened the window.");
      }

      [Test]
      [Description("The relay closes IMAP and POP3, which asks for a confirmation on a bench where they are on; the unconfirmed apply is refused and changes nothing. Negative control: the diff marks exactly the two listeners, the confirmation field is a boolean, and a confirmation is of a diff, so one without the diff's token is refused.")]
      public void ClosingAListenerIsNeverDoneSilently()
      {
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/relay", null), 200);
         List<string> listeners = diff.GetProperty("entries").EnumerateArray()
            .Where(e => e.GetProperty("caution").ValueKind == JsonValueKind.String && e.GetProperty("caution").GetString() == "closes_listener")
            .Select(e => e.GetProperty("name").GetString()).ToList();
         CollectionAssert.AreEquivalent(new[] { "protocolimap", "protocolpop3" }, listeners);

         if (!_settings.ServiceIMAP)
            Assert.Ignore("IMAP is off on this bench, so closing it asks for nothing here.");

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("closes a listener", refused.body);
         ClassicAssert.IsTrue(_settings.ServiceIMAP, "A refused apply closed IMAP.");

         (int status, string body) wrongType = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{\"confirm\":\"yes\"}");
         ClassicAssert.AreEqual(400, wrongType.status, "A confirmation given as a string is not one: " + wrongType.body);

         (int status, string body) noToken = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{\"confirm\":true}");
         ClassicAssert.AreEqual(400, noToken.status, "A confirmation without the diff's token confirms nothing anybody saw: " + noToken.body);
         StringAssert.Contains("diff_token", noToken.body);
         ClassicAssert.IsTrue(_settings.ServiceIMAP);
      }

      [Test]
      [Description("An apply from a diff the server no longer answers is refused as stale and changes nothing: the token of a diff read before a setting moved. Negative control: the token read after the move applies.")]
      public void AnApplyFromAStaleDiffIsRefused()
      {
         bool greylistingBefore = _settings.AntiSpam.GreyListingEnabled;
         putBack_.Add(() => _settings.AntiSpam.GreyListingEnabled = greylistingBefore);

         _settings.AntiSpam.GreyListingEnabled = false;
         string stale = DiffToken("gateway");
         _settings.AntiSpam.GreyListingEnabled = true;

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/gateway/apply", "{\"confirm\":true,\"diff_token\":\"" + stale + "\"}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         ClassicAssert.IsTrue(JsonDocument.Parse(refused.body).RootElement.GetProperty("stale").GetBoolean(), refused.body);
         StringAssert.Contains("changed since the diff", refused.body);
         ClassicAssert.IsTrue(_settings.AntiSpam.GreyListingEnabled, "A stale apply changed a setting.");
         ClassicAssert.IsFalse(SnapshotHeld(), "A stale apply saved a way back.");

         Json(Admin("POST", "/api/v1/settings/profiles/gateway/apply", ConfirmBody("gateway")), 200);
         ClassicAssert.IsFalse(_settings.AntiSpam.GreyListingEnabled);
      }

      [Test]
      [Description("A setting the server's environment manages is skipped by the apply, with the sentence naming its variable, and the diff says so first; the changed-from-stock document does not name the profile as its source, though the variable holds the profile's own value. Negative control: the profile's other settings are set.")]
      public void ASettingTheEnvironmentManagesIsSkipped()
      {
         const string key = "AccountLockoutThreshold";
         (bool present, string value) before = IniKey(key);
         putBack_.Add(() => PutIniKeyBack(key, before));
         bool spfBefore = _settings.AntiSpam.UseSPF;
         putBack_.Add(() => _settings.AntiSpam.UseSPF = spfBefore);
         _settings.AntiSpam.UseSPF = false;

         environmentUsed_ = true;
         Verb run = RunVerb("--apply-environment", new Dictionary<string, string> { { "HM_SETTING_" + key, "10" } });
         ClassicAssert.AreEqual(0, run.ExitCode, run.Output);
         _application.Reinitialize();

         JsonElement entry = Entry("office", key);
         ClassicAssert.AreEqual("HM_SETTING_" + key, entry.GetProperty("environment").GetString(), entry.ToString());

         JsonElement applied = Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         JsonElement skipped = applied.GetProperty("entries").EnumerateArray().First(e => e.GetProperty("name").GetString() == key);
         ClassicAssert.AreEqual("skipped", skipped.GetProperty("result").GetString(), skipped.ToString());
         StringAssert.Contains("HM_SETTING_" + key, skipped.GetProperty("reason").GetString());
         ClassicAssert.IsTrue(_settings.AntiSpam.UseSPF, "The profile's other settings were not set.");

         JsonElement changed = Json(Admin("GET", "/api/v1/settings/changed", null), 200);
         JsonElement listed = changed.GetProperty("settings").EnumerateArray().First(e => e.GetProperty("name").GetString() == key);
         ClassicAssert.AreEqual(JsonValueKind.Null, listed.GetProperty("profile").ValueKind, "A value the environment set is credited to the profile: " + listed);

         // The way back, and the record released, before the teardown's own.
         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         Verb release = RunVerb("--apply-environment", new Dictionary<string, string>());
         ClassicAssert.AreEqual(0, release.ExitCode, release.Output);
         _application.Reinitialize();
      }

      // ---- the records ------------------------------------------------------------------

      [Test]
      [Description("A way back that cannot be read is said to be: the list answers it unreadable, an apply is refused, the revert fails with HM7300 in the error log, and the discard - over REST - clears it. Negative control: after the discard the list holds no way back.")]
      public void AWayBackThatCannotBeReadCanOnlyBeDiscarded()
      {
         _application.Database.ExecuteSQL("update hm_settings set settingstring = 'not a way back' where settingname = 'ProfileSnapshot'");
         LogHandler.DeleteErrorLog();

         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         ClassicAssert.IsFalse(list.GetProperty("snapshot").GetProperty("readable").GetBoolean(), list.GetProperty("snapshot").ToString());

         (int status, string body) apply = Admin("POST", "/api/v1/settings/profiles/office/apply", "{}");
         ClassicAssert.AreEqual(409, apply.status, apply.body);
         StringAssert.Contains("cannot be read", apply.body);

         (int status, string body) revert = Admin("POST", "/api/v1/settings/profiles/revert", "{}");
         ClassicAssert.AreEqual(500, revert.status, revert.body);
         StringAssert.Contains("HM7300", revert.body);
         string errors = LogHandler.ReadAndDeleteErrorLog();
         StringAssert.Contains("7300", errors, "The unreadable way back is reported as HM7300: " + errors);

         Json(Admin("POST", "/api/v1/settings/profiles/discard", "{}"), 200);
         ClassicAssert.AreEqual(JsonValueKind.Null, Json(Admin("GET", "/api/v1/settings/profiles", null), 200).GetProperty("snapshot").ValueKind);
      }

      [Test]
      [Description("Keeping the shipped settings is recorded: the checklist's item is not needed, and the choice is refused while a profile is in effect. Negative control: a profile can still be applied afterwards.")]
      public void KeepingTheShippedSettingsIsRecorded()
      {
         long since = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
         Json(Admin("POST", "/api/v1/settings/profiles/decline", "{}"), 200);
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         ClassicAssert.AreEqual("declined", list.GetProperty("applied").GetProperty("state").GetString(), list.ToString());
         ClassicAssert.AreEqual("", _settings.AppliedProfile);

         JsonElement item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("not_needed", item.GetProperty("state").GetString(), item.ToString());
         ClassicAssert.AreEqual("profile_declined", item.GetProperty("findings")[0].GetProperty("code").GetString());

         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=declined&since=" + since, null), 200);
         ClassicAssert.AreEqual(1, audit.GetProperty("total").GetInt32(), audit.ToString());

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/decline", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);
      }

      // ---- COM, the verbs, the checklist and the changed document -------------------

      [Test]
      [Description("COM answers the documents REST answers, applies with the diff's token, reverts, keeps and says which profile is in effect. Negative control: a name that is no profile, a confirmation without its token, a second apply and a revert with nothing to revert raise errors.")]
      public void TheComRoundTrip()
      {
         // Every [Settings] key of the profile as the store holds it - stored or not,
         // and its text - which the changed document cannot tell apart at a default.
         Dictionary<string, (bool present, string value)> iniBefore = IniKeysOf("office");

         string overRest = Admin("GET", "/api/v1/settings/profiles/gateway", null).body;
         ClassicAssert.AreEqual(overRest, _settings.GetProfile("gateway"), "COM and REST describe the profile differently.");
         ClassicAssert.AreEqual(4, JsonDocument.Parse(_settings.GetProfiles()).RootElement.GetProperty("profiles").GetArrayLength());

         Assert.Throws<COMException>(() => _settings.GetProfile("nonesuch"));
         COMException noToken = Assert.Throws<COMException>(() => _settings.ApplyProfile("office", true, ""));
         StringAssert.Contains("DiffToken", noToken.Message);

         ClassicAssert.AreEqual("", _settings.AppliedProfile);
         string token = JsonDocument.Parse(_settings.GetProfile("office")).RootElement.GetProperty("diff_token").GetString();
         JsonElement applied = JsonDocument.Parse(_settings.ApplyProfile("office", false, token)).RootElement;
         ClassicAssert.AreEqual("office", applied.GetProperty("profile").GetString());
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);

         COMException second = Assert.Throws<COMException>(() => _settings.ApplyProfile("office", false, ""));
         StringAssert.Contains("is held as the way back", second.Message);

         _settings.RevertProfile();
         ClassicAssert.AreEqual("", _settings.AppliedProfile);
         Assert.Throws<COMException>(() => _settings.RevertProfile());
         Assert.Throws<COMException>(() => _settings.DiscardProfileSnapshot());

         // The other way out: the settings kept and the way back forgotten. Kept means
         // no way back, so the settings are put back by hand - registered before the
         // discard, so that a failure below still puts them back.
         _settings.ApplyProfile("office", false, "");
         putBack_.Add(() => RestoreSettingsTheProfileSet("office", iniBefore));
         _settings.DiscardProfileSnapshot();
         ClassicAssert.AreEqual("office", _settings.AppliedProfile, "A kept profile is still in effect.");
         ClassicAssert.AreEqual("kept", JsonDocument.Parse(_settings.GetProfiles()).RootElement.GetProperty("applied").GetProperty("state").GetString());
         JsonElement item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("profile_kept", item.GetProperty("findings")[0].GetProperty("code").GetString(), item.ToString());

         // Declining while a profile is in effect is refused over COM as over REST.
         Assert.Throws<COMException>(() => _settings.DeclineProfiles());
      }

      [Test]
      [Description("An event script run on the reverting thread by the revert's own write - a way back that writes the scripting switch on, which runs the script's top level - that asks for a profile verb over COM is refused at once with the reason, and the revert finishes. No shipped profile sets the switch, so the way back is written here; an error reported during a write, which fires OnError on the same thread, is the other way such a script runs. The script writes what it was told to a file of its own: the switch's load runs the top level with none of an event's objects, EventLog among them. Negative control: up to 6.3.5 the verb locked the profiles' mutex a second time on the thread holding it, which MSVC answers by throwing resource_deadlock_would_occur, so the script was told only that an unknown error occurred.")]
      public void AnEventScriptRunByARevertCannotReenterTheProfiles()
      {
         var scripting = _settings.Scripting;
         string originalLanguage = scripting.Language;
         bool originalEnabled = scripting.Enabled;

         // Beside the server's own logs, which the service writes and this suite
         // reads: the load that runs the script has no EventLog to write to. Until
         // 6.3.6 the script wrote there, every write failed under On Error Resume
         // Next, and this test failed on Windows whatever the verb answered.
         string outcomeFile = Path.Combine(Path.GetDirectoryName(LogHandler.GetEventLogFileName()), "profiles-reentry-outcome.txt");
         if (File.Exists(outcomeFile))
            File.Delete(outcomeFile);

         putBack_.Add(() =>
         {
            var putBack = _settings.Scripting;
            // settings-store-exempt: this is the event script file, not hMailServer.ini.
            File.WriteAllText(putBack.CurrentScriptFile, "");
            putBack.Enabled = originalEnabled;
            putBack.Language = originalLanguage;
            putBack.Reload();
            if (File.Exists(outcomeFile))
               File.Delete(outcomeFile);
         });

         // Read with scripting off, and not run: nothing runs it until the revert
         // switches scripting on, on the thread that holds the profiles' lock. The
         // load runs the top level once to compile it and once for each handler it
         // looks for, and each run adds a line.
         scripting.Enabled = false;
         scripting.Language = "VBScript";
         // settings-store-exempt: this is the event script file, not hMailServer.ini.
         File.WriteAllText(scripting.CurrentScriptFile,
            "On Error Resume Next\r\n" +
            "Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\n" +
            "Set outcome = fso.OpenTextFile(\"" + outcomeFile.Replace("\"", "\"\"") + "\", 8, True)\r\n" +
            "Set reentry = CreateObject(\"hMailServer.Application\")\r\n" +
            "Call reentry.Authenticate(\"Administrator\", \"" + AdminPassword + "\")\r\n" +
            "If Err.Number <> 0 Then outcome.WriteLine(\"reentry-setup: \" & Err.Description)\r\n" +
            "Err.Clear\r\n" +
            "Call reentry.Settings.DiscardProfileSnapshot()\r\n" +
            "If Err.Number = 0 Then\r\n" +
            "   outcome.WriteLine(\"reentry: taken\")\r\n" +
            "Else\r\n" +
            "   outcome.WriteLine(\"reentry: \" & Err.Description)\r\n" +
            "End If\r\n" +
            "Err.Clear\r\n" +
            "outcome.Close\r\n");
         scripting.Reload();

         // The way back a profile that switched scripting off would have left - no
         // shipped profile sets the switch, so it is written here: the revert writes
         // the switch on again, through its settings row, on this request's thread.
         long applied = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 60;
         _application.Database.ExecuteSQL(
            "update hm_settings set settingstring = '{\"profile\":\"office\",\"applied\":" + applied +
            ",\"entries\":[[\"d\",\"usescriptserver\",\"1\",\"0\",1]]}' where settingname = 'ProfileSnapshot'");
         _application.Reinitialize();

         // The revert returns, where the script's call would have locked the mutex its
         // thread holds.
         JsonElement reverted = JsonDocument.Parse(_settings.RevertProfile()).RootElement;
         ClassicAssert.IsTrue(_settings.Scripting.Enabled, "The revert did not switch scripting back on: " + reverted);
         ClassicAssert.IsFalse(SnapshotHeld(), "The revert did not clear the way back: " + reverted);

         // The runs were made inside the revert, which has returned.
         ClassicAssert.IsTrue(File.Exists(outcomeFile), "The script's top level did not run, or could not write " + outcomeFile + ".");
         string[] outcome = File.ReadAllLines(outcomeFile).Where(line => line.Length > 0).ToArray();
         string told = string.Join(" | ", outcome);

         ClassicAssert.IsNotEmpty(outcome, "The script's top level wrote nothing.");
         foreach (string line in outcome)
         {
            StringAssert.StartsWith("reentry: A profile could not be discarded: this thread is applying or reverting one", line,
               "A run of the script on the reverting thread was not refused as re-entered (" + outcome.Length + " run(s)): " + told);
         }
      }

      [Test]
      [Description("The verbs refuse while the service runs, and with it stopped: the relay without --confirm exits 3; --if-new leaves a server of ten accounts as it is (exit 5); none records the shipped settings; --once then applies nothing; the office profile applies and a second apply is refused while its way back is held; --revert-profile puts it back; and each is on the audit trail under the command line's name. Negative control: nothing is changed by a refused run.")]
      public void TheVerbsRefuseBesideARunningServiceAndApplyWithItStopped()
      {
         if (TestSetup.ConsoleServer)
            Assert.Ignore("The server under test is a console server, which the verbs' service check cannot see stopped.");

         Verb running = RunVerb("--apply-profile office", new Dictionary<string, string>());
         ClassicAssert.AreEqual(4, running.ExitCode, running.Output);
         StringAssert.Contains("service is running", running.Output);
         ClassicAssert.AreEqual("", _settings.AppliedProfile, "A refused run applied a profile.");

         bool imapOn = _settings.ServiceIMAP;
         for (int i = 0; i < 10; i++)
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "inservice" + i + "@" + _domain.Name, AccountPassword);

         long since = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
         ServerExecutable();
         StopService();
         try
         {
            Verb relay = RunVerb("--apply-profile relay --once", new Dictionary<string, string>());
            if (imapOn)
            {
               ClassicAssert.AreEqual(3, relay.ExitCode, relay.Output);
               StringAssert.Contains("--confirm", relay.Output);
            }

            Verb inService = RunVerb("--apply-profile office --if-new", new Dictionary<string, string>());
            ClassicAssert.AreEqual(5, inService.ExitCode, inService.Output);
            StringAssert.Contains("10 accounts", inService.Output);

            Verb none = RunVerb("--apply-profile none --once", new Dictionary<string, string>());
            ClassicAssert.AreEqual(0, none.ExitCode, none.Output);
            StringAssert.Contains("Recorded", none.Output);

            Verb once = RunVerb("--apply-profile office --once", new Dictionary<string, string>());
            ClassicAssert.AreEqual(0, once.ExitCode, once.Output);
            StringAssert.Contains("--once leaves the settings as they are", once.Output);
            StringAssert.DoesNotContain("set to", once.Output);

            Verb office = RunVerb("--apply-profile office", new Dictionary<string, string>());
            ClassicAssert.AreEqual(0, office.ExitCode, office.Output);
            StringAssert.Contains("set to", office.Output);
            StringAssert.Contains("The office profile:", office.Output);

            Verb gateway = RunVerb("--apply-profile gateway --confirm", new Dictionary<string, string>());
            ClassicAssert.AreEqual(1, gateway.ExitCode, gateway.Output);
            StringAssert.Contains("is held as the way back", gateway.Output);

            Verb revert = RunVerb("--revert-profile", new Dictionary<string, string>());
            ClassicAssert.AreEqual(0, revert.ExitCode, revert.Output);
            StringAssert.Contains("is reverted", revert.Output);
         }
         finally
         {
            RestartServerAndReacquireCom();
         }

         ClassicAssert.AreEqual("", _settings.AppliedProfile);
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         ClassicAssert.AreEqual("reverted", list.GetProperty("applied").GetProperty("state").GetString());

         foreach ((string action, string actor) in new[] { ("applied", "hmailserver --apply-profile"), ("reverted", "hmailserver --revert-profile"), ("declined", "hmailserver --apply-profile") })
         {
            JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=" + action + "&since=" + since, null), 200);
            ClassicAssert.AreEqual(1, audit.GetProperty("total").GetInt32(), action + ": " + audit);
            ClassicAssert.AreEqual(actor, audit.GetProperty("entries")[0].GetProperty("actor").GetString(), action + ": " + audit);
         }
      }

      [Test]
      [Description("The setup checklist's first item asks for a profile on a young server, is not needed on one of ten accounts, and says which was applied and that it was reverted. Negative control: the item's state moves with the accounts and the record.")]
      public void TheChecklistOffersAProfileFirst()
      {
         JsonElement item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("profile", item.GetProperty("key").GetString());
         ClassicAssert.AreEqual("profile_none", item.GetProperty("findings")[0].GetProperty("code").GetString(), "The bench has fewer than ten accounts: " + item);
         ClassicAssert.AreEqual("action_needed", item.GetProperty("state").GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("done", item.GetProperty("state").GetString());
         ClassicAssert.AreEqual("profile_applied", item.GetProperty("findings")[0].GetProperty("code").GetString());
         ClassicAssert.AreEqual("office", item.GetProperty("findings")[0].GetProperty("args")[0].GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("profile_reverted", item.GetProperty("findings")[0].GetProperty("code").GetString());

         // The young server's limit: ten accounts put the server in service.
         _application.Database.ExecuteSQL(RecordsSql);
         // PerformBasicSetup leaves the one domain; the sample domain's are not counted.
         int existing = _domain.Accounts.Count;
         for (int i = existing; i < 10; i++)
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "account" + i + "@" + _domain.Name, AccountPassword);
         item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("profile_in_service", item.GetProperty("findings")[0].GetProperty("code").GetString(), item.ToString());
         ClassicAssert.AreEqual("not_needed", item.GetProperty("state").GetString());
         ClassicAssert.AreEqual("10", item.GetProperty("findings")[0].GetProperty("args")[0].GetString());
      }

      [Test]
      [Description("The changed-from-stock document names the applied profile as the source of each value its apply wrote, and the profile itself - and not of a value the apply found already there. Negative control: after the revert no value names a profile.")]
      public void TheChangedDocumentNamesTheProfileAsTheSource()
      {
         bool spfBefore = _settings.AntiSpam.UseSPF;
         putBack_.Add(() => _settings.AntiSpam.UseSPF = spfBefore);
         bool mxBefore = _settings.AntiSpam.UseMXChecks;
         putBack_.Add(() => _settings.AntiSpam.UseMXChecks = mxBefore);
         _settings.AntiSpam.UseSPF = false;
         _settings.AntiSpam.UseMXChecks = true;

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         JsonElement changed = Json(Admin("GET", "/api/v1/settings/changed", null), 200);
         JsonElement spf = changed.GetProperty("settings").EnumerateArray().First(e => e.GetProperty("name").GetString() == "usespf");
         ClassicAssert.AreEqual("office", spf.GetProperty("profile").GetString(), spf.ToString());
         JsonElement mx = changed.GetProperty("settings").EnumerateArray().First(e => e.GetProperty("name").GetString() == "usemxchecks");
         ClassicAssert.AreEqual(JsonValueKind.Null, mx.GetProperty("profile").ValueKind, "A value the apply found already there is credited to it: " + mx);
         ClassicAssert.AreEqual("office", changed.GetProperty("profile").GetProperty("name").GetString());
         ClassicAssert.AreEqual("applied", changed.GetProperty("profile").GetProperty("state").GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         changed = Json(Admin("GET", "/api/v1/settings/changed", null), 200);
         ClassicAssert.IsFalse(changed.GetProperty("settings").EnumerateArray().Any(e => e.GetProperty("profile").ValueKind == JsonValueKind.String),
            "A reverted profile is still named as a source.");
      }

      // ---- the generator ----------------------------------------------------------

      [Test]
      [Description("build/generate-profiles.py --check passes on the tree, and refuses, on a copy of the profiles, each thing a profile may not set or say: a port, a directory, a database value, a secret, a record the server keeps, a name that is no setting, a value outside its row's range, a reason of two sentences, a caution not earned, a filter turned off without its caution, an address the server binds to, and a check weakened that the summary does not name. Negative control: the unchanged copy passes.")]
      public void TheGeneratorRefusesWhatAProfileMayNotSet()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH, so the generator cannot run here.");

         string generator = RepositoryFile(Path.Combine("build", "generate-profiles.py"));
         (int code, string output) tree = RunPython(python, "\"" + generator + "\" --check");
         ClassicAssert.AreEqual(0, tree.code, tree.output);
         StringAssert.Contains("4 profiles", tree.output);

         string source = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(generator)),
            "hmailserver", "source", "Server", "Common", "Application", "Profiles");

         // Each added to a profile that does not already name the setting, so that it
         // is refused for what it is and not as a duplicate.
         var refusals = new List<(string profile, string entry, string refusal)>
         {
            ("office", "{ \"store\": \"ini\", \"name\": \"RestApiPort\", \"value\": 8080, \"why\": \"a port\" }", "is a port"),
            ("office", "{ \"store\": \"ini\", \"name\": \"ArchiveDir\", \"value\": \"C:\\\\archive\", \"why\": \"a directory\" }", "a directory"),
            ("office", "{ \"store\": \"ini\", \"name\": \"DatabaseStatementTimeout\", \"value\": 5, \"why\": \"a database value\" }", "about the database connection"),
            ("office", "{ \"store\": \"ini\", \"name\": \"BATVSecret\", \"value\": \"x\", \"why\": \"a secret\" }", "holds a secret"),
            ("office", "{ \"store\": \"database\", \"name\": \"ProfileSnapshot\", \"value\": \"x\", \"why\": \"a record\" }", "record the server keeps"),
            ("office", "{ \"store\": \"database\", \"name\": \"NoSuchSettingAtAll\", \"value\": 1, \"why\": \"not a setting\" }", "not one of the hm_settings properties"),
            ("office", "{ \"store\": \"database\", \"name\": \"BounceHardBounceThreshold\", \"value\": 0, \"why\": \"out of range\" }", "outside 1 to 100"),
            ("office", "{ \"store\": \"database\", \"name\": \"OutboundBackoffMinutes\", \"value\": 3, \"why\": \"one sentence. And another\" }", "one sentence"),
            ("office", "{ \"store\": \"database\", \"name\": \"OutboundBackoffMinutes\", \"value\": 3, \"caution\": \"turns_filter_off\", \"why\": \"a caution not earned\" }", "does not earn"),
            ("office", "{ \"store\": \"database\", \"name\": \"ascheckptr\", \"value\": 0, \"why\": \"a filter off with no caution\" }", "turns_filter_off"),
            ("office", "{ \"store\": \"ini\", \"name\": \"RestApiBindAddress\", \"value\": \"0.0.0.0\", \"why\": \"a bind address\" }", "binds to"),
            ("office", "{ \"store\": \"database\", \"name\": \"ascheckptr\", \"value\": 0, \"caution\": \"turns_filter_off\", \"why\": \"a check weakened the summary does not name\" }", "summary does not say so"),
         };

         foreach ((string profile, string entry, string refusal) in refusals.Prepend(("office", (string)null, (string)null)))
         {
            string copy = Path.Combine(Path.GetTempPath(), "hm-profiles-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(copy);
            try
            {
               foreach (string file in Directory.GetFiles(source, "*.json"))
                  File.Copy(file, Path.Combine(copy, Path.GetFileName(file)));

               if (entry != null)
               {
                  string path = Path.Combine(copy, profile + ".json");
                  string text = File.ReadAllText(path);
                  int entries = text.IndexOf("\"entries\": [", StringComparison.Ordinal);
                  ClassicAssert.Greater(entries, 0, "The profile has no entries list to add to.");
                  int at = entries + "\"entries\": [".Length;
                  File.WriteAllText(path, text.Substring(0, at) + "\n    " + entry + "," + text.Substring(at), new UTF8Encoding(false));
               }

               (int code, string output) run = RunPython(python, "\"" + generator + "\" --check --profiles \"" + copy + "\"");
               if (entry == null)
               {
                  ClassicAssert.AreEqual(0, run.code, "The unchanged copy is refused: " + run.output);
                  continue;
               }
               ClassicAssert.AreEqual(1, run.code, entry + ": " + run.output);
               StringAssert.Contains(refusal, run.output, entry);
            }
            finally
            {
               Directory.Delete(copy, true);
            }
         }
      }

      // ---- helpers ----------------------------------------------------------------

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      /// <summary>
      ///    hMailServer.exe with these arguments, beside the service it administers,
      ///    with exactly these HM_ variables in its environment and none of the
      ///    runner's. One that has not finished in three minutes is stopped.
      /// </summary>
      private Verb RunVerb(string arguments, Dictionary<string, string> variables)
      {
         string executable = ServerExecutable();

         var start = new ProcessStartInfo(executable, arguments)
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

               Assert.Fail(arguments + " did not finish within three minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      private string executable_;

      // Asked of the server once, over COM, and kept: a verb run with the service
      // stopped has no server to ask.
      private string ServerExecutable()
      {
         if (executable_ != null)
            return executable_;

         string program = _settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return executable_ = candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      /// <summary>The service stopped, and its process gone, as TestSetup's restart stops it.</summary>
      private static void StopService()
      {
         using (Process process = Process.Start(new ProcessStartInfo("net", "stop hMailServer")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         }))
         {
            process.StandardOutput.ReadToEnd();
            process.StandardError.ReadToEnd();
            process.WaitForExit(90000);
         }

         DateTime deadline = DateTime.UtcNow.AddSeconds(60);
         while (DateTime.UtcNow < deadline && Process.GetProcessesByName("hmailserver").Length > 0)
            Thread.Sleep(250);

         ClassicAssert.IsFalse(ServiceRunning(), "The hMailServer service did not stop.");
      }

      private static bool ServiceRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status != System.ServiceProcess.ServiceControllerStatus.Stopped;
         }
      }

      /// <summary>
      ///    For a profile kept rather than reverted: each value it set put back - a
      ///    [Settings] key stored or removed with the text it had, a property to the
      ///    value the changed document held before the test - through the ini route or
      ///    the settings group the diff names, which is what the way back would have
      ///    done, done by hand. Each setting on its own, so that one that cannot be put
      ///    back does not stop the rest.
      /// </summary>
      private void RestoreSettingsTheProfileSet(string name, Dictionary<string, (bool present, string value)> iniBefore)
      {
         Dictionary<string, JsonElement> before = new Dictionary<string, JsonElement>(StringComparer.OrdinalIgnoreCase);
         foreach (JsonElement entry in JsonDocument.Parse(settingsBefore_).RootElement.EnumerateArray())
            before[entry.GetProperty("name").GetString()] = entry.Clone();

         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/" + name, null), 200);
         foreach (JsonElement entry in diff.GetProperty("entries").EnumerateArray())
         {
            JsonElement current = entry.Clone();
            Guarded(() => RestoreOne(current, before, iniBefore));
         }
      }

      private void RestoreOne(JsonElement entry, Dictionary<string, JsonElement> before, Dictionary<string, (bool present, string value)> iniBefore)
      {
         string setting = entry.GetProperty("name").GetString();
         string route = entry.GetProperty("route").GetString();
         bool listed = before.TryGetValue(setting, out JsonElement old);

         if (entry.GetProperty("store").GetString() == "ini" && iniBefore.TryGetValue(setting, out (bool present, string value) stored))
         {
            if (stored.present)
               _settings.SetIniSetting(setting, stored.value);
            else
               _settings.DeleteIniSetting(setting);
            return;
         }

         // A property: the value it had, or its shipped default when the document
         // did not list it, written as the row's own type.
         string value = listed ? old.GetProperty("value").GetString() : _settings.GetSettingDefault(setting);
         string[] parts = route.Substring("PUT ".Length).Split(' ');
         JsonElement group = Json(Admin("GET", parts[0], null), 200);
         JsonElement kind = group.GetProperty(parts[1]);
         string body;
         switch (kind.ValueKind)
         {
            case JsonValueKind.True:
            case JsonValueKind.False:
               body = "{\"" + parts[1] + "\":" + (value != "0" ? "true" : "false") + "}";
               break;
            case JsonValueKind.Number:
               body = "{\"" + parts[1] + "\":" + value + "}";
               break;
            default:
               // The office profile sets no text and no row of words; a profile that
               // did would need its value in the row's own form here.
               Assert.Fail(setting + " is kept by a row this helper does not put back: " + kind.ValueKind);
               return;
         }

         (int status, string answer) put = Admin("PUT", parts[0], body);
         ClassicAssert.AreEqual(200, put.status, setting + " could not be put back: " + put.answer);
      }

      private bool SnapshotHeld()
      {
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         return list.GetProperty("snapshot").ValueKind == JsonValueKind.Object;
      }

      private string DiffToken(string profile)
         => Json(Admin("GET", "/api/v1/settings/profiles/" + profile, null), 200).GetProperty("diff_token").GetString();

      /// <summary>A confirmation of the diff the server answers now, as a page that showed it sends one.</summary>
      private string ConfirmBody(string profile)
         => "{\"confirm\":true,\"diff_token\":\"" + DiffToken(profile) + "\"}";

      /// <summary>A [Settings] key as the store holds it: whether it is stored, and its text.</summary>
      private (bool present, string value) IniKey(string key)
      {
         JsonElement ini = Json(Admin("GET", "/api/v1/settings/ini/" + key, null), 200);
         return (ini.GetProperty("present").GetBoolean(), ini.GetProperty("value").GetString());
      }

      private void PutIniKeyBack(string key, (bool present, string value) before)
      {
         if (before.present)
            _settings.SetIniSetting(key, before.value);
         else
            _settings.DeleteIniSetting(key);
         _application.Reinitialize();
      }

      private Dictionary<string, (bool present, string value)> IniKeysOf(string profile)
      {
         var keys = new Dictionary<string, (bool present, string value)>(StringComparer.OrdinalIgnoreCase);
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/" + profile, null), 200);
         foreach (JsonElement entry in diff.GetProperty("entries").EnumerateArray())
         {
            if (entry.GetProperty("store").GetString() == "ini")
               keys[entry.GetProperty("name").GetString()] = IniKey(entry.GetProperty("name").GetString());
         }
         return keys;
      }

      private bool DirectoryAnswered(string address)
      {
         (int status, string body) answer = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/me/directory", Basic(address, AccountPassword), null);
         return Json(answer, 200).GetProperty("enabled").GetBoolean();
      }

      private bool QuarantineEnabled(string address)
      {
         (int status, string body) answer = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/me/quarantine", Basic(address, AccountPassword), null);
         return Json(answer, 200).GetProperty("enabled").GetBoolean();
      }

      private JsonElement TraceSource()
         => Json(Admin("GET", "/api/v1/reports", null), 200).GetProperty("sources").GetProperty("message_trace");

      /// <summary>The changed-from-stock document's settings, as text - what "every value" means here.</summary>
      private string ChangedSettings()
         => Json(Admin("GET", "/api/v1/settings/changed", null), 200).GetProperty("settings").GetRawText();

      private Dictionary<string, string> ChangedValues()
      {
         var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         foreach (JsonElement entry in Json(Admin("GET", "/api/v1/settings/changed", null), 200).GetProperty("settings").EnumerateArray())
            values[entry.GetProperty("name").GetString()] = entry.GetProperty("value").GetString();
         return values;
      }

      private JsonElement Entry(string profile, string name)
      {
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/" + profile, null), 200);
         foreach (JsonElement entry in diff.GetProperty("entries").EnumerateArray())
         {
            if (string.Equals(entry.GetProperty("name").GetString(), name, StringComparison.OrdinalIgnoreCase))
               return entry.Clone();
         }

         Assert.Fail(profile + " has no entry " + name);
         return default;
      }

      private static JsonElement Json((int status, string body) answer, int status)
      {
         ClassicAssert.AreEqual(status, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("RestApiProfiles teardown: " + e.Message);
         }
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      private static (int code, string output) RunPython(string python, string arguments)
      {
         var start = new ProcessStartInfo(python, arguments)
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
               Assert.Fail("The generator did not finish within two minutes, and was stopped.");
            }
            process.WaitForExit();
            return (process.ExitCode, output.Result + error.Result);
         }
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
