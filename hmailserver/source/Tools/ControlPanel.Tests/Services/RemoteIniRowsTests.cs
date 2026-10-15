// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The settings pages' hMailServer.ini rows ask the server, on a Control Panel signed
   /// in to a server on another machine as on the server itself. The switches, texts,
   /// choices and numbers of ServerSettingsView read and wrote only when the file was on
   /// this computer (IniFeatureStore.IsAvailable), so that a remote Control Panel showed
   /// each at its default as though it were the server's value and a save dropped its
   /// edit and counted it saved - which the rows moved onto these pages by review finding
   /// F273 (the Windows Event Log, ManageSieve's bind address, the REST API's and the
   /// metrics listener's addresses, certificates and keys, the OpenTelemetry endpoints,
   /// PROXY and XCLIENT) had not done on the INI-only page they came from, which refused
   /// to show what it could not read. The rows are views, not compiled here, so the rule
   /// is held in their source, and the store's half of it - the server's value read and
   /// the edit written to it with no file on this computer - by running the store. The
   /// [Database] rows of Performance's Database connections card are the file's alone,
   /// as the server does not answer that section over COM: where the file cannot be read
   /// here they are marked Not read, their card says why and a save leaves them, where
   /// they were live boxes at their defaults and a save said an edit was saved.
   /// </summary>
   public class RemoteIniRowsTests : IDisposable
   {
      private readonly Func<string, string> savedRead_ = IniFeatureStore.ComReadSetting;
      private readonly Action<string, string> savedWrite_ = IniFeatureStore.ComWriteSetting;
      private readonly string savedHost_ = IniFeatureStore.SessionHost;

      public void Dispose()
      {
         IniFeatureStore.ComReadSetting = savedRead_;
         IniFeatureStore.ComWriteSetting = savedWrite_;
         IniFeatureStore.SessionHost = savedHost_;
      }

      /// <summary>
      /// With no hMailServer.ini on this computer and a session to the server, the store
      /// reads the server's values - the example of the finding: a metrics listener on
      /// 9090 bound to 0.0.0.0, ManageSieve on every address, PROXY on - and writes an
      /// edit to the server, where the rows showed 127.0.0.1 and off and wrote nothing.
      /// </summary>
      [Fact]
      public void WithNoFileOnThisComputer_TheServersValuesAreReadAndTheEditsWrittenToIt()
      {
         var server = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
         {
            ["MetricsServerPort"] = "9090",
            ["MetricsServerBindAddress"] = "0.0.0.0",
            ["ManageSieveServerBindAddress"] = "0.0.0.0",
            ["SMTPProxyProtocolEnabled"] = "1",
            ["WindowsEventLogLevel"] = "2"
         };
         var written = new List<(string Key, string Value)>();
         IniFeatureStore.ComReadSetting = key => server.TryGetValue(key, out string value) ? value : "";
         IniFeatureStore.ComWriteSetting = (key, value) => written.Add((key, value));

         var store = new IniFeatureStore(iniPath: null);
         Assert.False(store.IsAvailable);
         Assert.True(store.SettingsReachable);

         Assert.Equal("9090", store.Read("MetricsServerPort", "0"));
         Assert.Equal("0.0.0.0", store.Read("MetricsServerBindAddress", "127.0.0.1"));
         Assert.Equal("0.0.0.0", store.Read("ManageSieveServerBindAddress", "127.0.0.1"));
         Assert.True(store.ReadBool("SMTPProxyProtocolEnabled", false));
         Assert.False(store.ReadBool("SMTPXClientEnabled", false));
         Assert.Equal("2", store.Read("WindowsEventLogLevel", "1"));
         Assert.Null(store.FallbackCause);

         store.WriteBool("SMTPXClientEnabled", true);
         store.Write("SMTPXClientTrustedIPs", "10.0.0.6");
         Assert.Equal(new[] { ("SMTPXClientEnabled", "1"), ("SMTPXClientTrustedIPs", "10.0.0.6") }, written);

         // With no session as well there is nothing to ask: the rows show no value, and
         // the page has none to show them on.
         IniFeatureStore.ComReadSetting = null;
         Assert.False(new IniFeatureStore(iniPath: null).SettingsReachable);
      }

      /// <summary>
      /// Every [Settings] row class of the settings pages reads and saves while the
      /// section can be reached at all, by the server or the file (SettingsReachable),
      /// and none asks whether the file is on this computer, but the [Database] row,
      /// which is the file's alone; a withheld row's save goes the same way.
      /// </summary>
      [Fact]
      public void TheSettingsRows_AskTheServer_AndOnlyTheDatabaseRowTheFile()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         var rows = new Dictionary<string, string>
         {
            ["IniBool"] = Member(view, "private class IniBool : ComSetting, IIniSetting"),
            ["IniNumber"] = Member(view, "private class IniNumber : ComSetting, IIniSetting"),
            ["IniText"] = Member(view, "private class IniText : ComSetting, IIniSetting"),
            ["IniChoice"] = Member(view, "private class IniChoice : ComSetting, IIniSetting"),
            ["IniRangeNumber"] = Member(ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.Monitoring.cs")), "private sealed class IniRangeNumber : ComSetting, IIniSetting"),
            ["IniSecret"] = Member(ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.IniSecret.cs")), "private sealed class IniSecret : ComPassword, IIniSetting"),
            ["SaveWithheld"] = Member(view, "protected bool SaveWithheld(IniFeatureStore store, string key)")
         };

         foreach ((string name, string body) in rows)
         {
            Assert.DoesNotContain("IsAvailable", body, StringComparison.Ordinal);
            Assert.True(body.Contains("SettingsReachable", StringComparison.Ordinal), name + " does not ask whether the server or the file can be reached.");
         }

         // The four that read as their editor is built, and save, each ask before both.
         foreach (string name in new[] { "IniBool", "IniText", "IniChoice" })
            Assert.Equal(2, Regex.Matches(rows[name], @"\bIniStore\.SettingsReachable\b").Count);
         Assert.Matches(new Regex(@"if \(Reachable && !int\.TryParse\(ReadRaw\(\), out current\)\)"), rows["IniNumber"]);
         Assert.Matches(new Regex(@"if \(Ignored \|\| !Reachable \|\| number_ == null\)"), rows["IniNumber"]);
         Assert.Contains("protected virtual bool Reachable => IniStore != null && IniStore.SettingsReachable;", rows["IniNumber"], StringComparison.Ordinal);

         // The [Database] row asks whether the file here is the server's, not only whether
         // there is one (release review part B, CPB-1).
         string database = Member(view, "private class SectionIniNumber : IniNumber");
         Assert.Contains("protected override bool Reachable => IniStore != null && IniStore.LocalSectionAccess(out _) == IniFeatureStore.FileSectionAccess.Readable;", database, StringComparison.Ordinal);
      }

      /// <summary>
      /// The rows the moves brought onto these pages are of those classes: Monitoring
      /// &amp; alerts', the Windows Event Log's on Logging, ManageSieve's on Protocols, and
      /// the front-end proxies' shown on Incoming relays. None is a [Database] row.
      /// </summary>
      [Theory]
      [InlineData("BuildMonitoring", "RestApiBindAddress,MetricsServerBindAddress,MetricsServerAuthToken,OtelServiceName")]
      [InlineData("BuildLogging", "WindowsEventLogEnabled,WindowsEventLogLevel")]
      [InlineData("BuildProtocols", "ManageSieveServerPort,ManageSieveServerBindAddress")]
      [InlineData("BuildFrontEndProxies", "SMTPProxyProtocolEnabled,SMTPProxyProtocolTrustedIPs,SMTPXClientEnabled,SMTPXClientTrustedIPs")]
      public void TheMovedRows_AreRowsThatAskTheServer(string builder, string keys)
      {
         string view = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         int start = view.IndexOf("private void " + builder + "()", StringComparison.Ordinal);
         Assert.True(start >= 0, "ServerSettingsView." + builder + " is gone.");
         int end = view.IndexOf("private void Build", start + 1, StringComparison.Ordinal);
         string body = view.Substring(start, (end < 0 ? view.Length : end) - start);

         var asking = new HashSet<string>(StringComparer.Ordinal) { "IniBool", "IniNumber", "IniText", "IniChoice", "IniRangeNumber", "IniSecret" };
         foreach (Match row in Regex.Matches(body, @"\bnew (Ini\w+|SectionIniNumber)\b"))
            Assert.True(asking.Contains(row.Groups[1].Value), builder + " places a " + row.Groups[1].Value + ", which does not ask the server.");

         foreach (string key in keys.Split(','))
            Assert.Matches(new Regex(@"new Ini\w+\s*\{\s*Path = """ + Regex.Escape(key) + @""""), body);
      }

      /// <summary>
      /// With no hMailServer.ini on this computer the [Database] section has nothing to
      /// give but the caller's default - which the Database connections card showed as
      /// the server's value - and nowhere to write, and the server is not asked: COM
      /// does not carry the section.
      /// </summary>
      [Fact]
      public void WithNoFileOnThisComputer_TheDatabaseSectionHasNoValueAndNoWrite()
      {
         var asked = new List<string>();
         IniFeatureStore.ComReadSetting = key => { asked.Add(key); return "9"; };
         IniFeatureStore.ComWriteSetting = (key, value) => asked.Add(key + "=" + value);

         var store = new IniFeatureStore(iniPath: null);
         Assert.False(store.IsAvailable);
         Assert.True(store.SettingsReachable);
         Assert.Equal("6", store.ReadFrom("Database", "ConnectionAttempts", "6"));
         Assert.Throws<InvalidOperationException>(() => store.WriteTo("Database", "ConnectionAttempts", "9"));
         Assert.Null(store.ReadDenied());
         Assert.Empty(asked);
      }

      /// <summary>
      /// A file on this computer that this user may not read - from 6.3.6 the file is
      /// the administrators' alone (hMailServer.exe /ProtectFolders), and the Control
      /// Panel runs as the user who started it - answers a [Database] read with the
      /// caller's default as well, and the store says why (ReadDenied), which is what the
      /// rows ask before they show a value. The file is denied to this process's own
      /// account here, so that the test needs no second account and no elevation.
      /// </summary>
      [Fact]
      public void AFileThisUserMayNotRead_IsSaidToBe_AndItsDatabaseSectionAnswersTheDefault()
      {
         string path = Path.Join(Path.GetTempPath(), "cp-remote-ini-" + Guid.NewGuid().ToString("N") + ".ini");
         File.WriteAllText(path, "[Database]\r\nConnectionAttempts=9\r\n");
         try
         {
            var store = new IniFeatureStore(path);
            Assert.True(store.IsAvailable);
            Assert.Null(store.ReadDenied());

            SecurityIdentifier account = WindowsIdentity.GetCurrent().User;
            var deny = new FileSystemAccessRule(account, FileSystemRights.ReadData, AccessControlType.Deny);
            var file = new FileInfo(path);
            FileSecurity security = file.GetAccessControl();
            security.AddAccessRule(deny);
            file.SetAccessControl(security);
            try
            {
               bool bypassed;
               try
               {
                  File.ReadAllBytes(path);
                  bypassed = true;
               }
               catch (UnauthorizedAccessException)
               {
                  bypassed = false;
               }

               Assert.SkipWhen(bypassed, "This account reads the file past a deny entry of its own, so it cannot be made another's here.");

               Assert.NotNull(store.ReadDenied());
               Assert.Equal("6", store.ReadFrom("Database", "ConnectionAttempts", "6"));
            }
            finally
            {
               security = file.GetAccessControl();
               security.RemoveAccessRuleSpecific(deny);
               file.SetAccessControl(security);
            }

            // The default came of the denial: the file holds the value for a reader it lets in.
            Assert.Equal("9", store.ReadFrom("Database", "ConnectionAttempts", "6"));
         }
         finally
         {
            File.Delete(path);
         }
      }

      /// <summary>
      /// A [Database] row this Control Panel cannot read is marked Not read before its
      /// editor is built, so that it is not among the rows a save writes and counts, and
      /// its card says why once, at its top; it is not counted among the settings that
      /// could not be read, since the server was not asked. Why is the row's own: the
      /// file is not on this computer, or this user may not read it.
      /// </summary>
      [Fact]
      public void ADatabaseRowThatCannotBeReadHere_IsNotRead_AndItsCardSaysWhy()
      {
         string view = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string build = Member(view, "private void BuildUi()");

         Match road = Regex.Match(build,
            @"if \(setting is SectionIniNumber (\w+) && \1\.Unreadable\(\) is string (\w+)\)\s*\{\s*fileProblem \?\?= \2;\s*fields\.Children\.Add\(lastRow = UnreadRow_\(setting\)\);\s*continue;\s*\}");
         Assert.True(road.Success, "BuildUi does not mark a [Database] row it cannot read Not read.");
         Assert.True(road.Index < build.IndexOf("lastRow = setting.CreateRow(value);", StringComparison.Ordinal), "The [Database] row's editor is built before it is asked whether it can be read.");
         Assert.True(road.Index < build.IndexOf("built_.Add(setting);", StringComparison.Ordinal), "The [Database] row is among the rows a save writes.");
         Assert.DoesNotContain("failedReads_", road.Value, StringComparison.Ordinal);
         Assert.DoesNotContain("unread++", road.Value, StringComparison.Ordinal);

         // Declared per card, and said once at its top.
         int cardLoop = build.IndexOf("foreach (CardDef card in tab.Cards)", StringComparison.Ordinal);
         int declared = build.IndexOf("string fileProblem = null;", StringComparison.Ordinal);
         int settingLoop = build.IndexOf("foreach (ComSetting setting in card.Settings)", StringComparison.Ordinal);
         Assert.True(cardLoop >= 0 && cardLoop < declared && declared < settingLoop, "The [Database] rows' reason is not kept per card.");
         Assert.Matches(new Regex(@"if \(fileProblem != null\)\s*\{\s*var (\w+) = new Scaffold\.InlineNotice\s*\{\s*Level = StatusLevel\.Warning,\s*Text = fileProblem,[^}]*\};[^}]*fields\.Children\.Insert\(0, \1\);\s*\}"), build);

         // The reason: no file on this computer; a file of this computer's own, the server
         // being on another (CPB-1); or one this user may not read.
         string database = Member(view, "private class SectionIniNumber : IniNumber");
         string unreadable = Member(database, "public string Unreadable()");
         Assert.Contains(": IniStore.LocalSectionAccess(out denied);", unreadable, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"case IniFeatureStore\.FileSectionAccess\.NoFile:\s*return L\(""The settings marked Not read are kept in the \[Database\] section of hMailServer\.ini, which was not found on this computer, and no COM property carries them\."), unreadable);
         Assert.Matches(new Regex(@"case IniFeatureStore\.FileSectionAccess\.OtherComputer:\s*return L\(""The settings marked Not read are kept in the \[Database\] section of the hMailServer\.ini on the computer the server runs on, and no COM property carries them; this Control Panel is signed in to a server on another computer"), unreadable);
         Assert.Matches(new Regex(@"case IniFeatureStore\.FileSectionAccess\.Denied:\s*return F\(""The settings marked Not read are kept in the \[Database\] section of hMailServer\.ini, which could not be read: \{0\} "), unreadable);

         // The database type the card names is read on the same terms.
         Assert.Contains("string databaseType = iniStore_.LocalSectionAccess(out _) == IniFeatureStore.FileSectionAccess.Readable", view, StringComparison.Ordinal);
      }

      /// <summary>
      /// A readable hMailServer.ini on this computer - the installer writes one for the
      /// Control Panel component alone - and a session to a server on another computer:
      /// the file is this computer's, and the sections no COM property carries cannot be
      /// shown from it as the server's (release review part B, CPB-1 and CPB-2). With the
      /// session to this computer, or none, the same file is the server's.
      /// </summary>
      [Fact]
      public void AReadableFileHere_IsNotTheServers_WhenTheServerIsOnAnotherComputer()
      {
         string path = Path.Join(Path.GetTempPath(), "cp-remote-ini-" + Guid.NewGuid().ToString("N") + ".ini");
         File.WriteAllText(path, "[Database]\r\nNumberOfConnections=5\r\n[SendingLimits]\r\nMaxMessagesPerAccountPerPeriod=0\r\n");
         try
         {
            var store = new IniFeatureStore(path);
            Assert.True(store.IsAvailable);
            Assert.Null(store.ReadDenied());

            IniFeatureStore.SessionHost = "mail.example.com";
            Assert.Equal(IniFeatureStore.FileSectionAccess.OtherComputer, store.LocalSectionAccess(out string denied));
            Assert.Null(denied);

            foreach (string here in new[] { null, "", "localhost", "127.0.0.1", Environment.MachineName })
            {
               IniFeatureStore.SessionHost = here;
               Assert.Equal(IniFeatureStore.FileSectionAccess.Readable, store.LocalSectionAccess(out _));
            }

            Assert.Equal(IniFeatureStore.FileSectionAccess.NoFile, new IniFeatureStore(iniPath: null).LocalSectionAccess(out _));
         }
         finally
         {
            File.Delete(path);
         }
      }

      /// <summary>
      /// A write the file refuses - one this user may read and not write - is a failure
      /// the page reports, where the write's answer was dropped and the row counted saved
      /// (CPB-1, CPB-2). The file is denied to this process's own account for writing.
      /// </summary>
      [Fact]
      public void AWriteTheFileRefuses_Throws()
      {
         string path = Path.Join(Path.GetTempPath(), "cp-remote-ini-" + Guid.NewGuid().ToString("N") + ".ini");
         File.WriteAllText(path, "[Database]\r\nConnectionAttempts=6\r\n");
         try
         {
            var store = new IniFeatureStore(path);
            SecurityIdentifier account = WindowsIdentity.GetCurrent().User;
            var deny = new FileSystemAccessRule(account, FileSystemRights.WriteData | FileSystemRights.AppendData, AccessControlType.Deny);
            var file = new FileInfo(path);
            FileSecurity security = file.GetAccessControl();
            security.AddAccessRule(deny);
            file.SetAccessControl(security);
            try
            {
               bool bypassed;
               try
               {
                  using (new FileStream(path, FileMode.Open, FileAccess.Write))
                     bypassed = true;
               }
               catch (UnauthorizedAccessException)
               {
                  bypassed = false;
               }

               Assert.SkipWhen(bypassed, "This account writes the file past a deny entry of its own, so it cannot be made read-only to it here.");

               var refused = Assert.Throws<System.ComponentModel.Win32Exception>(() => store.WriteTo("Database", "ConnectionAttempts", "9"));
               Assert.Equal(5, refused.NativeErrorCode);
               Assert.Contains("ConnectionAttempts", refused.Message, StringComparison.Ordinal);
            }
            finally
            {
               security = file.GetAccessControl();
               security.RemoveAccessRuleSpecific(deny);
               file.SetAccessControl(security);
            }

            // Nothing was written.
            Assert.Equal("6", store.ReadFrom("Database", "ConnectionAttempts", "0"));
         }
         finally
         {
            File.Delete(path);
         }
      }

      /// <summary>
      /// Server limits &amp; expert settings' sending limits - [SendingLimits] and
      /// [SendingLimitsOverrides], which the server reads from its own file alone
      /// (RateLimiter) and no COM property carries - are read only where the file here is
      /// the server's: otherwise each row is Not read, has no editor and is left out of a
      /// save, the card says why at its top, and the line under the header names no file;
      /// a write the file refuses is reported (release review part B, CPB-2).
      /// </summary>
      [Fact]
      public void TheSendingLimits_AreNotRead_WhereTheFileHereIsNotTheServers()
      {
         string feature = ReadControlPanelFile(Path.Join("Views", "FeatureSettingsView.xaml.cs"));

         string number = Member(feature, "private class NumberSetting : Setting");
         int asked = number.IndexOf("if (Section != null && !FileSectionIsTheServers_(store))", StringComparison.Ordinal);
         int read = number.IndexOf("IniDirect.ReadValue(store.IniPath, Section, Key, Default)", StringComparison.Ordinal);
         Assert.True(asked > 0 && read > asked, "A [SendingLimits] number is read before it is asked whether the file here is the server's.");
         Assert.Contains("return NotReadRow();", number, StringComparison.Ordinal);

         string lines = Member(feature, "private class SectionLinesSetting : Setting");
         asked = lines.IndexOf("if (!FileSectionIsTheServers_(store))", StringComparison.Ordinal);
         read = lines.IndexOf("IniDirect.ReadSectionLines(store.IniPath, Section)", StringComparison.Ordinal);
         Assert.True(asked > 0 && read > asked, "The overrides are read before it is asked whether the file here is the server's.");

         // No editor, no save.
         Assert.Matches(new Regex(@"public override void Save\(IniFeatureStore store\)\s*\{\s*// A row marked Not read has no editor, and is left as the server has it\.\s*if \(box_ == null\)\s*return;"), number);
         Assert.Matches(new Regex(@"public override void Save\(IniFeatureStore store\)\s*\{\s*// A row marked Not read has no editor, and is left as the server has it\.\s*if \(box_ == null\)\s*return;"), lines);

         Assert.Contains("store.LocalSectionAccess(out _) == IniFeatureStore.FileSectionAccess.Readable", feature, StringComparison.Ordinal);
         Assert.Contains("Compute = () => SendingLimitsNotice_(store_)", feature, StringComparison.Ordinal);
         Assert.Contains("&& FileSectionIsTheServers_(store_)", feature, StringComparison.Ordinal);
         Assert.Contains("ProfileApi.WriteStringChecked(section, key, value, iniPath);", feature, StringComparison.Ordinal);
         Assert.Contains("ProfileApi.WriteSectionLinesChecked(section, lines, iniPath);", feature, StringComparison.Ordinal);
      }

      /// <summary>
      /// Directory authentication (LDAP) and Directory synchronisation read and write the
      /// [LDAP] section, which no COM property carries, from the hMailServer.ini on this
      /// computer - only where it is the server's (release review part B's rule for CPB-1,
      /// found on these two pages by the adversarial check of rf5-rf6-cp). A Control Panel
      /// on a workstation, whose installer leaves a readable file without [LDAP], signed in
      /// to a server elsewhere, showed that file's defaults as the server's, saved edits into
      /// it, and Directory synchronisation's state card said directory access was switched
      /// off. With a readable file here and the session to another computer, neither page
      /// reads the file or writes it, each says why under its header, and each state card
      /// says it cannot tell; the schedule record reads no file then either.
      /// </summary>
      [Fact]
      public void TheDirectoryPages_ReadAndWriteNoLdapSectionHere_WhenTheServerIsOnAnotherComputer()
      {
         string path = Path.Join(Path.GetTempPath(), "cp-remote-ini-" + Guid.NewGuid().ToString("N") + ".ini");
         File.WriteAllText(path, "[Settings]\r\n");
         try
         {
            var store = new IniFeatureStore(path);
            IniFeatureStore.SessionHost = "mail.example.com";
            Assert.True(store.IsAvailable);
            Assert.False(store.IsAvailable && IniFeatureStore.ServerIsOnThisComputer);
            IniFeatureStore.SessionHost = null;
            Assert.True(store.IsAvailable && IniFeatureStore.ServerIsOnThisComputer);
         }
         finally
         {
            File.Delete(path);
         }

         const string serversFile = "private bool ServersFile_ => store_.IsAvailable && IniFeatureStore.ServerIsOnThisComputer;";
         foreach (string page in new[] { "LdapSettingsView.cs", "DirectorySyncView.cs" })
         {
            string view = ReadControlPanelFile(Path.Join("Views", page));
            Assert.Contains(serversFile, view, StringComparison.Ordinal);

            string read = Member(view, "private string IniRead_(string key, string fallback)");
            Assert.Matches(new Regex(@"^\s*\{\s*if \(!ServersFile_\)\s*return fallback;"), read);
            Assert.Contains("denied_ = ServersFile_ ? store_.ReadDenied() : null;", view, StringComparison.Ordinal);
            Assert.Contains("public bool HasUnsavedChanges => ServersFile_ && baseline_.Differs(EditorValues_());", view, StringComparison.Ordinal);
            Assert.Matches(new Regex(@"private bool Save_\(\)\s*\{\s*if \((denied_ != null \|\| !ServersFile_|!ServersFile_ \|\| denied_ != null)\)\s*return false;"), view);
            Assert.Contains("if (ServersFile_", view, StringComparison.Ordinal);
         }

         string ldap = ReadControlPanelFile(Path.Join("Views", "LdapSettingsView.cs"));
         Assert.Matches(new Regex(@"else if \(store_\.IsAvailable\)\s*saveState_\.Settle\(SettingsPageState\.Unavailable, L\(""This Control Panel is signed in to a server on another computer: these settings are kept in the \[LDAP\] section of the hMailServer\.ini there, and the file here is this computer's own\."), ldap);
         Assert.Matches(new Regex(@"if \(denied_ == null && !ServersFile_\)\s*\{\s*string unknown = L\(""Cannot tell from here: it is the \[LDAP\] section of the server's own hMailServer.ini that decides, and this Control Panel cannot read it.""\);"), ldap);
         Assert.Matches(new Regex(@"if \(!ServersFile_\)\s*throw new InvalidOperationException\(L\(""This Control Panel is signed in to a server on another computer:"), ldap);

         string sync = ReadControlPanelFile(Path.Join("Views", "DirectorySyncView.cs"));
         Assert.Contains("bool editable = ServersFile_ && denied_ == null;", sync, StringComparison.Ordinal);
         Assert.Contains("private bool SelectionUnsaved_ => ServersFile_ && denied_ == null && selectionBaseline_.Differs(SelectionValues_());", sync, StringComparison.Ordinal);
         Assert.Contains("saveState_.Settle(SettingsPageState.Unavailable, L(\"These selections are kept in the [LDAP] section of the hMailServer.ini on the server's computer, and the file here is this computer's own, so they cannot be edited here.", sync, StringComparison.Ordinal);
         string state = Member(sync, "private void RefreshState_()");
         int unknown = state.IndexOf("State_(StatusLevel.Information, L(\"Cannot tell from here. The [LDAP] section that decides whether a run can happen is kept in the hMailServer.ini on the server's computer, and the file here is this computer's own.", StringComparison.Ordinal);
         int enabled = state.IndexOf("bool ldapOn = string.Equals(IniRead_(\"Enabled\", \"0\"), \"1\", StringComparison.Ordinal);", StringComparison.Ordinal);
         Assert.True(unknown > 0 && enabled > unknown, "Directory synchronisation's state card reads [LDAP] Enabled before it asks whether the file here is the server's.");

         string schedule = ReadControlPanelFile(Path.Join("Services", "DirectorySyncSchedule.Session.cs"));
         Assert.Matches(new Regex(@"public void ReadFile\(string iniPath\)\s*\{\s*if \(IniFeatureStore\.ServerIsOnThisComputer\)\s*Read\("), schedule);
         Assert.Contains("=> IniFeatureStore.ServerIsOnThisComputer && Saved(ServerStarted(), EnabledIn_(iniPath), MinutesIn_(iniPath));", schedule, StringComparison.Ordinal);
      }

      /// <summary>
      /// The CHANGELOG says which file a Control Panel signed in to a server on another
      /// computer read the [Database] rows from: this computer's own, which the installer's
      /// Control Panel component leaves - where the corrected entry said "that computer's",
      /// which reads as the server's (the adversarial check of rf5-rf6-cp) - and it says what
      /// the two Directory pages do there now.
      /// </summary>
      [Fact]
      public void TheChangelog_NamesThisComputersFile_AsTheOneReadOnAnotherComputer()
      {
         string changelog = null;
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null && changelog == null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "CHANGELOG.md");
            if (File.Exists(candidate) && Directory.Exists(Path.Join(directory.FullName, "hmailserver")))
               changelog = candidate;
         }

         Assert.True(changelog != null, "CHANGELOG.md could not be found above the test binaries.");
         string text = Regex.Replace(File.ReadAllText(changelog), @"\s+", " ");
         Assert.Contains("One signed in to a server on another computer read them from this computer's own hMailServer.ini, which the installer's Control Panel component leaves,", text, StringComparison.Ordinal);
         Assert.DoesNotContain("read them from that computer's own hMailServer.ini", text, StringComparison.Ordinal);
         Assert.Contains("**Fixed: Directory authentication (LDAP) and Directory synchronisation read and wrote the [LDAP] section of this computer's hMailServer.ini when signed in to a server on another computer.**", text, StringComparison.Ordinal);
      }

      private static string Member(string source, string signature)
      {
         string body = WithheldRowTests.MemberText(source, signature);
         Assert.True(body != null, "'" + signature + "' is gone.");
         return body;
      }

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         throw new FileNotFoundException(relative);
      }
   }
}
