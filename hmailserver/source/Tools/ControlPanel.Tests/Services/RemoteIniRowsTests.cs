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

      public void Dispose()
      {
         IniFeatureStore.ComReadSetting = savedRead_;
         IniFeatureStore.ComWriteSetting = savedWrite_;
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

         string database = Member(view, "private class SectionIniNumber : IniNumber");
         Assert.Contains("protected override bool Reachable => IniStore != null && IniStore.IsAvailable;", database, StringComparison.Ordinal);
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

         // The reason: no file on this computer, or one this user may not read.
         string database = Member(view, "private class SectionIniNumber : IniNumber");
         string unreadable = Member(database, "public string Unreadable()");
         Assert.Matches(new Regex(@"if \(!Reachable\)\s*return L\(""The settings marked Not read are kept in the \[Database\] section of hMailServer\.ini, which was not found on this computer, and no COM property carries them\."), unreadable);
         Assert.Matches(new Regex(@"string denied = IniStore\.ReadDenied\(\);\s*return denied == null\s*\? null\s*: F\(""The settings marked Not read are kept in the \[Database\] section of hMailServer\.ini, which could not be read: \{0\} "), unreadable);
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
