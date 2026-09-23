// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// XNode is a global-scope TYPEDEF of struct _tagXMLNode, not a class - and not in
// namespace HM. Declaring `class XNode;` inside the namespace makes a distinct
// HM::XNode and changes the signature of every XMLStore/XMLLoad that takes the real
// one; declaring it at global scope makes it a class where it is a typedef. So the
// underlying struct is what gets forward-declared, and the typedef comes with the
// real header where it is needed.
struct _tagXMLNode;
typedef _tagXMLNode XNode;

namespace HM
{
   /// <summary>
   /// hm_inisettings: the store the [Settings] section of hMailServer.INI used to
   /// be. From schema 6042 THE DATABASE IS THE TRUTH and the file is its cache.
   ///
   /// WHY THE STORE MOVED. A setting that lives only in hMailServer.INI has four
   /// problems and they are one problem: the file exists only on the server. It
   /// cannot be read or written by a Control Panel or a Control Deck connected to
   /// another host; two nodes cannot share it, so an active-active pair cannot
   /// share a configuration; a change to it is saved now and applied at the next
   /// start, with no way to publish it to a running server; and - the one that
   /// loses data - it is in no backup at all. BackupExecuter archives the database
   /// and the message store and never touches the ini, so an operator who restores
   /// onto replacement hardware gets their domains, accounts and mail back and
   /// none of their server settings. The project's rule since 15 September 2026,
   /// in .github/CONTRIBUTING.md, is that a setting belongs in the database; this
   /// is the class that makes that true of the 241 keys already in the file.
   ///
   /// WHY THE FILE IS STILL WRITTEN. Because some readers cannot go to the
   /// database, and one of them by definition never can: hMailServer.exe /Register
   /// reads [Settings] ServiceAccountName and ServiceAccountPassword with no
   /// database open at all, because registering the service is what happens before
   /// there is one. So every row is written into the file as well, and the file
   /// remains a complete, readable copy of the configuration - a cache, not a
   /// store. An administrator can still read it; what they can no longer do is
   /// change a setting by editing it.
   ///
   /// THE MERGE, which is no longer three-way. Each row still remembers what the
   /// file held when the two last agreed (inisettingfilevalue), but that is now
   /// used to tell an EDIT from a stale copy rather than to decide who wins:
   ///
   ///   no row                 the file has a key the store has never seen: a
   ///                          fresh install, or the first start after the
   ///                          migration. It is ADOPTED into the table and used.
   ///   file == filevalue      the file is the cache this store last wrote. The
   ///                          row is used; if the file has drifted from it the
   ///                          line is rewritten.
   ///   file != filevalue      somebody EDITED the file. The row is still used -
   ///                          the database is the store - the key is named in the
   ///                          error log, and the line is put back to the stored
   ///                          value so the direct readers stay correct. Reported
   ///                          once, because after that the two agree again.
   ///   row with no key        the line was deleted from the file. That is no
   ///                          longer how a setting is returned to its default -
   ///                          DELETE over COM or REST is - so the line is written
   ///                          back rather than the row dropped.
   ///
   /// THE DOOR. [SettingsOverride] in the same file is applied over everything,
   /// stored or not, and is named in the error log at every start. It exists for
   /// the support engineer whose database is unreachable or whose stored value is
   /// wrong, and it is deliberately a section nobody edits by accident: it is not
   /// written by the server, not offered by any editor, and it announces itself
   /// every time the service starts. See IniFileSettings::LoadDatabaseSettings,
   /// which applies it, because it has to apply even when this table cannot be
   /// read at all.
   ///
   /// filevalue is only advanced when the file write SUCCEEDS. If the service
   /// account cannot write the ini, the row stays marked as un-synchronised and
   /// the next start tries again, rather than recording a lie.
   /// </summary>
   class IniSettingStore
   {
   public:

      IniSettingStore();
      ~IniSettingStore();

      /// <summary>
      /// Reads the table, reconciles the [Settings] section of the ini against it,
      /// and returns the values the server should run with. Call once, after the
      /// database is open and its schema version has been accepted.
      ///
      /// False means the table could not be read; the caller should carry on with
      /// the ini alone rather than refuse to start, because a server that will not
      /// boot because its settings store is momentarily unavailable is worse than
      /// one running on the copy sitting in front of it. That fallback is also the
      /// only reason the file is kept complete.
      ///
      /// A start and a reinitialise run it (IniFileSettings::LoadDatabaseSettings),
      /// and nothing else: it WRITES - the file's lines, the rows' filevalue, a row
      /// for a line the table has never seen - and a write racing a save's own
      /// could undo that save, so the reload a save runs reads the rows alone
      /// (ReadStoredValues).
      /// </summary>
      bool Synchronize(std::map<String, String> &resolvedValues);

