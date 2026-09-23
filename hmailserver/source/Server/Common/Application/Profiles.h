// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// Profiles, so a new server starts in the right shape (docs/Profiles.md).
   ///
   /// A PROFILE IS A DOCUMENT IN THE TREE - Common/Application/Profiles/<name>.json,
   /// embedded as ProfilesData by build/generate-profiles.py - not a row in the
   /// database: four ship (office, gateway, relay, sender), each a name, a title, a
   /// paragraph, the steps it cannot take for an administrator, and some thirty
   /// entries of {store, name, value, why}. The generator holds every name to a
   /// setting a profile may set - never a port, a directory, a database value or a
   /// password - and every value to its setting's type.
   ///
   /// A DIFF FIRST. Every entry is answered with the value the server holds now,
   /// whether applying would change it, the route it is written through, when the
   /// change takes effect, and why it is there; an entry the server's environment
   /// manages is answered as skipped, with the variable that sets it; an entry whose
   /// REST settings row would refuse the value is answered as refused, in the row's
   /// sentence.
   ///
   /// THEN A SNAPSHOT, THEN THE WRITES. Applying saves the value each entry it will
   /// change has now - the hm_settings row ProfileSnapshot, one snapshot, the latest -
   /// before it writes anything, and then writes each entry through the setter the
   /// REST settings row for it runs (RestApiSettingsRows, the setter
   /// --apply-environment lends), or, for a [Settings] key no row keeps, through
   /// IniFileSettings::WriteSettingsValue, the locked write every save takes. Never
   /// straight into the property set: a property no row keeps is refused, which the
   /// generator's shipped profiles never name. Reverting writes the snapshot's
   /// values back - each one only while the setting still holds what the profile
   /// wrote, so that a change an administrator made since is never undone - and
   /// clears it. A second apply is refused while a snapshot is held; reverting it or
   /// discarding it (keeping the settings, forgetting the way back) makes room.
   ///
   /// A LISTENER OR A TRUST IS NEVER TAKEN SILENTLY. An entry that closes a
   /// listener, trusts an address range or turns a filter off carries a caution;
   /// where it would change something, the diff says so and the apply is refused
   /// until it is confirmed - confirm on the REST body, the confirmed argument over
   /// COM, --confirm on the command line.
   ///
   /// THE RECORDS. ProfileSnapshot holds the way back; ProfileApplied which profile
   /// was last applied, when, and whether it was then reverted or kept - which is
   /// what makes the container's HM_PROFILE apply once and not at every start
   /// (--apply-profile --once). Neither is a setting: no default, in no create
   /// script, in no changed-from-stock list, not nameable from the environment, and
   /// left out of the XML backup, which describes a configuration and not this
   /// server's history with a profile. Both are made empty at every start
   /// (EnsureRecords) so that no request thread has to insert one.
   /// </summary>
   class Profiles
   {
   public:

      enum Outcome
      {
         OutcomeDone,
         // No profile of that name: 404.
         OutcomeUnknown,
         // The request cannot be done in the state the server is in - a snapshot is
         // held, there is none to revert, an entry needs a confirmation, an entry is
         // refused: 409, with the sentence and what to do.
         OutcomeRefused,
         // Something failed part way: 500. What was written is in the answer.
         OutcomeFailed
      };

      /// <summary>The hm_settings rows the records are kept in.</summary>
      static const TCHAR *SnapshotName();
      static const TCHAR *AppliedName();

      /// <summary>
      /// Makes the two record rows, empty, when the database has none. Called from
      /// Configuration::Load on the one thread running then, as EnsureSettingDefaults_
      /// is, so that every later write of them is an update and no request thread
      /// inserts into the property set.
      /// </summary>
      static void EnsureRecords(PropertySet &properties);

      /// <summary>
      /// GET /api/v1/settings/profiles and Settings.GetProfiles: every profile with its
      /// title, paragraph and entry count, how many entries would change and how many
      /// of those need a confirmation, and the two records.
      /// </summary>
      static AnsiString ListJson();

      /// <summary>
      /// GET /api/v1/settings/profiles/{name} and Settings.GetProfile: the diff. False
      /// for a name that is no profile.
      /// </summary>
      static bool DiffJson(const String &name, AnsiString &json);

      /// <summary>
      /// POST /api/v1/settings/profiles/{name}/apply, Settings.ApplyProfile and
      /// --apply-profile. json is the answer (or, when refused, the detail beside the
      /// sentence); sentence says why when it is not done.
      /// </summary>
      static Outcome Apply(const String &name, bool confirmed, AnsiString &json, String &sentence);

      /// <summary>POST /api/v1/settings/profiles/revert and Settings.RevertProfile.</summary>
      static Outcome Revert(AnsiString &json, String &sentence);

      /// <summary>
      /// POST /api/v1/settings/profiles/discard and Settings.DiscardProfileSnapshot:
      /// the settings are kept and the way back forgotten.
      /// </summary>
      static Outcome Discard(AnsiString &json, String &sentence);

      /// <summary>
      /// The profile whose settings are in effect - applied and not reverted - or
      /// empty. Settings.AppliedProfile over COM.
      /// </summary>
      static String AppliedProfile();

      /// <summary>
      /// What the record says of the last profile applied: its name (empty when none
      /// has been), when, and its state - applied, reverted or kept.
      /// </summary>
      static void ReadApplied(String &profile, __int64 &applied, String &state, __int64 &changed);

      /// <summary>
      /// The applied profile, when it set this setting and the setting still holds the
      /// value it wrote: what the changed-from-stock document names as the source.
      /// </summary>
      static bool SourceOf(const String &store, const String &name, const String &value, String &profile);

      /// <summary>
      /// hmailserver --apply-profile NAME [--confirm] [--once], and hMailServer.exe the
      /// same on Windows: the configuration and the database opened as the service opens
      /// them, the profile applied with its lines printed, and the records written. 0
      /// applied (or, with --once, a profile applied before); 1 refused or failed; 2 no
      /// such profile, or no database; 3 an entry needs --confirm.
      /// </summary>
      static int RunCommand(const String &name, bool confirmed, bool once);

   private:

      static void Print_(bool error, const String &line);
   };
}
