// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text.Json;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The permission rule the Control Panel applies before it asks Windows for the
   /// elevation of an administrator's install of an update (InstallationPermissions),
   /// against the one table of cases the elevated stage is held to as well:
   /// build/testdata/install-permissions-cases.json. And the C++ tester's copy of that
   /// table (Common/Util/AdminUpdateTester.cpp) held to the file case for case, so
   /// that the two implementations of the rule cannot drift apart unseen.
   /// </summary>
   public class InstallationPermissionsTests
   {
      private sealed class Case
      {
         public string Name;
         public string Place;
         public bool Pass;
         public string Sid;
         public bool Owner;
         public bool Inherited;
         public string Sddl;
      }

      private static string Root()
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            if (File.Exists(Path.Combine(at.FullName, "build", "testdata", "install-permissions-cases.json")))
               return at.FullName;
         }
         throw new DirectoryNotFoundException("the repository is not above " + AppContext.BaseDirectory);
      }

      private static List<Case> Cases()
      {
         using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(Root(), "build", "testdata", "install-permissions-cases.json")));
         var cases = new List<Case>();
         foreach (JsonElement item in document.RootElement.GetProperty("cases").EnumerateArray())
         {
            cases.Add(new Case
            {
               Name = item.GetProperty("name").GetString(),
               Place = item.GetProperty("place").GetString(),
               Pass = item.GetProperty("pass").GetBoolean(),
               Sid = item.TryGetProperty("sid", out JsonElement sid) ? sid.GetString() : "",
               Owner = item.TryGetProperty("owner", out JsonElement owner) && owner.GetBoolean(),
               Inherited = item.TryGetProperty("inherited", out JsonElement inherited) && inherited.GetBoolean(),
               Sddl = item.GetProperty("sddl").GetString()
            });
         }
         return cases;
      }

      public static IEnumerable<object[]> CaseNames() => Cases().Select(c => new object[] { c.Name });

      [Theory]
      [MemberData(nameof(CaseNames))]
      public void EachCaseOfTheTableIsJudgedAsItSays(string name)
      {
         Case item = Cases().Single(c => c.Name == name);
         var place = item.Place == "above" ? InstallationPermissions.Place.AboveApp : InstallationPermissions.Place.Inside;
         InstallationPermissions.Finding finding = InstallationPermissions.Evaluate(new RawSecurityDescriptor(item.Sddl), place);

         Assert.Equal(!item.Pass, finding.Failed);
         if (!item.Pass)
         {
            Assert.Equal(item.Sid, finding.Sid, ignoreCase: true);
            Assert.Equal(item.Owner, finding.Owner);
            Assert.Equal(item.Inherited, finding.Inherited);
            Assert.False(string.IsNullOrEmpty(finding.Principal));
            Assert.False(string.IsNullOrEmpty(finding.Right));
         }
      }

      [Fact]
      public void TheTableHoldsBothPassingAndFailingCasesForBothPlaces()
      {
         List<Case> cases = Cases();
         Assert.Contains(cases, c => c.Place == "above" && c.Pass);
         Assert.Contains(cases, c => c.Place == "above" && !c.Pass);
         Assert.Contains(cases, c => c.Place == "inside" && c.Pass);
         Assert.Contains(cases, c => c.Place == "inside" && !c.Pass);
         Assert.Contains(cases, c => c.Owner);
         Assert.Contains(cases, c => c.Inherited);
      }

      [Fact]
      public void TheCppTesterCarriesTheSameCasesInTheSameOrder()
      {
         string tester = File.ReadAllText(Path.Combine(Root(), "hmailserver", "source", "Server", "Common", "Util", "AdminUpdateTester.cpp"));
         int begin = tester.IndexOf("// PERMISSION-CASES-BEGIN", StringComparison.Ordinal);
         int end = tester.IndexOf("// PERMISSION-CASES-END", StringComparison.Ordinal);
         Assert.True(begin >= 0 && end > begin, "AdminUpdateTester.cpp has no marked table of cases");
         var row = new Regex(@"\{ L""([^""]+)"", AdminUpdateChecks::(PlaceInside|PlaceAboveApp), (true|false), L""([^""]*)"", (true|false), (true|false), L""([^""]+)"" \}");
         var rows = row.Matches(tester.Substring(begin, end - begin)).Cast<Match>().ToList();
         List<Case> cases = Cases();
         Assert.Equal(cases.Count, rows.Count);
         for (int i = 0; i < cases.Count; i++)
         {
            Match m = rows[i];
            Assert.Equal(cases[i].Name, m.Groups[1].Value);
            Assert.Equal(cases[i].Place == "above" ? "PlaceAboveApp" : "PlaceInside", m.Groups[2].Value);
            Assert.Equal(cases[i].Pass ? "true" : "false", m.Groups[3].Value);
            Assert.Equal(cases[i].Sid, m.Groups[4].Value);
            Assert.Equal(cases[i].Owner ? "true" : "false", m.Groups[5].Value);
            Assert.Equal(cases[i].Inherited ? "true" : "false", m.Groups[6].Value);
            Assert.Equal(cases[i].Sddl, m.Groups[7].Value);
         }
      }

      [Fact]
      public void ACallbackGrantCountsAsAGrant()
      {
         // A condition could be true, so a conditional Modify for Users is a Modify.
         var descriptor = new RawSecurityDescriptor("O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(XA;;0x1301bf;;;BU;(Member_of {SID(BA)}))");
         InstallationPermissions.Finding finding = InstallationPermissions.Evaluate(descriptor, InstallationPermissions.Place.Inside);
         Assert.True(finding.Failed);
         Assert.Equal("S-1-5-32-545", finding.Sid);
      }

      [Fact]
      public void TheTrustedPrincipalsAreSystemTheAdministratorsAndTrustedInstaller()
      {
         Assert.True(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null)));
         Assert.True(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)));
         Assert.True(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(InstallationPermissions.TrustedInstallerSid)));
         Assert.False(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null)));
         Assert.False(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null)));
         Assert.False(InstallationPermissions.IsTrustedSid(new SecurityIdentifier(WellKnownSidType.CreatorOwnerSid, null)));
         Assert.False(InstallationPermissions.IsTrustedSid(new SecurityIdentifier("S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809")));
         Assert.False(InstallationPermissions.IsTrustedSid(null));
      }

      [Fact]
      public void AFolderThisUserOwnsFailsOnItsOwnerWithTheCommandThatGivesItBack()
      {
         // The test's own folder under %TEMP%: owned by the user running the tests,
         // who is not trusted to own what runs elevated.
         string folder = Path.Combine(Path.GetTempPath(), "hm-permissions-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(folder);
         try
         {
            InstallationPermissions.Finding finding = InstallationPermissions.CheckObject(folder, InstallationPermissions.Place.Inside, folder);
            if (new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
               return; // an elevated run owns it as the Administrators: nothing to see
            Assert.True(finding.Failed);
            Assert.True(finding.Owner);
            Assert.Contains("/setowner *S-1-5-32-544", finding.Fix);
         }
         finally
         {
            Directory.Delete(folder, true);
         }
      }

      [Fact]
      public void AnInstallationWithoutBinIsRefusedAndOneThroughALinkIsToo()
      {
         string folder = Path.Combine(Path.GetTempPath(), "hm-permissions-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(folder);
         try
         {
            // Whatever the owner, the rule stops at the first failure; a folder with
            // no Bin, or one reached through a junction, never passes.
            InstallationPermissions.Finding missing = InstallationPermissions.CheckInstallation(Path.Combine(folder, "app"), Array.Empty<string>());
            Assert.True(missing.Failed);

            string real = Path.Combine(folder, "real");
            Directory.CreateDirectory(Path.Combine(real, "Bin"));
            string link = Path.Combine(folder, "link");
            Directory.CreateSymbolicLink(link, real);
            InstallationPermissions.Finding linked = InstallationPermissions.CheckInstallation(Path.Combine(link, "Bin"), Array.Empty<string>());
            Assert.True(linked.Failed);
         }
         catch (IOException)
         {
            // No right to make a link here (neither an administrator nor Developer
            // Mode): the missing-Bin half above has run.
         }
         catch (UnauthorizedAccessException)
         {
         }
         finally
         {
            Directory.Delete(folder, true);
         }
      }
   }
}