      /// <summary>
      /// The value of every row, as name -> value: what a save reads back
      /// (IniFileSettings::Reload). READ-ONLY, where Synchronize is not: it writes
      /// neither the file nor the table, adopts no line the table has never seen,
      /// and reports nothing - no HM5803, no HM5804. That reconcile is left to the
      /// start and the reinitialise, because run on every save it raced the store
      /// writes of saves on other threads: a reconcile that read a row before
      /// another save's DELETE and the file after it wrote the line back, and the
      /// other save's own reload then adopted it as a new setting, so a DELETE
      /// answered 200 and the setting came back; and one that read a row before a
      /// PUT and the file after it put the old value back into both. A line the
      /// table does not hold is not in the answer and is read from the file by the
      /// loader, as the start reads it. False when the table cannot be read, and
      /// the caller keeps what it has.
      /// </summary>
      static bool ReadStoredValues(std::map<String, String> &values);

      /// <summary>
      /// The [SettingsOverride] section of the ini: the values an administrator has
      /// declared must be used whatever the database holds. Read from the FILE and
      /// never stored, because the case it exists for is a database that cannot be
      /// reached or cannot be trusted. Static and free of any database call, so
      /// that it works in exactly that case.
      /// </summary>
      static void ReadOverrides(std::map<String, String> &values);

      // ---- backup -----------------------------------------------------------

      bool XMLStore(XNode *pBackupNode);
      bool XMLLoad(XNode *pBackupNode);

      /// <summary>Every row, as name -> (value, filevalue). Public for the backup path.</summary>
      static bool ReadAllRows(std::map<String, std::pair<String, String> > &rows);

      // ---- administrative writes --------------------------------------------
      //
      // The COM surface for these settings, which is what lets a Control Panel on
      // another machine administer them at all. The ordering below is the whole
      // design, and it INVERTED when the store moved: THE ROW IS WRITTEN FIRST and
      // the file is brought into line afterwards.
      //
      // It has to be that way round now. File-then-row, which is what this did
      // while the file was the truth, would on a failed row write leave the file
      // holding a value the store does not have - and the next start, seeing a file
      // edited away from its stored value, would correctly discard it. A save that
      // reported success and was silently reverted at the next restart is the exact
      // defect this project keeps removing, so the authoritative write goes first
      // and is the one that decides the answer.

      /// <summary>
      /// Sets one [Settings] value from an administrator, in row-then-file order.
      /// False means the DATABASE could not be written and nothing has been stored.
      ///
      /// True with the file write having failed is a real outcome and is reported
      /// rather than swallowed: the value IS stored and WILL be used, but until the
      /// file can be written the handful of readers that go to it directly - notably
      /// hMailServer.exe /Register, which has no database - keep seeing the old one.
      ///
      /// Note what this does NOT do: it does not make the value take effect in the
      /// running process. IniFileSettings::WriteSettingsValue, which every door to
      /// the store goes through, does that after this returns - it reloads and
      /// publishes the store (docs/SettingsApply.md) - and a write straight to this
      /// store, as the audit trail's revert makes, calls IniFileSettings::Reload
      /// itself.
      /// </summary>
      static bool WriteSetting(const String &name, const String &value);

      /// <summary>
      /// Removes a setting's key from the file and drops its row, which is now the
      /// ONLY way to return a setting to its default - deleting the line by hand no
      /// longer does it, because the row would simply be written back. False means
      /// the setting is still set.
      ///
      /// The FILE goes first here, which is the opposite of WriteSetting above and
      /// follows from the same rule: leave behind whichever residue the next start
      /// repairs, never the one it mistakes for an instruction. A leftover row is
      /// repaired - Synchronize writes its line back. A leftover line is a key the
      /// table has never seen and is ADOPTED, so dropping the row first and then
      /// failing to remove the line would put the setting straight back at the next
      /// start, after reporting success.
      /// </summary>
      static bool RemoveSetting(const String &name);

      /// <summary>
      /// Whether a name can be stored at all: non-empty, within the 100 character
      /// column, and free of the characters that would make it a different key -
      /// or a different section - when written to an ini file.
      /// </summary>
      static bool IsStorableName(const String &name);

      /// <summary>Whether a value fits the column. 4000 characters, as ReadIniSection_ enforces.</summary>
      static bool IsStorableValue(const String &value);

