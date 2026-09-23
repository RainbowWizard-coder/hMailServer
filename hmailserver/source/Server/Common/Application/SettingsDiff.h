// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// What this server has changed from stock: every setting whose value is not
   /// the one this version ships with, with the shipped value beside it.
   ///
   /// WHY IT EXISTS. It is the first question anybody asks when a server behaves
   /// oddly and the first thing worth putting in a support message, and until now
   /// there was no way to answer it. A server holds 391 settings across its two
   /// stores; an administrator has usually changed twenty. Reading all 391 to find
   /// those twenty is a job nobody does, so instead everybody guesses, and the
   /// difference between two installations - the one that works and the one that
   /// does not - stays invisible.
   ///
   /// THE TWO STORES, which are one answer here.
   ///
   ///   ini       hm_inisettings: the keys that were the [Settings] section of
   ///             hMailServer.ini, whose shipped default is the third argument of
   ///             the read in IniFileSettings and reaches the binary through
   ///             IniSettingsBaseline.h.
   ///   database  hm_settings: the store the COM Settings object has always used,
   ///             whose shipped default is what a database made by this version's
   ///             create script holds, and reaches the binary through
   ///             SettingsDefaults.h.
   ///
   /// Both headers are generated from the source that decides the value, and both
   /// are checked on every pull request (build/check-ini-coverage.py and
   /// build/generate-settings-defaults.py --check), so a default changed in the
   /// code cannot drift away from the list without CI saying so. A list of what
   /// has been changed that compares against last month's defaults is worse than
   /// no list: it is confidently wrong.
   ///
   /// WHAT "DEFAULT" MEANS, and the one case worth stating. It is the value the
   /// server behaves as though the setting has when nothing has been stored for
   /// it - NOT what a fresh installation's files happen to contain. The two part
   /// company for the keys the installer writes: hMailServer.ini ships with lines
   /// the packaging put there (RestApiBindAddress=127.0.0.1, WindowsEventLogLevel=2
   /// and a dozen more), every one of which is adopted into the store at the first
   /// start. Those lines are not changes to anything - they say what the server
   /// would have done anyway - so they do not appear here, which is what keeps this
   /// list short enough to read. A key the installer wrote with a value that is NOT
   /// the built-in one does appear, and should: the installation is different from
   /// stock, whoever made it so.
   ///
   /// A ROW NOTHING READS IS NOT A CHANGE. An installation upgraded from an old
   /// schema holds hm_settings rows no create script ever made - useordb,
   /// usespamhaus, tarpitcount and nine more, inserted by upgrade scripts for
   /// features that have since gone, and relaymode, which a create script still
   /// seeds and nothing reads. They differ from a database made today and they
   /// cannot explain anything a server is doing, so they are left out: a list of
   /// what has been changed that carried a dozen fossils on every upgraded
   /// installation is a list nobody finishes reading. SettingsDefaults::PropertyNames
   /// is what decides, generated from the server's own constants.
   ///
   /// A SECRET IS NAMED AND NOT SHOWN. A setting whose name says its value is a
   /// secret is listed with "(set)" for a value it has and empty for one it has
   /// not, exactly as GET /api/v1/settings/ini lists one, by the same rule -
   /// IniSettingStore::IsSecretName - because a second rule for the same question
   /// is a rule that will one day disagree. The point of the list is that the
   /// setting has been changed; the value is a separate question with a separate
   /// route.
   /// </summary>
   class SettingsDiff
   {
   public:

      struct Entry
      {
         Entry() : secret(false), known(true), overridden(false), environmentFile(false) {}

         /// <summary>"ini" or "database": which store holds it.</summary>
         String store;
         String name;
         /// <summary>
         /// The value STORED for this setting - or forced for it, see overridden -
         /// masked when the name says it is a secret.
         ///
         /// Deliberately not called the effective value. For all but a handful it is
         /// both, but the loader clamps a few: PasswordHashIterations outside the
         /// range this server will derive a hash with is reported as HM5561 and the
         /// built-in work factor used, and PreferredHashAlgorithm that is not a
         /// password-hashing scheme is reported as HM5528 and 4 used. What is listed
         /// there is what somebody set, which is what the list is for - the error log
         /// says what the server did with it.
         /// </summary>
         String value;
         /// <summary>What this version ships with, masked by the same rule.</summary>
         String shipped;
         bool secret;
         /// <summary>
         /// False for a setting this build has no default for, which is a
         /// difference from stock all the same - stock has no such value at all.
         /// Two shapes reach it: a key an administrator stores for a script of
         /// their own, which the API and COM allow on purpose, and the handful the
         /// server reads by name where they are used rather than through the
         /// loader (the webmail's branding and file-link keys and their
         /// ".&lt;domain&gt;" twins).
         /// </summary>
         bool known;
         /// <summary>
         /// True where the [SettingsOverride] section of hMailServer.ini is forcing
         /// this value over whatever is stored. That section is the deliberate door
         /// for a database that is unreachable or holds a value the server will not
         /// start on, and it is invisible to every page and every backup - so a list
         /// of what this server has changed that did not say so would be pointing at
         /// a stored value nobody is running on. A key forced this way need not have
         /// a row or a line of its own, and is listed all the same.
         /// </summary>
         bool overridden;
         /// <summary>
         /// The variable the server's environment sets this setting from, empty when
         /// it does not (docs/EnvironmentConfiguration.md): an environment-managed
         /// value that differs from stock is listed with its source, because the
         /// environment put it there and a change made anywhere else is refused.
         /// environmentFile says the value came from a file - a Secret's - and it is
         /// then shown as (set), as a secret's value is.
         /// </summary>
         String environment;
         bool environmentFile;
      };

      /// <summary>
      /// Every setting whose value is not the shipped one, ini first and each store
      /// by name. Reads the settings this process holds - the overlay IniFileSettings
      /// loaded at start, the [SettingsOverride] keys forced over it, and this
      /// process's own writes, plus the property set - so it answers for the server
      /// as it is RUNNING, which is the question being asked. A value stored by
      /// another process since this one started (hmailserver --set-setting, another
      /// node on the same database) is the value this server last loaded until it
      /// restarts.
      ///
      /// considered is how many settings the two stores have between them, which
      /// is what the number of changed ones is read against: "3" means nothing and
      /// "3 of 391" means something. It is the settings this build KNOWS, plus any
      /// name the server holds that it ships no default for - not the rows it
      /// happens to hold, which would make the denominator smallest on the servers
      /// that have changed least.
      /// </summary>
      static void Read(std::vector<Entry> &entries, int &considered);

      /// <summary>
      /// The same list as one JSON document, which is what both the REST route and
      /// the COM call answer, so that the two cannot describe the server
      /// differently. {"changed": n, "considered": m, "settings": [{store, name,
      /// value, default, secret, known, overridden}]}.
      /// </summary>
      static AnsiString Json();

      /// <summary>
      /// What this version ships with for ONE setting, as text: the [Settings] key
      /// of that name if there is one, otherwise the hm_settings property. False
      /// for a name neither store ships a default for, which is not an error about
      /// the name - the API and COM store any name on purpose - but is all this
      /// can honestly say about it.
      ///
      /// The ini store is asked first because its names are the ones an
      /// administrator types. No name is in both today, and if one ever were the
      /// two stores would be holding the same setting twice, which is a problem to
      /// fix rather than a tie to break here.
      /// </summary>
      static bool ShippedValue(const String &name, String &value);
   };
}
