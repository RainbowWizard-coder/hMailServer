// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   /// <summary>
   /// Every setting from the environment (docs/EnvironmentConfiguration.md).
   ///
   /// ONE RULE, NO TABLE. HM_SETTING_&lt;NAME&gt; is the [Settings] key NAME - one of
   /// the keys build/ini-settings-baseline.txt lists, matched without case and
   /// stored in the baseline's spelling - and HM_PROPERTY_&lt;NAME&gt; is the
   /// hm_settings row NAME - one SettingsDefaults.h lists and this build reads.
   /// Either with _FILE after it names a file to read the value from, which is how
   /// a Kubernetes Secret mounted as a file supplies one; the file's trailing line
   /// breaks are stripped. A name that is not one of those is refused with the
   /// nearest one named; a prefix that looks like one of the two and is not
   /// (HM_SETTINGS_, HM_PROPERTIES_, a prefix in small letters) is refused naming
   /// the right one; a plain form and a _FILE form both set, or two variables
   /// naming the same setting, are refused as well. The four REST variables the
   /// container has always taken - HM_REST_PORT, HM_REST_BIND_ADDRESS,
   /// HM_REST_CERTIFICATE_FILE and HM_REST_PRIVATE_KEY_FILE - are aliases of
   /// HM_SETTING_RestApiPort and the other three (the last two are paths, not _FILE
   /// forms), and keep meaning what they meant, an empty one included: unset.
   ///
   /// THE VERB. `hmailserver --apply-environment` (and hMailServer.exe
   /// --apply-environment on Windows) reads the process's environment, opens the
   /// configuration and the database as the service does, checks every value
   /// FIRST - against the setting's kind, the listener checks --set-setting makes,
   /// and the check the setting's REST settings row makes, with that row's sentence
   /// (a sentence that could quote a secret is replaced by one that does not) - and
   /// only when every one passes writes them: through the REST row's setter where a
   /// row lends one (RestApiSettingsRows), so a value is stored as REST stores it,
   /// else a [Settings] key through IniFileSettings::WriteSettingsValue, the locked
   /// write path every save takes, and a property through the property set. One
   /// line per variable: set, unchanged, or refused and why. A refusal changes
   /// nothing and exits 1, so a container does not start on a setting it was not
   /// given.
   ///
   /// THE ENVIRONMENT WINS, EVERY START. A value the environment sets is written at
   /// every run of the verb, which the container's entrypoint makes at every start.
   /// So the server records which names the environment manages - the hm_settings
   /// row EnvironmentManagedSettings, the comma-separated variables of the last
   /// run, rewritten by every run and so cleared of a variable no longer set - and
   /// refuses an administrator's change to one of them: over REST with 409, over
   /// COM with the same sentence, and so in the Control Panel and the Deck, which
   /// show the setting as managed. The edit would last only until the next start;
   /// refusing it is what keeps it from being accepted and lost.
   ///
   /// A SECRET STAYS ONE. A value a _FILE supplied is recorded as such - the row
   /// EnvironmentSecretSettings, names only - and stays withheld after the variable
   /// is gone, until an administrator writes another value: never printed by the
   /// verb, answered by the settings routes and the changed document as (set) or
   /// null, and recorded by the audit trail as (changed). A setting whose name says
   /// it is a secret is treated the same way wherever this row prints or answers.
   /// </summary>
   class EnvironmentSettings
   {
   public:

      enum Store
      {
         StoreIni,        // a [Settings] key, hm_inisettings
         StoreDatabase    // an hm_settings row
      };

      enum Source
      {
         SourceVariable,  // the variable's own value
         SourceFile       // the file a _FILE variable names
      };

      /// <summary>One variable of the process's environment.</summary>
      struct Variable
      {
         String name;
         String value;
      };

      /// <summary>
      /// One name the environment manages, as the marker records it: the variable
      /// that set it, which store, the setting's own spelling and where the value
      /// came from. Never the value.
      /// </summary>
      struct Entry
      {
         Entry() : store(StoreIni), source(SourceVariable) {}

         String variable;
         Store store;
         String name;
         Source source;
      };

      /// <summary>
      /// What an administrator's change to a managed setting throws, from the two
      /// store chokepoints (PropertySet's setters and IniSettingStore's writes)
      /// when an audit actor is installed on the thread - which every COM setter
      /// and every REST request installs, and --apply-environment does not. COM's
      /// catch reports the sentence (COMError::GenerateGenericMessage); the REST
      /// dispatch answers it with 409. Deliberately not a std::exception, so that
      /// nothing written for those catches it by accident on the way out.
      /// IniSettingStore's writes throw it for one more refusal of the same kind,
      /// with its own sentence: a change to the live update's trust, which is read
      /// from hMailServer.INI alone (IniSettingStore::IsTrustAnchorName).
      /// </summary>
      class Refusal
      {
      public:
         explicit Refusal(const String &sentence) : sentence_(sentence) {}
         const String &Sentence() const { return sentence_; }
      private:
         String sentence_;
      };

      /// <summary>
      /// The whole verb: the process's environment read, the configuration and the
      /// database opened as the service opens them, every variable checked, then
      /// written and the records rewritten, the lines printed. 0 when everything was
      /// set or unchanged (and when nothing was named), 1 on any refusal or when the
      /// database could not be opened, 2 when the configuration names no database.
      /// </summary>
      static int RunCommand();

      /// <summary>
      /// The variables of this process's environment, in the order the platform
      /// hands them over (GetEnvironmentStringsW on Windows, environ elsewhere).
      /// </summary>
      static void ReadProcessEnvironment(std::vector<Variable> &variables);

      /// <summary>
      /// What the marker holds, resolved: every variable the last --apply-environment
      /// wrote, with its store, its setting and its source. Read from the property set
      /// the server loaded at its start or last reinitialise - the verb run against a
      /// running server reaches it at its next reinitialise or start, as the values
      /// do. An entry this build cannot resolve (a key a later build renamed) is left
      /// out and reported once per load (HM7290).
      /// </summary>
      static void ReadManaged(std::vector<Entry> &entries);

      /// <summary>
      /// The settings whose value came from a file of the environment and has not
      /// been rewritten since, by anybody but the environment itself: the record
      /// EnvironmentSecretSettings. Each entry's source is SourceFile.
      /// </summary>
      static void ReadSecretOrigins(std::vector<Entry> &entries);

      /// <summary>Whether one setting's value came from a file, and is still withheld.</summary>
      static bool IsSecretOrigin(Store store, const String &name);

      /// <summary>
      /// Called by the store chokepoints after an administrator wrote a new value to a
      /// setting whose value came from a file: the new value is the administrator's,
      /// not the file's, and is shown as any other is. Writes the record without an
      /// audit row of its own and without being refused.
      /// </summary>
      static void ForgetSecretOrigin(Store store, const String &name);

      /// <summary>
      /// One line in the application log at every start and reinitialise naming the
      /// settings the environment sets, which no interface will change; nothing when
      /// there are none.
      /// </summary>
      static void AnnounceManaged();

      /// <summary>Whether the environment manages one setting, and the entry when it does.</summary>
      static bool IsManaged(Store store, const String &name, Entry &entry);

      /// <summary>
      /// Whether the value of one setting is withheld from every answer: a file of
      /// the environment supplied it, now or before, and nobody has written another.
      /// </summary>
      static bool IsWithheld(Store store, const String &name);

      /// <summary>
      /// The same, read straight from the database rather than from a loaded
      /// configuration: for hmailserver --set-setting, which opens the database and
      /// loads nothing else, to say that a key it has just stored is one the next
      /// --apply-environment writes back.
      /// </summary>
      static bool IsManagedInDatabase(Store store, const String &name, Entry &entry);

      /// <summary>
      /// The one sentence every refusal says, naming what was asked for (a REST key,
      /// a [Settings] key, a property) and the variable - never the value.
      /// </summary>
      static String RefusalSentence(const String &what, const Entry &entry);

      /// <summary>
      /// The chokepoints' check. Throws Refusal when an audit actor is installed on
      /// this thread (an administrator's change through COM or REST) and the setting
      /// is managed. The caller calls it only for a change that moves the value: a
      /// write of the value already held changes nothing and is let through.
      /// </summary>
      static void RefuseAdministratorChange(Store store, const String &name);

      /// <summary>"variable" or "file", as REST and the changed document answer it.</summary>
      static const char *SourceWord(Source source);

      /// <summary>
      /// The hm_settings row the marker is kept in. Not a setting - it has no
      /// default, is in no create script, is not in SettingsDefaults.h and so in no
      /// "changed from stock" list, cannot be named by HM_PROPERTY_, and is left
      /// out of the XML backup, because it describes the deployment a database is
      /// running under and not the configuration a restore should carry.
      /// </summary>
      static const TCHAR *MarkerName();

      /// <summary>The hm_settings row the settings whose value came from a file are kept in; as MarkerName.</summary>
      static const TCHAR *SecretMarkerName();

      /// <summary>
      /// The file --apply-environment keeps in the data directory while the
      /// environment manages anything - so that the container's entrypoint, which
      /// otherwise runs the verb only when a variable names a setting, runs it on a
      /// start without them and the settings are given back.
      /// </summary>
      static String MarkerFile();

      /// <summary>
      /// The sentence a value the settings store cannot hold is refused with, by the
      /// REST ini route, --set-setting and --apply-environment alike.
      /// </summary>
      static const char *NotStorableSentence();

      /// <summary>
      /// A [Settings] key's value checked as --set-setting and --apply-environment
      /// both check one: storable at all, a whole number where the server reads one,
      /// a port a port, RestApiBindAddress an address HttpServer::Listen takes.
      /// Empty when the value is acceptable; the sentence never repeats the value.
      /// </summary>
      static String CheckIniValue(const String &name, const String &value);

      /// <summary>
      /// Resolves the variables that name settings, without any database: the
      /// prefixes and the aliases, _FILE, the names against the baseline and
      /// SettingsDefaults.h, the files read. Public for the internal tester.
      /// </summary>
      struct Request
      {
         Request() : store(StoreIni), source(SourceVariable), legacy(false), number(0), isNumber(false), lent(false) {}

         String variable;     // as found in the environment
         String canonical;    // as the marker records it: the setting's spelling, or the alias
         Store store;
         String name;         // the setting's own spelling; empty when unresolved
         Source source;
         bool legacy;
         String value;        // the value to store - never printed
         String problem;      // why it is refused; empty when it is not
         long number;         // the value as a number, where the setting takes one
         bool isNumber;
         bool lent;           // written through its REST row's setter
      };

      static void Resolve(const std::vector<Variable> &variables, std::vector<Request> &requests);

      /// <summary>The nearest of the candidates to a name, compared without case.</summary>
      static String Nearest(const String &name, const std::vector<String> &candidates);

   private:

      static bool ResolveVariableName_(const String &variable, Request &request, String &base);
      static bool MisspeltPrefix_(const String &variable, String &problem);
      static void ResolveSettingName_(const String &base, Request &request);
      static bool ReadValueFile_(const String &path, String &value, String &problem);
      static void ParseMarker_(const String &marker, std::vector<Entry> &entries, std::vector<String> &unresolved);
      static void ReadRecord_(const TCHAR *row, std::vector<Entry> &entries, bool report);
      static bool IsSecret_(const Request &request, const std::vector<Entry> &secrets);
      static String CheckValue_(Request &request, const std::vector<Entry> &secrets);
      static int Apply_(std::vector<Request> &requests);
      static bool WriteRecord_(const TCHAR *row, const String &value, String &problem);
      static void Print_(bool error, const String &line);
   };
}
