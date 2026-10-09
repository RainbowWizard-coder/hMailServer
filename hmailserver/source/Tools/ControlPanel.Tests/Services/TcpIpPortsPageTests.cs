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
         string summary = WithheldRowTests.MemberText(code, "private void ShowListenerSummary(List<PortRow> rows, bool local)");
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
         string summary = WithheldRowTests.MemberText(code, "private void ShowListenerSummary(List<PortRow> rows, bool local)");
         Assert.True(summary != null, "ShowListenerSummary is gone.");

         int state = summary.IndexOf("ListenerProbe.WhyNoPortIsListenedOn(state)", StringComparison.Ordinal);
         int information = summary.IndexOf("ShowSummary_(StatusLevel.Information, notRunning,", StringComparison.Ordinal);
         int warning = summary.IndexOf("ShowSummary_(StatusLevel.Warning, ListenerProbe.NotListeningSummary(names)", StringComparison.Ordinal);
         Assert.True(state >= 0 && information > state && warning > information, "The server's state is not read before the warning.");
         Assert.Contains("int? state = ReadServerState_();", summary, StringComparison.Ordinal);
         Assert.Contains("NoticeActions.OpenPage(\"status\",", summary, StringComparison.Ordinal);
         string notRunning = summary.Substring(information, summary.IndexOf("return;", information, StringComparison.Ordinal) - information);
         Assert.DoesNotContain("ReinitialiseButton_", notRunning, StringComparison.Ordinal);

         string read = WithheldRowTests.MemberText(code, "private static int? ReadServerState_()");
         Assert.True(read != null, "ReadServerState_ is gone.");
         Assert.Contains("(int)ServerSession.Current.Application.ServerState", read, StringComparison.Ordinal);
         Assert.Contains("return null;", read, StringComparison.Ordinal);

         string reinitialise = WithheldRowTests.MemberText(code, "private void Reinitialise_()");
         int ask = reinitialise.IndexOf("ListenerProbe.ReinitialiseQuestionWhenNotRunning(ReadServerState_())", StringComparison.Ordinal);
         int act = reinitialise.IndexOf("Application.Reinitialize()", StringComparison.Ordinal);
         Assert.True(ask >= 0 && act > ask, "A reinitialise of a server not running does not ask first.");
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
