// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Directory authentication page saved the [LDAP] section switched on with the
   /// host name or the search base empty, without a question, and the server reads it
   /// within two seconds (review finding F130). These hold when the page asks, what it
   /// says for each case, and that it asks before it writes anything.
   /// </summary>
   public class LdapIncompleteSaveTests
   {
      [Theory]
      [InlineData(true, true, true, true)]
      [InlineData(true, true, false, true)]
      [InlineData(true, false, true, true)]
      [InlineData(true, false, false, false)]  // complete: nothing to ask
      [InlineData(false, true, true, false)]   // off: filled in at leisure, as the state card invites
      public void ASave_AsksFirst_OnlyWhenItSwitchesOnWhatIsIncomplete(bool enabled, bool serverMissing, bool searchBaseMissing, bool asks)
         => Assert.Equal(asks, LdapIncompleteSave.AsksFirst(enabled, serverMissing, searchBaseMissing));

      [Fact]
      public void TheQuestion_NamesWhatIsMissing_ByItsCaption_AndWhatHappensMeanwhile()
      {
         string both = LdapIncompleteSave.Question(true, true);
         string server = LdapIncompleteSave.Question(true, false);
         string searchBase = LdapIncompleteSave.Question(false, true);

         Assert.Contains("Directory server host name", both, StringComparison.Ordinal);
         Assert.Contains("Search base", both, StringComparison.Ordinal);
         Assert.Contains("Directory server host name", server, StringComparison.Ordinal);
         Assert.DoesNotContain("Search base", server, StringComparison.Ordinal);
         Assert.Contains("Search base", searchBase, StringComparison.Ordinal);
         Assert.DoesNotContain("Directory server host name", searchBase, StringComparison.Ordinal);

         foreach (string question in new[] { both, server, searchBase })
         {
            Assert.Contains("Windows logon", question, StringComparison.Ordinal);
            Assert.EndsWith("Save it anyway?", question, StringComparison.Ordinal);
         }

         Assert.Null(LdapIncompleteSave.Question(false, false));
      }

      [Fact]
      public void ThePage_AsksBeforeItWritesTheSection_WithCancelForEnter()
      {
         string source = File.ReadAllText(ControlPanelFile_(Path.Join("Views", "LdapSettingsView.cs")));
         string save = WithheldRowTests.MemberText(source, "private bool Save_()");
         Assert.True(save != null, "LdapSettingsView.Save_ is gone.");

         int asks = save.IndexOf("LdapIncompleteSave.AsksFirst(config.Enabled, serverMissing, searchBaseMissing)", StringComparison.Ordinal);
         int writes = save.IndexOf("IniWrite_(\"Enabled\"", StringComparison.Ordinal);
         Assert.True(asks > 0 && writes > asks, "Save_ no longer asks about an incomplete configuration before it writes the section.");
         string question = save[asks..writes];
         Assert.Contains("L(\"_Save anyway\")", question, StringComparison.Ordinal);
         Assert.Contains("cancelByDefault: true, level: StatusLevel.Warning", question, StringComparison.Ordinal);
         Assert.Contains("return false;", question, StringComparison.Ordinal);
         Assert.Contains("config.UsesSearch() && config.SearchBase.Length == 0", save, StringComparison.Ordinal);
      }

      private static string ControlPanelFile_(string relative)
      {
         for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
         }

         throw new FileNotFoundException("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
      }
   }
}
