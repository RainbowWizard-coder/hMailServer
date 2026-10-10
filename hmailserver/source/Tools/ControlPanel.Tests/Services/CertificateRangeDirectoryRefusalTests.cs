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
   /// The SSL certificates, IP ranges and Directory authentication (LDAP) pages say
   /// a value they refuse on the box at fault (review finding F129, with F212's
   /// certificate and IP range parts). "Name, certificate file and private key file
   /// are required." and "Name, lower IP and upper IP are required." were message
   /// boxes captioned "Control Panel" that pointed at no box; a priority that was
   /// no number became 15 without a word; the ends of a range were sent as typed
   /// for the server to refuse; LDAP's port and timeout were refused only in the
   /// notice under the header; and the certificate form's hint sent the reader to
   /// "the box below" for a box beside the others.
   /// </summary>
   public class CertificateRangeDirectoryRefusalTests
   {
      [Fact]
      public void AddCertificate_SaysEachEmptyBox_OnThatBox()
      {
         string add = Member("SslCertificatesView.xaml.cs", "private async void Add_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ClearErrors(NewCertNameRow, NewCertFileRow, NewKeyFileRow);", add, StringComparison.Ordinal);
         Assert.Contains("NewCertNameRow.Error = L(\"Enter a name for the certificate.\");", add, StringComparison.Ordinal);
         Assert.Contains("NewCertFileRow.Error = L(\"Enter the path of the certificate file, or choose the file with the … button beside the box.\");", add, StringComparison.Ordinal);
         Assert.Contains("NewKeyFileRow.Error = L(\"Enter the path of the private key file, or choose the file with the … button beside the box.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (DialogFields.ShowFirstError(NewCertNameRow, NewCertFileRow, NewKeyFileRow))", add, StringComparison.Ordinal);
         Assert.DoesNotContain("are required.", add, StringComparison.Ordinal);

         // The hint names the box it means, which is beside the others, not below.
         string page = Read("SslCertificatesView.xaml.cs");
         Assert.Contains("Enter its passphrase in the Key passphrase box before adding", page, StringComparison.Ordinal);
         Assert.DoesNotContain("in the box below", page, StringComparison.Ordinal);
      }

      [Fact]
      public void AddRange_RefusesEachValue_AndNeverPutsInAPriorityOfItsOwn()
      {
         string add = Member("IPRangesView.xaml.cs", "private void Add_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("DialogFields.ClearErrors(NewNameRow, NewLowerRow, NewUpperRow, NewPriorityRow);", add, StringComparison.Ordinal);
         Assert.Contains("NewNameRow.Error = L(\"Enter a name for the range.\");", add, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.IsAddress(lower))", add, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.IsAddress(upper))", add, StringComparison.Ordinal);
         Assert.Contains("InlineFormChecks.OrderOf(lower, upper)", add, StringComparison.Ordinal);
         Assert.Contains("L(\"The upper address must not be below the lower.\")", add, StringComparison.Ordinal);
         Assert.Contains("L(\"The upper address must be of the same kind as the lower: both IPv4, or both IPv6.\")", add, StringComparison.Ordinal);
         Assert.Contains("if (!InlineFormChecks.TryWholeNumber(NewPriority.Text, int.MinValue, int.MaxValue, out int priority))", add, StringComparison.Ordinal);
         // The range dialog's sentence, short enough for the form's narrow column; the
         // page's description says that the higher priority wins.
         Assert.Contains("NewPriorityRow.Error = L(\"Enter a whole number.\");", add, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(priorityRow_, L(\"Enter a whole number.\"));", Member("IPRangeDialog.cs", "private void Save()"), StringComparison.Ordinal);
         Assert.DoesNotContain("priority = 15", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(L(\"Name, lower IP and upper IP are required.\")", add, StringComparison.Ordinal);
      }

      // The certificates' form is two lines of two fields with Add in its card's footer
      // since review finding F236 (CertificatesAddFlowTests holds it).
      [Theory]
      [InlineData("IPRangesView.xaml", "AddRangeForm", "NewPriorityRow", "AddRangeButton")]
      public void TheAddButton_LinesUpWithTheBoxes(string file, string form, string row, string button)
      {
         XElement page = XDocument.Parse(Read(file)).Root;
         FormLineAssert.ButtonLinesUpWithItsRow(page, form, row, button, file);

         // Every field of the line in its group, none on the line's bottom edge.
         foreach (XElement field in FormLineAssert.Named(page, form, file).Elements())
         {
            if (field.Name.LocalName != "FieldRow")
               continue;
            Assert.Equal((string)FormLineAssert.Named(page, row, file).Attribute("LabelGroup"), (string)field.Attribute("LabelGroup"));
            Assert.Null(field.Attribute("VerticalAlignment"));
         }
      }

      [Fact]
      public void DirectoryAuthentication_SaysAPortOrATimeoutItRefuses_OnTheBoxToo()
      {
         string page = Read("LdapSettingsView.cs");
         Assert.Contains("content.Children.Add(portRow_ = LabelledBox_(L(\"_Port\"), port_, \"ldap-port\",", page, StringComparison.Ordinal);
         Assert.Contains("content.Children.Add(timeoutRow_ = LabelledBox_(L(\"Time_out in seconds\"), timeout_, \"ldap-timeout\",", page, StringComparison.Ordinal);

         // In the notice under the header, as every refused save on the settings
         // pages is (T2.5), and on the box, with the keyboard put in it.
         Assert.Contains("DialogFields.ClearErrors(portRow_, timeoutRow_);", page, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(portRow_, L(\"Enter a port from 1 to 65535, or leave the box empty for the automatic port.\"));", page, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(timeoutRow_, L(\"Enter a whole number of seconds from 1 to 90.\"));", page, StringComparison.Ordinal);
         Assert.Contains("saveState_.Refuse(portError, () => !TryPort_(out _, out _, out _));", page, StringComparison.Ordinal);
         Assert.Contains("saveState_.Refuse(timeoutError, () => !TryTimeout_(out _, out _, out _));", page, StringComparison.Ordinal);
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
