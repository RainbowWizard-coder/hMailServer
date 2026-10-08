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
   /// The Administrator roles page opened its role box on serveradmin and gave it
   /// without a question (review finding F055). These hold which roles reach
   /// everything, that the box opens on no role, and that giving such a role asks
   /// first, with Cancel the answer Enter gives.
   /// </summary>
   public class RoleGrantTests
   {
      [Theory]
      [InlineData("serveradmin", "administer_server,administer_domain,view_configuration", true)]
      [InlineData("ServerAdmin", "", true)]                                    // by its name, whatever the server lists
      [InlineData("operations", "view_configuration, administer_server", true)] // a role some server lists with administer_server (this one refuses to save such a custom role)
      [InlineData("helpdesk", "administer_domain", false)]
      [InlineData("auditor", "view_configuration,view_discovery", false)]
      [InlineData("compliance", "view_discovery", false)]
      [InlineData("almost", "administer_servers,not_administer_server", false)] // a token, not a substring
      [InlineData("nothing", null, false)]
      public void ReachesEverything_IsServerAdminOrAdministerServer(string role, string permissions, bool everything)
         => Assert.Equal(everything, RoleGrant.ReachesEverything(role, permissions));

      [Fact]
      public void TheQuestion_NamesTheAccountAndTheRole()
      {
         string question = RoleGrant.Question("helen@example.test", "serveradmin");
         Assert.Contains("helen@example.test", question, StringComparison.Ordinal);
         Assert.Contains("serveradmin", question, StringComparison.Ordinal);
      }

      [Fact]
      public void TheRoleBox_OpensOnNoRole_AndAServerWideRoleIsAskedAbout()
      {
         string page = ReadRolesView();

         string reload = WithheldRowTests.MemberText(page, "private void Reload_()");
         Assert.True(reload != null, "RolesView.Reload_ is gone.");
         Assert.DoesNotContain("SelectedIndex = 0", reload, StringComparison.Ordinal);

         string assign = WithheldRowTests.MemberText(page, "private void Assign_()");
         Assert.True(assign != null, "RolesView.Assign_ is gone.");
         int asks = assign.IndexOf("RoleGrant.ReachesEverything(", StringComparison.Ordinal);
         int writes = assign.IndexOf("Write_(", StringComparison.Ordinal);
         Assert.True(asks >= 0 && writes > asks, "Give the role gives a role that reaches everything without asking.");
         Assert.Contains("cancelByDefault: true", assign, StringComparison.Ordinal);
      }

      private static string ReadRolesView()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", "Views", "RolesView.cs");
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/Views/RolesView.cs could not be found from the test binaries.");
         return null;
      }
   }
}
