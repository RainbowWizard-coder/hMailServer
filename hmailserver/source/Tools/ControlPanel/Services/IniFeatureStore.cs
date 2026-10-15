// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Win32;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// Reads and writes feature switches in hMailServer.INI ([Settings]
   /// section). Available when running on the server machine itself.
   /// </summary>
   public class IniFeatureStore
   {
      private const string Section = "Settings"; // no-loc

      public string IniPath { get; }

      /// <summary>
      /// Whether hMailServer.INI is on THIS machine. Unchanged in meaning: callers
      /// that need the file itself - ApiKeyStore locating hMailServerApiKeys.ini
      /// beside it, the log-folder card - still depend on this and still get the
      /// honest answer. It does not say the file is the server's: a Control Panel
      /// installed on a workstation has one of its own, which the installer writes for
      /// every component. A row that shows a section of it as the server's asks
      /// <see cref="LocalSectionAccess"/>.
      /// </summary>
      public bool IsAvailable => IniPath != null;

      /// <summary>
      /// The host of the server the session is signed in to, set with the COM reads
      /// below as the session is attached (ServerSession); null with no session, which
      /// is this computer, as ServerSession.IsLocalSession takes it.
      /// </summary>
      public static string SessionHost;

      /// <summary>Whether the server signed in to is the one on this computer (LocalHostNames, ServerSession.IsLocalSession's test).</summary>
      public static bool ServerIsOnThisComputer => LocalHostNames.IsLocal(SessionHost);

      /// <summary>Whether a section of hMailServer.ini that the server does not answer over COM can be shown as the server's from here, and if not, why.</summary>
      public enum FileSectionAccess
      {
         /// <summary>The file is here, it is the server's - the session is to this computer - and this user may read it.</summary>
         Readable,

         /// <summary>No hMailServer.ini on this computer.</summary>
         NoFile,

         /// <summary>The session is to a server on another computer: the file here is this computer's, not the server's.</summary>
         OtherComputer,

         /// <summary>The file is here and this user may not read it (from 6.3.6, /ProtectFolders).</summary>
         Denied
      }

      /// <summary>
      /// Whether the [Database], [SendingLimits] and [SendingLimitsOverrides] sections -
      /// which no COM property carries, so that the file is the only place to read them -
      /// can be shown as the server's from here, and why not (release review part B,
      /// findings CPB-1 and CPB-2). The file being on this computer was taken for that,
      /// and the installer leaves a readable Bin\hMailServer.INI for the Control Panel
      /// component alone, so that a Control Panel on a workstation signed in to a server
      /// elsewhere showed the workstation's values as the server's, wrote its edits into
      /// the workstation's file and said they were saved. <paramref name="denied"/> is
      /// the reason a read was refused, where it was.
      /// </summary>
      public FileSectionAccess LocalSectionAccess(out string denied)
      {
         denied = null;
         if (!IsAvailable)
            return FileSectionAccess.NoFile;
         if (!ServerIsOnThisComputer)
            return FileSectionAccess.OtherComputer;

         denied = ReadDenied();
         return denied == null ? FileSectionAccess.Readable : FileSectionAccess.Denied;
      }

      /// <summary>
      /// How to read and write the [Settings] section over COM, for when the file is
      /// not on this machine. Supplied by the application once a session exists.
      ///
      /// Injected rather than called directly, and that is not ceremony: this file is
      /// compiled straight into ControlPanel.Tests, and reaching ServerSession from
      /// here would drag COM and System.ServiceProcess into the test assembly behind
      /// it - the same trap CertificateInspector's expiry constant fell into. Null in
      /// the tests, and null before a connection exists, which is why every use is
      /// guarded.
      ///
      /// [Settings] only. [Database] and [Directories] are deliberately NOT reachable
      /// this way: they are the bootstrap that says where the database IS, and a
      /// server whose database location could be changed through the database it is
      /// currently using is a server that can be pointed somewhere else by anyone who
      /// reaches the one it is on.
      /// </summary>
      public static Func<string, string> ComReadSetting;

      /// <summary>
      /// Writes a [Settings] value over COM. An empty value is stored empty - a key the
      /// server then reads as empty, not as its default - and never deletes the key: see
      /// Write, and ServerSession's comment on SetIniSetting.
      /// </summary>
      public static Action<string, string> ComWriteSetting;

      /// <summary>
      /// Settings.IniSettingNames over COM: every [Settings] name the server holds, in its
      /// store or in the file, one per line. What tells a key stored with an empty value
      /// from one that is absent, since GetIniSetting answers "" for both (release review
      /// finding CP-4; <see cref="ReadHeld"/>). Null in the tests and before a session
      /// exists, and then nothing is told apart.
      /// </summary>
      public static Func<string> ComSettingNames;

      // The names as the last read of them found them, and when: one read serves a page's
      // rows, and a write or a moment's age sends the next row to the server again.
      private static HashSet<string> heldNames_;
      private static DateTime heldNamesAt_;
      private static readonly TimeSpan HeldNamesFor = TimeSpan.FromSeconds(5);

      /// <summary>
      /// Settings.IniSettingAppliesAll over COM: every [Settings] key the server's
      /// loader reads, with its mark, as one JSON object (SettingApplies.ParseMarks).
      /// Injected for the reason ComReadSetting is. Null in the tests and before a
      /// session exists.
      /// </summary>
      public static Func<string> ComAllApplies;

      /// <summary>
      /// Settings.PendingIniSettings over COM: the keys saved and not yet running,
      /// one per line. Null in the tests and before a session exists.
      /// </summary>
      public static Func<string> ComPendingSettings;

      /// <summary>
      /// Settings.GetChangedFromDefault over COM, read for its environment array: the
      /// settings the server's environment sets (Services/EnvironmentSettings,
      /// docs/EnvironmentConfiguration.md). Null in the tests and before a session
      /// exists.
      /// </summary>
      public static Func<string> ComChangedDocument;

      // The settings the environment sets, as the last read found them; null when
      // nothing is known - no read yet, a read that failed, or a credential the
      // document is refused to - and then no field is marked.
      private static IReadOnlyList<EnvironmentSettings.Entry> environment_;

      // The settings whose value a file of the environment supplied, from the same
      // read: their value is not shown in a field (EnvironmentSettings.ParseFromFile).
      private static IReadOnlyList<EnvironmentSettings.Withheld> fromFile_;

      /// <summary>
      /// Raised after every write through <see cref="Write"/>, on the thread that
      /// wrote. The window listens, so its pending bar follows every page's save
      /// without each page having to tell it - once per save, not once per field
      /// (MainWindow coalesces, and <see cref="PendingNamesSinceLastWrite"/> reuses
      /// a page's own read).
      /// </summary>
      public static event Action Written;

      // The marks, as the last successful IniSettingAppliesAll gave them: a mark is
      // a property of the server's build, so one read serves every page until the
      // session moves - but a read that FAILED is not remembered, and the next page
      // asks again. serverMarks_ is null until a read has said either way, true
      // once one has succeeded, and false for a server that has no such member.
      private static Dictionary<string, string> marks_;
      private static bool? serverMarks_;
      private static readonly object appliesLock_ = new();

      // The pending list as the last read found it, and how many writes have gone
      // to the server since, so a refresh straight after a page's own read reuses it.
      private static IReadOnlyList<string> lastPending_;
      private static int writesSinceRead_ = int.MaxValue;

      /// <summary>
      /// Asks the server for every mark in ONE call, when a page is opened - rather
      /// than one call per field, which on a remote link was a hundred round trips
      /// before the page could show. A failure leaves whatever an earlier read found
      /// and remembers nothing, except that a server with no IniSettingAppliesAll is
      /// an older one: its keys all apply at the next start, and the pages say so.
      /// </summary>
      public static void LoadMarks()
      {
         LoadEnvironment();

         Func<string> ask = ComAllApplies;
         if (ask == null)
            return;

         try
         {
            Dictionary<string, string> marks = SettingApplies.ParseMarks(ask());
            lock (appliesLock_)
            {
               marks_ = marks;
               serverMarks_ = true;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            if (SettingApplies.IsMissingMember(fatalCheck))
            {
               lock (appliesLock_)
               {
                  marks_ = null;
                  serverMarks_ = false;
               }
            }

            // Anything else - a reinitialise in progress, a dropped link, an answer
            // that did not parse - is a moment's, and the next page asks again.
         }
      }

      /// <summary>
      /// False when the connected server is one from before 22 September 2026: it
      /// marks no key and lists nothing as pending, because every [Settings] key
      /// applies at its next start. The pages then say what they said then - that a
      /// saved value applies after a service restart - and never that a save took
      /// effect. Null until the server has been asked, or when every ask failed.
      /// </summary>
      public static bool? ServerMarksSettings
      {
         get
         {
            lock (appliesLock_)
               return serverMarks_;
         }
      }

      /// <summary>
      /// When a saved value of this [Settings] key reaches the running server, as
      /// the last <see cref="LoadMarks"/> read it - "now", "reinitialize" or
      /// "restart" - and "restart" for every key of an older server, which applies
      /// them all at its next start. Null when nothing is known, or the server's
      /// loader does not read the key: a page shows no mark rather than a guessed
      /// one. Never a COM call.
      /// </summary>
      public static string AppliesFor(string key)
      {
         if (string.IsNullOrEmpty(key))
            return null;

         lock (appliesLock_)
         {
            if (serverMarks_ == false)
               return SettingApplies.Restart;

            return marks_ != null && marks_.TryGetValue(key, out string known) ? known : null;
         }
      }

      /// <summary>Forgets the marks and the pending list, when the session moves to another server.</summary>
      public static void ForgetApplies()
      {
         lock (appliesLock_)
         {
            marks_ = null;
            serverMarks_ = null;
            lastPending_ = null;
            writesSinceRead_ = int.MaxValue;
            environment_ = null;
            fromFile_ = null;
            heldNames_ = null;
         }
      }

      /// <summary>
      /// Reads which settings the server's environment sets, with the marks, when a
      /// page is opened: one call, which a page's own save does not repeat. A failure -
      /// the document is the server administrator's, and a domain administrator is
      /// refused it - leaves nothing known, and no field is marked; the server still
      /// refuses a change to a managed setting, with its own sentence.
      /// </summary>
      public static void LoadEnvironment()
      {
         Func<string> ask = ComChangedDocument;
         if (ask == null)
            return;

         IReadOnlyList<EnvironmentSettings.Entry> entries;
         IReadOnlyList<EnvironmentSettings.Withheld> fromFile;
         try
         {
            string document = ask();
            entries = EnvironmentSettings.Parse(document);
            fromFile = EnvironmentSettings.ParseFromFile(document);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            entries = null;
            fromFile = null;
         }

         lock (appliesLock_)
         {
            environment_ = entries;
            fromFile_ = fromFile;
         }
      }

      /// <summary>
      /// Whether this [Settings] key's value came from a file of the server's
      /// environment and is not shown; false when nothing is known. Never a COM call.
      /// </summary>
      public static bool WithheldKey(string key)
      {
         lock (appliesLock_)
            return EnvironmentSettings.IsWithheld(fromFile_, EnvironmentSettings.StoreIni, key);
      }

      /// <summary>The same for the hm_settings row a COM settings path names.</summary>
      public static bool WithheldComPath(string comPath)
      {
         lock (appliesLock_)
            return EnvironmentSettings.IsWithheld(fromFile_, EnvironmentSettings.StoreDatabase, EnvironmentSettings.PropertyOf(comPath));
      }

      /// <summary>Every setting the environment sets, as the last read found them; empty when nothing is known.</summary>
      public static IReadOnlyList<EnvironmentSettings.Entry> EnvironmentManaged()
      {
         lock (appliesLock_)
            return environment_ ?? Array.Empty<EnvironmentSettings.Entry>();
      }

      /// <summary>"variable" or "file" when the environment sets this [Settings] key; null otherwise. Never a COM call.</summary>
      public static string EnvironmentForKey(string key)
         => EnvironmentSettings.Find(EnvironmentManaged(), EnvironmentSettings.StoreIni, key)?.Source;

      /// <summary>
      /// "variable" or "file" when the environment sets the hm_settings row a COM
      /// settings path names (Services/EnvironmentSettings.PropertyOf); null otherwise.
      /// </summary>
      public static string EnvironmentForComPath(string comPath)
         => EnvironmentSettings.Find(EnvironmentManaged(), EnvironmentSettings.StoreDatabase, EnvironmentSettings.PropertyOf(comPath))?.Source;

      /// <summary>
      /// The keys saved and not yet running, as the server lists them now - one COM
      /// call. NULL when the server cannot say: there is nobody to ask, the server
      /// is an older one without the member, or the read failed. Null is not "none":
      /// a page that is told null says what it said before a save could publish.
      /// </summary>
      public static IReadOnlyList<string> PendingNames()
      {
         Func<string> ask = ComPendingSettings;
         if (ask == null)
            return null;

         try
         {
            IReadOnlyList<string> pending = SettingApplies.ParsePending(ask());
            lock (appliesLock_)
            {
               lastPending_ = pending;
               writesSinceRead_ = 0;
            }
            return pending;
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            if (SettingApplies.IsMissingMember(fatalCheck))
            {
               lock (appliesLock_)
               {
                  marks_ = null;
                  serverMarks_ = false;
               }
            }

            return null;
         }
      }

      /// <summary>
      /// The pending list for a refresh that follows writes: the last read when no
      /// write has gone to the server since it - a page that read the list after its
      /// own save - and a fresh read otherwise. What keeps a save to one read of the
      /// list, however many fields it wrote.
      /// </summary>
      public static IReadOnlyList<string> PendingNamesSinceLastWrite()
      {
         lock (appliesLock_)
         {
            if (writesSinceRead_ == 0 && lastPending_ != null)
               return lastPending_;
         }

         return PendingNames();
      }

      /// <summary>
      /// Whether the [Settings] section can be reached at all, by either route. This
      /// is what a settings page should ask before deciding it cannot edit anything.
      /// </summary>
      public bool SettingsReachable => IsAvailable || ComReadSetting != null;

      public IniFeatureStore()
      {
         IniPath = Locate();
      }

      /// <summary>
      /// A store of the file at <paramref name="iniPath"/>, or of none: what a Control
      /// Panel on a machine other than the server's has, which a test builds without
      /// the registry and the service that <see cref="Locate"/> reads.
      /// </summary>
      internal IniFeatureStore(string iniPath)
      {
         IniPath = iniPath;
      }

      private static string Locate()
      {
         // Installed server: registry InstallLocation.
         foreach (RegistryView view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
         {
            try
            {
               using RegistryKey baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
               using RegistryKey key = baseKey.OpenSubKey(@"SOFTWARE\hMailServer");
               string install = key?.GetValue("InstallLocation") as string;
               if (string.IsNullOrEmpty(install))
                  continue;

               string path = Path.Join(install, "Bin", "hMailServer.ini");
               if (File.Exists(path))
                  return path;

               path = Path.Join(install, "hMailServer.ini");
               if (File.Exists(path))
                  return path;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Try the next view.
            }
         }

         // Development tree: the INI next to the running service binary.
         try
         {
            using var searcher = new System.Management.ManagementObjectSearcher(
               "SELECT PathName FROM Win32_Service WHERE Name='hMailServer'"); // no-loc
            foreach (System.Management.ManagementObject service in searcher.Get())
            {
               string pathName = service["PathName"] as string;
               if (string.IsNullOrEmpty(pathName))
                  continue;

               string exe = pathName.Trim();
               if (exe.StartsWith("\""))
                  exe = exe.Substring(1, exe.IndexOf('"', 1) - 1);
               else if (exe.Contains(' '))
                  exe = exe.Substring(0, exe.IndexOf(' '));

               string dir = Path.GetDirectoryName(exe);
               if (dir == null)
                  continue;

               string path = Path.Join(dir, "hMailServer.ini");
               if (File.Exists(path))
                  return path;
            }
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
         }

         return null;
      }

      /// <summary>
      /// Reads a [Settings] value: over COM when there is a session, and from the
      /// file otherwise.
      ///
      /// THE ORDER INVERTED when the settings store moved into the database (schema
      /// 6042, Roadmap2 section 13). The file used to be preferred because it was the
      /// copy the server ran on and the table followed it. It is now the other way
      /// round: the table is the store, the file is its cache, and a
      /// [SettingsOverride] entry can make the running server use something that is
      /// in neither. Only the server knows the effective value, so the server is
      /// asked first, and the file is what is left when there is nobody to ask.
      /// </summary>
      public string Read(string key, string defaultValue = "")
      {
         Func<string, string> read = ComReadSetting;

         if (read != null)
         {
            try
            {
               string value = read(key);

               // GetIniSetting answers "" for an absent key and for one stored empty
               // alike, and this read takes "" for the caller's default. The rows that
               // show a value with a default of its own read through ReadHeld, which
               // asks the server's list of the names it holds which of the two it is
               // (release review finding CP-4).
               return string.IsNullOrEmpty(value) ? defaultValue : value;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // An older server without these members, or a session that has gone
               // away. Fall through to the file, which on the server itself is a
               // complete copy written by the server - and remember that this was a
               // fall-back, and why, so that the page can say its values are the
               // file's and may not be what the server runs (FallbackCause).
               FallbackCause ??= fatalCheck;
            }
         }

         if (IsAvailable)
            return ProfileApi.ReadString(Section, key, defaultValue, IniPath, 2048);

         return defaultValue;
      }

      /// <summary>
      /// Why a read since the last <see cref="ForgetFallback"/> fell back from the
      /// server to hMailServer.ini on this computer - the first failure of the COM
      /// read, which a page puts in words - or null when every read the server was
      /// asked for answered. A page that shows values read this way says they are
      /// the file's: the file is the server's cache of its settings store, and may
      /// differ from what the running server uses, and a save needs the server.
      /// Before, a lost link left the page looking as it always did, with Save
      /// enabled and, on API &amp; monitoring, a port of 0.
      /// </summary>
      public Exception FallbackCause { get; private set; }

      /// <summary>Starts a new count of fall-backs: a page calls it before it reads its values again.</summary>
      public void ForgetFallback() => FallbackCause = null;

      /// <summary>
      /// A switch as the server reads it: "1" is on. A key the server holds empty is off
      /// whatever the default, since the loader makes 0 of it (<see cref="ReadNumber"/>).
      /// </summary>
      public bool ReadBool(string key, bool defaultValue)
         => ReadNumber(key, defaultValue ? "1" : "0").Trim() == "1";

      /// <summary>
      /// Reads a [Settings] value as the server holds it (release review finding CP-4):
      /// what <see cref="Read"/> answers, except that a key the server holds with an
      /// empty value is answered empty, and <paramref name="heldEmpty"/> is true, rather
      /// than as <paramref name="defaultValue"/>. The server's loader hands such a key on
      /// as empty, and most of its readers then use the empty value and not the default:
      /// ScriptAllowedObjects stored empty lets a script create no object, an empty
      /// AcmeDirectoryUrl renews nothing and an empty RestApiBindAddress keeps the REST API
      /// from starting without TLS - and the pages showed each at its default, the first
      /// with the warning that any object may be created. A few readers put their default
      /// back for an empty value - OtelServiceName, OAuth2UsernameClaim, MtaStsPolicyMode
      /// and UpdateChannel - which a row says with <see cref="HeldEmptyPlaceholder"/>.
      /// GetIniSetting answers "" for an absent key and for an empty one alike, so the
      /// server's list of the names it holds (<see cref="ComSettingNames"/>) says which;
      /// where it cannot be asked, the default is answered, as before. The file, read
      /// without a session, tells the two apart itself: GetPrivateProfileString answers ""
      /// for "Key=".
      /// </summary>
      public string ReadHeld(string key, string defaultValue, out bool heldEmpty)
      {
         heldEmpty = false;
         Func<string, string> read = ComReadSetting;

         if (read != null)
         {
            try
            {
               string value = read(key);
               if (!string.IsNullOrEmpty(value) || string.IsNullOrEmpty(defaultValue))
                  return value ?? "";

               if (HoldsName_(key) == true)
               {
                  heldEmpty = true;
                  return "";
               }

               return defaultValue;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // As Read: the file, and why the server was not asked.
               FallbackCause ??= fatalCheck;
            }
         }

         if (IsAvailable)
         {
            string value = ProfileApi.ReadString(Section, key, defaultValue, IniPath, 2048);
            heldEmpty = value.Length == 0 && !string.IsNullOrEmpty(defaultValue);
            return value;
         }

         return defaultValue;
      }

      /// <summary>
      /// What a row's box says of a key the server holds empty (<see cref="ReadHeld"/>):
      /// "(empty)", where the server uses the empty value; and "(empty = hmailserver)",
      /// where the server's reader of the key puts a value of its own in the place of an
      /// empty one - <paramref name="emptyMeans"/>, which is then what the server uses.
      /// OtelServiceName held empty is the service name "hmailserver" to the three
      /// exporters (OtelTracer, OtelLogExporter, OtelMetricsExporter), OAuth2UsernameClaim
      /// "email" to the token validator, and UpdateChannel the stable feed; the rows said
      /// "(empty)" for them as for a key the server reads as empty (the adversarial check
      /// of rf5-rf6-cp).
      /// </summary>
      public static string HeldEmptyPlaceholder(string emptyMeans)
         => string.IsNullOrEmpty(emptyMeans) ? L("(empty)") : F("(empty = {0})", emptyMeans);

      /// <summary>
      /// A number as the server's loader reads it, as text: <see cref="ReadHeld"/>'s value,
      /// and "0" for a key the server holds empty, which its loader makes 0 of whatever
      /// the default (_ttoi), where the pages showed the default - a script timeout of 60
      /// for a server that sets none.
      /// </summary>
      public string ReadNumber(string key, string defaultValue)
      {
         if (string.Equals(defaultValue, "0", StringComparison.Ordinal))
            return Read(key, defaultValue);

         string value = ReadHeld(key, defaultValue, out bool heldEmpty);
         return heldEmpty ? "0" : value;
      }

      // Whether the server holds the key, by its list of names; null when the list
      // cannot be read.
      private static bool? HoldsName_(string key)
      {
         Func<string> ask = ComSettingNames;
         if (ask == null)
            return null;

         HashSet<string> names;
         lock (appliesLock_)
            names = heldNames_ != null && DateTime.UtcNow - heldNamesAt_ < HeldNamesFor ? heldNames_ : null;

         if (names == null)
         {
            try
            {
               names = new HashSet<string>(
                  (ask() ?? "").Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries),
                  StringComparer.OrdinalIgnoreCase);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               return null;
            }

            lock (appliesLock_)
            {
               heldNames_ = names;
               heldNamesAt_ = DateTime.UtcNow;
            }
         }

         return names.Contains(key);
      }

      /// <summary>
      /// Writes a [Settings] value over COM, which stores it in hm_inisettings - the
      /// settings store - and brings the file's copy into line in the same call.
      ///
      /// THIS NO LONGER WRITES THE FILE ITSELF, even when the file is right here, and
      /// that is the whole point of the change. While the file was the store, writing
      /// it was the write. Now the table is the store and a line in the file is a
      /// cache entry: writing it directly would produce a value that reads back
      /// correctly on the next visit to this page, is named in the server's error log
      /// as an edit that was ignored, and is put back to the stored value at the next
      /// start. That "saved, and silently reverted" shape is the exact defect this
      /// project keeps removing, so the only write left is the one that goes to the
      /// store.
      ///
      /// Which means a Control Panel with no session cannot change a setting even
      /// sitting on the server, and it says so rather than pretending. Reading still
      /// falls back to the file, because a complete copy of the configuration is
      /// exactly what the file still is.
      /// </summary>
      public void Write(string key, string value)
      {
         Action<string, string> write = ComWriteSetting;

         if (write == null)
            throw new InvalidOperationException(
               "Settings are stored in the server's database, so changing one needs a connection to the server. " +
               "Editing hMailServer.ini by hand does not change a setting: the stored value is used, and the edit is " +
               "reported in the server's error log and undone at the next start.");

         write(key, value);

         lock (appliesLock_)
         {
            if (writesSinceRead_ != int.MaxValue)
               writesSinceRead_++;
            heldNames_ = null;
         }

         // The server published the value as it stored it: a key marked now is in
         // effect, and one marked reinitialize or restart may be pending.
         Written?.Invoke();
      }

      public void WriteBool(string key, bool value) => Write(key, value ? "1" : "0");

      /// <summary>
      /// Reads a value from a named section rather than from [Settings].
      ///
      /// Most of hMailServer.INI is [Settings], which is why this class defaults to
      /// it - but not all of it, and a page that edits [Database] or [Directories]
      /// through Read() above would write keys of the right name into a section the
      /// server never looks at. That is the worst shape of bug this project has:
      /// the value appears saved, reads back correctly on the next visit, and does
      /// nothing.
      /// </summary>
      public string ReadFrom(string section, string key, string defaultValue = "")
      {
         if (!IsAvailable)
            return defaultValue;
         return ProfileApi.ReadString(section, key, defaultValue, IniPath, 2048);
      }

      /// <summary>
      /// Writes a value to a named section, and throws with Windows' reason when the file
      /// refused it (<see cref="ProfileApi.WriteStringChecked"/>): the write's answer was
      /// dropped, so that a file this user may read and not write - the administrators'
      /// alone from 6.3.6 - was written to without a word, and the page said the value was
      /// saved (release review part B, CPB-1). See <see cref="ReadFrom"/>.
      /// </summary>
      public void WriteTo(string section, string key, string value)
      {
         if (!IsAvailable)
            throw new InvalidOperationException("hMailServer.ini was not found on this machine.");
         ProfileApi.WriteStringChecked(section, key, value, IniPath);
      }

      /// <summary>
      /// Why hMailServer.INI is here and cannot be read, or null when it can be (or
      /// is not here at all). From 6.3.6 the installer gives the file - and the
      /// log, data, temp, database and event folders - to SYSTEM, the
      /// Administrators and the service alone (hMailServer.exe /ProtectFolders), so
      /// a Control Panel the administrator did not run as administrator cannot
      /// read it: every value read from it would then be the default, and a page
      /// built on one - the log folder above all - would say the wrong thing. The
      /// pages that read it say this instead.
      /// </summary>
      public string ReadDenied()
      {
         if (!IsAvailable)
            return null;
         try
         {
            using var stream = new FileStream(IniPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return null;
         }
         catch (UnauthorizedAccessException denied)
         {
            return denied.Message;
         }
         catch (Exception other) when (!ExceptionPolicy.IsFatal(other))
         {
            return null;
         }
      }

      /// <summary>Reads the configured log folder from the [Directories] section.</summary>
      public string GetLogFolder()
      {
         string folder = ReadFrom("Directories", "LogFolder", "");
         return string.IsNullOrWhiteSpace(folder) ? null : folder;
      }
   }
}
