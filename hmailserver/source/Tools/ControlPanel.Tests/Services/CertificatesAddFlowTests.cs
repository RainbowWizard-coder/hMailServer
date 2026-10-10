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
         Assert.Contains("CertGrid.ScrollIntoView(reselect);", Read("SslCertificatesView.xaml.cs"), StringComparison.Ordinal);
         Assert.Contains("ListOutcome.Added(name, DateTime.Now)", add, StringComparison.Ordinal);
         Assert.Contains("OpenPortsButton_()", add, StringComparison.Ordinal);
         Assert.DoesNotContain("MessageBox.Show(", add, StringComparison.Ordinal);
         Assert.Contains("NoticeActions.OpenPage(\"ports\"", Read("SslCertificatesView.xaml.cs"), StringComparison.Ordinal);
      }

      /// <summary>
      /// The outcome is said above the list and Add below it, so at 1280 by 800, where
      /// the page scrolls to reach Add, the sentence and Open TCP/IP ports were out of
      /// view - and a passphrase that could not be saved, which was a message box, with
      /// them. Every outcome is scrolled into view once laid out, and every act's
      /// outcome is said through it.
      /// </summary>
      [Fact]
      public void TheOutcome_IsScrolledIntoView_WhereverTheActWasPressed()
      {
         string code = Read("SslCertificatesView.xaml.cs");
         string show = Member(code, "private void ShowOutcome_(StatusLevel level, string text, UIElement action = null)");
         int visible = show.IndexOf("OutcomeNotice.Visibility = Visibility.Visible;", StringComparison.Ordinal);
         int into = show.IndexOf("Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Loaded, new Action(() =>", StringComparison.Ordinal);
         Assert.True(visible >= 0 && into > visible, "ShowOutcome_ does not scroll the notice into view once it is shown and laid out.");
         // The row the act selected first, in the grid the notice has shortened - whose
         // ScrollIntoView scrolls the page too - and the notice once that is laid out.
         string laidOut = show.Substring(into);
         int row = laidOut.IndexOf("CertGrid.ScrollIntoView(CertGrid.SelectedItem);", StringComparison.Ordinal);
         int notice = laidOut.IndexOf("Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, new Action(() => OutcomeNotice.BringIntoView()));", StringComparison.Ordinal);
         Assert.True(row > 0 && notice > row, "The grid is not scrolled to its row before the notice is brought into view.");

         // The notice is the page's one place for an outcome: nothing else sets its text.
         Assert.Single(System.Text.RegularExpressions.Regex.Matches(code, @"OutcomeNotice\.Text\s*="));
         foreach (string act in new[]
         {
            "private void SavePassphrase_Click(object sender, RoutedEventArgs e)",
            "private void ClearPassphrase_Click(object sender, RoutedEventArgs e)",
            "private async void Add_Click(object sender, RoutedEventArgs e)"
         })
         {
            string member = Member(code, act);
            Assert.Contains("ShowOutcome_(StatusLevel.Critical,", member, StringComparison.Ordinal);
            Assert.Contains("ShowOutcome_(StatusLevel.Good,", member, StringComparison.Ordinal);
         }
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
         Assert.Contains("Header=\"{loc:L 'Name'}\" Binding=\"{Binding Name}\" Width=\"1.5*\" MinWidth=\"160\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Text=\"{Binding CertificateFileName}\" ToolTip=\"{Binding CertificateFile}\"", xaml, StringComparison.Ordinal);
         Assert.Contains("Text=\"{Binding PrivateKeyFileName}\" ToolTip=\"{Binding PrivateKeyFile}\"", xaml, StringComparison.Ordinal);
      }

      /// <summary>
      /// Every column is a share of the grid with a floor, the name's the largest: with
      /// the status columns and Used by fixed at 570 pixels, the name was held at its
      /// floor at 1280 by 800 and cut there - narrower than the 170 pixels the finding
      /// reported it cut at.
      /// </summary>
      [Fact]
      public void EveryColumn_IsAShareWithAFloor_TheNamesTheLargest()
      {
         XElement page = XElement.Parse(Read("SslCertificatesView.xaml"));
         XElement grid = page.DescendantsAndSelf().First(e => (string)e.Attribute(X + "Name") == "CertGrid");
         var columns = grid.Descendants().Where(e => e.Name.LocalName is "DataGridTextColumn" or "DataGridTemplateColumn").ToList();
         Assert.Equal(6, columns.Count);

         double Share(XElement column)
         {
            string width = (string)column.Attribute("Width") ?? "";
            Assert.True(width.EndsWith("*", StringComparison.Ordinal), "A column of the certificates grid has the fixed width " + width + ".");
            Assert.True(column.Attribute("MinWidth") != null, "A column of the certificates grid has no floor.");
            return width == "*" ? 1 : double.Parse(width.TrimEnd('*'), System.Globalization.CultureInfo.InvariantCulture);
         }

         double name = Share(columns[0]);
         foreach (XElement column in columns.Skip(1))
            Assert.True(Share(column) < name, "The name's share is not the largest.");
      }

      /// <summary>
      /// Six columns in 921 pixels at 1280 by 800 left the two file columns 113 pixels,
      /// and their headers were cut in twelve languages - "Private Schlüss...", whole in
      /// German before the columns were narrowed (the skeptic's review of F235 and F236).
      /// The grid's headers wrap at their spaces, whole in their tool tips, and the file
      /// columns' floors hold the widest word of their headers in any language: Dutch
      /// Privésleutelbestand, 106 pixels in the headers' 12-pixel semibold, inside the
      /// header's padding of 10 a side and the room of its sort mark.
      /// </summary>
      [Fact]
      public void TheGridsHeaders_WrapAtTheirSpaces_AndTheFileColumnsHoldTheirWidestWord()
      {
         XElement page = XElement.Parse(Read("SslCertificatesView.xaml"));
         XElement grid = Named(page, "CertGrid");
         Assert.Equal("{StaticResource CertificateGridHeader}", (string)grid.Attribute("ColumnHeaderStyle"));

         XElement style = page.Descendants().FirstOrDefault(e => e.Name.LocalName == "Style" && (string)e.Attribute(X + "Key") == "CertificateGridHeader");
         Assert.True(style != null, "SslCertificatesView.xaml has no style CertificateGridHeader.");
         Assert.Equal("{StaticResource {x:Type DataGridColumnHeader}}", (string)style.Attribute("BasedOn"));
         XElement text = style.Descendants().Single(e => e.Name.LocalName == "TextBlock");
         Assert.Equal("WrapWithOverflow", (string)text.Attribute("TextWrapping"));
         Assert.Equal("CharacterEllipsis", (string)text.Attribute("TextTrimming"));
         Assert.Equal("{Binding}", (string)text.Attribute("ToolTip"));

         foreach (string header in new[] { "Certificate file", "Private key file" })
         {
            XElement column = grid.Descendants().Single(e => (string)e.Attribute("Header") == "{loc:L '" + header + "'}");
            double floor = double.Parse((string)column.Attribute("MinWidth"), System.Globalization.CultureInfo.InvariantCulture);
            Assert.True(floor >= 128, "The " + header + " column's floor, " + floor + ", is narrower than the widest word of its header in some language.");
         }

         // Used by's floor holds Ukrainian використовується, 102 pixels, whole on one line
         // before its share was cut; and a header of two lines takes no row from the
         // grid's floor of four, where three certificates scrolled in the grid.
         XElement usedBy = grid.Descendants().Single(e => (string)e.Attribute("Header") == "{loc:L 'Used by'}");
         Assert.True(double.Parse((string)usedBy.Attribute("MinWidth"), System.Globalization.CultureInfo.InvariantCulture) >= 124,
            "The Used by column's floor is narrower than the widest word of its header in some language.");
         Assert.Equal("186", (string)grid.Attribute("MinHeight"));

         // Every column but the name reaches its floor at one width - its floor is 160
         // times its share - and the name's at a narrower one: the grid gives star columns
         // their floors in one pass and shares out the rest without looking again, and
         // takes the width of its scroll bar from its star columns in proportion without
         // passing on what a column at its floor cannot give, so that a column left just
         // above its floor made the columns overrun the grid by 3 or 4 pixels at 1280 by
         // 800, and it scrolled sideways.
         var columns = grid.Descendants().Where(e => e.Name.LocalName is "DataGridTextColumn" or "DataGridTemplateColumn").ToList();
         double Number(string text) => double.Parse(text, System.Globalization.CultureInfo.InvariantCulture);
         double Ratio(XElement column)
         {
            string width = (string)column.Attribute("Width");
            return Number((string)column.Attribute("MinWidth")) / (width == "*" ? 1 : Number(width.TrimEnd('*')));
         }

         foreach (XElement column in columns.Skip(1))
            Assert.True(Math.Abs(Ratio(column) - 160) < 0.5, (string)column.Attribute("Header") + "'s floor is " + Ratio(column) + " times its share, not 160.");
         Assert.True(Ratio(columns[0]) < 160, "The name reaches its floor no narrower than the other columns.");
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
