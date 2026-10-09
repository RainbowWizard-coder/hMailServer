// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The hMailServer.ini pages say when their values are the file's. A read the
   /// server could not answer fell back to hMailServer.ini on this computer without
   /// telling the page, which then looked as it always did - "Editing ...", Save
   /// enabled - with the file's values, which are the server's cache and may not be
   /// what it runs. IniFeatureStore now remembers the fall-back and its cause, and
   /// FeatureSettingsView says so, in a pill and a notice at the cause's level, and
   /// holds Save.
   /// </summary>
   public class IniFallbackTests : IDisposable
   {
      private readonly Func<string, string> savedRead_ = IniFeatureStore.ComReadSetting;

      public void Dispose() => IniFeatureStore.ComReadSetting = savedRead_;

      [Fact]
      public void AReadTheServerCouldNotAnswer_IsRememberedWithItsCause()
      {
         var lost = new COMException("The RPC server is unavailable.", unchecked((int)0x800706BA));
         IniFeatureStore.ComReadSetting = _ => throw lost;

         var store = new IniFeatureStore();
         Assert.Null(store.FallbackCause);

         store.Read("RestApiPort", "0");
         store.Read("RestApiBindAddress", "127.0.0.1");
         Assert.Same(lost, store.FallbackCause);

         // A lost link is said at Critical, as the shell's pill and every other page say it.
         Assert.Equal(StatusLevel.Critical, ComErrorText.LevelFor(store.FallbackCause));

         // A page that reads again starts a new count.
         store.ForgetFallback();
         Assert.Null(store.FallbackCause);
      }

      /// <summary>
      /// A server older than GetIniSetting answered: it has no such member. The page
      /// falls back as it does for a lost link, and says so as a Warning, the level of
      /// a server that answered but could not give the values.
      /// </summary>
      [Fact]
      public void AServerWithoutGetIniSetting_FallsBackAtWarning()
      {
         IniFeatureStore.ComReadSetting = _ => throw new COMException("Unknown name.", unchecked((int)0x80020006));

         var store = new IniFeatureStore();
         store.Read("RestApiPort", "0");
         Assert.NotNull(store.FallbackCause);
         Assert.Equal(StatusLevel.Warning, ComErrorText.LevelFor(store.FallbackCause));
      }

      [Fact]
      public void AReadTheServerAnswered_IsNoFallBack()
      {
         IniFeatureStore.ComReadSetting = key => key == "RestApiPort" ? "8081" : "";

         var store = new IniFeatureStore();
         Assert.Equal("8081", store.Read("RestApiPort", "0"));
         Assert.Equal("default", store.Read("Unset", "default"));
         Assert.Null(store.FallbackCause);
      }

      /// <summary>
      /// The page reads afresh each build, says a fall-back in its pill and its notice
      /// at the level its cause has - Connection lost at Critical for a lost link, Read
      /// from file at Warning for an older server - and holds Save, refuses a save it
      /// cannot make, and no
      /// longer claims to edit the file: its line says the settings database, and its
      /// subtitles do not name hMailServer.INI.
      /// </summary>
      [Fact]
      public void TheIniPages_SayTheirValuesAreTheFilesAndHoldSave()
      {
         string source = File.ReadAllText(ViewPath("FeatureSettingsView.xaml.cs"));

         string build = WithheldRowTests.MemberText(source, "private void BuildUi()");
         Assert.True(build != null, "FeatureSettingsView.xaml.cs no longer has BuildUi.");
         int forget = build.IndexOf("store_.ForgetFallback();", StringComparison.Ordinal);
         int editors = build.IndexOf("setting.CreateEditor(store_)", StringComparison.Ordinal);
         Assert.True(forget >= 0 && forget < editors, "BuildUi does not forget the last build's fall-back before it reads again.");

         // The pill and the notice are in the state the cause makes, at that state's
         // level, which is the cause's: one lost link, one level, on every page and in
         // both places on this one - no level of the notice's own beside the pill's.
         Match fallBack = Regex.Match(build, @"if \(store_\.FallbackCause != null\)\s*\{.*?return;", RegexOptions.Singleline);
         Assert.True(fallBack.Success, "BuildUi has no fall-back branch.");
         Match state = Regex.Match(fallBack.Value, @"SaveButton\.IsEnabled = false;\s*SetState_\(SettingsPageStatus\.ForFallback\(store_\.FallbackCause\),(.*?)\);\s*return;", RegexOptions.Singleline);
         Assert.True(state.Success, "The fall-back's pill is not in the state its cause makes.");
         Assert.DoesNotContain("StatusLevel.", fallBack.Value, StringComparison.Ordinal);
         Assert.DoesNotContain("LevelFor(", state.Groups[1].Value, StringComparison.Ordinal);
         Assert.DoesNotContain("SettingsPageState.NotConnected", fallBack.Value, StringComparison.Ordinal);
         Assert.Contains("ServerSession.DescribeComError(store_.FallbackCause)", build, StringComparison.Ordinal);
         Assert.Contains("L(\"Stored in the server's settings database.\")", build, StringComparison.Ordinal);
         Assert.DoesNotContain("Editing {0}", source, StringComparison.Ordinal);

         string save = WithheldRowTests.MemberText(source, "private bool Save_()");
         Assert.Matches(new Regex(@"if \(store_\.FallbackCause != null\)\s*return false;"), save);

         Assert.Empty(Regex.Matches(source, @"Header\.Subtitle = L\(""[^""]*\(hMailServer\.INI\)").Select(m => m.Value));
      }

      private static string ViewPath(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
