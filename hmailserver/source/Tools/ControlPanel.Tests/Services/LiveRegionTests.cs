// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Every live region in the Control Panel announces what it shows. Marking an
   /// element AutomationProperties.LiveSetting Polite or Assertive is a promise:
   /// a screen reader says the region's name when the element raises
   /// LiveRegionChanged, and a repository-wide search found nothing in the tree
   /// that raised it - the tour's notification was the one announcement there
   /// was. So "Released 3 messages", "Deleted.", "Could not load…", a sign-in
   /// failure and every Save or Test outcome were silent, and the two result
   /// lines that carried a fixed accessible name said their label in place of
   /// their message to a reader that was moved to them. The WPF-free test project
   /// cannot listen for the event, so these tests hold the source to the shape
   /// that raises it: one helper (Services/Announce.cs) raises the event, every
   /// scaffold component that is a live region calls it, a field's error calls
   /// it, and a result line built from a TextBlock calls it and is named by its
   /// text alone.
   /// </summary>
   public class LiveRegionTests
   {
      [Fact]
      public void Announce_RaisesTheLiveRegionEvent_OnTheElementsOwnPeer()
      {
         string helper = ReadControlPanelFile(Path.Join("Services", "Announce.cs"));
         Assert.Contains("AutomationEvents.LiveRegionChanged", helper, StringComparison.Ordinal);
         Assert.Contains("RaiseAutomationEvent(AutomationEvents.LiveRegionChanged)", helper, StringComparison.Ordinal);
         Assert.Contains("public static void Request(UIElement element)", helper, StringComparison.Ordinal);

         // Deferred past the layout that shows the element, and never for one that
         // is not on screen or is not a live region: an event on a collapsed
         // element is noise, and on an element with LiveSetting Off it is ignored.
         Assert.Contains("DispatcherPriority.Loaded", helper, StringComparison.Ordinal);
         Assert.Contains("element.IsVisible", helper, StringComparison.Ordinal);
         Assert.Contains("AutomationLiveSetting.Off", helper, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("InlineNotice.cs")]
      [InlineData("LoadingState.cs")]
      [InlineData("EmptyState.cs")]
      public void EveryScaffoldLiveRegion_IsOne_AndAnnouncesItself(string component)
      {
         string source = ReadControlPanelFile(Path.Join("Views", "Scaffold", component));
         Assert.Contains("AutomationProperties.SetLiveSetting(this, AutomationLiveSetting.Polite)", source, StringComparison.Ordinal);

         // As its text changes, and as it comes into view: a page fills a notice
         // in while it is collapsed and shows it afterwards, or the other way
         // round, and both orders are one announcement.
         Assert.Contains("Announce.Request(", source, StringComparison.Ordinal);
         Assert.Contains("IsVisibleChanged +=", source, StringComparison.Ordinal);
      }

      /// <summary>
      /// A pill is a live region where a page makes it one - a list page's count
      /// beside its title (ListPagePatternTests) - and then announces itself as a
      /// notice does, as its text or level changes and as it comes into view: the
      /// list pages' counts were Information notices, which said themselves, until
      /// the list page pattern moved them into pills that said nothing, so that a
      /// search's result count reached a sighted administrator alone (WCAG 2.2
      /// SC 4.1.3; review finding F288). A pill is never a live region by itself:
      /// the shell's and a grid's cells' stay quiet, and queue nothing.
      /// </summary>
      [Fact]
      public void AStatusPill_ThatIsALiveRegion_AnnouncesItself()
      {
         string source = ReadControlPanelFile(Path.Join("Views", "Scaffold", "StatusPill.cs"));
         Assert.Contains("Announce.Request(pill)", source, StringComparison.Ordinal);
         Assert.Contains("IsVisibleChanged +=", source, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(this)", source, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.GetLiveSetting(this) != AutomationLiveSetting.Off", source, StringComparison.Ordinal);
         Assert.DoesNotContain("SetLiveSetting(this", source, StringComparison.Ordinal);
      }

      [Fact]
      public void AFieldsError_IsAnnounced_AsItIsSet()
      {
         string row = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FieldRow.cs"));
         Assert.Contains("e.Property == ErrorProperty", row, StringComparison.Ordinal);
         Assert.Contains("Announce.Request(message)", row, StringComparison.Ordinal);

         // The message is the template's live region, assertive: a validation
         // message is the one thing a form must say at once.
         string template = ReadControlPanelFile(Path.Join("Views", "Scaffold", "Scaffold.xaml"));
         Match error = Regex.Match(template, @"<TextBlock x:Name=""PART_Error""[^>]*>", RegexOptions.Singleline);
         Assert.True(error.Success, "Scaffold.xaml has no PART_Error.");
         Assert.Contains("AutomationProperties.LiveSetting=\"Assertive\"", error.Value, StringComparison.Ordinal);
      }

      /// <summary>
      /// A result line built from a TextBlock and declared a live region announces
      /// what it says and is named by it: with a fixed name, a reader said "Clear
      /// logon-failure list result" and "Never-ban list status" where the outcome
      /// was. Held over every view, so that the next result line is held too. The
      /// two lines it was written for are notices now, which announce themselves
      /// and are named "Word: text" by the component, and carry their level as a
      /// mark and a word rather than as a colour alone.
      /// </summary>
      [Fact]
      public void EveryTextBlockLiveRegion_Announces_AndIsNamedByItsText()
      {
         string views = ControlPanelDirectory(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         foreach (string path in Directory.EnumerateFiles(views, "*.cs"))
         {
            string source = File.ReadAllText(path);
            foreach (Match live in Regex.Matches(source, @"AutomationProperties\.SetLiveSetting\(\s*(\w+)\s*,"))
            {
               string field = live.Groups[1].Value;
               if (field == "this" || !Regex.IsMatch(source, @"\bTextBlock\s+" + Regex.Escape(field) + @"\b"))
                  continue;

               string name = Path.GetFileName(path);
               Assert.True(source.Contains("Announce.Request(" + field + ")", StringComparison.Ordinal),
                  name + " declares " + field + " a live region and never announces it.");
               Assert.False(Regex.IsMatch(source, @"AutomationProperties\.SetName\(\s*" + Regex.Escape(field) + @"\s*,"),
                  name + " gives " + field + " a fixed accessible name, which a reader says in place of its text.");
            }
         }

         string settings = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.True(settings.Contains("result_ = new Scaffold.InlineNotice", StringComparison.Ordinal),
            "The ComAction result line is not a notice: a line of coloured text carries its outcome by colour alone.");
         string neverBan = ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.NeverBanNames.cs"));
         Assert.True(neverBan.Contains("status_ = new Scaffold.InlineNotice", StringComparison.Ordinal),
            "The never-ban status line is not a notice: a line of coloured text carries its outcome by colour alone.");
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string ReadControlPanelFile(string relative) => File.ReadAllText(ControlPanelFile(relative));

      private static string ControlPanelDirectory(string relative) => Path.GetDirectoryName(ControlPanelFile(relative));

      private static string ControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative);
            if (File.Exists(candidate))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative.Replace('\\', '/') + " could not be found from the test binaries.");
         return null;
      }
   }
}