      /// <summary>
      /// The [Settings] keys the database must never hold, which stay in the file
      /// and nowhere else: today only PasswordPepper.
      ///
      /// The pepper is the one setting whose whole purpose is to be somewhere the
      /// password hashes are not. Crypt::ApplyPepper_ mixes it into every Argon2id
      /// and scrypt hash so that a copy of the database is not enough to attack them;
      /// stored in hm_inisettings it sits in the same database as hm_accounts, and
      /// XMLStore puts it in the same backup archive, which is exactly the copy it
      /// is meant to be missing from. So a file-only key is never adopted, never
      /// served from the store, written only to the file when an administrator sets
      /// it over COM or REST, and any row an earlier start adopted is taken out -
      /// after making sure the file holds the value the server was running on.
      /// </summary>
      static bool IsFileOnlyName(const String &name);

      /// <summary>
      /// Whether a setting's VALUE is a secret, by its name: a password, a shared
      /// secret, a token, a pepper. What GET /api/v1/settings/ini masks in its
      /// list, because that list is what a configuration export is built from and
      /// an export goes into a repository.
      ///
      /// The audit trail's rule (AuditTrail::IsSecretName: fragments matched
      /// anywhere in the name, less an exact list of names that carry a fragment
      /// and hold no secret), narrowed. That rule errs towards masking,
      /// which is right for a trail that records a change and wrong for a list
      /// that feeds an export: a key masked here is a key the export drops. So the
      /// fragments stand, and the shapes that match one without being a secret are
      /// let through - a path (RestApiPrivateKeyFile is where the key is, not the
      /// key), an address, a policy about passwords rather than one, the choice of
      /// a hash algorithm, the work factors of one. A key this build does not know
      /// that matches a fragment is masked: that way costs a value in an export,
      /// the other way costs a secret in a repository.
      ///
      /// Over the 241 [Settings] keys this server reads the two rules answer the
      /// same for every name but three - MetricsServerPrivateKeyFile,
      /// RestApiPrivateKeyFile and WebServicesPrivateKeyFile, which the trail
      /// masks and this list shows - and nothing this list masks is shown by the
      /// trail, since a name the trail lets through never reaches the shapes
      /// below. Those three are the shape rule against the trail's exact one, and
      /// the difference is meant: the trail names its exceptions one at a time (it
      /// names the two public key FILES), so a key file added later is withheld
      /// there until somebody decides it is not a secret, which costs a reader one
      /// detail; here it is let through by its shape, because a path dropped from
      /// an export describes a listener that cannot start.
      /// </summary>
      static bool IsSecretName(const String &name);

      /// <summary>
      /// Whether a name is a [Settings] key this build reads - one of the keys in
      /// build/ini-settings-baseline.txt, carried into the binary as
      /// IniSettingsBaseline.h - and, when it is, the key's own spelling. Matched
      /// without case, as the file matches keys, and answered in the server's
      /// spelling because the rows are matched WITH case (see Synchronize): a row
      /// named restapiport beside the RestApiPort row would be a second setting on
      /// PostgreSQL and a failed insert on the other three.
      ///
      /// For `hmailserver --set-setting`, which refuses any other name: storing a
      /// key nothing reads would change nothing and report success. The REST API
      /// and COM store any name on purpose, for a setting a script of the
      /// administrator's own reads, and are not held to this. Nor is the handful
      /// of keys read by name where they are used rather than by the loader - the
      /// webmail's branding and file-link policy, with their ".<domain>" twins -
      /// which the baseline does not list and which have REST pages of their own.
      /// </summary>
      static bool IsSettingTheServerReads(const String &name, String &spelling);

      /// <summary>
      /// The value this version of the server reads a [Settings] key with when
      /// nothing has been stored for it - the third argument of the read in
      /// IniFileSettings, carried into the binary by IniSettingsBaseline.h - and
      /// whether the read is the integer one. False for a name this build does not
      /// read, which has no default because nothing consults it.
      ///
      /// isInteger is how a stored value is compared with this: the integer read
      /// is GetPrivateProfileInt, which stops at the first character that is not
      /// part of a number and yields 0 for one that does not start with one, so a
      /// stored "0009" is the same setting as a default of 9 and is not a
      /// difference, while for a string read it plainly would be.
      ///
      /// WHAT "DEFAULT" MEANS, and it is worth saying because one case of it is
      /// not obvious. It is the value the server BEHAVES as though the setting
      /// has when it has no value of its own - not the value a fresh installation
      /// has in its file. The two differ for the keys the installer writes:
      /// hMailServer.ini ships with lines the packaging put there, every one of
      /// which is adopted into the store at the first start, and a line whose
      /// value is the same as this default is not a change to anything. So a key
      /// the installer wrote with the shipped value does not appear in the list of
      /// what an installation has changed, which is what makes that list short
      /// enough to read.
      /// </summary>
      static bool DefaultFor(const String &name, String &value, bool &isInteger);

