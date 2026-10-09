// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// Review finding F207: the empty states stated the fact and never offered the
   /// action. Eleven list pages shared "Nothing here yet - Add creates the first
   /// entry."; the certificates, IP ranges, routes and ports pages said "No ... yet."
   /// with no button, though with no IP range the server refuses every connection;
   /// the REST API keys page said it twice, a card apart; the recipient, member and
   /// never-ban lists were blank boxes; and EmptyState.Action, the component's
   /// "the action that does it", was set nowhere but on two pages. Every test here
   /// fails against that: each collection that can be added to says what its
   /// emptiness means, each list page's empty state carries an action, the lists in
   /// dialogs have an empty state in the list's place, and a list card whose empty
   /// state carries an action is floored at what it holds, so that the action is
   /// never cut off in a short window.
   /// </summary>
   public class EmptyStateActionTests
   {
      private static readonly XNamespace Wpf = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";
      private static readonly XNamespace Scaffold = "clr-namespace:hMailServer.ControlPanel.Views.Scaffold";

      /// <summary>
      /// Every collection the page can add to says what an empty list means, in a
      /// sentence of its own: the one collection that cannot be added to (the server
      /// messages) is never empty.
      /// </summary>
      [Fact]
      public void EveryCollectionThatCanBeAddedTo_SaysWhatItsEmptinessMeans()
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         List<string> bodies = SpecBodies(specs).ToList();
         Assert.True(bodies.Count >= 14, "Only " + bodies.Count + " collection specs were found in CollectionSpecs.cs.");

         var silent = new List<string>();
         foreach (string body in bodies)
         {
            if (body.Contains("CanAdd = false", StringComparison.Ordinal))
               continue;
            if (!Regex.IsMatch(body, @"EmptyText = L\(""[^""]{20,}"""))
               silent.Add(Regex.Match(body, @"Title = L\(""(?<t>[^""]*)""\)").Groups["t"].Value);
         }

         Assert.True(silent.Count == 0, "These lists say nothing of their own when empty: " + string.Join(", ", silent));
      }

      /// <summary>
      /// An exemption list's empty state says what the list itself lets past, and
      /// nothing more: an empty one does not put every sender through the protection,
      /// since others are let past by other means - an SPF pass (Bypass on SPF
      /// success, on as shipped), the greylisting bypass list and the MX/A bypass
      /// past greylisting, and an IP range with spam protection off and an
      /// authenticated sender past spam protection. "every server that connects is
      /// greylisted" and "every sender goes through spam protection" said otherwise.
      /// </summary>
      [Theory]
      [InlineData("SpamWhiteList")]
      [InlineData("GreyListWhiteList")]
      [InlineData("GreyListBypass")]
      public void AnExemptionListsEmptyState_SaysWhatTheListLetsPast_NotThatEveryoneGoesThrough(string list)
      {
         string specs = ReadControlPanelFile(Path.Join("Views", "CollectionSpecs.cs"));
         int at = specs.IndexOf("public static CollectionEditorView " + list + "()", StringComparison.Ordinal);
         Assert.True(at >= 0, "CollectionSpecs." + list + " is gone.");
         string body = SpecBodies(specs.Substring(at)).First();

         string empty = Regex.Match(body, @"EmptyText = L\(""(?<t>[^""]*)""\)").Groups["t"].Value;
         Assert.Contains(" let past ", empty, StringComparison.Ordinal);
         Assert.DoesNotContain("every", empty, StringComparison.OrdinalIgnoreCase);
      }

      [Fact]
      public void TheCollectionEditor_ShowsTheSpecsSentence_AndOffersTheFirstEntry()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "CollectionEditorView.cs"));
         string build = WithheldRowTests.MemberText(code, "private void Build()");
         Assert.True(build != null, "CollectionEditorView.Build is gone.");
         Assert.Contains("empty_.Action = first;", build, StringComparison.Ordinal);
         Assert.Contains("OpenDialog(null)", build, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"if \(spec_\.CanAdd\)\s*\{\s*var first = new Button \{ Content = L\(""Add the _first entry…""\) \};"), build);

         string reload = WithheldRowTests.MemberText(code, "private void Load_(int selectId, int selectIndex, bool focus)");
         Assert.True(reload != null, "CollectionEditorView.Load_ is gone.");
         Assert.Contains("spec_.EmptyText ??", reload, StringComparison.Ordinal);
      }

      public static TheoryData<string, string, string> XamlListPages => new()
      {
         { "IPRangesView.xaml", "RangeGrid", "RestoreDefaults_Click" },
         { "RoutesView.xaml", "RouteGrid", "AddRouteAction_Click" },
         { "TcpIpPortsView.xaml", "PortGrid", "RestoreDefaults_Click" },
         { "SslCertificatesView.xaml", "CertGrid", "SetUpAcme_Click" },
      };

      /// <summary>
      /// The four list pages built in XAML: the empty state over the grid carries the
      /// action that ends the emptiness - the defaults put back, the add form, ACME.
      /// </summary>
      [Theory]
      [MemberData(nameof(XamlListPages))]
      public void TheEmptyState_CarriesAnAction(string file, string grid, string handler)
      {
         XElement page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", file))).Root;
         XElement list = page.Descendants(Wpf + "DataGrid").Single(g => (string)g.Attribute(X + "Name") == grid);
         XElement empty = page.Descendants(Scaffold + "EmptyState").SingleOrDefault();
         Assert.True(empty != null, file + " has no empty state.");

         XElement action = empty.Element(Scaffold + "EmptyState.Action");
         Assert.True(action != null, file + ": the empty state carries no action.");
         XElement button = action.Elements().Single();
         Assert.Equal("Button", button.Name.LocalName);
         Assert.Equal(handler, (string)button.Attribute("Click"));
         Assert.Matches(new Regex(@"^\{loc:L '[^']*_[^']*'\}$"), (string)button.Attribute("Content"));

         // Laid over the grid, in the card the grid fills.
         Assert.Same(list.Parent.Parent, empty.Parent);
      }

      /// <summary>
      /// A list card whose empty state carries an action is floored at what it holds:
      /// the grid's rows on a FillPanel of its own and the empty state laid over them,
      /// so that a sentence and an action taller than the rows raise the card's floor.
      /// At the list card's fixed 157 pixels the action was cut off in a short window.
      /// </summary>
      [Theory]
      [MemberData(nameof(XamlListPages))]
      public void TheListCard_IsFlooredAtWhatItHolds(string file, string grid, string handler)
      {
         Assert.False(string.IsNullOrEmpty(handler));
         XElement page = XDocument.Parse(ReadControlPanelFile(Path.Join("Views", file))).Root;
         XElement card = page.Descendants(Wpf + "DataGrid").Single(g => (string)g.Attribute(X + "Name") == grid)
            .Ancestors(Scaffold + "Card").First();
         Assert.Null((string)card.Attribute("MinHeight"));
         string unheld = ListCardFloorProblem(card, grid, grid == "CertGrid" ? 4 : 3);
         Assert.True(unheld == null, file + ": " + unheld);
      }

      [Theory]
      [InlineData("CollectionEditorView.cs", "private void Build()", "grid_", true)]
      [InlineData("DomainPolicyListView.cs", "private void Build()", "entries_", true)]
      [InlineData("QuarantineView.cs", "private void Build()", "list_", false)]
      public void TheListCardBuiltInCode_IsFlooredAtWhatItHolds_AndItsEmptyStateCarriesAnAction(string file, string member, string grid, bool actionInBuild)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         string build = WithheldRowTests.MemberText(code, member);
         Assert.True(build != null, file + ": " + member + " is gone.");
         Assert.Contains("rows.Children.Add(FillPanel.Filling(" + grid + ", FillPanel.ListRowsFloor));", build, StringComparison.Ordinal);
         Assert.Contains("host.Children.Add(rows);", build, StringComparison.Ordinal);
         Assert.Contains("host.Children.Add(empty_);", build, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"FillPanel\.Filling\(\w*[cC]ard, 0\)"), build);
         Assert.DoesNotContain("FillPanel.ListCardFloor", build, StringComparison.Ordinal);

         // The quarantine page sets its empty state's action as it reads, since
         // it is there only while quarantining is off.
         Assert.Contains("empty_.Action = ", actionInBuild ? build : code, StringComparison.Ordinal);
      }

      [Fact]
      public void TheRowsFloor_IsTheHeadersAndThreeRows_ClearOfAHorizontalBar()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "Scaffold", "FillPanel.cs"));
         Assert.Contains("public const double ListRowsFloor = 33 + 3 * 34 + 8;", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// The REST API keys page says it has no keys once, by the list, with the way
      /// to the first one; the key store's notice above it goes while there are none.
      /// </summary>
      [Fact]
      public void TheApiKeysPage_SaysItHasNoKeysOnce_ByTheList_WithTheFirstKey()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "ApiKeysView.cs"));
         string list = WithheldRowTests.MemberText(code, "private Card BuildListCard_()");
         Assert.True(list != null, "ApiKeysView.BuildListCard_ is gone.");
         Assert.Contains("empty_.Action = first;", list, StringComparison.Ordinal);
         Assert.Contains("CreateKey_()", list, StringComparison.Ordinal);

         string reload = WithheldRowTests.MemberText(code, "private void Reload_()");
         Assert.True(reload != null, "ApiKeysView.Reload_ is gone.");
         Assert.Contains("summary_.Visibility = keys.Count == 0 ? Visibility.Collapsed : Visibility.Visible;", reload, StringComparison.Ordinal);
         Assert.DoesNotContain("L(\"No keys yet.\")", code, StringComparison.Ordinal);
         Assert.Single(Regex.Matches(code, Regex.Escape("L(\"No API keys exist.")));
      }

      /// <summary>The lists in dialogs and on the Auto-ban page: an empty state in the list's place, shown only after a read that worked.</summary>
      [Theory]
      [InlineData("RecipientsDialog.cs", "listHost.Children.Add(empty_);")]
      [InlineData("GroupMembersDialog.cs", "listHost.Children.Add(empty_);")]
      [InlineData("ServerSettingsView.NeverBanNames.cs", "gridHost.Children.Add(empty_);")]
      public void TheListsInDialogs_HaveAnEmptyStateInTheListsPlace(string file, string laidOver)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Contains(laidOver, code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"StatusText\.Show\(empty_, null, (rows\.)?[Cc]ount, null,"), code);
         Assert.Contains("empty_.Visibility = Visibility.Collapsed;", code, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every list empty state laid over a grid is kept under the grid's column
      /// headers, centred where the rows would be. Laid over the whole grid it was
      /// centred over the headers as well, and where the card is no taller than the
      /// empty state - the narrowest window, once the card is floored at what it
      /// holds - its glyph sat against the headers' line, and across it under the
      /// quarantine's taller header row.
      /// </summary>
      [Theory]
      [InlineData("IPRangesView.xaml.cs", "EmptyStatus", "RangeGrid")]
      [InlineData("RoutesView.xaml.cs", "EmptyStatus", "RouteGrid")]
      [InlineData("TcpIpPortsView.xaml.cs", "EmptyStatus", "PortGrid")]
      [InlineData("SslCertificatesView.xaml.cs", "EmptyStatus", "CertGrid")]
      [InlineData("CollectionEditorView.cs", "empty_", "grid_")]
      [InlineData("DomainPolicyListView.cs", "empty_", "entries_")]
      [InlineData("QuarantineView.cs", "empty_", "list_")]
      public void TheEmptyState_IsKeptUnderTheGridsColumnHeaders(string file, string empty, string grid)
      {
         string code = ReadControlPanelFile(Path.Join("Views", file));
         Assert.Matches(new Regex(@"(Scaffold\.)?EmptyStateUnderHeaders\.Keep\(" + Regex.Escape(empty) + ", " + Regex.Escape(grid) + @"\);"), code);
      }

      /// <summary>The headers' height is measured, when the grid is loaded and whenever it changes, not assumed.</summary>
      [Fact]
      public void UnderTheHeaders_IsMeasured_NotAssumed()
      {
         string code = ReadControlPanelFile(Path.Join("Views", "Scaffold", "EmptyStateUnderHeaders.cs"));
         string keep = WithheldRowTests.MemberText(code, "public static void Keep(EmptyState empty, DataGrid grid)");
         Assert.True(keep != null, "EmptyStateUnderHeaders.Keep is gone.");
         Assert.Contains("headers = FindHeaders_(grid);", keep, StringComparison.Ordinal);
         Assert.Contains("headers.SizeChanged += (_, _) => Place();", keep, StringComparison.Ordinal);
         Assert.Contains("grid.Loaded += (_, _) => Place();", keep, StringComparison.Ordinal);
         Assert.Contains("new Point(0, headers.ActualHeight)", keep, StringComparison.Ordinal);
         Assert.Contains("empty.Margin = new Thickness(margin.Left, top, margin.Right, margin.Bottom);", keep, StringComparison.Ordinal);
         Assert.Contains("child is DataGridColumnHeadersPresenter found", code, StringComparison.Ordinal);
      }

      // ---- helpers -------------------------------------------------------------------------

      /// <summary>
      /// Null when a list card's floor holds its grid's rows and its empty state, or
      /// why it does not. Two shapes hold: the card floored at a MinHeight of its own
      /// that holds the rows and an empty state with no action (a glyph and one
      /// sentence, 139 pixels); or the card floored at what it holds, the grid the
      /// filling part of a FillPanel of its own with a MinHeight of the column headers
      /// and the rows, and the empty state laid over that FillPanel in one grid.
      /// </summary>
      internal static string ListCardFloorProblem(XElement card, string grid, int rows)
      {
         const double Chrome = 16 + 2;   // the card's padding and border
         const double Headers = 33;
         const double Row = 34;
         const double EmptyWithoutAction = 139;

         XElement list = card.Descendants(Wpf + "DataGrid").FirstOrDefault(g => (string)g.Attribute(X + "Name") == grid);
         if (list == null)
            return "the card holds no grid named " + grid + ".";

         double cardFloor = Height(card.Attribute("MinHeight"));
         if (cardFloor > 0)
         {
            if (cardFloor < Chrome + Headers + rows * Row)
               return "the list's floor is " + cardFloor + " pixels, less than " + rows + " rows.";
            if (cardFloor < Chrome + EmptyWithoutAction)
               return "the list's floor is " + cardFloor + " pixels, which cuts off the empty state's sentence.";
            if (card.Descendants(Scaffold + "EmptyState.Action").Any())
               return "the card is floored at " + cardFloor + " pixels, which an empty state with an action does not fit in a short window; floor it at what it holds.";
            return null;
         }

         XElement panel = list.Parent;
         if (panel == null || panel.Name != Scaffold + "FillPanel")
            return "the card has no floor of its own and its grid is not on a FillPanel of its own.";
         if ((string)list.Attribute(Scaffold + "FillPanel.Fill") != "True")
            return "the grid is not the filling part of its FillPanel.";
         double gridFloor = Height(list.Attribute("MinHeight"));
         if (gridFloor < Headers + rows * Row)
            return "the grid's floor is " + gridFloor + " pixels, less than its headers and " + rows + " rows.";
         if (panel.Parent == null || !panel.Parent.Elements(Scaffold + "EmptyState").Any())
            return "no empty state is laid over the grid's FillPanel, so nothing raises the card's floor when the list is empty.";
         return null;
      }

      private static double Height(XAttribute attribute)
         => double.Parse((string)attribute ?? "0", CultureInfo.InvariantCulture);

      /// <summary>The body of every <c>new CollectionSpec { ... }</c> in the file, by brace counting.</summary>
      private static IEnumerable<string> SpecBodies(string code)
      {
         foreach (Match m in Regex.Matches(code, @"new CollectionSpec\s*\{"))
         {
            int depth = 0;
            for (int i = m.Index + m.Length - 1; i < code.Length; i++)
            {
               if (code[i] == '{')
                  depth++;
               else if (code[i] == '}' && --depth == 0)
               {
                  yield return code.Substring(m.Index, i + 1 - m.Index);
                  break;
               }
            }
         }
      }

      private static string ReadControlPanelFile(string relative)
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
