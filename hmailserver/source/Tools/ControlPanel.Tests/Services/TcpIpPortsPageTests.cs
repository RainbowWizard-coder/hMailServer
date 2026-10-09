// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text.RegularExpressions;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The TCP/IP ports page's grid in a 1280 by 800 window, in Japanese: the
   /// Listening column read "待ち受けていませ" for 待ち受けていません (Not
   /// listening), the last glyph cut with nothing to say so. The grid's fixed
   /// columns shrink to their MinWidth once the grid is narrower than they are and
   /// the certificate column has given up all it can, and none of them had a floor
   /// of its own, so the 12 pixels the page keeps clear of its scroll bar took
   /// three from the Listening column, which had held the word by that much. Every
   /// test here fails against that page: no column is narrower than its own
   /// header, the Listening column holds its widest word in the current language
   /// with the mark before it, and the width of a cell's words is measured in the
   /// cell's own font with the cell template's padding. Source-shape tests, as the
   /// Suppression list's are: this project has no WPF to measure a word with.
   /// </summary>
   public class TcpIpPortsPageTests
   {
      [Fact]
      public void EveryColumn_IsFlooredAtItsHeader_AndTheListeningColumnAtItsWidestWord()
      {
         string code = Source();
         string floors = WithheldRowTests.MemberText(code, "private void FloorColumns_()");
         Assert.True(floors != null, "FloorColumns_ is gone, so the ports grid's columns have no floor.");
         Assert.Contains("foreach (DataGridColumn column in PortGrid.Columns)", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(column, GridStyles.HeaderWidth(column.Header as string));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(AddressColumn, GridStyles.CellWidth(\"255.255.255.255\"));", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(PortColumn, GridStyles.CellWidth(\"65535\"));", floors, StringComparison.Ordinal);
         Assert.Contains("foreach (string name in SecurityNames)", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(SecurityColumn, GridStyles.CellWidth(name));", floors, StringComparison.Ordinal);
         Assert.Contains("foreach (string name in SmtpAuthNames)", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(AuthColumn, GridStyles.CellWidth(name));", floors, StringComparison.Ordinal);
         Assert.Contains("foreach (ListenerState state in Enum.GetValues<ListenerState>())", floors, StringComparison.Ordinal);
         Assert.Contains("Floor_(ListeningColumn, GridStyles.CellWidth(ListenerProbe.WordFor(state), ListeningMarkWidth + ListeningMarkGap));", floors, StringComparison.Ordinal);

         string floor = WithheldRowTests.MemberText(code, "private static void Floor_(DataGridColumn column, double width)");
         Assert.True(floor != null && floor.Contains("column.MinWidth = Math.Max(column.MinWidth, width);", StringComparison.Ordinal), "A floor can lower a column's MinWidth.");

         string ctor = WithheldRowTests.MemberText(code, "public TcpIpPortsView()");
         Assert.True(ctor != null && ctor.Contains("FloorColumns_();", StringComparison.Ordinal), "The page is built without flooring its columns.");

         // Every column the floors name is in the grid, under that name.
         string xaml = ReadControlPanelFile(Path.Join("Views", "TcpIpPortsView.xaml"));
         foreach (string name in new[] { "ProtocolColumn", "AddressColumn", "PortColumn", "SecurityColumn", "AuthColumn", "ListeningColumn" })
            Assert.Contains("x:Name=\"" + name + "\"", xaml, StringComparison.Ordinal);
      }

      [Fact]
      public void TheMarkBeforeTheWord_IsCountedAtTheSizeItIsDrawn()
      {
         string code = Source();
         Assert.Contains("private const double ListeningMarkWidth = 10;", code, StringComparison.Ordinal);
         Assert.Contains("private const double ListeningMarkGap = 6;", code, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Width = ListeningMarkWidth,\s*Height = ListeningMarkWidth,"), code);
         Assert.DoesNotContain("Width = 10,", code, StringComparison.Ordinal);

         string xaml = ReadControlPanelFile(Path.Join("Views", "TcpIpPortsView.xaml"));
         Assert.Contains("<DataGridTemplateColumn x:Name=\"ListeningColumn\" Header=\"{loc:L 'Listening'}\"", xaml, StringComparison.Ordinal);
         Assert.Matches(new Regex(@"Content=""\{Binding ListeningMark\}""\s*VerticalAlignment=""Center"" Margin=""0,0,6,0"""), xaml);
      }

      [Fact]
      public void CellWidth_MeasuresTheWordsAsTheWindowDrawsThem_WithTheCellsMarginAndPaddingAndWhatComesBefore()
      {
         string code = ReadControlPanelFile(Path.Join("Services", "GridStyles.cs"));
         string width = WithheldRowTests.MemberText(code, "public static double CellWidth(string text, double before = 0)");
         Assert.True(width != null, "GridStyles.CellWidth is gone, so no column can be floored at its words.");
         Assert.Contains("TryFindResource(typeof(DataGridCell))", width, StringComparison.Ordinal);
         Assert.Contains("Setting(style, Control.FontSizeProperty, 13.0)", width, StringComparison.Ordinal);
         Assert.Contains("Setting(style, Control.FontWeightProperty, FontWeights.Normal)", width, StringComparison.Ordinal);
         Assert.Contains("TextFormattingMode mode = window != null ? TextOptions.GetTextFormattingMode(window) : TextFormattingMode.Ideal;", width, StringComparison.Ordinal);
         Assert.Contains("double pixelsPerDip = window != null ? VisualTreeHelper.GetDpi(window).PixelsPerDip : 1.0;", width, StringComparison.Ordinal);
         Assert.Contains("new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,", width, StringComparison.Ordinal);
         Assert.Contains("new Typeface(family, FontStyles.Normal, weight, FontStretches.Normal), size, Brushes.Black, null, mode, pixelsPerDip);", width, StringComparison.Ordinal);
         Assert.Contains("return Math.Ceiling(words.WidthIncludingTrailingWhitespace) + CellTextMargin.Left + CellTextMargin.Right + CellPadding.Left + CellPadding.Right + before;", width, StringComparison.Ordinal);
         Assert.Contains("private static readonly Thickness CellPadding = new(10, 0, 10, 0);", code, StringComparison.Ordinal);
         Assert.Contains("private static readonly Thickness CellTextMargin = new(2, 0, 2, 0);", code, StringComparison.Ordinal);

         // The padding is the cell template's, in App.xaml: a Border with Padding="10,0" around the content.
         string xaml = ReadControlPanelFile("App.xaml");
         int cell = xaml.IndexOf("<Style TargetType=\"DataGridCell\">", StringComparison.Ordinal);
         Assert.True(cell >= 0, "The DataGridCell style is gone from App.xaml.");
         int template = xaml.IndexOf("<ControlTemplate TargetType=\"DataGridCell\">", cell, StringComparison.Ordinal);
         Assert.True(template >= 0, "The DataGridCell style has no template of its own.");
         Assert.Contains("Padding=\"10,0\"", xaml.Substring(template, 400), StringComparison.Ordinal);
         Assert.Contains("<Setter Property=\"FontSize\" Value=\"13\" />", xaml.Substring(cell, template - cell), StringComparison.Ordinal);
      }

      /// <summary>
      /// The listener warning offers what its commonest cause needs: Reinitialise
      /// now, as the notice's action, which asks first with Cancel the answer Enter
      /// gives - every connected client is disconnected for a moment - and reads
      /// the page again after. The sentence is ListenerProbe's whole one, which
      /// names each port as an endpoint is written.
      /// </summary>
      [Fact]
      public void TheListenerWarning_OffersReinitialiseNow()
      {
         string code = Source();
         string summary = WithheldRowTests.MemberText(code, "private void ShowListenerSummary(List<PortRow> rows, bool local, int? state)");
         Assert.True(summary != null, "ShowListenerSummary is gone.");
         Assert.Contains("ListenerProbe.NotListeningSummary(names), ReinitialiseButton_()", summary, StringComparison.Ordinal);
         Assert.Contains("ListenerProbe.Endpoint(row.Address, row.Port)", summary, StringComparison.Ordinal);

         string button = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button ReinitialiseButton_()");
         Assert.True(button != null, "ReinitialiseButton_ is gone.");
         Assert.Contains("MessageBox.Confirm(", button, StringComparison.Ordinal);
         Assert.Contains("cancelByDefault: true", button, StringComparison.Ordinal);
         Assert.Contains("Reinitialise_();", button, StringComparison.Ordinal);

         string reinitialise = WithheldRowTests.MemberText(code, "private void Reinitialise_()");
         Assert.True(reinitialise != null, "Reinitialise_ is gone.");
         Assert.Contains("Application.Reinitialize()", reinitialise, StringComparison.Ordinal);
         Assert.Contains("Reload();", reinitialise, StringComparison.Ordinal);
      }

      /// <summary>
      /// With the server paused, in maintenance mode, starting or stopping, no
      /// port is listened on, and the page says why at the Information level with
      /// no Reinitialise now, which would end maintenance mode or resume the
      /// paused server: it blamed a port just added and offered it. The state is
      /// read before the warning is built, and a reinitialise of a server that is
      /// not running - from the warning or after a save - asks again first.
      /// </summary>
      [Fact]
      public void AServerNotRunning_IsSaidAtInformation_WithNoReinitialise_AndAReinitialiseOfItAsksAgain()
      {
         string code = Source();
         string summary = WithheldRowTests.MemberText(code, "private void ShowListenerSummary(List<PortRow> rows, bool local, int? state)");
         Assert.True(summary != null, "ShowListenerSummary is gone.");

         int state = summary.IndexOf("ListenerProbe.WhyNoPortIsListenedOn(state)", StringComparison.Ordinal);
         int information = summary.IndexOf("ShowSummary_(StatusLevel.Information, notRunning,", StringComparison.Ordinal);
         int warning = summary.IndexOf("ShowSummary_(StatusLevel.Warning, ListenerProbe.NotListeningSummary(names)", StringComparison.Ordinal);
         Assert.True(state >= 0 && information > state && warning > information, "The server's state is not read before the warning.");
         string read = WithheldRowTests.MemberText(code, "private void Read_()");
         Assert.True(read != null, "Read_ is gone.");
         Assert.Contains("int? serverState = local ? ReadServerState_() : null;", read, StringComparison.Ordinal);
         Assert.Contains("ShowListenerSummary(rows, local, serverState);", read, StringComparison.Ordinal);
         Assert.Contains("OpenStatusButton_()", summary, StringComparison.Ordinal);
         Assert.Contains("NoticeActions.OpenPage(\"status\",", WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button OpenStatusButton_()") ?? "", StringComparison.Ordinal);
         string notRunning = summary.Substring(information, summary.IndexOf("return;", information, StringComparison.Ordinal) - information);
         Assert.DoesNotContain("ReinitialiseButton_", notRunning, StringComparison.Ordinal);

         string readState = WithheldRowTests.MemberText(code, "private static int? ReadServerState_()");
         Assert.True(readState != null, "ReadServerState_ is gone.");
         Assert.Contains("(int)ServerSession.Current.Application.ServerState", readState, StringComparison.Ordinal);
         Assert.Contains("return null;", readState, StringComparison.Ordinal);

         string reinitialise = WithheldRowTests.MemberText(code, "private void Reinitialise_()");
         int ask = reinitialise.IndexOf("ListenerProbe.ReinitialiseQuestionWhenNotRunning(ReadServerState_())", StringComparison.Ordinal);
         int act = reinitialise.IndexOf("Application.Reinitialize()", StringComparison.Ordinal);
         Assert.True(ask >= 0 && act > ask, "A reinitialise of a server not running does not ask first.");
      }

      /// <summary>
      /// Each row of a server that is not running says what the line under the
      /// list says, at its level: the rows were each a Warning beside the
      /// Information line, and each row's tool tip and accessible name blamed a
      /// port just added or another program for a port that a paused server, or
      /// one in maintenance mode, does not listen on. The state is read once, with
      /// the ports, for the rows and the line alike.
      /// </summary>
      [Fact]
      public void EachRowOfAServerNotRunning_SaysWhatTheLineSays_AtItsLevel()
      {
         string read = WithheldRowTests.MemberText(Source(), "private void Read_()");
         Assert.True(read != null, "Read_ is gone.");
         int state = read.IndexOf("int? serverState = local ? ReadServerState_() : null;", StringComparison.Ordinal);
         int rows = read.IndexOf("rows.Add(new PortRow", StringComparison.Ordinal);
         Assert.True(state >= 0 && rows > state, "The server's state is not read before the rows are built.");
         Assert.Contains("StatusSemantics.For(ListenerProbe.LevelFor(state, serverState))", read, StringComparison.Ordinal);
         Assert.Contains("ListeningWord = ListenerProbe.WordFor(state, serverState),", read, StringComparison.Ordinal);
         Assert.Contains("ListeningTip = ListenerProbe.TipFor(state, protocol, serverState),", read, StringComparison.Ordinal);
         Assert.DoesNotContain("ListenerProbe.LevelFor(state))", read, StringComparison.Ordinal);
         Assert.DoesNotContain("ListenerProbe.WordFor(state)", read, StringComparison.Ordinal);
         // Once, for the rows and the line alike: the line reads no state of its own.
         string summary = WithheldRowTests.MemberText(Source(), "private void ShowListenerSummary(List<PortRow> rows, bool local, int? state)");
         Assert.True(summary != null, "ShowListenerSummary is gone.");
         Assert.DoesNotContain("ReadServerState_()", summary, StringComparison.Ordinal);
      }

      /// <summary>
      /// The line under the list finds the ports not listened on by each row's
      /// state, not by its word: while the server is starting or stopping such a
      /// port reads Unknown, as the line says that whether its ports are being
      /// listened on cannot be said yet, and a line that compared the word with
      /// "Not listening" would find none of them and say nothing. The word was
      /// kept as "Not listening" beside that sentence for this reason alone.
      /// </summary>
      [Fact]
      public void TheLineFindsThePortsNotListenedOn_ByTheirState_NotByTheirWord()
      {
         string code = Source();
         string summary = WithheldRowTests.MemberText(code, "private void ShowListenerSummary(List<PortRow> rows, bool local, int? state)");
         Assert.True(summary != null, "ShowListenerSummary is gone.");
         Assert.Contains("var down = rows.FindAll(r => r.State == ListenerState.NotListening);", summary, StringComparison.Ordinal);
         Assert.DoesNotContain("ListeningWord", summary, StringComparison.Ordinal);
         Assert.DoesNotContain("WordFor(", summary, StringComparison.Ordinal);

         Assert.Contains("public ListenerState State { get; set; }", code, StringComparison.Ordinal);
         string read = WithheldRowTests.MemberText(code, "private void Read_()");
         Assert.True(read != null, "Read_ is gone.");
         Assert.Contains("State = state,", read, StringComparison.Ordinal);

         // Nothing else on the page tells a row's state by its word.
         Assert.DoesNotMatch(new Regex(@"ListeningWord\s*[!=]="), code);
      }

      /// <summary>
      /// The line's actions are made once and kept across reads, and the keyboard
      /// focus leaves the line before its action does: Reinitialise now read the
      /// page again as its last step, which put a new button in the focused one's
      /// place or collapsed the line, and the focus fell to the window, so Tab and
      /// a screen reader started again from the top of the page.
      /// </summary>
      [Fact]
      public void TheLinesActions_AreKeptAcrossReads_AndTheFocusLeavesTheLineFirst()
      {
         string code = Source();

         string button = WithheldRowTests.MemberText(code, "private Wpf.Ui.Controls.Button ReinitialiseButton_()");
         Assert.True(button != null, "ReinitialiseButton_ is gone.");
         Assert.Contains("if (reinitialiseButton_ != null)", button, StringComparison.Ordinal);
         Assert.Contains("return reinitialiseButton_ = button;", button, StringComparison.Ordinal);
         Assert.Contains("=> openStatusButton_ ??= NoticeActions.OpenPage(", code, StringComparison.Ordinal);

         string show = WithheldRowTests.MemberText(code, "private void ShowSummary_(StatusLevel level, string text, object action = null)");
         Assert.True(show != null, "ShowSummary_ is gone.");
         int guard = show.IndexOf("if (!ReferenceEquals(ListenerSummary.Content, action) && ListenerSummary.IsKeyboardFocusWithin)", StringComparison.Ordinal);
         int content = show.IndexOf("ListenerSummary.Content = action;", StringComparison.Ordinal);
         Assert.True(guard >= 0 && content > guard, "The focused action leaves the line before the focus does.");

         string hide = WithheldRowTests.MemberText(code, "private void HideSummary_()");
         Assert.True(hide != null, "HideSummary_ is gone.");
         Assert.True(hide.IndexOf("MoveFocusFromSummary_();", StringComparison.Ordinal) < hide.IndexOf("Visibility.Collapsed", StringComparison.Ordinal));

         // Every way the line is put away goes through HideSummary_.
         Assert.Single(System.Text.RegularExpressions.Regex.Matches(code, @"ListenerSummary\.Visibility = Visibility\.Collapsed;"));

         string move = WithheldRowTests.MemberText(code, "private void MoveFocusFromSummary_()");
         Assert.True(move != null, "MoveFocusFromSummary_ is gone.");
         Assert.Contains("PortGrid.Focus()", move, StringComparison.Ordinal);
         Assert.Contains("FocusNavigationDirection.Next", move, StringComparison.Ordinal);
      }

      private static string Source() => ReadControlPanelFile(Path.Join("Views", "TcpIpPortsView.xaml.cs"));

      private static string ReadControlPanelFile(string relative)
      {
         var directory = new DirectoryInfo(AppContext.BaseDirectory);
         while (directory != null)
         {
            string candidate = Path.Join(directory.FullName, "ControlPanel", relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
               return File.ReadAllText(candidate);
            directory = directory.Parent;
         }

         Assert.Fail("ControlPanel/" + relative + " could not be found from the test binaries.");
         return null;
      }
   }
}