      /// <summary>
      /// Every setting name the server holds: the union of the table and the
      /// [Settings] section, which after a start are the same list.
      ///
      /// The union rather than either alone, and the reason is the two states where
      /// they differ. A row whose line has not been written yet - the file was not
      /// writable at the time - is a setting that exists and is in force, and
      /// leaving it out would hide it from configuration-as-code and from the Deck.
      /// A key in the file with no row is a setting about to be adopted, and hiding
      /// that would make a value an administrator can see in the file unreachable
      /// from every tool. Falls back to the file alone when the table cannot be
      /// read, which is the same fallback Synchronize takes.
      /// </summary>
      static void ReadSettingNames(std::vector<String> &names);

   private:

      /// <summary>One whole section of the ini, as name -> value.</summary>
      static void ReadIniSection_(const String &section, std::map<String, String> &values);

      /// <summary>
      /// Puts the standing note in the file: what the [Settings] section now is,
      /// where the settings actually live, and what [SettingsOverride] is for.
      /// Written as keys of a [SettingsStore] section rather than as comment lines,
      /// because the only portable way this process has to write the file is the
      /// profile API, and it writes keys - the POSIX build implements exactly that
      /// API and nothing else (Common/Util/IniFile.cpp). Rewritten only when it
      /// differs, so a start does not dirty the file for nothing.
      /// </summary>
      static void WriteFileNotice_();

      static bool InsertRow_(const String &name, const String &value, const String &fileValue);
      static bool UpdateRow_(const String &name, const String &value, const String &fileValue);

      /// <summary>
      /// Drops a setting's row, which is what returns it to its default. Called only
      /// from RemoveSetting now: Synchronize used to drop a row whose key had left
      /// the file, and with the database as the store it writes the line back
      /// instead - see the comment there for why that inverted with the precedence.
      /// </summary>
      static bool DeleteRow_(const String &name);

      /// <summary>One row's stored value. False when there is no row, or no database.</summary>
      static bool ReadRow_(const String &name, String &value);

      /// <summary>
      /// The file's value for the one file-only key, which has no row for ReadRow_ to
      /// read. False when the file has no line for it, or an empty one: an empty
      /// pepper and no pepper are the same thing to Crypt, so either is "no value".
      /// </summary>
      static bool ReadFileValue_(const String &name, String &value);

      /// <summary>
      /// The audit trail's record of one administrative change to a [Settings] key:
      /// the value the row held and the value it holds now, "(default)" for the side
      /// on which there is no row, and a secret as "(changed)" - AuditTrail decides
      /// that by the name, as it does for hm_settings. The one file-only key,
      /// PasswordPepper, has no row on either side; its line in the file, read by
      /// ReadFileValue_, stands in for the row, so that a new pepper - after which
      /// no hash made before it verifies - is a row in the trail like any other.
      ///
      /// THIS is the settings chokepoint for hm_inisettings, and it is called from
      /// the two writes an administrator makes through an interface - WriteSetting
      /// and RemoveSetting - and from nothing else. Every interface reaches them
      /// through IniFileSettings::WriteSettingsValue and RemoveSettingsValue: COM
      /// SetIniSetting and DeleteIniSetting, PUT and DELETE /api/v1/settings/ini,
      /// the /api/v1/settings fields that are these keys, the tarpit setters, the
      /// branding and file-link routes. Synchronize does not record: it reconciles
      /// the file with the store at start-up, under no actor, and the stored value
      /// is what it keeps. XMLLoad does not record either: a restore runs on the
      /// maintenance work queue, where no actor is ever installed, so a call there
      /// could not write a row (see the comment in XMLLoad). hm_inisettings is not
      /// one of the statement chokepoint's tables, so a change is recorded here
      /// once and not a second time there.
      ///
      /// Nothing is recorded when nothing moved - a write of the value already
      /// stored, or a removal of a key that had no row - which is the same rule
      /// PropertySet applies, for the same reason: a settings page that writes
      /// every field it shows would otherwise bury the one that changed.
      /// withheld: the value replaced came from a file of the server's environment
      /// (EnvironmentSettings::IsWithheld), so the change is recorded as a secret's
      /// is, and the value is the administrator's from here on.
      /// </summary>
      static void RecordChange_(const String &name, bool wasStored, const String &previousValue, bool isStored, const String &value, bool withheld);

      /// <summary>
      /// Whether an administrator's change replaces a value a file of the server's
      /// environment supplied; false when no administrator is making it.
      /// </summary>
      static bool IsWithheld_(const String &name);

      /// <summary>
      /// Writes one key into a section of the ini. Returns false when the write did
      /// not take - which is not fatal, but must stop filevalue being advanced.
      /// </summary>
      static bool WriteIniValue_(const String &section, const String &name, const String &value);
   };
}
