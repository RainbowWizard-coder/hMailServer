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

namespace RegressionTests.API
{
   /// <summary>
   ///    Profiles, so a new server starts in the right shape (docs/Profiles.md): the
   ///    four setting profiles the server ships, each one's diff against this server,
   ///    an apply that saves the way back before it writes, the way back itself, and
   ///    the refusals that keep a listener or a trust from being taken silently. Over
   ///    REST (/api/v1/settings/profiles), COM (Settings.GetProfile and the rest) and
   ///    the verb (hMailServer.exe --apply-profile), with the setup checklist's first
   ///    item and the changed-from-stock document's source beside them.
   ///
   ///    Every apply is reverted in the teardown, the profile's two records are
   ///    emptied, and the bench's changed-from-stock document is compared before and
   ///    after each test: the fixture leaves the settings as it found them. Each test
   ///    has its negative control. Ports: the REST listener on 10030 (the row's range
   ///    is 10030-10039).
   /// </summary>
   [TestFixture]
   public class RestApiProfiles : TestFixtureBase
   {
      private static int RestPort = 10030;
      private const string AdminPassword = "testar";

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
      ///    behind, which would refuse every apply of this fixture with 409. This
      ///    runs before any SetUp.
      /// </summary>
      [OneTimeSetUp]
      public void ClearStaleRecords()
      {
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
         }
         ClassicAssert.AreEqual(JsonValueKind.Null, list.GetProperty("snapshot").ValueKind, "No way back is held on a bench no profile was applied to.");

         (int status, string body) missing = Admin("GET", "/api/v1/settings/profiles/nonesuch", null);
         ClassicAssert.AreEqual(404, missing.status, missing.body);
         StringAssert.Contains("no profile named nonesuch", missing.body);
      }

