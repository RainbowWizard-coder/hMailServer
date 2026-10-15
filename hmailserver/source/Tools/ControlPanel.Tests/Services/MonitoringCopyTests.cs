// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The monitoring and maintenance pages' smaller words and places (review finding
   /// F313): "Queue id" beside a grid headed ID, "Delivery retriggered"; a page
   /// titled Discovery under a subtitle and catalogues that say eDiscovery, and
   /// "Journaling and dual delivery" among titles that join with "&"; a bare Copy on
   /// Diagnostics and MX query; a "who is connected" link to the session counts;
   /// the message source dialog opening with the keyboard in its path; the log level
   /// a spinner from 1 to 9 with one step that counts; Open log folder in the last
   /// card; and an event script's COM allow-list of "*" raising nothing.
   /// </summary>
   public class MonitoringCopyTests
   {
      [Fact]
      public void TheQueueSaysIdAsItsGridDoes_AndARetryInWords()
      {
         Assert.Contains("F(\"Queue ID {0} - select and choose \\\"Follow this message\\\".\", QueueId)",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "MessageTraceView.cs")), StringComparison.Ordinal);
         // Through t213c's QueueWords.Retried, which names one message and counts more (F248).
         Assert.Contains("return F(\"Message {0} will be tried again now.\", ids[0]);",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Services", "QueueWords.cs")), StringComparison.Ordinal);
      }

      [Fact]
      public void ThePagesAreTitledAsTheirSubjectsAreCalled_TheOldTitlesFindThem()
      {
         NavNode discovery = NavigationMap.Pages.Single(p => p.Key == "discovery");
         Assert.Equal("eDiscovery", discovery.Title);
         Assert.Contains("Discovery", discovery.Aliases);
         NavNode journaling = NavigationMap.Pages.Single(p => p.Key == "journaling");
         Assert.Equal("Journaling & dual delivery", journaling.Title);
         Assert.Contains("Journaling and dual delivery", journaling.Aliases);
      }

      [Fact]
      public void CopySaysWhatItCopies_AndTheStatusLinkSaysWhatItShows()
      {
         string utility = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "UtilityViews.cs"));
         Assert.Equal(2, utility.Split("Content = L(\"_Copy results\")").Length - 1);
         Assert.DoesNotContain("Content = L(\"_Copy\")", utility, StringComparison.Ordinal);
         Assert.Contains("Link(L(\"Server status (session counts)\"), \"status\")",
            ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "StalledMailView.cs")), StringComparison.Ordinal);
      }

      [Fact]
      public void TheMessageSourceShowsItsPathAsTheDescription_AndStartsInTheSource()
      {
         string dialog = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "MessageViewerDialog.cs"));
         Assert.Contains("UseFrame(L(\"Message source\"), body, close, null, description: filePath, width: DesignTokens.Dialog.WidestWidth)", dialog, StringComparison.Ordinal);
         Assert.DoesNotContain("pathBox", dialog, StringComparison.Ordinal);
      }

      [Fact]
      public void TheLogLevelIsItsTwoValues_AndTheFolderIsInTheHeader()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         Assert.Contains("detail.Settings.Add(new IniChoice", page, StringComparison.Ordinal);
         Assert.Contains("Options = new[] { (9, L(\"Full detail\")), (2, L(\"Quieter\")) },", page, StringComparison.Ordinal);
         Assert.Contains("HeaderActions.Children.Insert(0, openFolder);", page, StringComparison.Ordinal);
         Assert.DoesNotContain("ButtonText = L(\"Open log folder\")", page, StringComparison.Ordinal);
         Assert.Contains("x:Name=\"HeaderActions\"", ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml")), StringComparison.Ordinal);
      }

      [Fact]
      public void TheComAllowListIsCaptioned_AndWarnsWhileItIsAny()
      {
         string page = ConfirmationPatternTests.ReadControlPanelFile(Path.Join("Views", "ServerSettingsView.xaml.cs"));
         // With what empty means again, as 6.3.6 had it: a key held empty lets a script
         // create none, and is shown so (release review finding CP-4).
         Assert.Contains("Label = L(\"COM objects a script may create (empty = none beyond hMailServer's own)\"),", page, StringComparison.Ordinal);
         Assert.Contains("afterBuildUi_ = () => WireAnyObjectWarning_(allowedObjects);", page, StringComparison.Ordinal);
         Assert.Contains("(box.Text ?? \"\").Trim() == \"*\" ? Visibility.Visible : Visibility.Collapsed", page, StringComparison.Ordinal);
      }
   }
}
