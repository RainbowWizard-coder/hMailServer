// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The TCP/IP ports page gives a TLS port its certificate and says a change in a
   /// notice, not a question (review finding F222). The add form offered SSL/TLS and
   /// both STARTTLS modes but no certificate, so the port it added had none and did
   /// not start; the port's dialog saved such a port with "(none)"; and "Reinitialise
   /// the server now?" followed every add, edit, delete and restore.
   /// </summary>
   [Collection("Loc")]
   public class PortChangesTests : IDisposable
   {
      private readonly object session_ = new();

      public void Dispose()
      {
         PortChanges.Applied(session_);
         Loc.Reset();
      }

      [Fact]
      public void AChange_WaitsUntilItsSessionsServerIsReinitialised()
      {
         Assert.False(PortChanges.ArePending(session_));

         PortChanges.Changed(session_);
         Assert.True(PortChanges.ArePending(session_));

         // A sign-in to another server does not show the first one's changes, and
         // its reinitialise does not take them.
         var other = new object();
         Assert.False(PortChanges.ArePending(other));
         PortChanges.Applied(other);
         Assert.True(PortChanges.ArePending(session_));

         PortChanges.Applied(session_);
         Assert.False(PortChanges.ArePending(session_));
         Assert.False(PortChanges.ArePending(null));
      }

      [Theory]
      [InlineData(0, 0, 2, null)]
      [InlineData(0, 3, 2, null)]
      [InlineData(1, 3, 2, null)]
      [InlineData(1, 0, 2, "Choose the certificate this port presents.")]
      [InlineData(2, 0, 2, "Choose the certificate this port presents.")]
      [InlineData(3, 0, 1, "Choose the certificate this port presents.")]
      [InlineData(1, 0, 0, "A port with SSL/TLS or STARTTLS presents a certificate, and the server has none yet: add one on the TLS certificates page first.")]
      public void APortWithConnectionSecurity_NeedsItsCertificate(int security, int certificate, int onServer, string problem)
         => Assert.Equal(problem, PortChanges.CertificateProblem(security, certificate, onServer));

      [Fact]
      public void TheNotice_SaysWhatTheServerListensOn()
         => Assert.Equal("Ports changed - the server goes on listening on the old set until it is reinitialised.", PortChanges.PendingNotice);

      /// <summary>
      /// The add form has the certificate and refuses a TLS port without one on its
      /// box; an add, an edit and a delete mark the change and read the page again,
      /// asking nothing; Restore defaults still asks; a reinitialise takes the notice.
      /// </summary>
      [Fact]
      public void ThePage_AddsTheCertificate_AndSaysAChangeInItsNotice()
      {
         string code = Read(Path.Join("Views", "TcpIpPortsView.xaml.cs"));

         string add = WithheldRowTests.MemberText(code, "private void Add_Click(object sender, RoutedEventArgs e)");
         Assert.True(add != null, "TcpIpPortsView.Add_Click is gone.");
         Assert.Contains("PortCertificateRow.Error = PortChanges.CertificateProblem(security, certificate, certificateCount_);", add, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowFirstError(PortAddressRow, PortNumberRow, PortCertificateRow)", add, StringComparison.Ordinal);
         Assert.Contains("port.SSLCertificateID = certificate;", add, StringComparison.Ordinal);
         Assert.Contains("PortChanges.Changed(ServerSession.Current);", add, StringComparison.Ordinal);
         Assert.DoesNotContain("OfferReinitialize_()", add, StringComparison.Ordinal);

         foreach (string member in new[] { "private void Edit_Click(object sender, RoutedEventArgs e)", "private void Delete_Click(object sender, RoutedEventArgs e)" })
         {
            string body = WithheldRowTests.MemberText(code, member);
            Assert.True(body != null, member + " is gone.");
            Assert.Contains("PortChanges.Changed(ServerSession.Current);", body, StringComparison.Ordinal);
            Assert.DoesNotContain("OfferReinitialize_()", body, StringComparison.Ordinal);
         }

         string restore = WithheldRowTests.MemberText(code, "private void RestoreDefaults_Click(object sender, RoutedEventArgs e)");
         Assert.True(restore != null, "TcpIpPortsView.RestoreDefaults_Click is gone.");
         Assert.Contains("PortChanges.Changed(ServerSession.Current);", restore, StringComparison.Ordinal);
         Assert.Contains("OfferReinitialize_();", restore, StringComparison.Ordinal);

         string reinitialise = WithheldRowTests.MemberText(code, "private void Reinitialise_()");
         Assert.True(reinitialise != null, "TcpIpPortsView.Reinitialise_ is gone.");
         Assert.Contains("PortChanges.Applied(ServerSession.Current);", reinitialise, StringComparison.Ordinal);

         string shown = WithheldRowTests.MemberText(code, "private void ShowPortsChanged_()");
         Assert.True(shown != null, "TcpIpPortsView.ShowPortsChanged_ is gone.");
         Assert.Contains("PortsChangedNotice.Content = reinitialise;", shown, StringComparison.Ordinal);
         Assert.Contains("ListenerSummary.Content = null;", shown, StringComparison.Ordinal);

         string main = Read("MainWindow.xaml.cs");
         string apply = WithheldRowTests.MemberText(main, "private void ApplyPending_Click(object sender, RoutedEventArgs e)");
         Assert.True(apply != null && apply.Contains("PortChanges.Applied(session);", StringComparison.Ordinal), "The pending bar's Apply now does not take the ports' changes.");
         string restart = WithheldRowTests.MemberText(main, "private async void RestartPending_Click(object sender, RoutedEventArgs e)");
         Assert.True(restart != null && restart.Contains("PortChanges.Applied(ServerSession.Current);", StringComparison.Ordinal), "A restart of the service does not take the ports' changes.");
      }

      [Fact]
      public void TheForm_HasTheCertificate_AndTheNoticeIsAboveTheList()
      {
         XElement page = XDocument.Parse(Read(Path.Join("Views", "TcpIpPortsView.xaml"))).Root;
         XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
         XElement Named(string name) => page.Descendants().Single(e => (string)e.Attribute(x + "Name") == name);

         XElement row = Named("PortCertificateRow");
         Assert.Equal("PortLine", (string)row.Attribute("LabelGroup"));
         Assert.Contains(row.Descendants(), e => (string)e.Attribute(x + "Name") == "NewCertificate");
         Assert.Equal("False", (string)Named("NewCertificate").Attribute("IsEnabled"));
         Assert.Equal("NewSecurity_SelectionChanged", (string)Named("NewSecurity").Attribute("SelectionChanged"));

         XElement notice = Named("PortsChangedNotice");
         XElement grid = Named("PortGrid");
         Assert.True(notice.IsBefore(grid), "The changes' notice is not above the list.");

         string dialog = Read(Path.Join("Views", "TcpIpPortDialog.cs"));
         string save = WithheldRowTests.MemberText(dialog, "private void Save()");
         Assert.True(save != null, "TcpIpPortDialog.Save is gone.");
         Assert.Contains("PortChanges.CertificateProblem(DialogFields.ComboValue(security_), DialogFields.ComboValue(certificate_), certificateCount_)", save, StringComparison.Ordinal);
         Assert.Contains("DialogFields.ShowError(certificateRow_, certificateProblem);", save, StringComparison.Ordinal);
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
