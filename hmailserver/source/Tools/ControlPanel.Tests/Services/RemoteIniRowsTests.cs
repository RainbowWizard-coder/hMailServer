// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
   /// the edit written to it with no file on this computer - by running the store.
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
