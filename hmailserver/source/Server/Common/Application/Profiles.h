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
   /// setting a profile may set - never a port, a directory, an address the server
   /// binds to or calls, a command, a database value or a password - every value to
   /// its setting's type, every caution to the entry that earns it, and every
   /// summary to naming what its profile takes away.
   ///
   /// A DIFF FIRST. Every entry is answered with the value the server holds now,
   /// whether applying would change it, the route it is written through, when the
   /// change takes effect, and why it is there; an entry the server's environment
   /// manages is answered as skipped, with the variable that sets it; an entry whose
   /// REST settings row would refuse the value is answered as refused, in the row's
   /// sentence. The diff carries a token of what it showed (diff_token): an apply
   /// that brings one is refused when the server no longer answers the same diff.
   ///
   /// THEN A SNAPSHOT, THEN THE WRITES. Applying saves the value each entry it will
   /// change has now - the hm_settings row ProfileSnapshot, one snapshot, the latest -
   /// before it writes anything, and refuses to write anything when the database did
   /// not take it; then it writes each entry through the setter the REST settings
   /// row for it runs (RestApiSettingsRows, the setter --apply-environment lends),
   /// or, for a [Settings] key no row keeps, through
   /// IniFileSettings::WriteSettingsValue, the locked write every save takes. Never
   /// straight into the property set: a property no row keeps is refused, which the
   /// generator's shipped profiles never name. Reverting writes the snapshot's
   /// values back - each one only while the setting still holds what the profile
   /// wrote, so that a change an administrator made since is never undone - and
   /// clears it. A second apply is refused while a snapshot is held; reverting it or
   /// discarding it (keeping the settings, forgetting the way back) makes room.
   ///
   /// A LISTENER, A TRUST OR DATA IS NEVER TAKEN SILENTLY. An entry that closes a
   /// listener, trusts an address range, turns a filter or a check off or weakens
   /// it carries a caution, and so does a window of days; where such an entry would
   /// change something - for a window, where it would shorten the one in force - the
   /// diff says so and the apply is refused until it is confirmed: confirm with the
   /// diff's token on the REST body, the confirmed argument and the token over COM,
   /// --confirm on the command line.
   ///
   /// THE RECORDS. ProfileSnapshot holds the way back; ProfileApplied which profile
   /// was last applied, when, which settings it wrote, and whether it was then
   /// reverted, kept, or failed part way - or that no profile is wanted (declined).
   /// That is what makes the container's HM_PROFILE apply once and not at every start
   /// (--apply-profile --once). Both are read from the database whenever they decide
   /// anything, not from the copy this process loaded, so that a record the command
   /// line wrote beside a running server is the one the server reads. Neither is a
   /// setting: no default, in no create script, in no changed-from-stock list, not
   /// nameable from the environment, and left out of the XML backup, which describes
   /// a configuration and not this server's history with a profile. Both are created,
   /// empty, at start-up where they are missing (EnsureRecords), so that no request
   /// thread has to insert one; an existing record is never emptied there.
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
         // refused, the diff changed since it was read: 409, with the sentence and
         // what to do.
         OutcomeRefused,
         // Something failed part way: 500. What was written is in the answer.
         OutcomeFailed
      };

      /// <summary>What the record ProfileApplied says, read from the database.</summary>
      struct Applied
      {
         Applied() : applied(0), changed(0), readable(true) {}

         // Empty when no profile has been applied - or when none is wanted, which
         // state then says (declined).
         String profile;
         // applied, reverted, kept, failed or declined; empty when there is no record.
         String state;
         __int64 applied;
         __int64 changed;
         // The settings the apply wrote, "i:<key>" and "d:<property>": what the
         // changed-from-stock document names the profile as the source of.
         std::vector<String> set;
         // False when the record could not be read, or not parsed: something is
         // recorded, and nothing may take it for none.
         bool readable;

         // Whether a record says anything at all, a profile or its refusal.
         bool Recorded() const { return !profile.IsEmpty() || state == _T("declined"); }
      };

      /// <summary>The hm_settings rows the records are kept in.</summary>
      static const TCHAR *SnapshotName();
      static const TCHAR *AppliedName();

      /// <summary>
      /// Makes the two record rows, empty, when the database has none. Called from
      /// Configuration::Load on the one thread running then, as EnsureSettingDefaults_
      /// is, so that every later write of them is an update and no request thread
      /// inserts into the property set. A row that is there is left as it is.
      /// </summary>
      static void EnsureRecords(PropertySet &properties);

      /// <summary>
      /// GET /api/v1/settings/profiles and Settings.GetProfiles: every profile with its
      /// title, paragraph and entry count, how many entries would change and how many
      /// of those need a confirmation, and the two records.
      /// </summary>
      static AnsiString ListJson();

      /// <summary>
      /// GET /api/v1/settings/profiles/{name} and Settings.GetProfile: the diff, with
      /// its token. False for a name that is no profile.
      /// </summary>
      static bool DiffJson(const String &name, AnsiString &json);

      /// <summary>
      /// POST /api/v1/settings/profiles/{name}/apply, Settings.ApplyProfile and
      /// --apply-profile. token, when not empty, is the diff_token of the diff the
      /// caller showed, and an apply whose diff the server no longer answers is
      /// refused. json is the answer (or, when refused, the detail beside the
      /// sentence); sentence says why when it is not done.
      /// </summary>
      static Outcome Apply(const String &name, bool confirmed, const String &token, AnsiString &json, String &sentence);

      /// <summary>POST /api/v1/settings/profiles/revert, Settings.RevertProfile and --revert-profile.</summary>
      static Outcome Revert(AnsiString &json, String &sentence);

      /// <summary>
      /// POST /api/v1/settings/profiles/discard and Settings.DiscardProfileSnapshot:
      /// the settings are kept and the way back forgotten.
      /// </summary>
      static Outcome Discard(AnsiString &json, String &sentence);

      /// <summary>
      /// POST /api/v1/settings/profiles/decline, Settings.DeclineProfiles and
      /// --apply-profile none: records that this server keeps the settings this
      /// version ships with, so that the checklist stops asking for a profile. No
      /// setting is written. Refused while a way back is held or a profile is in
      /// effect; a profile may be applied afterwards all the same.
      /// </summary>
      static Outcome Decline(AnsiString &json, String &sentence);

      /// <summary>
      /// The profile whose settings are in effect - applied and not reverted - or
      /// empty. Settings.AppliedProfile over COM.
      /// </summary>
      static String AppliedProfile();

      /// <summary>The record of the last profile applied, read from the database.</summary>
      static Applied ReadAppliedRecord();

      /// <summary>
      /// What the record says of the last profile applied: its name (empty when none
      /// has been), when, and its state - applied, reverted, kept, failed or
      /// declined.
      /// </summary>
      static void ReadApplied(String &profile, __int64 &applied, String &state, __int64 &changed);

      /// <summary>
      /// The applied profile, when its apply wrote this setting and the setting still
      /// holds the value it wrote: what the changed-from-stock document names as the
      /// source. A value the apply found already there, or that somebody set to the
      /// same value since, is nobody's but its setter's, and a setting the environment
      /// manages is the environment's - the caller leaves those out.
      /// </summary>
      static bool SourceOf(const Applied &record, const String &store, const String &name, const String &value, String &profile);

      /// <summary>
      /// hmailserver --apply-profile NAME [--confirm] [--once] [--if-new], and
      /// hMailServer.exe the same on Windows: the configuration and the database opened
      /// as the service opens them, the profile applied with its lines printed, and the
      /// records written. NAME none records that no profile is wanted. 0 applied (or,
      /// with --once, something recorded before); 1 refused or failed; 2 no such
      /// profile, or no database; 3 an entry needs --confirm; 5 --if-new and the server
      /// is in service. 4, the service is running, is the Windows entry point's, which
      /// asks the service controller before this is called.
      /// </summary>
      static int RunCommand(const String &name, bool confirmed, bool once, bool ifNew);

      /// <summary>hmailserver --revert-profile: the way back, from the command line. 0 put back, 1 refused or failed, 2 no database.</summary>
      static int RunRevertCommand();

   private:

      static void Print_(bool error, const String &line);
      static int OpenForCommand_(const TCHAR *verb);
   };

   /// <summary>
   /// The four verbs called on the thread that is applying or reverting a profile -
   /// as an event script run by one of its writes calls them - each refused at once,
   /// with the sentence, and nothing read or written; and the thread free again
   /// afterwards. No database is reached. Throws std::logic_error with a sentence.
   /// ClassTester runs it inside the Windows service; profiles-tests on Linux.
   /// </summary>
   class ProfilesTester
   {
   public:
      void Test();
   };
}
