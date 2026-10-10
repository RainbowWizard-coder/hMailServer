// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.Win32;
using hMailServer.ControlPanel.Services;

// System.Windows.Documents is needed for Run/Inlines and declares its own
// Typography; the alias picks the Control Panel's type scale, the same way
// DnsRecordsView resolves the same clash.
using Typography = hMailServer.ControlPanel.Services.Typography;

// The status marks are System.Windows.Shapes.Path; the implicit usings bring in
// System.IO.Path. The alias picks the drawing one.
using Path = System.Windows.Shapes.Path;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// The SSL certificates list, with what this machine can actually verify
   /// about each entry rather than just the two file paths: does the
   /// certificate file exist and parse, when does it expire and how many days
   /// remain, does the key file exist, is it encrypted, and does the key belong
   /// to the certificate. The checks live in
   /// <see cref="CertificateInspector"/> and run off the UI thread; against a
   /// remote server they honestly degrade to "cannot be checked from here"
   /// instead of showing a false green.
   ///
   /// This page is also the editor for the one property of a certificate that
   /// is a secret: <c>PrivateKeyPassword</c>, the passphrase of an encrypted
   /// key file (COM, database version 6009). The stored value is
   /// DPAPI-protected at rest and its COM getter requires the
   /// server-administrator session; this page only ever tests whether it is
   /// EMPTY and never displays it. Without it, an encrypted key does not load
   /// (server error 6170) and every TLS port using the certificate does not
   /// start - a mail outage caused by a configuration mistake, which is why the
   /// wording here is blunt.
   /// </summary>
   public partial class SslCertificatesView : UserControl, IPageLifecycle, IRefreshablePage
   {
      /// <summary>Whether a passphrase is stored on the server for a certificate.</summary>
      internal enum StoredPassphrase
      {
         /// <summary>Could not be read - the getter requires the server-administrator session.</summary>
         Unknown,
         NotSet,
         Set
      }

      public class CertRow
      {
         public string Name { get; set; }
         public string CertificateFile { get; set; }
         public string PrivateKeyFile { get; set; }

         /// <summary>
         /// The short status words, public so the search box (which reflects
         /// over public properties) finds rows by state: typing "expired"
         /// filters to the expired certificates.
         /// </summary>
         public string CertificateState { get; set; }
         public string PrivateKeyState { get; set; }

         /// <summary>The TCP/IP ports that present it, "SMTP 465, IMAP 993", or Not used (Services/CertificateRows).</summary>
         public string UsedBy { get; set; }

         // What a screen reader says for the row: its cells, not the name of
         // this class, which is what a grid row is announced as otherwise.
         public override string ToString() => Name + ", " + CertificateState + ", " + PrivateKeyState;

         // Internal on purpose: ListSearch reflects over public properties, and
         // a finding's ToString would only add noise to the search.
         /// <summary>The two files' names alone, which the columns show; the paths are their tool tips (review finding F236).</summary>
         public string CertificateFileName => CertificateRows.FileName(CertificateFile);
         public string PrivateKeyFileName => CertificateRows.FileName(PrivateKeyFile);

         internal int Id;
         internal StoredPassphrase Passphrase;
         internal CertificateHealth Health;

         internal CertificateFinding CertBadge;        // the "Certificate" grid cell
         internal CertificateFinding KeyBadge;         // the "Private key" grid cell
         internal CertificateFinding PassphraseLine;   // details pane
      }

      /// <summary>
      /// Guards against a slow inspection finishing after a newer reload: only
      /// the results of the latest reload may reach the grid.
      /// </summary>
      private int reloadVersion_;

      public SslCertificatesView()
      {
         InitializeComponent();
         hMailServer.ControlPanel.Services.SelectionGate.Bind(CertGrid, EditButton, DeleteButton);

         // Enter, F2 and a double-click edit the selected certificate, Delete deletes
         // it after asking and Insert goes to the add form; Enter in the form adds the
         // certificate, and Enter in the passphrase box saves the passphrase
         // (Views/GridKeys, FormSubmit).
         GridKeys.Wire(CertGrid, GridKeys.Pressing(EditButton), GridKeys.Pressing(DeleteButton), GridKeys.Focusing(NewCertName));
         PemHint.Text = CertificateDialog.PfxHint;

         // Save passphrase can be pressed while its box holds one, where an empty box
         // answered with a message box (review finding F236).
         PassphraseBox.PasswordChanged += (s, e) => SavePassphraseButton.IsEnabled = !string.IsNullOrEmpty(PassphraseBox.Password);
         FormSubmit.Bind(AddCertForm, AddCertButton, NewCertName, NewCertFile, NewKeyFile, NewKeyPassphrase);
         FormSubmit.Bind(PassphraseBox, SavePassphraseButton, PassphraseBox);

         // The empty state under the grid's column headers, centred where the
         // rows would be, never drawn across the headers' line.
         Scaffold.EmptyStateUnderHeaders.Keep(EmptyStatus, CertGrid);
      }

      public void OnEnter() => Reload();

      /// <summary>F5: the certificates again, the selected one kept.</summary>
      void IRefreshablePage.Refresh() => Reload((CertGrid.SelectedItem as CertRow)?.Id ?? 0);

      /// <summary>The empty list's way to a certificate other than the form below: ACME issues and renews one.</summary>
      private void SetUpAcme_Click(object sender, RoutedEventArgs e)
         => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("acme");

      public void OnLeave()
      {
      }

      // ---- loading ------------------------------------------------------------

      private async void Reload(int reselectId = 0, Action after = null)
      {
         int version = ++reloadVersion_;

         // The first load waits for the file checks; a reload keeps the rows it
         // has rather than blanking them for the wait.
         if (CertGrid.Items.Count == 0)
         {
            EmptyStatus.Visibility = Visibility.Collapsed;
            Loading.Visibility = Visibility.Visible;
         }

         var rows = new List<CertRow>();
         Exception failure = null;

         try
         {
            ReadRowsFromCom(rows);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            rows.Clear();
            failure = ex;
         }

         bool local = CertificateInspector.SessionIsLocal(ServerSession.Current?.Host);

         if (failure == null && rows.Count > 0)
         {
            // The file checks are plain disk reads, but the paths may point at a
            // network share, so they stay off the UI thread. Inspect never throws.
            await Task.Run(() =>
            {
               foreach (CertRow row in rows)
                  row.Health = CertificateInspector.Inspect(row.CertificateFile, row.PrivateKeyFile, local);
            });
         }

         if (version != reloadVersion_)
            return;   // superseded by a newer reload

         Loading.Visibility = Visibility.Collapsed;

         foreach (CertRow row in rows)
            ComposeFindings(row);

         CertGrid.ItemsSource = rows;
         if (failure != null)
         {
            // A read that failed is said as on the other list pages (Services/ReadFailure):
            // what could not be read and then why, with Try again, the list emptied and
            // Add disabled until a read works - where the notice gave the reason alone,
            // and nothing on the page read the list again.
            ReadFailure.Show(LoadNotice, EmptyStatus, CertGrid, L("Could not read the certificates: "), failure, () => Reload(), AddCertButton);
         }
         else
         {
            ReadFailure.Clear(LoadNotice, AddCertButton);
            StatusText.Show(EmptyStatus, LoadNotice, rows.Count, null, EmptyText_);
         }
         CountPill.Visibility = failure == null ? Visibility.Visible : Visibility.Collapsed;
         ShowListState_();

         if (reselectId != 0)
         {
            CertRow reselect = rows.FirstOrDefault(row => row.Id == reselectId);
            if (reselect != null)
               CertGrid.SelectedItem = reselect;
         }

         UpdateDetailsPane();
         after?.Invoke();
      }

      private static void ReadRowsFromCom(List<CertRow> rows)
      {
         // Which ports present each certificate, read once for every row; null when
         // the ports could not be read, which the column says rather than "Not used".
         Dictionary<int, List<string>> ports = PortsByCertificate_();

         dynamic certs = ServerSession.Current.Application.Settings.SSLCertificates;
         try
         {
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic cert = certs.Item[i];
               try
               {
                  var row = new CertRow
                  {
                     Id = (int)cert.ID,
                     Name = (string)cert.Name,
                     CertificateFile = (string)cert.CertificateFile,
                     PrivateKeyFile = (string)cert.PrivateKeyFile
                  };
                  row.UsedBy = CertificateRows.UsedBy(ports == null ? null : ports.TryGetValue(row.Id, out List<string> presenting) ? presenting : new List<string>());

                  // Only the EMPTINESS of the stored passphrase is tested; the
                  // value itself is never rendered anywhere on this page. The
                  // getter is gated on the server-administrator session (it
                  // returns a decryptable secret), so a rejection here degrades
                  // to "unknown" rather than to a guess.
                  try
                  {
                     string password = (string)cert.PrivateKeyPassword;
                     row.Passphrase = string.IsNullOrEmpty(password) ? StoredPassphrase.NotSet : StoredPassphrase.Set;
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                     row.Passphrase = StoredPassphrase.Unknown;
                  }

                  rows.Add(row);
               }
               finally
               {
                  ServerSession.Release((object)cert);
               }
            }
         }
         finally
         {
            ServerSession.Release((object)certs);
         }
      }

      /// <summary>
      /// Combines the file facts from the inspector with the COM facts this view
      /// holds (is a passphrase stored) into the findings the page renders.
      /// </summary>
      private static void ComposeFindings(CertRow row)
      {
         CertificateHealth health = row.Health ?? new CertificateHealth();

         row.CertBadge = health.CertificateFile;
         row.PassphraseLine = PassphraseFinding(row.Passphrase, health.KeyIsEncrypted);

         // The grid's "Private key" cell shows whichever of the key findings is
         // asking for the most attention; the details pane always shows all of
         // them. StatusLevel is ordered by attention, so the comparison is the
         // enum itself.
         row.KeyBadge = WorstOf(row.PassphraseLine, health.Pair, health.PrivateKeyFile);

         row.CertificateState = row.CertBadge?.Word ?? "";
         row.PrivateKeyState = row.KeyBadge?.Word ?? "";
      }

      /// <summary>
      /// What the stored-passphrase state means for THIS key file. Verified
      /// against SslContextInitializer::GetPassword_: an encrypted key with no
      /// stored passphrase is error 6170, "The key was not loaded", and the
      /// listener does not start; a stored passphrase is only proven right when
      /// the server loads the key.
      /// </summary>
      private static CertificateFinding PassphraseFinding(StoredPassphrase state, bool? keyIsEncrypted)
      {
         switch (state)
         {
            case StoredPassphrase.Set:
               if (keyIsEncrypted == false)
               {
                  return new CertificateFinding(StatusLevel.Information, L("Passphrase stored, key not encrypted"),
                     L("A passphrase is stored, but the key file is not encrypted, so it is never used. Harmless - it simply does nothing."));
               }
               return new CertificateFinding(StatusLevel.Good, L("Passphrase stored"),
                  L("A passphrase is stored for this certificate. It is never displayed here - saving a new one replaces it. Whether it is the RIGHT passphrase is only proven when the server loads the key: after the next restart, check the application error log."));

            case StoredPassphrase.NotSet:
               if (keyIsEncrypted == true)
               {
                  return new CertificateFinding(StatusLevel.Critical, L("Encrypted key, no passphrase"),
                     L("The key file is encrypted and no passphrase is stored, so the key will not load (server error 6170) and every TLS port using this certificate will not start - inbound mail on those ports stops. Enter the passphrase below, or replace the file with an unencrypted key."));
               }
               if (keyIsEncrypted == false)
               {
                  return new CertificateFinding(StatusLevel.Normal, L("No passphrase needed"),
                     L("The key file is not encrypted, so no passphrase is needed and none is stored."));
               }
               return new CertificateFinding(StatusLevel.Information, L("No passphrase stored"),
                  L("Whether the key file needs one cannot be checked from here. If it is encrypted, it will not load without a passphrase (server error 6170) and the TLS ports using this certificate will not start."));

            default:
               return new CertificateFinding(StatusLevel.Information, L("Passphrase state unknown"),
                  L("Whether a passphrase is stored could not be read - reading it requires the server-administrator account. Saving a new one from here still works."));
         }
      }

      private static CertificateFinding WorstOf(params CertificateFinding[] findings)
      {
         CertificateFinding worst = findings.Where(f => f != null).MaxBy(f => f.Level);
         return worst ?? new CertificateFinding(StatusLevel.Information, L("Not checked"), L("This entry was not checked."));
      }

      // ---- rendering ------------------------------------------------------------

      /// <summary>
      /// Colour, shape AND word - the same three channels every status badge in
      /// this application uses, so none of them is load-bearing on its own.
      /// Built in code because the brush must be a resource REFERENCE
      /// (ThemeTokens republishes the status brushes on every theme change; a
      /// bound brush would keep the old colour). The mark and the word take the
      /// level's fill-text shade, not its status colour: the word sits on the
      /// card, or on a hovered or selected row, where the dark theme's danger
      /// and information colours measured 4.1:1 and 3.5:1.
      /// </summary>
      private static FrameworkElement Badge(CertificateFinding finding)
      {
         StatusPresentation presentation = StatusSemantics.For(finding.Level);

         var row = new Grid();
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         var mark = new Path { Width = 11, Height = 11, Stretch = Stretch.Fill, Margin = new Thickness(0, 3, 6, 0), VerticalAlignment = VerticalAlignment.Top };
         ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.FillTextBrushKey);
         row.Children.Add(mark);

         var text = new TextBlock { FontSize = Typography.Label, TextWrapping = TextWrapping.Wrap };
         var word = new Run(finding.Word) { FontWeight = FontWeights.SemiBold };
         word.SetResourceReference(TextElement.ForegroundProperty, presentation.FillTextBrushKey);
         text.Inlines.Add(word);

         // The cell is compact; the full sentence is in the details pane below
         // and, for convenience, on the tooltip. The accessible name carries all
         // of it, severity first.
         text.ToolTip = finding.Detail;
         System.Windows.Automation.AutomationProperties.SetName(text,
            presentation.SeverityWord + ". " + finding.Word + ". " + finding.Detail);

         Grid.SetColumn(text, 1);
         row.Children.Add(text);
         return row;
      }

      /// <summary>One captioned status sentence in the details pane.</summary>
      private static FrameworkElement DetailLine(string caption, CertificateFinding finding)
      {
         StatusPresentation presentation = StatusSemantics.For(finding.Level);

         var row = new Grid { Margin = new Thickness(0, 0, 0, 6) };
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

         var mark = new Path { Width = 11, Height = 11, Stretch = Stretch.Fill, Margin = new Thickness(0, 4, 8, 0), VerticalAlignment = VerticalAlignment.Top };
         ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.FillTextBrushKey);
         row.Children.Add(mark);

         var text = new TextBlock { FontSize = Typography.Label, TextWrapping = TextWrapping.Wrap };
         text.Inlines.Add(new Run(caption + ": ") { FontWeight = FontWeights.SemiBold });
         var word = new Run(finding.Word + " — ") { FontWeight = FontWeights.SemiBold };
         word.SetResourceReference(TextElement.ForegroundProperty, presentation.FillTextBrushKey);
         text.Inlines.Add(word);
         text.Inlines.Add(new Run(finding.Detail));

         System.Windows.Automation.AutomationProperties.SetName(text,
            caption + ". " + presentation.SeverityWord + ". " + finding.Word + ". " + finding.Detail);

         Grid.SetColumn(text, 1);
         row.Children.Add(text);
         return row;
      }

      private void CertificateBadge_Loaded(object sender, RoutedEventArgs e)
      {
         if (sender is ContentControl cell && cell.DataContext is CertRow row && row.CertBadge != null)
            cell.Content = Badge(row.CertBadge);
      }

      private void KeyBadge_Loaded(object sender, RoutedEventArgs e)
      {
         if (sender is ContentControl cell && cell.DataContext is CertRow row && row.KeyBadge != null)
            cell.Content = Badge(row.KeyBadge);
      }

      private void CertGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         passphraseAnyway_ = false;
         UpdateDetailsPane();
      }

      // Set while the selected certificate's passphrase editor was asked for with
      // Set a passphrase anyway..., until another certificate is selected.
      private bool passphraseAnyway_;

      private void SetPassphraseAnyway_Click(object sender, RoutedEventArgs e)
      {
         passphraseAnyway_ = true;
         UpdateDetailsPane();
         PassphraseBox.Focus();
      }

      /// <summary>
      /// The passphrase editor as the selected certificate needs it (review finding
      /// F236): offered for a key that is or may be encrypted, or that has a passphrase
      /// stored, and behind Set a passphrase anyway... for one this computer read as
      /// unencrypted with none stored; Remove stored passphrase while one is stored or
      /// the page cannot tell, and Save passphrase while its box holds one - where both
      /// could be pressed whatever the state, and Save with an empty box opened a
      /// message box.
      /// </summary>
      private void ShowPassphraseEditor_(CertRow row)
      {
         bool offered = passphraseAnyway_ || CertificateRows.OffersPassphrase(
            row.Passphrase == StoredPassphrase.Set, row.Passphrase == StoredPassphrase.Unknown, row.Health?.KeyIsEncrypted);
         SetPassphraseAnywayButton.Visibility = offered ? Visibility.Collapsed : Visibility.Visible;
         PassphraseRow.Visibility = offered ? Visibility.Visible : Visibility.Collapsed;
         DetailsCard.Footer = offered ? PassphraseActions : null;
         ClearPassphraseButton.IsEnabled = row.Passphrase != StoredPassphrase.NotSet;
         SavePassphraseButton.IsEnabled = !string.IsNullOrEmpty(PassphraseBox.Password);
      }

      private void UpdateDetailsPane()
      {
         if (CertGrid.SelectedItem is not CertRow row)
         {
            DetailsCard.Visibility = Visibility.Collapsed;
            return;
         }

         DetailsCard.Visibility = Visibility.Visible;
         DetailsCard.Title = row.Name;
         PassphraseBox.Password = "";

         DetailsLines.Children.Clear();

         CertificateHealth health = row.Health ?? new CertificateHealth();
         if (health.CertificateFile != null)
            DetailsLines.Children.Add(DetailLine(L("Certificate file"), health.CertificateFile));
         if (health.PrivateKeyFile != null)
            DetailsLines.Children.Add(DetailLine(L("Private key file"), health.PrivateKeyFile));
         if (health.Pair != null)
            DetailsLines.Children.Add(DetailLine(L("Key matches certificate"), health.Pair));
         if (row.PassphraseLine != null)
            DetailsLines.Children.Add(DetailLine(L("Passphrase"), row.PassphraseLine));

         ShowPassphraseEditor_(row);
      }

      // ---- the passphrase editor -------------------------------------------------

      private void SavePassphrase_Click(object sender, RoutedEventArgs e)
      {
         if (CertGrid.SelectedItem is not CertRow row)
            return;

         // The button waits for a passphrase in its box; Enter in an empty box does nothing.
         string passphrase = PassphraseBox.Password;
         if (string.IsNullOrEmpty(passphrase))
            return;

         if (row.Passphrase == StoredPassphrase.Set &&
             !MessageBox.Confirm(F("A passphrase is already stored for '{0}'. Saving replaces it - the old one cannot be recovered from here. Replace it?", row.Name), L("Control Panel"),
                L("_Replace the passphrase"), destructive: true))
         {
            return;
         }

         OutcomeNotice.Visibility = Visibility.Collapsed;
         if (WritePassphrase(row, passphrase, out string error))
         {
            PassphraseBox.Password = "";
            Reload(row.Id, () => ShowOutcome_(StatusLevel.Good, Sentences(F("The passphrase of {0} is saved.", row.Name),
               L("The TCP/IP ports that present it read its files again when the server is next reinitialised."))));
         }
         else
         {
            ShowOutcome_(StatusLevel.Critical, F("Could not save the passphrase: {0}", error));
         }
      }

      private void ClearPassphrase_Click(object sender, RoutedEventArgs e)
      {
         if (CertGrid.SelectedItem is not CertRow row)
            return;

         if (!MessageBox.Confirm(F("Remove the stored passphrase for '{0}'? If the key file is encrypted, the key will stop loading at the next restart (server error 6170) and the TLS ports using this certificate will not start.", row.Name), L("Control Panel"),
               L("Re_move stored passphrase"), destructive: true))
         {
            return;
         }

         OutcomeNotice.Visibility = Visibility.Collapsed;
         if (WritePassphrase(row, "", out string error))
            Reload(row.Id, () => ShowOutcome_(StatusLevel.Good, F("The stored passphrase of {0} is removed.", row.Name)));
         else
            ShowOutcome_(StatusLevel.Critical, F("Could not remove the passphrase: {0}", error));
      }

      /// <summary>
      /// Writes the passphrase to one certificate, found by ID rather than by
      /// name because names are not guaranteed unique and a secret written to
      /// the wrong certificate would fail silently at the next restart.
      /// </summary>
      private static bool WritePassphrase(CertRow row, string passphrase, out string error)
      {
         error = null;
         dynamic certs = null;
         try
         {
            certs = ServerSession.Current.Application.Settings.SSLCertificates;
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic cert = certs.Item[i];
               try
               {
                  if ((int)cert.ID == row.Id)
                  {
                     cert.PrivateKeyPassword = passphrase;
                     cert.Save();
                     return true;
                  }
               }
               finally
               {
                  ServerSession.Release((object)cert);
               }
            }

            error = L("The certificate no longer exists - it was deleted after this list was read.");
            return false;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            error = ServerSession.DescribeComError(ex);
            return false;
         }
         finally
         {
            ServerSession.Release((object)certs);
         }
      }

      // ---- search / browse / add / delete ----------------------------------------

      /// <summary>The header's Refresh: the certificates and their file checks read again, as F5 reads them.</summary>
      private void Refresh_Click(object sender, RoutedEventArgs e)
      {
         OutcomeNotice.Visibility = Visibility.Collapsed;
         Reload((CertGrid.SelectedItem as CertRow)?.Id ?? 0);
      }

      /// <summary>What the last act did, at its level, after the list it acted on was read again.</summary>
      private void ShowOutcome_(StatusLevel level, string text, UIElement action = null)
      {
         OutcomeNotice.Level = level;
         OutcomeNotice.Text = text;
         OutcomeNotice.Content = action;
         OutcomeNotice.Visibility = Visibility.Visible;
      }

      /// <summary>
      /// Edit...: the selected certificate's name and files in a dialog, saved in
      /// place - its id kept, so that every TCP/IP port that presents it keeps it -
      /// and the list read again with it selected (review finding F235).
      /// </summary>
      private void Edit_Click(object sender, RoutedEventArgs e)
      {
         if (CertGrid.SelectedItem is not CertRow row)
            return;

         OutcomeNotice.Visibility = Visibility.Collapsed;
         string saved = CertificateDialog.Edit(Window.GetWindow(this), row.Id);
         if (saved == null)
            return;

         Reload(row.Id, () => ShowOutcome_(StatusLevel.Good, Sentences(ListOutcome.Saved(saved, DateTime.Now),
            L("The TCP/IP ports that present it read its files again when the server is next reinitialised."))));
      }

      /// <summary>
      /// The Certificate column sorts by how much a certificate asks for attention,
      /// then by how soon it expires (Services/CertificateRows), where it sorted by
      /// its status word, which put "Expired" before "Expires soon" and both among
      /// the good.
      /// </summary>
      private void CertGrid_Sorting(object sender, DataGridSortingEventArgs e)
      {
         if (!ReferenceEquals(e.Column, CertificateColumn))
            return;

         e.Handled = true;
         ListSortDirection direction = e.Column.SortDirection == ListSortDirection.Ascending ? ListSortDirection.Descending : ListSortDirection.Ascending;
         foreach (DataGridColumn column in CertGrid.Columns)
            column.SortDirection = null;
         e.Column.SortDirection = direction;

         if (CollectionViewSource.GetDefaultView(CertGrid.ItemsSource) is ListCollectionView view)
            view.CustomSort = new CertificateOrder_(direction);
      }

      private sealed class CertificateOrder_ : IComparer
      {
         private readonly int sign_;

         public CertificateOrder_(ListSortDirection direction) => sign_ = direction == ListSortDirection.Ascending ? 1 : -1;

         public int Compare(object x, object y)
         {
            if (x is not CertRow a || y is not CertRow b)
               return 0;

            return sign_ * CertificateRows.Compare(a.CertBadge?.Level ?? StatusLevel.Normal, a.Health?.DaysRemaining,
               b.CertBadge?.Level ?? StatusLevel.Normal, b.Health?.DaysRemaining);
         }
      }

      // What an empty list means, said by a read that finds no certificate and by
      // a search cleared over none.
      private static string EmptyText_ => L("No certificates yet, so no port can offer TLS. Add one from PEM files below, or let ACME issue one.");

      private void Search_TextChanged(object sender, EventArgs e) => ShowListState_();

      /// <summary>
      /// The search over the certificates read, the count beside the title and the empty
      /// state: "Shown: 1 of 4" while a search is typed in, and where it hides every
      /// row, the sentence that says so in place of the empty list's
      /// (Services/ListSearchState).
      /// </summary>
      private void ShowListState_() => ListSearchState.Show(CertGrid, SearchBar.SearchText, CountPill,
         n => Plural(n, "{0} certificate", "{0} certificates"), EmptyStatus, EmptyText_);

      private static string BrowsePem()
      {
         var dialog = new Microsoft.Win32.OpenFileDialog
         {
            Filter = L("PEM files (*.pem;*.crt;*.key)|*.pem;*.crt;*.key|All files (*.*)|*.*")
         };
         return dialog.ShowDialog() == true ? dialog.FileName : null;
      }

      private void BrowseCert_Click(object sender, RoutedEventArgs e)
      {
         string file = BrowsePem();
         if (file == null)
            return;

         NewCertFile.Text = file;

         // The name is the certificate's own, while the box is empty: it had to be
         // typed although the subject is in the file (review finding F236).
         if (string.IsNullOrWhiteSpace(NewCertName.Text) && CertificateInspector.NameOf(file) is string name)
            NewCertName.Text = name;
      }

      private async void BrowseKey_Click(object sender, RoutedEventArgs e)
      {
         string file = BrowsePem();
         if (file == null)
            return;

         NewKeyFile.Text = file;

         // The one thing this page can find out for the administrator before it
         // hurts: whether the picked key file is passphrase-protected. Local
         // sessions only - a remote server's paths are not this machine's files.
         if (!CertificateInspector.SessionIsLocal(ServerSession.Current?.Host))
            return;

         bool? encrypted = await Task.Run(() =>
         {
            try
            {
               return File.Exists(file) ? CertificateInspector.LooksLikeEncryptedPem(file) : (bool?)null;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               return null;
            }
         });

         if (encrypted == true)
         {
            ShowAddHint(StatusLevel.Warning,
               L("This key file is encrypted (passphrase-protected). Enter its passphrase in the Key passphrase box before adding, or the key will not load (server error 6170) and the TLS ports using this certificate will not start."));
         }
         else
         {
            AddHint.Visibility = Visibility.Collapsed;
         }
      }

      /// <summary>Colour, shape and word for the add-form hint, like every other
      /// status here: the notice draws all three from the level.</summary>
      private void ShowAddHint(StatusLevel level, string message)
      {
         AddHint.Level = level;
         AddHint.Text = message;
         AddHint.Visibility = Visibility.Visible;
      }

      private async void Add_Click(object sender, RoutedEventArgs e)
      {
         string name = NewCertName.Text.Trim();
         string certFile = NewCertFile.Text.Trim();
         string keyFile = NewKeyFile.Text.Trim();
         string passphrase = NewKeyPassphrase.Password;

         // Each box left empty is said on that box, all on one press, with the
         // keyboard in the first, where one message box named all three and none
         // of them (review finding F129).
         DialogFields.ClearErrors(NewCertNameRow, NewCertFileRow, NewKeyFileRow);
         if (name.Length == 0)
            NewCertNameRow.Error = L("Enter a name for the certificate.");
         if (certFile.Length == 0)
            NewCertFileRow.Error = L("Enter the path of the certificate file, or choose the file with the … button beside the box.");
         if (keyFile.Length == 0)
            NewKeyFileRow.Error = L("Enter the path of the private key file, or choose the file with the … button beside the box.");
         if (DialogFields.ShowFirstError(NewCertNameRow, NewCertFileRow, NewKeyFileRow))
            return;

         // Catch the outage before it is configured: an encrypted key with no
         // passphrase will not load (server error 6170) and the ports using it
         // will not start. Only determinable when the file is on this machine.
         if (passphrase.Length == 0 && CertificateInspector.SessionIsLocal(ServerSession.Current?.Host))
         {
            bool? encrypted = await Task.Run(() =>
            {
               try
               {
                  return File.Exists(keyFile) ? CertificateInspector.LooksLikeEncryptedPem(keyFile) : (bool?)null;
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  return null;
               }
            });

            if (encrypted == true &&
                !MessageBox.Confirm(L("The private key file is encrypted (passphrase-protected) and no passphrase was entered. The certificate will be added, but its key will not load (server error 6170) and the TLS ports using it will not start until the passphrase is saved.\n\nAdd it anyway?"),
                   L("Control Panel"), L("_Add anyway"), cancelByDefault: true, level: StatusLevel.Warning))
            {
               return;
            }
         }

         // Current cannot be null here today (this page only exists after
         // sign-in and nothing ever clears the session), but this handler has
         // awaited since the click and is async void - if a sign-out path ever
         // arrives, failing soft here beats the crash dialog.
         ServerSession session = ServerSession.Current;
         if (session == null)
            return;

         OutcomeNotice.Visibility = Visibility.Collapsed;
         int added;
         dynamic certs = session.Application.Settings.SSLCertificates;
         try
         {
            dynamic cert = certs.Add();
            cert.Name = name;
            cert.CertificateFile = certFile;
            cert.PrivateKeyFile = keyFile;
            if (passphrase.Length > 0)
               cert.PrivateKeyPassword = passphrase;
            cert.Save();
            added = (int)cert.ID;
            ServerSession.Release((object)cert);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ShowOutcome_(StatusLevel.Critical, F("Could not add the certificate: {0}", ServerSession.DescribeComError(ex)));
            return;
         }
         finally
         {
            ServerSession.Release((object)certs);
         }

         NewCertName.Text = NewCertFile.Text = NewKeyFile.Text = "";
         NewKeyPassphrase.Password = "";
         AddHint.Visibility = Visibility.Collapsed;

         // The new certificate selected, and what it needs next: a port that presents
         // it, on the page that has them (review finding F236). It was neither
         // selected nor said.
         Reload(added, () => ShowOutcome_(StatusLevel.Good,
            Sentences(ListOutcome.Added(name, DateTime.Now), L("Choose it for a port on the TCP/IP ports page to use it.")),
            OpenPortsButton_()));
      }

      // The outcome's way to the ports, made once and kept, as the TCP/IP ports page
      // keeps its notice's buttons.
      private Wpf.Ui.Controls.Button openPortsButton_;

      private Wpf.Ui.Controls.Button OpenPortsButton_() => openPortsButton_ ??= NoticeActions.OpenPage("ports", "certs-open-ports");

      private void Delete_Click(object sender, RoutedEventArgs e)
      {
         if (CertGrid.SelectedItem is not CertRow row)
            return;

         if (!MessageBox.Confirm(Confirmations.DeleteCertificate(row.Name, PortsPresenting_(row.Id)), L("Control Panel"),
                L("_Delete certificate"), destructive: true))
            return;

         OutcomeNotice.Visibility = Visibility.Collapsed;
         int at = CertGrid.SelectedIndex;
         bool deleted = false;

         dynamic certs = ServerSession.Current.Application.Settings.SSLCertificates;
         try
         {
            int count = (int)certs.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic cert = certs.Item[i];
               if ((int)cert.ID == row.Id)
               {
                  cert.Delete();
                  deleted = true;
                  ServerSession.Release((object)cert);
                  break;
               }
               ServerSession.Release((object)cert);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not delete the certificate: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
         finally
         {
            ServerSession.Release((object)certs);
         }

         // The outcome after the list is read again, with the row that took the
         // deleted one's place selected (Services/ListOutcome).
         Reload(0, () =>
         {
            int next = ListOutcome.IndexAfterDelete(CertGrid.Items.Count, at);
            if (next >= 0)
               CertGrid.SelectedIndex = next;
            if (deleted)
               ShowOutcome_(StatusLevel.Good, ListOutcome.Deleted(row.Name));
         });
      }

      /// <summary>
      /// The TCP/IP ports that present the certificate - an SSL/TLS or STARTTLS port
      /// that names it - as "SMTP 465", for the question asked before it is deleted:
      /// such a port does not start again until another certificate is chosen for it
      /// (review finding F128). Null when the ports could not be read.
      /// </summary>
      private static List<string> PortsPresenting_(int certificateId)
      {
         Dictionary<int, List<string>> ports = PortsByCertificate_();
         if (ports == null)
            return null;

         return ports.TryGetValue(certificateId, out List<string> presenting) ? presenting : new List<string>();
      }

      /// <summary>
      /// Every TCP/IP port that presents a certificate, by the certificate's id, as
      /// "SMTP 465": for the Used by column and the question before a delete. Null
      /// when the ports could not be read.
      /// </summary>
      private static Dictionary<int, List<string>> PortsByCertificate_()
      {
         var presenting = new Dictionary<int, List<string>>();
         dynamic ports = null;
         try
         {
            ports = ServerSession.Current.Application.Settings.TCPIPPorts;
            int count = (int)ports.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic port = ports.Item[i];
               try
               {
                  // eConnectionSecurity: 0 none, 1 SSL/TLS, 2 STARTTLS optional, 3 required.
                  int certificateId = (int)port.SSLCertificateID;
                  if ((int)port.ConnectionSecurity > 0 && certificateId > 0)
                  {
                     int protocol = (int)port.Protocol;
                     string name = protocol switch
                     {
                        ServerSession.SessionSmtp => "SMTP",
                        ServerSession.SessionPop3 => "POP3",
                        ServerSession.SessionImap => "IMAP",
                        _ => "#" + protocol
                     };
                     if (!presenting.TryGetValue(certificateId, out List<string> names))
                        presenting[certificateId] = names = new List<string>();
                     names.Add(name + " " + (int)port.PortNumber);
                  }
               }
               finally
               {
                  ServerSession.Release((object)port);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return null;
         }
         finally
         {
            ServerSession.Release((object)ports);
         }

         return presenting;
      }
   }
}
