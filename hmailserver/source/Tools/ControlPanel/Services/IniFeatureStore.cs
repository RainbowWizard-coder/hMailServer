// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Win32;

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
      /// beside it, the log-folder card, anything reading [Database] or
      /// [Directories] - still depend on this and still get the honest answer.
      /// </summary>
      public bool IsAvailable => IniPath != null;

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

      /// <summary>Writes a [Settings] value over COM. An empty value DELETES the key - see Write.</summary>
      public static Action<string, string> ComWriteSetting;

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
         try
         {
            entries = EnvironmentSettings.Parse(ask());
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            entries = null;
         }

         lock (appliesLock_)
            environment_ = entries;
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

               string path = Path.Join(install, "Bin", "hMailServer.INI");
               if (File.Exists(path))
                  return path;

               path = Path.Join(install, "hMailServer.INI");
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

               // The server cannot tell "absent" from "empty" either - an ini reader
               // never can - so an empty answer means the caller's default applies,
               // exactly as GetPrivateProfileString would decide below.
               return string.IsNullOrEmpty(value) ? defaultValue : value;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // An older server without these members, or a session that has gone
               // away. Fall through to the file, which on the server itself is a
               // complete copy written by the server.
            }
         }

         if (IsAvailable)
            return ProfileApi.ReadString(Section, key, defaultValue, IniPath, 2048);

         return defaultValue;
      }

      public bool ReadBool(string key, bool defaultValue)
         => Read(key, defaultValue ? "1" : "0").Trim() == "1";

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
               "Editing hMailServer.INI by hand does not change a setting: the stored value is used, and the edit is " +
               "reported in the server's error log and undone at the next start.");

         write(key, value);

         lock (appliesLock_)
         {
            if (writesSinceRead_ != int.MaxValue)
               writesSinceRead_++;
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

      /// <summary>Writes a value to a named section. See <see cref="ReadFrom"/>.</summary>
      public void WriteTo(string section, string key, string value)
      {
         if (!IsAvailable)
            throw new InvalidOperationException("hMailServer.INI was not found on this machine.");
         ProfileApi.WriteString(section, key, value, IniPath);
      }

      /// <summary>Reads the configured log folder from the [Directories] section.</summary>
      public string GetLogFolder()
      {
         string folder = ReadFrom("Directories", "LogFolder", "");
         return string.IsNullOrWhiteSpace(folder) ? null : folder;
      }
   }
}
