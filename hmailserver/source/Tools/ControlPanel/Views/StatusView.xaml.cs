// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Server / database / health overview — the parts of the classic Administrator
   /// Status pane that the live Dashboard does not already cover (version, database
   /// details, server state and the configuration warnings). Checking for,
   /// downloading and installing an update are on the Updates page
   /// (FeatureSettingsView.Updates.cs); a release that is waiting is said here in one
   /// line (<see cref="UpdateNotice"/>).
   ///
   /// Every readout on this page is a caption in one grid column and a value in the
   /// next, and WPF infers no relationship between the two, so the values are set
   /// through <see cref="SetValue_"/> rather than by assigning Text - see the
   /// comment there for what a screen reader heard before.
   /// </summary>
   public partial class StatusView : UserControl, IPageLifecycle
   {
      public StatusView()
      {
         InitializeComponent();
      }

      public void OnEnter() => Reload();

      public void OnLeave()
      {
      }

      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      private void OpenIpRanges_Click(object sender, RoutedEventArgs e)
         => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("ipranges"); // no-loc: a page key

      /// <summary>
      /// Pause and resume are Application.Stop()/Start() over COM - the engine stops
      /// while the Windows SERVICE keeps running, which is what the classic
      /// Administrator's pause did (its ucStatus button called exactly these). This
      /// went missing when that tool was retired: the Control Panel only offered a
      /// service-level restart, so "stop accepting mail for a moment without killing
      /// the process" stopped being possible from any UI.
      /// </summary>
      private void PauseResume_Click(object sender, RoutedEventArgs e)
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return;

         try
         {
            int state = (int)app.ServerState;

            if (state == ServerStateRunning_)
            {
               if (!MessageBox.Confirm(
                      L("Pause the mail server?\n\nNo new connections will be accepted and no mail will be delivered until it is resumed. The Windows service keeps running, so this Control Panel stays connected."),
                      L("Control Panel"), L("_Pause the mail server"), cancelByDefault: true))
                  return;

               app.Stop();
            }
            else if (state == ServerStateStopped_)
            {
               app.Start();
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The likeliest failure is rights: pausing requires an administrator-level
            // COM session. Said outright rather than as a raw HRESULT.
            MessageBox.Show(F("The server state could not be changed: {0}", ServerSession.DescribeComError(ex)),
               L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Error);
         }

         Reload();
      }

      private const int ServerStateStopped_ = 1;
      private const int ServerStateRunning_ = 3;

      private void UpdatePauseButton_(int state)
      {
         switch (state)
         {
            case ServerStateRunning_:
               PauseButton.Content = L("_Pause");
               PauseButtonIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.Pause24;
               PauseButton.IsEnabled = true;
               break;
            case ServerStateStopped_:
               // The glyph swaps with the verb - a "Resume" button wearing a
               // pause icon says two things at once, and one of them is wrong.
               PauseButton.Content = L("R_esume");
               PauseButtonIcon.Symbol = Wpf.Ui.Controls.SymbolRegular.Play24;
               PauseButton.IsEnabled = true;
               break;
            default:
               // Starting or stopping: a transition is already in progress.
               PauseButton.IsEnabled = false;
               break;
         }

         AutomationProperties.SetName(PauseButton, F("{0} the mail server engine", MnemonicText.Strip((string)PauseButton.Content)));
      }

      private static string DatabaseTypeName(int type) => type switch
      {
         1 => "MySQL / MariaDB",   // no-loc: product names
         2 => "Microsoft SQL Server",   // no-loc
         3 => "PostgreSQL",   // no-loc
         4 => L("Built-in (SQL Server Compact)"),
         _ => L("Unknown")
      };

      private static string ServerStateName(int state) => state switch
      {
         1 => L("Stopped"),
         2 => L("Starting"),
         3 => L("Running"),
         4 => L("Stopping"),
         // Maintenance mode: the listeners closed and delivery paused while what
         // is in flight finishes, the process up. Entered and left over REST
         // (POST and DELETE /api/v1/server/drain) and by the live update.
         5 => L("Draining"),
         _ => L("Unknown")
      };

      // The first read of this Reload that failed, said under the header, or
      // null while every read has worked.
      private string readError_;

      /// <summary>Notes a read that failed; the first one is the one the notice names.</summary>
      private string NotRead_(Exception why)
      {
         readError_ ??= ServerSession.DescribeComError(why);
         return L("Not read");
      }

      private void Reload()
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return;

         readError_ = null;

         // Server + database. A readout that could not be read says "Not read",
         // not "-": the dash is also what an empty value - a database with no
         // host - shows, and the two are different facts.
         try
         {
            SetValue_(VersionValue, L("Version"),
               (string)app.Version + " (" + (string)app.VersionArchitecture + ")");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck)) { SetValue_(VersionValue, L("Version"), NotRead_(fatalCheck)); }

         try
         {
            int state = (int)app.ServerState;
            SetValue_(StateValue, L("State"), ServerStateName(state));
            UpdatePauseButton_(state);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            SetValue_(StateValue, L("State"), NotRead_(fatalCheck));
            PauseButton.IsEnabled = false;
         }

         try
         {
            dynamic db = app.Database;
            SetValue_(DbTypeValue, L("Database type"), DatabaseTypeName((int)db.DatabaseType));
            string host = (string)db.ServerName;
            SetValue_(DbHostValue, L("Database host"), string.IsNullOrEmpty(host) ? "-" : host);
            string name = (string)db.DatabaseName;
            SetValue_(DbNameValue, L("Database name"), string.IsNullOrEmpty(name) ? "-" : name);
            SetValue_(DbVersionValue, L("Database schema version"), ((int)db.CurrentVersion).ToString());
            ServerSession.Release(db);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            string notRead = NotRead_(fatalCheck);
            SetValue_(DbTypeValue, L("Database type"), notRead);
            SetValue_(DbHostValue, L("Database host"), notRead);
            SetValue_(DbNameValue, L("Database name"), notRead);
            SetValue_(DbVersionValue, L("Database schema version"), notRead);
         }

         // Statistics + uptime
         try
         {
            var snap = ServerSession.Current.ReadStatus();
            SetValue_(ProcessedValue, L("Messages processed"), snap.ProcessedMessages.ToString("N0"));
            SetValue_(SpamValue, L("Spam detected"), snap.SpamBlocked.ToString("N0"));
            SetValue_(VirusValue, L("Viruses removed"), snap.VirusesRemoved.ToString("N0"));
            SetValue_(SmtpValue, L("SMTP sessions"), snap.SmtpSessions.ToString());
            SetValue_(ImapValue, L("IMAP sessions"), snap.ImapSessions.ToString());
            SetValue_(Pop3Value, L("POP3 sessions"), snap.Pop3Sessions.ToString());
            // The start is the server's time; the uptime is worked out only where the
            // server's clock is this computer's (review finding F247).
            SetValue_(StartedValue, L("Started"), ServerUptimeText.Started(snap.StartTime));
            SetValue_(UptimeValue, L("Uptime"), ServerUptimeText.Uptime(snap.StartTime, DateTime.Now, ServerSession.IsLocalSession));
            SinceStartHeading.Text = ServerUptimeText.SinceStart(snap.StartTime);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            string notRead = NotRead_(fatalCheck);
            SetValue_(ProcessedValue, L("Messages processed"), notRead);
            SetValue_(SpamValue, L("Spam detected"), notRead);
            SetValue_(VirusValue, L("Viruses removed"), notRead);
            SetValue_(SmtpValue, L("SMTP sessions"), notRead);
            SetValue_(ImapValue, L("IMAP sessions"), notRead);
            SetValue_(Pop3Value, L("POP3 sessions"), notRead);
            SetValue_(StartedValue, L("Started"), notRead);
            SetValue_(UptimeValue, L("Uptime"), notRead);
            SinceStartHeading.Text = L("Since the service started");
         }

         LoadWarnings(app);
         ShowReadFailure_();

         // A newer release waiting is said in one line, with the page that installs it.
         UpdateNotice.ShowIn(UpdateNoticeHost, "status-update-available");
      }

      /// <summary>
      /// A read that failed, said once under the header at the Critical level with
      /// Try again, and Pause disabled while it shows: with the link down every
      /// readout said "-", and the only explanation was an Information line at the
      /// foot of the warnings card. A read that worked hides it; Pause's own read
      /// has set it already. (The update steps are on the Updates page, which says
      /// a read of its own that failed in its card.)
      /// </summary>
      private void ShowReadFailure_()
      {
         if (readError_ == null)
         {
            ReadFailure.Clear(ReadNotice);
            return;
         }

         ReadFailure.Show(ReadNotice, null, null, L("Could not read the server: "), readError_, Reload,
            PauseButton);
      }

      /// <summary>
      /// Sets a readout and the accessible name that goes with it.
      ///
      /// The caption and the value are two <c>TextBlock</c>s in adjacent grid cells
      /// and WPF infers no relationship between them, so UI Automation saw eighteen
      /// bare values on this page: a screen reader announced "6.2.18 (x64)",
      /// "Running", "3", "3", "0" with nothing at all saying which was which - and
      /// three of those are single digits that could equally be the SMTP, IMAP or
      /// POP3 session count.
      ///
      /// <c>AutomationProperties.LabeledBy</c> looks like the tool for this and is
      /// not: on a TextBlock it *replaces* the announced name, so the reader would
      /// hear the caption and never the number. Naming the value "caption, value" is
      /// what the dashboard's KPI row already does, and doing the same here means
      /// the two pages sound alike.
      /// </summary>
      private static void SetValue_(TextBlock value, string label, string text)
      {
         value.Text = text;
         AutomationProperties.SetName(value, label + ", " + text);
      }

      private void LoadWarnings(dynamic app)
      {
         WarningsPanel.Children.Clear();
         int count = 0;

         try
         {
            // Each check names the page that puts it right, and opens it. The
            // checks are Services/ConfigurationChecks, which the Dashboard's
            // attention card reads as well (review finding F242).
            // As an object, so that the call is bound here and not at run time.
            System.Collections.Generic.IReadOnlyList<ConfigurationWarning> found = ConfigurationChecks.Read((object)app, out int autoban);
            foreach (ConfigurationWarning warning in found)
               count += AddWarning(warning.Level, warning.Text, warning.Page);

            // A count, which is a statistic and not a finding: it was filed as
            // an Information "warning", the one item in that card on most
            // servers.
            SetValue_(AutobanValue, L("Auto-ban IP ranges"), autoban.ToString("N0"));
            AutobanLink.Visibility = autoban > 0 ? Visibility.Visible : Visibility.Collapsed;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The failure is said once, under the header (ShowReadFailure_); the
            // card says only that its checks did not run, which is neither "no
            // warnings" nor a finding.
            WarningsPanel.Children.Clear();
            WarningsPanel.Children.Add(new InlineNotice { Level = StatusLevel.Normal, Text = L("Not checked: the server's settings could not be read."), Margin = new Thickness(0) });
            SetValue_(AutobanValue, L("Auto-ban IP ranges"), NotRead_(ex));
            AutobanLink.Visibility = Visibility.Collapsed;
            return;
         }

         if (count == 0)
            WarningsPanel.Children.Add(new InlineNotice { Level = StatusLevel.Good, Text = L("No configuration warnings."), Margin = new Thickness(0) });
      }

      /// <summary>
      /// Adds one configuration warning: a severity mark, the severity word, and
      /// the message.
      ///
      /// This used to be a filled pill in one of four hardcoded RGB values with
      /// <c>Brushes.White</c> printed on it, and that carried three defects at once.
      ///
      /// The colours never consulted the theme, so on a High Contrast desktop the
      /// one page that says "this server may be an open relay" drew its severities
      /// in exactly the four colours the user had just told Windows they could not
      /// read.
      ///
      /// White on the "Medium" amber #C28A00 is 3.03:1 and white on the "High"
      /// orange #D24F1A is 4.31:1, against the 4.5:1 that eleven-point text needs -
      /// so two of the four pills failed WCAG AA in every theme, not only in High
      /// Contrast. Black text would have fixed those two and broken Critical
      /// (3.50:1), which is what a filled pill costs you: two colours to get right
      /// per severity instead of one.
      ///
      /// And nothing joined the badge to the message, so a reader moving through the
      /// page heard "Critical" and then, as a separate unrelated item, a sentence.
      ///
      /// It is now an InlineNotice at the level the caller names - Critical,
      /// Warning or Information - which draws the shape, the word and the theme
      /// brush in all three themes and puts the word first in its accessible
      /// name. There is no foreground/background pair left to get wrong, because
      /// there is no fill; the shape carries the severity for a reader who cannot
      /// separate the colours.
      ///
      /// The level is a <see cref="StatusLevel"/>, not a word. It was a word -
      /// "Critical", "High", "Medium", "Info" - passed through the catalogue and
      /// matched back against the English, so in German "Kritisch" and "Mittel"
      /// matched nothing and fell to the default, Warning: the open relay was
      /// demoted and the auto-ban count promoted, and Japanese escaped only
      /// because its catalogue left the words in English. A level cannot be
      /// translated.
      /// </summary>
      /// <remarks>
      /// With the page that puts it right, the notice carries a button that opens
      /// it (Services/NoticeActions): each warning named another page and offered
      /// no way to it.
      /// </remarks>
      private int AddWarning(StatusLevel level, string text, string page = null)
      {
         WarningsPanel.Children.Add(new InlineNotice
         {
            Level = level,
            Text = text,
            Margin = new Thickness(0, 0, 0, 8),
            Content = page == null ? null : NoticeActions.OpenPage(page, "status-check-open-" + page)
         });
         return 1;
      }

   }
}
