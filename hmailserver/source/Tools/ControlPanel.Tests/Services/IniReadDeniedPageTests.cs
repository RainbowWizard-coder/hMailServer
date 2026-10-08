// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The two pages that read the [LDAP] section of hMailServer.ini straight from
   /// the file say so when the file cannot be read, rather than showing its
   /// fallbacks as the configuration. From 6.3.6 the file belongs to SYSTEM, the
   /// Administrators and the service (hMailServer.exe /ProtectFolders), so a
   /// Control Panel not run as administrator reads every value as its fallback:
   /// Directory authentication then showed the switch off and every field empty,
   /// and Directory synchronisation said "directory access is switched off" - of a
   /// directory that was on. Both pages now ask IniFeatureStore.ReadDenied() before
   /// they read anything, as the Live logs page does, and the synchronisation page
   /// takes the denied file through the branch that says it cannot tell, never the
   /// one that says the switch is off.
   ///
   /// Scanned rather than rendered, as UnreadRowTests is: this assembly has no WPF.
   /// Each test fails against the tree before the fix.
   /// </summary>
   public class IniReadDeniedPageTests
   {
      [Theory]
      [InlineData("LdapSettingsView.cs")]
      [InlineData("DirectorySyncView.cs")]
      public void ThePage_AsksWhetherTheFileCanBeRead_BeforeItReadsAnything(string file)
      {
         string source = ReadView(file);
         string load = WithheldRowTests.MemberText(source, "private void Load_()");
         Assert.True(load != null, file + " no longer has Load_.");

         int asks = load.IndexOf("store_.ReadDenied()", StringComparison.Ordinal);
         int reads = load.IndexOf("IniRead_(", StringComparison.Ordinal);
         Assert.True(asks >= 0, file + ".Load_ never asks whether hMailServer.ini can be read, so a file it cannot read answers every read with its fallback.");
         Assert.True(reads < 0 || asks < reads, file + ".Load_ reads a value before it asks whether the file can be read.");

         // The failure is said in a Critical notice, the editors are not filled,
         // and Save waits.
         Assert.True(source.Contains("deniedNotice_", StringComparison.Ordinal) && source.Contains("StatusLevel.Critical", StringComparison.Ordinal),
            file + " has no Critical notice for a file that cannot be read.");
         Assert.True(source.Contains("hMailServer.ini could not be read: {0}", StringComparison.Ordinal),
            file + " does not say that hMailServer.ini could not be read.");

         string save = WithheldRowTests.MemberText(source, "private void Save_()");
         Assert.True(save != null && save.Contains("denied_ != null", StringComparison.Ordinal), file + ".Save_ writes a file it could not read.");
      }

      [Fact]
      public void DirectorySynchronisation_NeverSaysTheSwitchIsOff_WhenTheFileCouldNotBeRead()
      {
         string state = WithheldRowTests.MemberText(ReadView("DirectorySyncView.cs"), "private void RefreshState_()");
         Assert.True(state != null, "DirectorySyncView.RefreshState_ is gone.");

         int denied = state.IndexOf("denied_ != null", StringComparison.Ordinal);
         int reads = state.IndexOf("IniRead_(\"Enabled\"", StringComparison.Ordinal);
         Assert.True(denied >= 0, "RefreshState_ does not route a file that cannot be read through the branch that says it cannot tell.");
         Assert.True(reads < 0 || denied < reads, "RefreshState_ reads Enabled, and so can say \"switched off\", before it asks whether the file could be read.");
      }

      [Fact]
      public void DirectoryAuthentication_SaysItCannotTell_WhenTheFileCouldNotBeRead()
      {
         string state = WithheldRowTests.MemberText(ReadView("LdapSettingsView.cs"), "private void RefreshState_()");
         Assert.True(state != null, "LdapSettingsView.RefreshState_ is gone.");

         int denied = state.IndexOf("denied_ != null", StringComparison.Ordinal);
         int collects = state.IndexOf("CollectSnapshot_()", StringComparison.Ordinal);
         Assert.True(denied >= 0 && (collects < 0 || denied < collects),
            "RefreshState_ judges the editors, and so can say \"Off\", before it asks whether the file could be read.");
      }

      private static string ReadView(string file)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", file);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/" + file + " could not be found from the test binaries.");
         return null;
      }
   }
}
