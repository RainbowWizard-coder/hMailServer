// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The IP ranges page shows each delivery direction with its authentication
   /// requirement, in the range dialog's words, and says when a range is an open relay
   /// (review finding F229). The card showed "Deliver remote → remote (relay)" apart
   /// from "Require auth: external to external", which the dialog alone held, so that
   /// unticking relay on the shipped Internet range stopped roaming users sending mail
   /// and ticking it on a range without the requirement made an open relay, with no word
   /// either way; and the page and the dialog had four names for the TLS setting.
   /// </summary>
   [Collection("Loc")]
   public class RelayExposureTests : IDisposable
   {
      public void Dispose() => Loc.Reset();

      [Theory]
      // Relaying that asks for authentication, or is not allowed, is no exposure.
      [InlineData("0.0.0.0", "255.255.255.255", true, true, RelayExposure.Reach.None)]
      [InlineData("0.0.0.0", "255.255.255.255", false, false, RelayExposure.Reach.None)]
      // One address - My computer as shipped - is a choice about one machine.
      [InlineData("127.0.0.1", "127.0.0.1", true, false, RelayExposure.Reach.None)]
      [InlineData("203.0.113.9", "203.0.113.9", true, false, RelayExposure.Reach.None)]
      // A network of its own: every machine of it may relay.
      [InlineData("192.168.1.0", "192.168.1.255", true, false, RelayExposure.Reach.Network)]
      [InlineData("10.0.0.0", "10.255.255.255", true, false, RelayExposure.Reach.Network)]
      [InlineData("172.16.0.1", "172.31.255.254", true, false, RelayExposure.Reach.Network)]
      [InlineData("fe80::", "fe80::ffff", true, false, RelayExposure.Reach.Network)]
      [InlineData("fd00::1", "fd00::ff", true, false, RelayExposure.Reach.Network)]
      // Anything that reaches past one: an open relay.
      [InlineData("0.0.0.0", "255.255.255.255", true, false, RelayExposure.Reach.Internet)]
      [InlineData("10.0.0.0", "11.0.0.0", true, false, RelayExposure.Reach.Internet)]
      [InlineData("172.16.0.0", "172.32.0.0", true, false, RelayExposure.Reach.Internet)]
      [InlineData("203.0.113.0", "203.0.113.255", true, false, RelayExposure.Reach.Internet)]
      [InlineData("10.0.0.1", "fe80::1", true, false, RelayExposure.Reach.Internet)]
      [InlineData("not an address", "10.0.0.9", true, false, RelayExposure.Reach.Internet)]
      public void ARangesReach_IsHowFarItLetsMailBeRelayedWithoutSigningIn(string lower, string upper, bool allow, bool requireAuth, RelayExposure.Reach reach)
         => Assert.Equal(reach, RelayExposure.Of(lower, upper, allow, requireAuth));

      [Fact]
      public void TheNotice_IsCriticalForAnOpenRelay_AndAWarningInsideANetwork()
      {
         Assert.Equal(StatusLevel.Critical, RelayExposure.LevelOf(RelayExposure.Reach.Internet));
         Assert.Equal(StatusLevel.Warning, RelayExposure.LevelOf(RelayExposure.Reach.Network));
         Assert.Null(RelayExposure.Notice(RelayExposure.Reach.None));
         Assert.StartsWith("Anyone in this range can relay mail through this server to any address without signing in: an open relay.", RelayExposure.Notice(RelayExposure.Reach.Internet), StringComparison.Ordinal);
         Assert.StartsWith("Every machine in this range can relay mail through this server", RelayExposure.Notice(RelayExposure.Reach.Network), StringComparison.Ordinal);
         Assert.Equal("Save the permissions of Office? Anyone in it can then relay mail through this server to any address without signing in: an open relay.", RelayExposure.SaveQuestion("Office"));
      }

      /// <summary>
      /// The card: the connections and the TLS box in the dialog's words, then each
      /// direction on a row with Allowed and Only after SMTP authentication, the row's
      /// caption the Allowed box's label; the authentication boxes are read, saved and
      /// cleared with the rest, and an open relay is asked about before it is saved.
      /// </summary>
      [Fact]
      public void TheCard_ShowsEachDirectionWithItsAuthentication_InTheDialogsWords()
      {
         XElement page = XDocument.Parse(Read(Path.Join("Views", "IPRangesView.xaml"))).Root;
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement Named(string name) => page.Descendants().Single(e => (string)e.Attribute(x + "Name") == name);

         Assert.Equal("{loc:L 'Allow SM_TP connections'}", (string)Named("PermSmtp").Attribute("Content"));
         Assert.Equal("{loc:L 'Require TLS _before authentication'}", (string)Named("PermTlsAuth").Attribute("Content"));
         // Each direction's own box allows it - its caption is the direction, which the
         // settings search index reads - and its authentication box is on its row.
         foreach ((string allowed, string authentication, string caption) in new[]
         {
            ("PermLL", "PermAuthLL", "_Local to local"), ("PermLR", "PermAuthLR", "Lo_cal to external"),
            ("PermRL", "PermAuthRL", "E_xternal to local"), ("PermRR", "PermAuthRR", "External to external (rela_y)")
         })
         {
            XElement box = Named(allowed);
            Assert.Equal("{loc:L '" + caption + "'}", (string)box.Attribute("Content"));
            Assert.Equal((string)box.Attribute("Grid.Row"), (string)Named(authentication).Attribute("Grid.Row"));
            Assert.Equal("1", (string)Named(authentication).Attribute("Grid.Column"));
            Assert.Same(Named("DeliveryGrid"), box.Parent);
         }
         Assert.DoesNotMatch(@"\{loc:L '[^']*remote", Read(Path.Join("Views", "IPRangesView.xaml")));

         string code = Read(Path.Join("Views", "IPRangesView.xaml.cs"));
         string boxes = WithheldRowTests.MemberText(code, "private IEnumerable<CheckBox> PermissionBoxes()");
         foreach (string box in new[] { "PermAuthLL", "PermAuthLR", "PermAuthRL", "PermAuthRR" })
            Assert.Contains(box, boxes, StringComparison.Ordinal);

         string shows = WithheldRowTests.MemberText(code, "private void ShowPermissions_()");
         Assert.Contains("PermAuthRR.IsChecked = (bool)range.RequireSMTPAuthExternalToExternal;", shows, StringComparison.Ordinal);
         Assert.Contains("ShowRelayExposure_();", shows, StringComparison.Ordinal);

         string save = WithheldRowTests.MemberText(code, "private bool SavePermissions_(RangeRow row)");
         Assert.Contains("range.RequireSMTPAuthExternalToExternal = PermAuthRR.IsChecked is true;", save, StringComparison.Ordinal);
         int asks = save.IndexOf("RelayExposure.SaveQuestion(row.Name)", StringComparison.Ordinal);
         int writes = save.IndexOf("range.Save();", StringComparison.Ordinal);
         Assert.True(asks >= 0 && writes > asks, "Permissions that make an open relay are saved without asking.");

         string wire = Read(Path.Join("Views", "DeliveryRow.cs"));
         Assert.Contains("DialogFields.EnabledBy(allowed, authentication);", wire, StringComparison.Ordinal);
         Assert.Contains("F(\"{0}: only after SMTP authentication\", direction)", wire, StringComparison.Ordinal);
      }

      /// <summary>The dialog: one Delivery tab of the same rows in place of Relaying and Require auth, the TLS box with the connections, and the same question before an open relay is saved.</summary>
      [Fact]
      public void TheDialog_HoldsTheSameRows_InOneDeliveryTab()
      {
         string dialog = Read(Path.Join("Views", "IPRangeDialog.cs"));
         Assert.Contains("Header = L(\"Delivery\"), Content = BuildDelivery()", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Relaying\")", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"Require auth\")", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("open relay!", dialog, StringComparison.Ordinal);
         Assert.Contains("L(\"Require TLS _before authentication\")", dialog, StringComparison.Ordinal);
         Assert.Contains("DeliveryRow.BuildGrid(", dialog, StringComparison.Ordinal);
         Assert.Contains("(L(\"External to external (rela_y)\"), rr_, authEE_)", dialog, StringComparison.Ordinal);

         string save = WithheldRowTests.MemberText(dialog, "private void Save()");
         Assert.Contains("RelayExposure.SaveQuestion(name_.Text.Trim())", save, StringComparison.Ordinal);

         // The setup checklist names the box by its caption.
         string checks = Read(Path.Join("Services", "ExternalSetupChecks.cs"));
         Assert.DoesNotContain("Require TLS for auth", checks, StringComparison.Ordinal);
         Assert.Contains("Require TLS before authentication", checks, StringComparison.Ordinal);
      }

      private static string Read(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
