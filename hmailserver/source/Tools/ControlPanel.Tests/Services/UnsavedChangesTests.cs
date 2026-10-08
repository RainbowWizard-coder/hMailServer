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
   /// Unsaved edits were thrown away without a word (review findings F116 and F167):
   /// a sidebar row, a Related link, a palette result, closing the window or Reload
   /// left a settings page whose pill said "Unsaved changes", and the shell's cached
   /// page read the server again when it came back. These hold the decision -
   /// nothing unsaved is never asked about, Stay never leaves, Save leaves only when
   /// everything was saved - and the wiring: every way of leaving a page goes through
   /// the question, and every page that can hold an edit says whether it does.
   /// </summary>
   public class UnsavedChangesTests
   {
      [Fact]
      public void NothingUnsaved_LeavesWithoutAsking()
      {
         bool asked = false, saved = false;
         Assert.True(UnsavedChanges.MayLeave(false, () => { asked = true; return UnsavedAnswer.Stay; }, () => saved = true));
         Assert.False(asked, "A page with nothing unsaved asked before it was left.");
         Assert.False(saved);
      }

      [Fact]
      public void Stay_NeverLeaves_AndSavesNothing()
      {
         bool saved = false;
         Assert.False(UnsavedChanges.MayLeave(true, () => UnsavedAnswer.Stay, () => saved = true));
         Assert.False(saved);
      }

      [Fact]
      public void Discard_Leaves_WithoutSaving()
      {
         bool saved = false;
         Assert.True(UnsavedChanges.MayLeave(true, () => UnsavedAnswer.Discard, () => saved = true));
         Assert.False(saved);
      }

      [Theory]
      [InlineData(true)]
      [InlineData(false)]
      public void Save_LeavesOnlyWhenEverythingWasSaved(bool everythingSaved)
      {
         int saves = 0;
         bool left = UnsavedChanges.MayLeave(true, () => UnsavedAnswer.Save, () => { saves++; return everythingSaved; });
         Assert.Equal(1, saves);
         Assert.Equal(everythingSaved, left);
      }

      [Fact]
      public void ABaseline_NotYetTaken_HoldsNothingUnsaved()
      {
         var baseline = new EditorBaseline();
         Assert.False(baseline.IsTaken);
         Assert.False(baseline.Differs(new[] { "anything" }));
      }

      [Fact]
      public void ABaseline_DiffersOnlyWhenAnEditorDoes()
      {
         var baseline = new EditorBaseline();
         baseline.Take(new[] { "mail.example.com", "636", "1", null });

         Assert.True(baseline.IsTaken);
         Assert.False(baseline.Differs(new[] { "mail.example.com", "636", "1", "" }), "An empty editor read as null was taken for an edit.");
         Assert.True(baseline.Differs(new[] { "mail.example.com", "389", "1", "" }));
         Assert.True(baseline.Differs(new[] { "Mail.example.com", "636", "1", "" }), "A change of case is an edit.");
         Assert.True(baseline.Differs(new[] { "mail.example.com", "636", "1" }), "An editor fewer is not the same page.");

         // Typed and then put back is not a change.
         baseline.Take(new[] { "a", "b" });
         Assert.False(baseline.Differs(new[] { "a", "b" }));

         baseline.Forget();
         Assert.False(baseline.Differs(new[] { "x" }), "A forgotten baseline still reported a change.");
      }

      [Fact]
      public void TheQuestions_NameWhatIsAskedAbout()
      {
         Assert.Contains("Authentication", UnsavedChanges.LeaveQuestion("Authentication"), StringComparison.Ordinal);
         Assert.Contains("Internet", UnsavedChanges.PermissionsQuestion("Internet"), StringComparison.Ordinal);
         Assert.NotEqual(UnsavedChanges.ReloadQuestion, UnsavedChanges.ReloadScriptQuestion);
      }

      // ---- the wiring --------------------------------------------------------------

      /// <summary>
      /// The interface every page implements says whether it holds unsaved changes and
      /// saves them, with defaults for the pages that edit nothing.
      /// </summary>
      [Fact]
      public void ThePageLifecycle_SaysWhetherAPageHoldsUnsavedChanges()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string lifecycle = WithheldRowTests.MemberText(shell, "public interface IPageLifecycle");
         Assert.True(lifecycle != null, "IPageLifecycle is gone from MainWindow.xaml.cs.");
         Assert.Contains("bool HasUnsavedChanges => false;", lifecycle, StringComparison.Ordinal);
         Assert.Contains("bool SaveChanges() => true;", lifecycle, StringComparison.Ordinal);
      }

      /// <summary>
      /// The sidebar's selection is the one door every navigation goes through - a row,
      /// a Related link, the palette, a page's own link, the rail, a tour - so the
      /// question is asked there, before the page is replaced; and closing the window
      /// and a change of language, which restarts the program, ask it too.
      /// </summary>
      [Fact]
      public void EveryWayOfLeavingAPage_AsksFirst()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");

         string select = WithheldRowTests.MemberText(shell, "private void NavTree_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)");
         Assert.True(select != null, "NavTree_SelectedItemChanged is gone.");
         int asks = select.IndexOf("MayLeaveCurrentPage_()", StringComparison.Ordinal);
         int replaces = select.IndexOf("ContentHost.Content = page", StringComparison.Ordinal);
         Assert.True(asks >= 0 && replaces > asks, "The sidebar replaces the page before it asks about unsaved changes.");
         Assert.Contains("RestoreSelection_()", select, StringComparison.Ordinal);

         string closing = WithheldRowTests.MemberText(shell, "protected override void OnClosing(System.ComponentModel.CancelEventArgs e)");
         Assert.True(closing != null && closing.Contains("MayLeaveCurrentPage_()", StringComparison.Ordinal) && closing.Contains("e.Cancel = true", StringComparison.Ordinal),
            "Closing the window does not ask about unsaved changes, or cannot be stopped by Stay.");

         string language = WithheldRowTests.MemberText(shell, "private void OfferLanguage_(string tag)");
         Assert.True(language != null && language.IndexOf("MayLeaveCurrentPage_()", StringComparison.Ordinal) < language.IndexOf("LanguageChoice.Offer(", StringComparison.Ordinal),
            "A change of language restarts the Control Panel without asking about unsaved changes.");
         Assert.Single(System.Text.RegularExpressions.Regex.Matches(shell, @"LanguageChoice\.Offer\("));

         // A reconnect and Apply now enter the page again, which reads the server:
         // never over unsaved changes.
         foreach (string signature in new[] { "private void OnSessionReconnected(ServerSession session)", "private void ApplyPending_Click(object sender, RoutedEventArgs e)" })
         {
            string member = WithheldRowTests.MemberText(shell, signature);
            Assert.True(member != null, signature + " is gone.");
            int guard = member.IndexOf("CurrentPageHasUnsavedChanges_()", StringComparison.Ordinal);
            int enter = member.LastIndexOf("EnterPage(ContentHost.Content)", StringComparison.Ordinal);
            Assert.True(guard >= 0 && enter > guard, signature + " enters the page again over its unsaved changes.");
         }
      }

      /// <summary>
      /// The pages the review found losing edits, each saying whether it holds one and
      /// saving it as its own Save does; and each Reload asking before it reads again.
      /// </summary>
      [Theory]
      [InlineData("Views/ServerSettingsView.xaml.cs", "private void Reload_Click(object sender, RoutedEventArgs e)", "UnsavedChanges.ReloadQuestion")]
      [InlineData("Views/FeatureSettingsView.xaml.cs", "private void Reload_Click(object sender, RoutedEventArgs e)", "UnsavedChanges.ReloadQuestion")]
      [InlineData("Views/LdapSettingsView.cs", "private void Reload_()", "UnsavedChanges.ReloadQuestion")]
      [InlineData("Views/DirectorySyncView.cs", "private void Reload_()", "UnsavedChanges.ReloadQuestion")]
      [InlineData("Views/ScriptsView.cs", "private void ReloadFromDisk_()", "UnsavedChanges.ReloadScriptQuestion")]
      [InlineData("Views/IPRangesView.xaml.cs", null, null)]
      public void APageThatEdits_SaysWhetherItHoldsUnsavedChanges_AndItsReloadAsks(string file, string reload, string question)
      {
         string code = ReadControlPanelFile(file);
         Assert.Contains("public bool HasUnsavedChanges", code, StringComparison.Ordinal);
         Assert.Contains("public bool SaveChanges()", code, StringComparison.Ordinal);

         if (reload == null)
            return;

         string member = WithheldRowTests.MemberText(code, reload);
         Assert.True(member != null, file + " has no " + reload + ".");
         int asks = member.IndexOf(question, StringComparison.Ordinal);
         Assert.True(asks >= 0 && member.IndexOf("HasUnsavedChanges", StringComparison.Ordinal) >= 0,
            file + ": Reload reads the settings again without asking about unsaved changes.");
      }

      /// <summary>
      /// A page with a "Save changes" button is a page that can hold an unsaved edit;
      /// one added without saying so is how F116 would come back.
      /// </summary>
      [Fact]
      public void EveryPageWithSaveChanges_SaysWhetherItHoldsUnsavedChanges()
      {
         string views = Path.Join(ControlPanelDirectory(), "Views");
         var missing = Directory.GetFiles(views, "*.cs")
            .Select(path => (Path: path, Code: File.ReadAllText(path), Markup: File.Exists(Path.ChangeExtension(path, null)) ? File.ReadAllText(Path.ChangeExtension(path, null)) : ""))
            .Where(f => f.Code.Contains("IPageLifecycle", StringComparison.Ordinal) && (f.Code + f.Markup).Contains("_Save changes", StringComparison.Ordinal))
            .Where(f => !f.Code.Contains("public bool HasUnsavedChanges", StringComparison.Ordinal))
            .Select(f => Path.GetFileName(f.Path))
            .ToList();

         Assert.True(missing.Count == 0, "These pages save settings and do not say when they hold unsaved changes: " + string.Join(", ", missing));
      }

      /// <summary>
      /// The IP ranges page asks before another range's permissions are put over
      /// changed ones, and keeps the changes across a reload that selects the range again.
      /// </summary>
      [Fact]
      public void TheIPRangesPage_AsksBeforeAnotherRangeReplacesChangedPermissions()
      {
         string code = ReadControlPanelFile("Views/IPRangesView.xaml.cs");

         string selection = WithheldRowTests.MemberText(code, "private void RangeGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)");
         Assert.True(selection != null, "RangeGrid_SelectionChanged is gone.");
         int guard = selection.IndexOf("HasUnsavedChanges", StringComparison.Ordinal);
         int shows = selection.IndexOf("ShowPermissions_()", StringComparison.Ordinal);
         Assert.True(guard >= 0 && shows > guard, "Choosing another range shows its permissions over unsaved ones without asking.");
         Assert.Contains("AskAboutPermissions_", selection, StringComparison.Ordinal);

         string ask = WithheldRowTests.MemberText(code, "private void AskAboutPermissions_(RangeRow edited)");
         Assert.True(ask != null && ask.Contains("UnsavedChanges.PermissionsQuestion", StringComparison.Ordinal), "The IP ranges page no longer asks about changed permissions.");

         string edit = WithheldRowTests.MemberText(code, "private void Edit_Click(object sender, RoutedEventArgs e)");
         Assert.True(edit != null && edit.IndexOf("HasUnsavedChanges", StringComparison.Ordinal) < edit.IndexOf("new IPRangeDialog(", StringComparison.Ordinal),
            "Properties opens the range's dialog over unsaved permissions on the card.");
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string ControlPanelDirectory()
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel");
            if (File.Exists(Path.Join(candidate, "MainWindow.xaml.cs")))
               return candidate;
            directory = directory.Parent;
         }

         Assert.Fail("The ControlPanel folder could not be found from the test binaries.");
         return null;
      }

      private static string ReadControlPanelFile(string relative)
         => File.ReadAllText(Path.Join(ControlPanelDirectory(), relative.Replace('/', Path.DirectorySeparatorChar)));
   }
}
