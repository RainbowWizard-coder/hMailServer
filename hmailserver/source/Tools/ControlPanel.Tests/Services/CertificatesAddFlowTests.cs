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
   /// Review finding F236: the SSL certificates page's add and passphrase flows said
   /// nothing and kept every control live whatever the state. A certificate added was
   /// neither selected nor said; its name had to be typed although the subject is in
   /// the file; five fields shared one line, so the passphrase's caption wrapped to two
   /// lines in English and three in German and the path boxes were 100 pixels at 760;
   /// Remove stored passphrase could be pressed with nothing stored, and Save passphrase
   /// with an empty box opened a message box; the passphrase editor was offered for a
   /// key that needs none; and the Name column was cut at 170 pixels while the paths
   /// took the rest. These hold the form's two lines, the name from the file, the added
   /// certificate selected with the way to the ports, the editor's state and the
   /// columns.
   /// </summary>
   public class CertificatesAddFlowTests
   {
      private static readonly XNamespace X = "http://schemas.microsoft.com/winfx/2006/xaml";

      [Theory]
      [InlineData(@"C:\Program Files\hMailServer\Data\Certificates\mail.example.com.crt", "mail.example.com.crt")]
      [InlineData("/etc/hmailserver/certs/key.pem", "key.pem")]
      [InlineData("cert.pem", "cert.pem")]
      [InlineData("", "")]
      [InlineData(null, "")]
      public void ThePathColumns_ShowTheFilesName(string path, string name)
         => Assert.Equal(name, CertificateRows.FileName(path));

      [Theory]
      // A key read as unencrypted, nothing stored: the editor waits behind Set a passphrase anyway...
      [InlineData(false, false, false, false)]
      // Encrypted, or not known here, or a passphrase stored to remove: the editor is offered.
      [InlineData(false, false, true, true)]
      [InlineData(false, false, null, true)]
      [InlineData(true, false, false, true)]
      [InlineData(false, true, false, true)]
      public void ThePassphraseEditor_IsOffered_WhereAKeyCanNeedOne(bool stored, bool unknown, bool? encrypted, bool offered)
         => Assert.Equal(offered, CertificateRows.OffersPassphrase(stored, unknown, encrypted));

      [Fact]
      public void TheForm_IsTwoLinesOfTwoFields_WithAddInTheCardsFooter()
      {
         XElement page = XDocument.Parse(Read("SslCertificatesView.xaml")).Root;
         XElement form = Named(page, "AddCertForm");
         Assert.Equal("True", (string)form.Attribute("Grid.IsSharedSizeScope"));

         var fields = form.Elements().Where(e => e.Name.LocalName == "FieldRow").ToList();
         Assert.Equal(4, fields.Count);
         foreach (XElement field in fields)
         {
            bool first = (string)field.Attribute("Grid.Row") == null;
            Assert.Equal(first ? "CertNameLine" : "CertFileLine", (string)field.Attribute("LabelGroup"));
         }

         // Add is not on a line of the form, and is the card's footer.
         XElement add = Named(page, "AddCertButton");
         Assert.False(form.Descendants().Contains(add), "Add is still on the form's line.");
         Assert.Equal("Card.Footer", add.Parent.Name.LocalName);
      }

      [Fact]
      public void TheName_IsTheCertificatesOwn_WhileTheBoxIsEmpty()
      {
         string code = Read("SslCertificatesView.xaml.cs");
         Assert.Contains("if (string.IsNullOrWhiteSpace(NewCertName.Text) && CertificateInspector.NameOf(file) is string name)", code, StringComparison.Ordinal);

         string inspector = ReadService("CertificateInspector.cs");
         Assert.Contains("certificate.GetNameInfo(X509NameType.SimpleName, false)", inspector, StringComparison.Ordinal);
      }

      [Fact]
      public void AnAddedCertificate_IsSelected_AndTheWayToThePortsIsSaid()
      {
         string add = Member(Read("SslCertificatesView.xaml.cs"), "private async void Add_Click(object sender, RoutedEventArgs e)");
         Assert.Contains("added = (int)cert.ID;", add, StringComparison.Ordinal);
         Assert.Contains("Reload(added, () => ShowOutcome_(StatusLevel.Good,", add, StringComparison.Ordinal);
         Assert.Contains("ListOutcome.Added(name, DateTime.Now)", add, StringComparison.Ordinal);
         Assert.Contains("OpenPortsButton_()", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(", add, StringComparison.Ordinal);
         Assert.Contains("NoticeActions.OpenPage(\"ports\"", Read("SslCertificatesView.xaml.cs"), StringComparison.Ordinal);
      }

      [Fact]
      public void ThePassphraseButtons_FollowWhatThereIsToDo()
      {
         string code = Read("SslCertificatesView.xaml.cs");
         string editor = Member(code, "private void ShowPassphraseEditor_(CertRow row)");
         Assert.Contains("ClearPassphraseButton.IsEnabled = row.Passphrase != StoredPassphrase.NotSet;", editor, StringComparison.Ordinal);
         Assert.Contains("SavePassphraseButton.IsEnabled = !string.IsNullOrEmpty(PassphraseBox.Password);", editor, StringComparison.Ordinal);
         Assert.Contains("DetailsCard.Footer = offered ? PassphraseActions : null;", editor, StringComparison.Ordinal);
         Assert.Contains("PassphraseBox.PasswordChanged += (s, e) => SavePassphraseButton.IsEnabled = !string.IsNullOrEmpty(PassphraseBox.Password);", code, StringComparison.Ordinal);

         // An empty box is answered by a button that cannot be pressed, not by a message box.
         string save = Member(code, "private void SavePassphrase_Click(object sender, RoutedEventArgs e)");
         Assert.DoesNotContain("Type the new passphrase first", save, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(", save, StringComparison.Ordinal);
      }

      [Fact]
      public void TheNameColumn_HasAShareAndAFloor_AndThePathsTheirFilesNames()
      {
         string xaml = Read("SslCertificatesView.xaml");
         Assert.Contains("Header=\"{loc:L 'Name'}\" Binding=\"{Binding Name}\" Width=\"1.2*\" MinWidth=\"160\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Text=\"{Binding CertificateFileName}\" ToolTip=\"{Binding CertificateFile}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Text=\"{Binding PrivateKeyFileName}\" ToolTip=\"{Binding PrivateKeyFile}\"", xaml, StringComparison.Ordinal);
      }

      private static XElement Named(XElement root, string name)
      {
         XElement found = root.DescendantsAndSelf().FirstOrDefault(e => (string)e.Attribute(X + "Name") == name);
         Assert.True(found != null, "SslCertificatesView.xaml has no element named " + name + ".");
         return found;
      }

      private static string Member(string code, string signature)
      {
         string member = WithheldRowTests.MemberText(code, signature);
         Assert.True(member != null, signature + " is gone.");
         return member;
      }

      private static string Read(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Views", file));

      private static string ReadService(string file) => FormLineAssert.ReadControlPanelFile(Path.Join("Services", file));
   }
}
