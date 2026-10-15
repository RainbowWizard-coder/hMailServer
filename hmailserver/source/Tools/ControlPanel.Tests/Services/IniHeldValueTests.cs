// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// A [Settings] value the server holds as empty is shown as the server holds it, not
   /// as the page's default (release review finding CP-4). GetIniSetting answers "" for
   /// an absent key and for one stored empty alike, and the store took "" for the
   /// caller's default, while the server reads a stored empty value as empty: a
   /// ScriptAllowedObjects held empty - which lets a script create no object - was shown
   /// as "*" under the warning that any object may be created, an empty AcmeDirectoryUrl
   /// as Let's Encrypt's, and a number held empty as its default where the loader makes 0
   /// of it. The server's list of the names it holds (Settings.IniSettingNames) tells the
   /// two apart; the store is run against a fake of it.
   /// </summary>
   public class IniHeldValueTests : IDisposable
   {
      private readonly Func<string, string> savedRead_ = IniFeatureStore.ComReadSetting;
      private readonly Action<string, string> savedWrite_ = IniFeatureStore.ComWriteSetting;
      private readonly Func<string> savedNames_ = IniFeatureStore.ComSettingNames;

      public IniHeldValueTests() => IniFeatureStore.ForgetApplies();

      public void Dispose()
      {
         IniFeatureStore.ComReadSetting = savedRead_;
         IniFeatureStore.ComWriteSetting = savedWrite_;
         IniFeatureStore.ComSettingNames = savedNames_;
         IniFeatureStore.ForgetApplies();
      }

      private static Dictionary<string, string> Server_(int[] namesAsked)
      {
         var server = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
         {
            ["ScriptAllowedObjects"] = "",
            ["AcmeDirectoryUrl"] = "",
            ["ScriptTimeout"] = "",
            ["IMAPCompressionEnabled"] = "",
            ["OtelServiceName"] = "mail-1",
            ["OAuth2UsernameClaim"] = ""
         };
         IniFeatureStore.ComReadSetting = key => server.TryGetValue(key, out string value) ? value : "";
         IniFeatureStore.ComSettingNames = () =>
         {
            namesAsked[0]++;
            return string.Join("\r\n", server.Keys);
         };
         return server;
      }

      [Fact]
      public void AKeyTheServerHoldsEmpty_IsReadEmpty_AndAnAbsentOneAsTheDefault()
      {
         var asked = new int[1];
         Server_(asked);
         var store = new IniFeatureStore(iniPath: null);

         Assert.Equal("", store.ReadHeld("ScriptAllowedObjects", "*", out bool held));
         Assert.True(held);
         Assert.Equal("", store.ReadHeld("AcmeDirectoryUrl", "https://acme-v02.api.letsencrypt.org/directory", out held));
         Assert.True(held);

         // Absent: the server's default, which is what it uses.
         Assert.Equal("*", store.ReadHeld("ScriptAllowedObjectsNotStored", "*", out held));
         Assert.False(held);

         // A value is the value, and the list is not asked for it.
         int before = asked[0];
         Assert.Equal("mail-1", store.ReadHeld("OtelServiceName", "hmailserver", out held));
         Assert.False(held);
         Assert.Equal(before, asked[0]);

         // Read keeps its meaning - "" is the caller's default - for the readers that want it.
         Assert.Equal("*", store.Read("ScriptAllowedObjects", "*"));
      }

      /// <summary>A number or a switch held empty is 0 to the server's loader (_ttoi), whatever the default.</summary>
      [Fact]
      public void ANumberOrASwitchHeldEmpty_IsWhatTheLoaderMakesOfIt()
      {
         Server_(new int[1]);
         var store = new IniFeatureStore(iniPath: null);

         Assert.Equal("0", store.ReadNumber("ScriptTimeout", "60"));
         Assert.False(store.ReadBool("IMAPCompressionEnabled", true));

         // Absent: the default.
         Assert.Equal("60", store.ReadNumber("DBConnectionAcquireTimeout", "60"));
         Assert.True(store.ReadBool("IMAPSort", true));
      }

      /// <summary>Where the list cannot be asked for - an older server, a credential refused it - the default is answered, as before.</summary>
      [Fact]
      public void WithoutTheList_TheDefaultIsAnswered()
      {
         Server_(new int[1]);
         IniFeatureStore.ComSettingNames = () => throw new InvalidOperationException("Access denied");
         var store = new IniFeatureStore(iniPath: null);

         Assert.Equal("*", store.ReadHeld("ScriptAllowedObjects", "*", out bool held));
         Assert.False(held);

         IniFeatureStore.ComSettingNames = null;
         Assert.Equal("*", store.ReadHeld("ScriptAllowedObjects", "*", out held));
         Assert.False(held);
      }

      /// <summary>One read of the list serves a page's rows, and a write sends the next row to the server again.</summary>
      [Fact]
      public void TheList_IsReadOnceForAPage_AndAgainAfterAWrite()
      {
         var asked = new int[1];
         Dictionary<string, string> server = Server_(asked);
         IniFeatureStore.ComWriteSetting = (key, value) => server[key] = value;
         var store = new IniFeatureStore(iniPath: null);

         store.ReadHeld("ScriptAllowedObjects", "*", out _);
         store.ReadHeld("AcmeDirectoryUrl", "x", out _);
         store.ReadNumber("ScriptTimeout", "60");
         Assert.Equal(1, asked[0]);

         store.Write("UpdateChannel", "");
         Assert.Equal("", store.ReadHeld("UpdateChannel", "stable", out bool held));
         Assert.True(held);
         Assert.Equal(2, asked[0]);
      }

      /// <summary>The file, read with no session, tells the two apart itself: GetPrivateProfileString answers "" for "Key=".</summary>
      [Fact]
      public void TheFile_TellsAKeyHeldEmptyFromAnAbsentOne()
      {
         IniFeatureStore.ComReadSetting = null;
         IniFeatureStore.ComSettingNames = null;
         string path = Path.Join(Path.GetTempPath(), "cp-held-" + Guid.NewGuid().ToString("N") + ".ini");
         File.WriteAllText(path, "[Settings]\r\nScriptAllowedObjects=\r\nScriptTimeout=\r\n");
         try
         {
            var store = new IniFeatureStore(path);
            Assert.Equal("", store.ReadHeld("ScriptAllowedObjects", "*", out bool held));
            Assert.True(held);
            Assert.Equal("*", store.ReadHeld("AbsentKey", "*", out held));
            Assert.False(held);
            Assert.Equal("0", store.ReadNumber("ScriptTimeout", "60"));
         }
         finally
         {
            File.Delete(path);
         }
      }

      /// <summary>
      /// The rows read through it: the text rows of both settings views show a key held
      /// empty as empty, said in their placeholder (with what the server uses for it, where
      /// that is not the empty value), and the number rows the 0 the loader
      /// makes of it; ScriptAllowedObjects says again what empty means, its warning
      /// follows a box that starts at what the server holds, and the store no longer
      /// says that an empty value deletes the key.
      /// </summary>
      [Fact]
      public void TheRows_ReadWhatTheServerHolds_AndScriptAllowedObjectsSaysWhatEmptyMeans()
      {
         string server = ReadControlPanelFile_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         string iniText = WithheldRowTests.MemberText(server, "private class IniText : ComSetting, IIniSetting");
         Assert.Contains("current = IniStore.ReadHeld(Path, Default, out heldEmpty);", iniText, StringComparison.Ordinal);
         Assert.Contains("heldEmpty ? IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans) : ExamplePlaceholder.Shown(Placeholder)", iniText, StringComparison.Ordinal);
         Assert.Contains("protected virtual string ReadRaw() => IniStore.ReadNumber(Path, Default.ToString());", server, StringComparison.Ordinal);
         Assert.Contains("Label = L(\"COM objects a script may create (empty = none beyond hMailServer's own)\"),", server, StringComparison.Ordinal);

         string feature = ReadControlPanelFile_(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Contains("string current = store.ReadHeld(Key, Default, out bool heldEmpty);", feature, StringComparison.Ordinal);
         Assert.Contains("? store.ReadNumber(Key, Default)", feature, StringComparison.Ordinal);

         string store = ReadControlPanelFile_(Path.Join("Services", "IniFeatureStore.cs"));
         Assert.DoesNotContain("An empty value DELETES the key", store, StringComparison.Ordinal);
      }

      /// <summary>
      /// A key whose server reader puts a value of its own in the place of an empty one is
      /// shown with it: OtelServiceName held empty is the service name "hmailserver" to the
      /// three exporters, and its box says "(empty = hmailserver)", where it said "(empty)"
      /// as for a key the server reads as empty (the adversarial check of rf5-rf6-cp). A
      /// key the server reads as empty keeps "(empty)".
      /// </summary>
      [Fact]
      public void AKeyWhoseServerReaderUsesItsDefaultForEmpty_IsShownWithTheDefault()
      {
         var asked = new int[1];
         Dictionary<string, string> server = Server_(asked);
         server["OtelServiceName"] = "";
         var store = new IniFeatureStore(iniPath: null);

         Assert.Equal("", store.ReadHeld("OtelServiceName", "hmailserver", out bool held));
         Assert.True(held);
         Assert.Equal("(empty = hmailserver)", IniFeatureStore.HeldEmptyPlaceholder("hmailserver"));
         Assert.Equal("", store.ReadHeld("OAuth2UsernameClaim", "email", out held));
         Assert.True(held);
         Assert.Equal("(empty = email)", IniFeatureStore.HeldEmptyPlaceholder("email"));

         // The server uses the empty value: "(empty)".
         Assert.Equal("(empty)", IniFeatureStore.HeldEmptyPlaceholder(null));
         Assert.Equal("(empty)", IniFeatureStore.HeldEmptyPlaceholder(""));
      }

      /// <summary>
      /// The rows of the four keys whose server reader puts its default in the place of an
      /// empty value - OtelServiceName (OtelTracer, OtelLogExporter, OtelMetricsExporter),
      /// OAuth2UsernameClaim (OAuth2TokenValidator::LoadConfig), MtaStsPolicyMode
      /// (WebServicesServer, anything but enforce, testing and none being enforce) and
      /// UpdateChannel (UpdateChecker::IsPreReleaseChannel) - say what the server uses, and
      /// the three row kinds that show a held value use it: MtaStsPolicyMode held empty is
      /// its "enforce" choice, where the list offered "(empty)" while the server served
      /// mode: enforce.
      /// </summary>
      [Fact]
      public void TheRowsOfTheKeysWhoseEmptyIsTheDefault_SayWhatTheServerUses()
      {
         string server = ReadControlPanelFile_(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("new IniText { Path = \"OtelServiceName\", Label = L(\"OpenTelemetry service name\"), Default = \"hmailserver\", EmptyMeans = \"hmailserver\", IniStore = iniStore_ });", server, StringComparison.Ordinal);

         string feature = ReadControlPanelFile_(Path.Join("Views", "FeatureSettingsView.xaml.cs"));
         Assert.Contains("new TextSetting { Key = \"UpdateChannel\", Default = \"stable\", EmptyMeans = \"stable\",", feature, StringComparison.Ordinal);
         Assert.Contains("new TextSetting { Key = \"OAuth2UsernameClaim\", DependsOn = OAuth2Switch, Default = \"email\", EmptyMeans = \"email\",", feature, StringComparison.Ordinal);
         Assert.Matches(new System.Text.RegularExpressions.Regex(@"Key = ""MtaStsPolicyMode"",\s*Default = ""enforce"",\s*EmptyMeans = ""enforce"","), feature);

         string text = WithheldRowTests.MemberText(feature, "private class TextSetting : Setting");
         Assert.Contains("PlaceholderText = heldEmpty ? IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans) : ExamplePlaceholder.Shown(Placeholder),", text, StringComparison.Ordinal);
         string word = WithheldRowTests.MemberText(feature, "private class WordSetting : Setting");
         int choice = word.IndexOf("if (chosen == null && heldEmpty && EmptyMeans != null)", StringComparison.Ordinal);
         int itself = word.IndexOf("chosen = new ComboBoxItem { Content = IniFeatureStore.HeldEmptyPlaceholder(EmptyMeans), Tag = \"\" };", StringComparison.Ordinal);
         Assert.True(choice > 0 && itself > choice, "MtaStsPolicyMode held empty is not the choice the server uses.");
      }

      private static string ReadControlPanelFile_(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException(relative);
      }
   }
}