      [Test]
      [Description("Every entry of every shipped profile is written through a REST settings route - a property through the row that keeps it, a [Settings] key through its row or the ini route - and none is refused by its setting. Negative control: the diff of a property names a settings group's route, never the ini route.")]
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
         }
      }

      [Test]
      [Description("The office diff answers every entry with the value this server holds now and whether applying would change it. Negative control: the maximum message size set to the profile's value answers changes false, and set to another answers the value and changes true.")]
      public void TheDiffAnswersTheCurrentValueAndWhetherItWouldChange()
      {
         int sizeBefore = _settings.MaxMessageSize;
         putBack_.Add(() => _settings.MaxMessageSize = sizeBefore);

         _settings.MaxMessageSize = 25600;
         JsonElement entry = Entry("office", "maxmessagesize");
         ClassicAssert.AreEqual("25600", entry.GetProperty("current").GetString());
         ClassicAssert.IsFalse(entry.GetProperty("changes").GetBoolean());
         ClassicAssert.AreEqual("limit", entry.GetProperty("levels").GetString(), "The maximum message size is a limit, and the diff says so.");

         _settings.MaxMessageSize = 20000;
         entry = Entry("office", "maxmessagesize");
         ClassicAssert.AreEqual("20000", entry.GetProperty("current").GetString());
         ClassicAssert.AreEqual("25600", entry.GetProperty("value").GetString());
         ClassicAssert.IsTrue(entry.GetProperty("changes").GetBoolean());

         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/office", null), 200);
         int changing = diff.GetProperty("entries").EnumerateArray().Count(e => e.GetProperty("changes").GetBoolean());
         ClassicAssert.AreEqual(changing, diff.GetProperty("changes").GetInt32(), "changes counts the entries that would change.");
         ClassicAssert.AreEqual(changing, list.GetProperty("profiles")[0].GetProperty("changes").GetInt32(), "The list counts what the diff counts.");
      }

      // ---- apply, and the way back ------------------------------------------------

      [Test]
      [Description("An apply changes exactly the profile's entries, reads each back, is one audit event with the counts, and a second apply is refused while the way back is held. Negative control: a setting the profile does not name is not changed.")]
      public void ApplyChangesExactlyTheEntriesAndIsRecordedOnce()
      {
         Dictionary<string, string> before = ChangedValues();
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/office", null), 200);
         var named = new HashSet<string>(diff.GetProperty("entries").EnumerateArray().Select(e => e.GetProperty("name").GetString()), StringComparer.OrdinalIgnoreCase);
         int changing = diff.GetProperty("changes").GetInt32();
         ClassicAssert.Greater(changing, 0, "The negative control needs the profile to change something on this bench.");

         JsonElement applied = Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
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

         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=applied&limit=5", null), 200);
         JsonElement row = audit.GetProperty("entries")[0];
         ClassicAssert.AreEqual("office", row.GetProperty("object").GetString());
         StringAssert.Contains(named.Count + " entries: " + changing + " set", row.GetProperty("detail").GetString());

         (int status, string body) second = Admin("POST", "/api/v1/settings/profiles/gateway/apply", "{\"confirm\":true}");
         ClassicAssert.AreEqual(409, second.status, second.body);
         StringAssert.Contains("is held as the way back", second.body);
      }

      [Test]
      [Description("The revert puts every value back - the changed-from-stock document the same before the apply and after the revert - and clears the way back; a revert with nothing to revert is refused. Negative control: between the apply and the revert the document differs.")]
      public void TheRevertPutsEveryValueBackAndClearsTheWayBack()
      {
         string before = ChangedSettings();

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         ClassicAssert.AreNotEqual(before, ChangedSettings(), "The negative control: an applied profile changes the document.");
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);

         JsonElement reverted = Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         ClassicAssert.AreEqual(0, reverted.GetProperty("failed").GetInt32(), reverted.ToString());
         ClassicAssert.AreEqual(0, reverted.GetProperty("skipped").GetInt32(), reverted.ToString());

         ClassicAssert.AreEqual(before, ChangedSettings(), "The revert did not put every value back as it was.");
         ClassicAssert.AreEqual("", _settings.AppliedProfile, "A reverted profile is in effect no longer.");
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         ClassicAssert.AreEqual(JsonValueKind.Null, list.GetProperty("snapshot").ValueKind);
         ClassicAssert.AreEqual("reverted", list.GetProperty("applied").GetProperty("state").GetString());

         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=reverted&limit=5", null), 200);
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

      // ---- a listener or a trust is never taken silently ----------------------------

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
         CollectionAssert.Contains(answer.GetProperty("confirmation_needed").EnumerateArray().Select(e => e.GetProperty("name").GetString()).ToList(), "usegreylisting");
         ClassicAssert.IsTrue(_settings.AntiSpam.GreyListingEnabled, "A refused apply changed a setting.");
         ClassicAssert.AreEqual(JsonValueKind.Null, Json(Admin("GET", "/api/v1/settings/profiles", null), 200).GetProperty("snapshot").ValueKind, "A refused apply saved a way back.");

         Json(Admin("POST", "/api/v1/settings/profiles/gateway/apply", "{\"confirm\":true}"), 200);
         ClassicAssert.IsFalse(_settings.AntiSpam.GreyListingEnabled, "The confirmed apply did not turn greylisting off.");
      }

      [Test]
      [Description("The relay closes IMAP and POP3, which asks for a confirmation on a bench where they are on; the unconfirmed apply is refused and changes nothing. Negative control: the diff marks exactly the two listeners, and the confirmation field is a boolean.")]
      public void ClosingAListenerIsNeverDoneSilently()
      {
         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/relay", null), 200);
         List<string> listeners = diff.GetProperty("entries").EnumerateArray()
            .Where(e => e.GetProperty("caution").ValueKind == JsonValueKind.String && e.GetProperty("caution").GetString() == "closes_listener")
            .Select(e => e.GetProperty("name").GetString()).ToList();
         CollectionAssert.AreEquivalent(new[] { "protocolimap", "protocolpop3" }, listeners);

         bool imap = _settings.ServiceIMAP;
         if (!imap)
            Assert.Ignore("IMAP is off on this bench, so closing it asks for nothing here.");

         (int status, string body) refused = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{}");
         ClassicAssert.AreEqual(409, refused.status, refused.body);
         StringAssert.Contains("closes a listener", refused.body);
         ClassicAssert.IsTrue(_settings.ServiceIMAP, "A refused apply closed IMAP.");

         (int status, string body) wrongType = Admin("POST", "/api/v1/settings/profiles/relay/apply", "{\"confirm\":\"yes\"}");
         ClassicAssert.AreEqual(400, wrongType.status, "A confirmation given as a string is not one: " + wrongType.body);
         ClassicAssert.IsTrue(_settings.ServiceIMAP);
      }

      [Test]
      [Description("A setting the server's environment manages is skipped by the apply, with the sentence naming its variable, and the diff says so first. Negative control: the profile's other settings are set.")]
      public void ASettingTheEnvironmentManagesIsSkipped()
      {
         const string key = "AccountLockoutThreshold";
         JsonElement ini = Json(Admin("GET", "/api/v1/settings/ini/" + key, null), 200);
         bool presentBefore = ini.GetProperty("present").GetBoolean();
         string valueBefore = ini.GetProperty("value").GetString();
         putBack_.Add(() =>
         {
            if (presentBefore)
               _settings.SetIniSetting(key, valueBefore);
            else
               _settings.DeleteIniSetting(key);
         });
         bool spfBefore = _settings.AntiSpam.UseSPF;
         putBack_.Add(() => _settings.AntiSpam.UseSPF = spfBefore);
         _settings.AntiSpam.UseSPF = false;

         environmentUsed_ = true;
         Verb run = RunVerb("--apply-environment", new Dictionary<string, string> { { "HM_SETTING_" + key, "3" } });
         ClassicAssert.AreEqual(0, run.ExitCode, run.Output);
         _application.Reinitialize();

         JsonElement entry = Entry("office", key);
         ClassicAssert.AreEqual("HM_SETTING_" + key, entry.GetProperty("environment").GetString(), entry.ToString());

         JsonElement applied = Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         JsonElement skipped = applied.GetProperty("entries").EnumerateArray().First(e => e.GetProperty("name").GetString() == key);
         ClassicAssert.AreEqual("skipped", skipped.GetProperty("result").GetString(), skipped.ToString());
         StringAssert.Contains("HM_SETTING_" + key, skipped.GetProperty("reason").GetString());
         ClassicAssert.AreEqual("3", Json(Admin("GET", "/api/v1/settings/ini/" + key, null), 200).GetProperty("value").GetString(), "The environment's value was overwritten.");
         ClassicAssert.IsTrue(_settings.AntiSpam.UseSPF, "The profile's other settings were not set.");

         // The way back, and the record released, before the teardown's own.
         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         Verb release = RunVerb("--apply-environment", new Dictionary<string, string>());
         ClassicAssert.AreEqual(0, release.ExitCode, release.Output);
         _application.Reinitialize();
      }

      // ---- COM, the verb, the checklist and the changed document -------------------

      [Test]
      [Description("COM answers the documents REST answers, applies, reverts and says which profile is in effect. Negative control: a name that is no profile, a second apply and a revert with nothing to revert raise errors.")]
      public void TheComRoundTrip()
      {
         string overRest = Admin("GET", "/api/v1/settings/profiles/gateway", null).body;
         ClassicAssert.AreEqual(overRest, _settings.GetProfile("gateway"), "COM and REST describe the profile differently.");
         ClassicAssert.AreEqual(4, JsonDocument.Parse(_settings.GetProfiles()).RootElement.GetProperty("profiles").GetArrayLength());

         Assert.Throws<COMException>(() => _settings.GetProfile("nonesuch"));

         ClassicAssert.AreEqual("", _settings.AppliedProfile);
         JsonElement applied = JsonDocument.Parse(_settings.ApplyProfile("office", false)).RootElement;
         ClassicAssert.AreEqual("office", applied.GetProperty("profile").GetString());
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);

         COMException second = Assert.Throws<COMException>(() => _settings.ApplyProfile("office", false));
         StringAssert.Contains("is held as the way back", second.Message);

         _settings.RevertProfile();
         ClassicAssert.AreEqual("", _settings.AppliedProfile);
         Assert.Throws<COMException>(() => _settings.RevertProfile());
         Assert.Throws<COMException>(() => _settings.DiscardProfileSnapshot());

         // The other way out: the settings kept and the way back forgotten.
         _settings.ApplyProfile("office", false);
         _settings.DiscardProfileSnapshot();
         ClassicAssert.AreEqual("office", _settings.AppliedProfile, "A kept profile is still in effect.");
         ClassicAssert.AreEqual("kept", JsonDocument.Parse(_settings.GetProfiles()).RootElement.GetProperty("applied").GetProperty("state").GetString());

         // Kept means no way back: the teardown's revert has nothing to do, so the
         // settings are put back here by applying the way back by hand - the values
         // the changed document held before.
         RestoreSettingsTheProfileSet("office");
      }

      [Test]
      [Description("hMailServer.exe --apply-profile applies the profile once: with --once it applies it where none was applied before and does nothing the second time, and without --once the way back refuses another. Negative control: the relay without --confirm exits 3 and changes nothing.")]
      public void TheVerbAppliesOnceWithTheRecord()
      {
         Verb relay = RunVerb("--apply-profile relay --once", new Dictionary<string, string>());
         ClassicAssert.AreEqual(3, relay.ExitCode, relay.Output);
         StringAssert.Contains("--confirm", relay.Output);
         _application.Reinitialize();
         ClassicAssert.AreEqual("", _settings.AppliedProfile, "An unconfirmed run recorded a profile.");

         Verb first = RunVerb("--apply-profile office --once", new Dictionary<string, string>());
         ClassicAssert.AreEqual(0, first.ExitCode, first.Output);
         StringAssert.Contains("set to", first.Output);
         StringAssert.Contains("The office profile:", first.Output);

         Verb second = RunVerb("--apply-profile office --once", new Dictionary<string, string>());
         ClassicAssert.AreEqual(0, second.ExitCode, second.Output);
         StringAssert.Contains("--once leaves the settings as they are", second.Output);
         StringAssert.DoesNotContain("set to", second.Output);

         Verb third = RunVerb("--apply-profile gateway --confirm", new Dictionary<string, string>());
         ClassicAssert.AreEqual(1, third.ExitCode, third.Output);
         StringAssert.Contains("is held as the way back", third.Output);

         _application.Reinitialize();
         ClassicAssert.AreEqual("office", _settings.AppliedProfile);
         JsonElement audit = Json(Admin("GET", "/api/v1/audit?object_type=profile&action=applied&limit=5", null), 200);
         ClassicAssert.AreEqual("hmailserver --apply-profile", audit.GetProperty("entries")[0].GetProperty("actor").GetString(),
            "The command line's apply is recorded under the command line's name.");
      }

      [Test]
      [Description("The setup checklist's first item asks for a profile on a young server, and says which was applied, and that it was reverted. Negative control: the item's state moves with the record.")]
      public void TheChecklistOffersAProfileFirst()
      {
         JsonElement checklist = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200);
         JsonElement item = checklist.GetProperty("items")[0];
         ClassicAssert.AreEqual("profile", item.GetProperty("key").GetString());
         string code = item.GetProperty("findings")[0].GetProperty("code").GetString();
         CollectionAssert.Contains(new[] { "profile_none", "profile_in_service" }, code);
         if (code == "profile_none")
            ClassicAssert.AreEqual("action_needed", item.GetProperty("state").GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("done", item.GetProperty("state").GetString());
         ClassicAssert.AreEqual("profile_applied", item.GetProperty("findings")[0].GetProperty("code").GetString());
         ClassicAssert.AreEqual("office", item.GetProperty("findings")[0].GetProperty("args")[0].GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         item = Json(Admin("GET", "/api/v1/setup/checklist?dns=0", null), 200).GetProperty("items")[0];
         ClassicAssert.AreEqual("profile_reverted", item.GetProperty("findings")[0].GetProperty("code").GetString());
      }

      [Test]
      [Description("The changed-from-stock document names the applied profile as the source of each value it set, and the profile itself. Negative control: after the revert no value names a profile.")]
      public void TheChangedDocumentNamesTheProfileAsTheSource()
      {
         bool spfBefore = _settings.AntiSpam.UseSPF;
         putBack_.Add(() => _settings.AntiSpam.UseSPF = spfBefore);
         _settings.AntiSpam.UseSPF = false;

         Json(Admin("POST", "/api/v1/settings/profiles/office/apply", "{}"), 200);
         JsonElement changed = Json(Admin("GET", "/api/v1/settings/changed", null), 200);
         JsonElement spf = changed.GetProperty("settings").EnumerateArray().First(e => e.GetProperty("name").GetString() == "usespf");
         ClassicAssert.AreEqual("office", spf.GetProperty("profile").GetString(), spf.ToString());
         ClassicAssert.AreEqual("office", changed.GetProperty("profile").GetProperty("name").GetString());
         ClassicAssert.AreEqual("applied", changed.GetProperty("profile").GetProperty("state").GetString());

         Json(Admin("POST", "/api/v1/settings/profiles/revert", "{}"), 200);
         changed = Json(Admin("GET", "/api/v1/settings/changed", null), 200);
         ClassicAssert.IsFalse(changed.GetProperty("settings").EnumerateArray().Any(e => e.GetProperty("profile").ValueKind == JsonValueKind.String),
            "A reverted profile is still named as a source.");
      }

      // ---- the generator ----------------------------------------------------------

      [Test]
      [Description("build/generate-profiles.py --check passes on the tree, and refuses, on a copy of the profiles, an entry that sets a port, a directory or a name that is no setting, and a filter turned off without its caution.")]
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
         var refusals = new Dictionary<string, string>
         {
            { "{ \"store\": \"ini\", \"name\": \"RestApiPort\", \"value\": 8080, \"why\": \"a port\" }", "is a port" },
            { "{ \"store\": \"ini\", \"name\": \"ArchiveDir\", \"value\": \"C:\\\\archive\", \"why\": \"a directory\" }", "a directory" },
            { "{ \"store\": \"database\", \"name\": \"NoSuchSettingAtAll\", \"value\": 1, \"why\": \"not a setting\" }", "not one of the hm_settings properties" },
            { "{ \"store\": \"database\", \"name\": \"usegreylisting\", \"value\": 0, \"why\": \"a filter off with no caution\" }", "turns_filter_off" },
         };

         foreach (KeyValuePair<string, string> refusal in refusals)
         {
            string copy = Path.Combine(Path.GetTempPath(), "hm-profiles-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(copy);
            try
            {
               foreach (string file in Directory.GetFiles(source, "*.json"))
                  File.Copy(file, Path.Combine(copy, Path.GetFileName(file)));

               string office = Path.Combine(copy, "office.json");
               string text = File.ReadAllText(office);
               int entries = text.IndexOf("\"entries\": [", StringComparison.Ordinal);
               ClassicAssert.Greater(entries, 0, "The office profile has no entries list to add to.");
               int at = entries + "\"entries\": [".Length;
               File.WriteAllText(office, text.Substring(0, at) + "\n    " + refusal.Key + "," + text.Substring(at), new UTF8Encoding(false));

               (int code, string output) refused = RunPython(python, "\"" + generator + "\" --check --profiles \"" + copy + "\"");
               ClassicAssert.AreEqual(1, refused.code, refusal.Key + ": " + refused.output);
               StringAssert.Contains(refusal.Value, refused.output, refusal.Key);
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

      /// <summary>
      ///    For a profile kept rather than reverted: each value it set put back to the
      ///    value the changed document held before the test, through the ini route or
      ///    the settings group the diff names - which is what the way back would have
      ///    done, done by hand.
      /// </summary>
      private void RestoreSettingsTheProfileSet(string name)
      {
         Dictionary<string, JsonElement> before = new Dictionary<string, JsonElement>(StringComparer.OrdinalIgnoreCase);
         foreach (JsonElement entry in JsonDocument.Parse(settingsBefore_).RootElement.EnumerateArray())
            before[entry.GetProperty("name").GetString()] = entry.Clone();

         JsonElement diff = Json(Admin("GET", "/api/v1/settings/profiles/" + name, null), 200);
         foreach (JsonElement entry in diff.GetProperty("entries").EnumerateArray())
         {
            string setting = entry.GetProperty("name").GetString();
            string route = entry.GetProperty("route").GetString();
            bool listed = before.TryGetValue(setting, out JsonElement old);

            if (route.StartsWith("PUT /api/v1/settings/ini/", StringComparison.Ordinal))
            {
               if (listed)
                  _settings.SetIniSetting(setting, old.GetProperty("value").GetString());
               else
                  _settings.DeleteIniSetting(setting);
               continue;
            }

            // A property: the value it had, or its shipped default when the document
            // did not list it, written as the row's own type.
            string value = listed ? old.GetProperty("value").GetString() : _settings.GetSettingDefault(setting);
            string[] parts = route.Substring("PUT ".Length).Split(' ');
            JsonElement group = Json(Admin("GET", parts[0], null), 200);
            JsonElement current = group.GetProperty(parts[1]);
            string body;
            switch (current.ValueKind)
            {
               case JsonValueKind.True:
               case JsonValueKind.False:
                  body = "{\"" + parts[1] + "\":" + (value != "0" ? "true" : "false") + "}";
                  break;
               case JsonValueKind.Number:
                  body = "{\"" + parts[1] + "\":" + value + "}";
                  break;
               default:
                  // The office profile sets no text and no row of words; a profile
                  // that did would need its value in the row's own form here.
                  Assert.Fail(setting + " is kept by a row this helper does not put back: " + current.ValueKind);
                  return;
            }

            (int status, string answer) put = Admin("PUT", parts[0], body);
            ClassicAssert.AreEqual(200, put.status, setting + " could not be put back: " + put.answer);
         }
      }

      private bool SnapshotHeld()
      {
         JsonElement list = Json(Admin("GET", "/api/v1/settings/profiles", null), 200);
         return list.GetProperty("snapshot").ValueKind == JsonValueKind.Object;
      }

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
