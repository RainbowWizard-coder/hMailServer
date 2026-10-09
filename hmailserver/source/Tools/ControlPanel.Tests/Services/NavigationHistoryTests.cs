// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Back and Forward (review finding F013). The Control Panel replaced the page
   /// and recorded nothing, so after a jump from the Dashboard, the setup
   /// checklist, an overview, the stalled-mail steps, a Related link or the
   /// palette the only way back was to find the page again. The history is held
   /// here as data - the order, what a visit drops, what Back keeps - and the
   /// window that steps through it, which cannot be made in a test, by its source.
   /// </summary>
   public class NavigationHistoryTests
   {
      [Fact]
      public void AnEmptyHistory_GoesNowhere()
      {
         var history = new NavigationHistory();

         Assert.Null(history.Current);
         Assert.False(history.CanGoBack);
         Assert.False(history.CanGoForward);
         Assert.Null(history.PeekBack);
         Assert.Null(history.Back());
         Assert.Null(history.Forward());

         history.Visit("dashboard");
         Assert.Equal("dashboard", history.Current.Key);
         Assert.False(history.CanGoBack);
         Assert.Null(history.Back());
         Assert.Equal("dashboard", history.Current.Key);
      }

      [Fact]
      public void BackAndForward_WalkThePagesInTheOrderTheyWereOpened()
      {
         var history = new NavigationHistory();
         history.Visit("dashboard");
         history.Visit("queue");
         history.Visit("stalledmail");

         Assert.Equal("queue", history.PeekBack.Key);
         Assert.Equal("queue", history.Back().Key);
         Assert.Equal("dashboard", history.Back().Key);
         Assert.False(history.CanGoBack);
         Assert.True(history.CanGoForward);
         Assert.Equal("queue", history.PeekForward.Key);
         Assert.Equal("queue", history.Forward().Key);
         Assert.Equal("stalledmail", history.Forward().Key);
         Assert.False(history.CanGoForward);
         Assert.Equal(3, history.Count);
      }

      /// <summary>A page opened after Back is the new way ahead: what Back left is dropped, as a browser drops it.</summary>
      [Fact]
      public void AVisitAfterBack_DropsThePagesAhead()
      {
         var history = new NavigationHistory();
         history.Visit("domains");
         history.Visit("groups");
         history.Visit("publicfolders");
         history.Back();
         history.Back();

         history.Visit("roles");

         Assert.Equal("roles", history.Current.Key);
         Assert.False(history.CanGoForward);
         Assert.Equal("domains", history.PeekBack.Key);
         Assert.Equal(2, history.Count);
      }

      /// <summary>The page on screen opened again - a Related link to itself, a palette row for it - is no visit.</summary>
      [Fact]
      public void ThePageOnScreenOpenedAgain_IsNoVisit()
      {
         var history = new NavigationHistory();
         history.Visit("logs");
         history.Visit("LOGS");
         history.Visit("");
         history.Visit(null);

         Assert.Equal(1, history.Count);
         Assert.False(history.CanGoBack);
      }

      /// <summary>Back returns to the place as well as the page: the scroll offset and the tab it was left on.</summary>
      [Fact]
      public void ThePageLeft_KeepsItsScrollAndItsTab()
      {
         var history = new NavigationHistory();
         history.Visit("antispam");
         history.Remember(640, 3);
         history.Visit("quarantine");
         history.Remember(-5, -7);

         HistoryEntry back = history.Back();
         Assert.Equal("antispam", back.Key);
         Assert.Equal(640, back.ScrollOffset);
         Assert.Equal(3, back.TabIndex);

         HistoryEntry forward = history.Forward();
         Assert.Equal(0, forward.ScrollOffset);
         Assert.Equal(-1, forward.TabIndex);

         // A page never left has nothing to put back.
         var fresh = new NavigationHistory();
         fresh.Visit("status");
         Assert.Equal(0, fresh.Current.ScrollOffset);
         Assert.Equal(-1, fresh.Current.TabIndex);
         fresh.Remember(double.NaN, 0);
         Assert.Equal(0, fresh.Current.ScrollOffset);
         Assert.Equal(0, fresh.Current.TabIndex);
      }

      [Fact]
      public void TheOldestPageGoes_PastTheCapacity()
      {
         var history = new NavigationHistory();
         for (int i = 0; i < NavigationHistory.Capacity + 5; i++)
            history.Visit("page" + i);

         Assert.Equal(NavigationHistory.Capacity, history.Count);
         HistoryEntry oldest = null;
         while (history.CanGoBack)
            oldest = history.Back();
         Assert.Equal("page5", oldest.Key);
      }

      /// <summary>Each sign-in starts with no history: the pages the last credential opened are not this one's to go back to.</summary>
      [Fact]
      public void Clear_ForgetsEveryPage()
      {
         var history = new NavigationHistory();
         history.Visit("dashboard");
         history.Visit("audit");
         history.Clear();

         Assert.Null(history.Current);
         Assert.Equal(0, history.Count);
         Assert.False(history.CanGoBack);

         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string connected = WithheldRowTests.MemberText(shell, "private void OnConnected()");
         Assert.True(connected != null, "MainWindow.OnConnected is gone.");
         int clear = connected.LastIndexOf("history_.Clear();", StringComparison.Ordinal);
         int first = connected.IndexOf("NavigateTo(mayUseDashboard", StringComparison.Ordinal);
         Assert.True(clear >= 0 && first > clear, "A sign-in no longer starts a history of its own before its first page.");
      }

      /// <summary>
      /// The window takes a step only once the page may be left - after the
      /// unsaved-changes question and once the page to open exists - so Stay
      /// moves neither the page nor the history; and it puts the page reached by
      /// a step back where it was after entering it.
      /// </summary>
      [Fact]
      public void TheWindow_TakesTheStepOnlyOnceThePageMayBeLeft()
      {
         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         string changed = WithheldRowTests.MemberText(shell, "private void NavTree_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)");
         Assert.True(changed != null, "MainWindow.NavTree_SelectedItemChanged is gone.");

         int mayLeave = changed.IndexOf("MayLeaveCurrentPage_()", StringComparison.Ordinal);
         int factory = changed.IndexOf("pageFactories_.TryGetValue", StringComparison.Ordinal);
         int record = changed.IndexOf("RecordVisit_(key, ContentHost.Content as DependencyObject)", StringComparison.Ordinal);
         int replace = changed.IndexOf("ContentHost.Content = page;", StringComparison.Ordinal);
         int enter = changed.IndexOf("EnterPage(page);", StringComparison.Ordinal);
         int restore = changed.IndexOf("RestoreVisit_(returningTo, page);", StringComparison.Ordinal);
         Assert.True(mayLeave >= 0 && factory > mayLeave && record > factory, "The history moves before the page may be left, or for a key with no page.");
         Assert.True(replace > record, "The page left is replaced before the history keeps how it was left.");
         Assert.True(restore > enter, "A page reached by Back is put back where it was before it is entered, which reads it again.");

         string history = ReadControlPanelFile("MainWindow.History.cs");
         Assert.Contains("historyStep_ = step;", history, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"historyStep_ = step;\s*try\s*\{\s*NavigateTo\(target\.Key\);\s*\}\s*finally\s*\{\s*historyStep_ = 0;"), history);
         Assert.Contains("history_.Remember(PageScrollOffset_(leaving), PageTabIndex_(leaving));", history, StringComparison.Ordinal);
      }

      /// <summary>
      /// Back and Forward are buttons at the start of the breadcrumb bar, named by
      /// the page each opens, as well as keys and the mouse's side buttons; the
      /// window binds no browse command, so the WM_APPCOMMAND Windows raises for a
      /// side button cannot step a second time.
      /// </summary>
      [Fact]
      public void BackAndForward_AreButtonsKeysAndTheMousesSideButtons()
      {
         string xaml = ReadControlPanelFile("MainWindow.xaml");
         int bar = xaml.IndexOf("x:Name=\"BreadcrumbBar\"", StringComparison.Ordinal);
         int back = xaml.IndexOf("x:Name=\"BackButton\"", StringComparison.Ordinal);
         int forward = xaml.IndexOf("x:Name=\"ForwardButton\"", StringComparison.Ordinal);
         int trail = xaml.IndexOf("x:Name=\"BreadcrumbHost\"", StringComparison.Ordinal);
         Assert.True(bar >= 0 && back > bar && forward > back && trail > forward, "Back and Forward are no longer at the start of the breadcrumb bar.");
         Assert.Contains("Symbol=\"ArrowLeft24\"", xaml, StringComparison.Ordinal);
         Assert.Contains("AutomationProperties.AutomationId=\"history-back\"", xaml, StringComparison.Ordinal);

         string history = ReadControlPanelFile("MainWindow.History.cs");
         Assert.Contains("e.ChangedButton == MouseButton.XButton1", history, StringComparison.Ordinal);
         Assert.Contains("e.ChangedButton == MouseButton.XButton2", history, StringComparison.Ordinal);
         Assert.Contains("F(\"Back to {0} (Alt+Left)\"", history, StringComparison.Ordinal);
         Assert.Contains("F(\"Forward to {0} (Alt+Right)\"", history, StringComparison.Ordinal);

         string keys = ReadControlPanelFile("MainWindow.Keys.cs");
         Assert.Matches(new Regex(@"case ShellCommand\.Back:[\s\S]{0,200}?return GoBack_\(\);"), keys);
         Assert.Matches(new Regex(@"case ShellCommand\.Forward:\s*return GoForward_\(\);"), keys);

         var browseBinding = new Regex(@"CommandBinding\(\s*NavigationCommands\.Browse|Command=""(?:NavigationCommands\.)?Browse");
         foreach (string file in new[] { "MainWindow.xaml.cs", "MainWindow.History.cs", "MainWindow.Keys.cs", "MainWindow.xaml" })
            Assert.DoesNotMatch(browseBinding, ReadControlPanelFile(file));

         string shell = ReadControlPanelFile("MainWindow.xaml.cs");
         Assert.Contains("WireHistory_();", shell, StringComparison.Ordinal);
         string breadcrumb = WithheldRowTests.MemberText(shell, "private void UpdateBreadcrumb(string key)");
         Assert.True(breadcrumb != null && breadcrumb.Contains("UpdateHistoryButtons_();", StringComparison.Ordinal),
            "The breadcrumb is rebuilt without its Back and Forward following the history.");
      }

      private static string ReadControlPanelFile(string relativePath)
      {
         for (var at = new DirectoryInfo(AppContext.BaseDirectory); at != null; at = at.Parent)
         {
            string candidate = Path.Join(at.FullName, "ControlPanel", relativePath);
            if (File.Exists(candidate) && Directory.Exists(Path.Join(at.FullName, "ControlPanel", "Views")))
               return File.ReadAllText(candidate);
         }

         throw new FileNotFoundException("the Control Panel's " + relativePath + " is not above " + AppContext.BaseDirectory);
      }
   }
}
