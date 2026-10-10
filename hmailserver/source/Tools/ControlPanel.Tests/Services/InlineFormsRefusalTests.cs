// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Xml.Linq;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The Routes, TCP/IP ports, Rules, Incoming relays and Server sendout forms say
   /// a value they refuse on the box at fault (review finding F212). Each answered
   /// with a message box - "Domain and target host are required.", "Enter a valid
   /// port number.", "Enter a name for the new rule.", "Name, lower IP and upper IP
   /// are required.", "From address and subject are required." - that took the
   /// keyboard from the form; the routes page made a port of "25a" 25 and sent its
   /// host unchecked, the ports page sent its bind address unchecked, and the relays
   /// page sent "abc" to "xyz" as a range. Each refusal is on its row now, every box
   /// at fault on one press, the keyboard in the first, a number a number, an address
   /// what the server reads as one.
   /// </summary>
   public class InlineFormsRefusalTests
   {
      [Fact]
      public void AddRoute_ChecksTheDomainTheHostAndThePort_OnTheirBoxes()
      {
         string add = Member("RoutesView.xaml.cs", "private void Add_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ClearErrors(RouteDomainRow, RouteHostRow, RoutePortRow);", add, StringComparison.Ordinal);
         Assert.Contains("RouteDomainRow.Error = L(\"Enter the domain the route is for, such as partner.com.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.IsHost(host))", add, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.TryPort(NewPort.Text, out int port))", add, StringComparison.Ordinal);
         Assert.Contains("RoutePortRow.Error = L(\"Enter a port from 1 to 65535.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(RouteDomainRow, RouteHostRow, RoutePortRow))", add, StringComparison.Ordinal);
         Assert.DoesNotContain("port = 25;", add, StringComparison.Ordinal);
         Assert.DoesNotContain("are required.", add, StringComparison.Ordinal);
      }

      [Fact]
      public void AddPort_ChecksTheAddressAndThePort_OnTheirBoxes()
      {
         string add = Member("TcpIpPortsView.xaml.cs", "private void Add_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("if (!InlineFormChecks.IsAddress(NewAddress.Text))", add, StringComparison.Ordinal);
         Assert.Contains("PortAddressRow.Error = L(\"Enter the address to listen on, such as 0.0.0.0 for every IPv4 address.\");", add, StringComparison.Ordinal);
         Assert.Contains("PortNumberRow.Error = L(\"Enter a port from 1 to 65535.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(PortAddressRow, PortNumberRow, PortCertificateRow))", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(L(\"Enter a valid port number.\")", add, StringComparison.Ordinal);
      }

      [Fact]
      public void AddRule_SaysAnEmptyName_OnItsBox()
      {
         string add = Member("RulesView.xaml.cs", "private void AddRule_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ShowError(NewRuleNameRow, L(\"Enter a name for the new rule.\"));", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(L(\"Enter a name for the new rule.\")", add, StringComparison.Ordinal);
      }

      [Fact]
      public void AddRelay_ChecksTheNameAndTheRangesEnds_OnTheirBoxes()
      {
         string page = Read("UtilityViews.cs");
         string add = WithheldRowTests.MemberText(page, "      private void Add()");
         Assert.True(add != null, "IncomingRelaysView.Add is gone.");
         Assert.Contains("DialogFields.ClearErrors(nameRow_, lowerRow_, upperRow_);", add, StringComparison.Ordinal);
         Assert.Contains("nameRow_.Error = L(\"Enter a name for the relay.\");", add, StringComparison.Ordinal);
         Assert.Contains("lowerRow_.Error = L(\"Enter the lower address of the range, such as 10.0.0.0.\");", add, StringComparison.Ordinal);
         Assert.Contains("upperRow_.Error = L(\"Enter the upper address of the range, such as 10.0.0.255.\");", add, StringComparison.Ordinal);
         Assert.Contains("InlineFormChecks.OrderOf(lower, upper)", add, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(nameRow_, lowerRow_, upperRow_))", add, StringComparison.Ordinal);
         Assert.DoesNotContain("are required.", add, StringComparison.Ordinal);

         // A line whose captions share a height, Add under a row of it.
         Assert.Contains("Grid.SetIsSharedSizeScope(row, true);", page, StringComparison.Ordinal);
         Assert.Contains("addLine.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto, SharedSizeGroup = \"RelayLine\" });", page, StringComparison.Ordinal);
         foreach (string row in new[] { "nameRow_", "lowerRow_", "upperRow_" })
            Assert.Contains("private readonly FieldRow " + row + " = new FieldRow { Label = ", page, StringComparison.Ordinal);
         Assert.Contains("LabelGroup = \"RelayLine\" };", page, StringComparison.Ordinal);
      }

      [Fact]
      public void TheSendout_SaysEachBoxItRefuses_OnThatBox()
      {
         string send = Member("UtilityViews.cs", "private void Send()");
         Assert.Contains("DialogFields.ClearErrors(fromRow_, subjectRow_, bodyRow_);", send, StringComparison.Ordinal);
         Assert.Contains("fromRow_.Error = L(\"Enter the address the message comes from, such as postmaster@example.com.\");", send, StringComparison.Ordinal);
         Assert.Contains("subjectRow_.Error = L(\"Enter a subject.\");", send, StringComparison.Ordinal);
         Assert.Contains("bodyRow_.Error = L(\"Write the message first: an empty one is not sent.\");", send, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(fromRow_, subjectRow_, bodyRow_))", send, StringComparison.Ordinal);
         Assert.DoesNotContain("From address and subject are required.", send, StringComparison.Ordinal);
      }

      [Theory]
      [InlineData("RoutesView.xaml", "AddRouteForm", "RoutePortRow", "AddRouteButton")]
      [InlineData("TcpIpPortsView.xaml", "AddPortForm", "PortNumberRow", "AddPortButton")]
      [InlineData("RulesView.xaml", "AddRuleForm", "NewRuleNameRow", "AddRuleButton")]
      public void TheAddButton_LinesUpWithTheBoxes(string file, string form, string row, string button)
      {
         XElement page = XDocument.Parse(Read(file)).Root;
         FormLineAssert.ButtonLinesUpWithItsRow(page, form, row, button, file);
      }

      // ---- helpers -------------------------------------------------------------------------

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));

      private static string Member(string file, string signature)
      {
         string member = WithheldRowTests.MemberText(Read(file), signature);
         Assert.True(member != null, file + ": " + signature + " is gone.");
         return member;
      }
   }
}
