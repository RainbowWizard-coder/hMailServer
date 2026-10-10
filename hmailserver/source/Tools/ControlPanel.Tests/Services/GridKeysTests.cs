// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The list pages' keys and the pages' forms (review finding F211). No grid
   /// answered Enter, Delete or F2, a double-click was the only way to open a row
   /// - and on a collection page a double-click on a column header's divider
   /// opened Edit as well - each quarantine row was about seven tab stops with no
   /// key to release, delete or preview, and Enter in an add form or in the MX
   /// query's and the diagnostics' boxes did nothing. The test project has no WPF
   /// to press a key in, so the helpers and every page's wiring are held to their
   /// source, as the domains page's keys are (DomainsRowVerbsTests).
   /// </summary>
   public class GridKeysTests
   {
      [Fact]
      public void GridKeys_AnswersEnterF2DeleteAndInsert_AndADoubleClickOnARowOnly()
      {
         string keys = Read(Path.Join("Views", "GridKeys.cs"));

         Assert.Contains("Key.Enter or Key.F2 => list.SelectedItem != null ? open : null", keys, StringComparison.Ordinal);
         Assert.Contains("Key.Delete => list.SelectedItem != null ? delete : null", keys, StringComparison.Ordinal);
         Assert.Contains("Key.Insert => add", keys, StringComparison.Ordinal);
         Assert.Contains("list.PreviewKeyDown +=", keys, StringComparison.Ordinal);

         // A double-click counts on a row, never on a header's divider or a
         // button inside a row.
         Assert.Matches(new Regex(@"list\.MouseDoubleClick \+=[\s\S]{0,200}?RowAt\(list, e\.OriginalSource as DependencyObject\) == null"), keys);
         Assert.Matches(new Regex(@"public static FrameworkElement RowAt\([\s\S]{0,300}?d is ButtonBase \|\| d is DataGridColumnHeader\)\s*return null;"), keys);

         // A key presses the page's own button, and not a disabled one.
         Assert.Contains("if (button == null || !button.IsEnabled || !button.IsVisible)", keys, StringComparison.Ordinal);

         // A key from a control inside a row is that control's.
         Assert.Contains("if (d is ButtonBase || d is TextBoxBase || d is PasswordBox || d is ComboBox)", keys, StringComparison.Ordinal);
      }

      /// <summary>Every list page's list is wired, and no list keeps the grid's own double-click.</summary>
      [Theory]
      [InlineData("CollectionEditorView.cs", "GridKeys.Wire(grid_, edit, del, addButton_);")]
      [InlineData("PublicFoldersView.cs", "GridKeys.Wire(list_, permissions, delete, addButton_);")]
      [InlineData("RoutesView.xaml.cs", "GridKeys.Wire(RouteGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteButton), GridKeys.Focusing(NewDomain));")]
      [InlineData("TcpIpPortsView.xaml.cs", "GridKeys.Wire(PortGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteButton), GridKeys.Focusing(NewProtocol));")]
      [InlineData("IPRangesView.xaml.cs", "GridKeys.Wire(RangeGrid, GridKeys.Pressing(PropertiesButton), GridKeys.Pressing(DeleteRangeButton), GridKeys.Focusing(NewName));")]
      [InlineData("RulesView.xaml.cs", "GridKeys.Wire(RuleGrid, null, GridKeys.Pressing(DeleteRuleButton), GridKeys.Focusing(NewRuleName));")]
      [InlineData("RulesView.xaml.cs", "GridKeys.Wire(CriteriaGrid, EditCriterionButton, RemoveCriterionButton, AddCriterionButton);")]
      [InlineData("RulesView.xaml.cs", "GridKeys.Wire(ActionsGrid, EditActionButton, RemoveActionButton, AddActionButton);")]
      [InlineData("SslCertificatesView.xaml.cs", "GridKeys.Wire(CertGrid, null, GridKeys.Pressing(DeleteButton), GridKeys.Focusing(NewCertName));")]
      [InlineData("QueueView.xaml.cs", "GridKeys.Wire(QueueGrid, ViewButton, RemoveButton, null);")]
      [InlineData("UtilityViews.cs", "GridKeys.Wire(relays_, GridKeys.Pressing(edit), GridKeys.Pressing(del), GridKeys.Focusing(name_));")]
      [InlineData("SuppressionListView.cs", "GridKeys.Wire(entries_, null, remove, addButton_);")]
      [InlineData("DomainPolicyListView.cs", "GridKeys.Wire(entries_, null, remove, addButton_);")]
      [InlineData("DomainPolicyListView.cs", "GridKeys.Wire(entries_, edit, null, null);")]
      [InlineData("JournalingView.cs", "GridKeys.Wire(rules_, null, GridKeys.Pressing(remove), GridKeys.Focusing(target_));")]
      public void EveryListPage_WiresItsList(string file, string wiring)
      {
         Assert.Contains(wiring, Read(Path.Join("Views", file)), StringComparison.Ordinal);
      }

      [Fact]
      public void NoListKeepsTheGridsOwnDoubleClick()
      {
         foreach (string xaml in new[] { "RoutesView.xaml", "TcpIpPortsView.xaml", "IPRangesView.xaml", "RulesView.xaml", "QueueView.xaml" })
            Assert.DoesNotContain("MouseDoubleClick=", Read(Path.Join("Views", xaml)), StringComparison.Ordinal);

         Assert.DoesNotContain("grid_.MouseDoubleClick", Read(Path.Join("Views", "CollectionEditorView.cs")), StringComparison.Ordinal);
         Assert.DoesNotContain("list_.MouseDoubleClick", Read(Path.Join("Views", "PublicFoldersView.cs")), StringComparison.Ordinal);
      }

      /// <summary>Enter in every form on a page presses its button; Escape empties the boxes the page names.</summary>
      [Theory]
      [InlineData("RoutesView.xaml.cs", "FormSubmit.Bind(AddRouteForm, AddRouteButton, NewDomain, NewHost);")]
      [InlineData("TcpIpPortsView.xaml.cs", "FormSubmit.Bind(AddPortForm, AddPortButton, NewPort);")]
      [InlineData("IPRangesView.xaml.cs", "FormSubmit.Bind(AddRangeForm, AddRangeButton, NewName, NewLower, NewUpper);")]
      [InlineData("RulesView.xaml.cs", "FormSubmit.Bind(AddRuleForm, AddRuleButton, NewRuleName);")]
      [InlineData("SslCertificatesView.xaml.cs", "FormSubmit.Bind(AddCertForm, AddCertButton, NewCertName, NewCertFile, NewKeyFile, NewKeyPassphrase);")]
      [InlineData("SslCertificatesView.xaml.cs", "FormSubmit.Bind(PassphraseBox, SavePassphraseButton, PassphraseBox);")]
      [InlineData("UtilityViews.cs", "FormSubmit.Bind(row, add_, name_, lower_, upper_);")]
      [InlineData("UtilityViews.cs", "FormSubmit.Bind(inputRow, run_, domain_);")]
      [InlineData("UtilityViews.cs", "FormSubmit.Bind(inputRow, run_);")]
      [InlineData("JournalingView.cs", "FormSubmit.Bind(form, add_, target_, address_, note_);")]
      [InlineData("RolesView.cs", "FormSubmit.Bind(form, give_, address_, domains_);")]
      [InlineData("DomainsView.Rows.cs", "FormSubmit.Bind(AddDomainForm, AddDomainButton, NewDomainBox);")]
      [InlineData("DomainsView.Rows.cs", "FormSubmit.Bind(AccountForm, AddAccountButton, NewAccountBox, NewAccountPassword);")]
      [InlineData("DomainsView.Rows.cs", "FormSubmit.Bind(AddAliasForm, AddAliasButton, NewAliasName, NewAliasValue);")]
      [InlineData("DomainsView.Rows.cs", "FormSubmit.Bind(AddListForm, AddListButton, NewDistAddress);")]
      public void EveryFormOnAPage_SubmitsOnEnter(string file, string binding)
      {
         Assert.Contains(binding, Read(Path.Join("Views", file)), StringComparison.Ordinal);
      }

      [Fact]
      public void FormSubmit_LeavesEnterToABoxThatTakesIt()
      {
         string form = Read(Path.Join("Views", "FormSubmit.cs"));

         // A box of several lines takes Enter for a new line, an open list for its choice.
         Assert.Contains("return !text.AcceptsReturn;", form, StringComparison.Ordinal);
         Assert.Contains("return !combo.IsDropDownOpen;", form, StringComparison.Ordinal);

         // The button is pressed as a click presses it, and not while it is disabled.
         Assert.Contains("if (GridKeys.Press(submit))", form, StringComparison.Ordinal);

         // Escape is the form's only when there is something to empty.
         Assert.Contains("clearOnEscape.Any(c => Text_(c).Length > 0)", form, StringComparison.Ordinal);
      }

      /// <summary>
      /// The quarantine's rows are one tab stop, after the header's box and its
      /// four actions, and the selected row answers Space, R, Delete, P, Enter and
      /// H; the row icons' tool tips name the keys. TabNavigation Once on the grid
      /// made the header's box the grid's one stop - Tab went through the header
      /// and out without reaching a row - and left each row's box, four actions,
      /// fold and two icons a stop each, as they were.
      /// </summary>
      [Fact]
      public void TheQuarantine_RowsAreOneTabStop_WithKeysOnTheSelectedRow()
      {
         string page = Read(Path.Join("Views", "QuarantineView.cs"));

         Assert.DoesNotContain("KeyboardNavigationMode.Once", page, StringComparison.Ordinal);
         Assert.Contains("GridKeys.RowsAreOneTabStop(list_);", page, StringComparison.Ordinal);
         Assert.Contains("WireRowKeys_();", page, StringComparison.Ordinal);

         // Nothing inside a row is a tab stop: the box, the icons, the fold.
         Assert.Matches(new Regex(@"private static FrameworkElementFactory CheckBox_\([\s\S]{0,300}?box\.SetValue\(Control\.IsTabStopProperty, false\);"), page);
         Assert.Matches(new Regex(@"private static FrameworkElementFactory IconButton_\([\s\S]{0,300}?button\.SetValue\(Control\.IsTabStopProperty, false\);"), page);
         Assert.Contains("fold.SetValue(Control.IsTabStopProperty, false);", page, StringComparison.Ordinal);
         // A group's row, which has no cells, takes the keyboard itself, and shows
         // that it is selected and where the keyboard is, as the grid's rows do.
         Assert.Matches(new Regex(@"groupRow\.Setters\.Add\(new Setter\(FocusableProperty, true\)\);"), page);
         Assert.Contains("selected.Setters.Add(new Setter(UIElement.OpacityProperty, 1.0, \"Pill\"));", page, StringComparison.Ordinal);
         Assert.Contains("var focused = new Trigger { Property = UIElement.IsKeyboardFocusedProperty, Value = true };", page, StringComparison.Ordinal);
         Assert.Contains("focused.Setters.Add(new Setter(UIElement.VisibilityProperty, Visibility.Visible, \"Ring\"));", page, StringComparison.Ordinal);

         // The rows' keys come from a row: Space on the header's box is the box's.
         Assert.Contains("if (GridKeys.RowAt(list_, e.OriginalSource as DependencyObject) == null)", page, StringComparison.Ordinal);
         // Up and Down move between rows, a group's - which has no cells - included.
         Assert.Matches(new Regex(@"case Key\.Up:\s*case Key\.Down:[\s\S]{0,300}?list_\.SelectedIndex = next;\s*GridKeys\.FocusSelectedRow\(list_\);"), page);

         Assert.Matches(new Regex(@"case Key\.Space:\s*review_\.Toggle\(row\);"), page);
         Assert.Matches(new Regex(@"case Key\.R:\s*Act_\(QuarantineReview\.IdsOf\(row\), ""release"", groupTitle\);"), page);
         Assert.Matches(new Regex(@"case Key\.Delete:\s*Act_\(QuarantineReview\.IdsOf\(row\), ""delete"", groupTitle\);"), page);
         Assert.Matches(new Regex(@"case Key\.P when row\.IsMessage:\s*case Key\.Enter when row\.IsMessage:\s*ShowPreview_\("), page);
         Assert.Matches(new Regex(@"case Key\.H when row\.IsMessage:\s*ShowHeaders_\("), page);

         Assert.Contains("nameof(QuarantineRow.ReleaseKeyTip), QuarantineRow.ReleaseKey", page, StringComparison.Ordinal);
         Assert.Contains("nameof(QuarantineRow.DeleteKeyTip), L(QuarantineRow.DeleteKey)", page, StringComparison.Ordinal);
         Assert.Contains("nameof(QuarantineRow.PreviewKeyTip), QuarantineRow.PreviewKey", page, StringComparison.Ordinal);
         Assert.Contains("nameof(QuarantineRow.HeadersKeyTip), QuarantineRow.HeadersKey", page, StringComparison.Ordinal);
         Assert.Contains("new Binding(nameof(QuarantineRow.CheckKeyTip))", page, StringComparison.Ordinal);
      }

      /// <summary>
      /// The rows' one stop: no cell is a tab stop, the grid's ItemsPresenter is
      /// while there are rows, and the keyboard that reaches it goes on to the
      /// selected row - the first, selected, where none is - or, come back from a
      /// row with Shift+Tab, to what is before the rows.
      /// </summary>
      [Fact]
      public void RowsAreOneTabStop_StopsOnTheRowsHostAndPassesTheKeyboardOn()
      {
         string keys = Read(Path.Join("Views", "GridKeys.cs"));
         Match helper = Regex.Match(keys, @"public static void RowsAreOneTabStop\(DataGrid grid\)\s*\{(?<body>[\s\S]*?)\r?\n      \}\r?\n");
         Assert.True(helper.Success, "GridKeys.RowsAreOneTabStop was not found.");
         string body = helper.Groups["body"].Value;

         Assert.Contains("KeyboardNavigation.SetTabNavigation(grid, KeyboardNavigationMode.Continue);", body, StringComparison.Ordinal);
         Assert.Contains("cells.Setters.Add(new Setter(Control.IsTabStopProperty, false));", body, StringComparison.Ordinal);
         Assert.Contains("KeyboardNavigation.SetIsTabStop(host, true);", body, StringComparison.Ordinal);
         Assert.Contains("new Binding(nameof(ItemsControl.HasItems)) { Source = grid }", body, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"IsWithin_\(old, host\)\)\s*\{\s*host\.MoveFocus\(new TraversalRequest\(FocusNavigationDirection\.Previous\)\);"), body);
         Assert.Matches(new Regex(@"if \(grid\.SelectedItem == null && grid\.Items\.Count > 0\)\s*grid\.SelectedIndex = 0;\s*FocusSelectedRow\(grid\);"), body);

         // The host is the grid's own presenter, not a column header's or a row's.
         Assert.Contains("ReferenceEquals(presenter.TemplatedParent, grid)", keys, StringComparison.Ordinal);
      }

      /// <summary>The tool tips name the keys; the accessible names stay the actions alone.</summary>
      [Fact]
      public void TheQuarantineTips_NameTheKeys()
      {
         var review = new QuarantineReview();
         review.Load(new[]
         {
            new HeldMessage { Id = 1, CreatedTime = "2026-10-09 08:00:00", Sender = "a@supply.example", Recipients = "ada@example.test", Score = 9, Subject = "Hello", Helo = "mx.supply.example", SenderDomain = "supply.example", HeloDomain = "supply.example" }
         });

         QuarantineRow message = review.Rows.First(r => r.IsMessage);
         Assert.Equal("Release (R)", message.ReleaseKeyTip);
         Assert.Equal("Delete (Del)", message.DeleteKeyTip);
         Assert.Equal("Preview the text (P)", message.PreviewKeyTip);
         Assert.Equal("Check this message (Space)", message.CheckKeyTip);
         Assert.Equal("Show the headers (H)", message.HeadersKeyTip);
         Assert.Equal("Release", message.ReleaseTip);
         Assert.Null(review.Rows.First(r => r.IsGroup).HeadersKeyTip);
      }

      /// <summary>The list of keys the palette shows names the keys of a list and of a form.</summary>
      [Fact]
      public void TheShortcutsList_NamesTheListKeys()
      {
         string keys = string.Join("\n", KeyboardShortcuts.Listed.Select(s => s.Keys));
         foreach (string key in new[] { "Enter, F2", "Del", "Ins", "Enter" })
            Assert.Contains(key, keys, StringComparison.Ordinal);
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
