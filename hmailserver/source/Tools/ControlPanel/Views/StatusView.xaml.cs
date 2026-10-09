// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Server / database / health overview — the parts of the classic Administrator
   /// Status pane that the live Dashboard does not already cover (version, database
   /// details, server state and the configuration warnings).
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
         settle_?.Stop();
      }

      // Asking the service again while it still says an install that has ended is
      // under way (AdministratorUpdate.ShouldAskAgainAfterInstall).
      private System.Windows.Threading.DispatcherTimer settle_;
      private int settleAttempts_;

      private void SettleAfterInstall_(int updateState)
      {
         if (!AdministratorUpdate.ShouldAskAgainAfterInstall(installOutcome_ != null, updateState, settleAttempts_))
         {
            settle_?.Stop();
            return;
         }
         if (settle_ == null)
         {
            settle_ = new System.Windows.Threading.DispatcherTimer { Interval = AdministratorUpdate.SettleInterval };
            settle_.Tick += (s, e) =>
            {
               settle_.Stop();
               settleAttempts_++;
               Reload();
            };
         }
         settle_.Start();
      }

      private void Refresh_Click(object sender, RoutedEventArgs e) => Reload();

      private void OpenIpRanges_Click(object sender, RoutedEventArgs e)
         => (Application.Current?.MainWindow as MainWindow)?.NavigateTo("ipranges"); // no-loc: a page key

      private void CheckUpdate_Click(object sender, RoutedEventArgs e) => RunUpdateAction_("check");
      private void DownloadUpdate_Click(object sender, RoutedEventArgs e) => RunUpdateAction_("download");
      private void InstallUpdate_Click(object sender, RoutedEventArgs e)
      {
         // A service that cannot run an installer - NT SERVICE\hMailServer, a new
         // installation's since 6.3.4 - has its update installed from here instead,
         // after Windows' own prompt (Views/UpdateInstallWindow.cs). Anything else
         // - LocalSystem, or a server before 6.3.6, which says what to do itself -
         // goes the way it always went.
         if (TryAdministratorInstall_())
            return;
         RunUpdateAction_("install");
      }

      /// <summary>
      /// The outcome of an administrator's install, at the top of the page: set by
      /// the Control Panel that opened again after it (MainWindow, --after-update).
      /// </summary>
      public void ShowInstallOutcome(AdministratorUpdate.Outcome outcome)
      {
         // Read again with the outcome known: the notice is drawn with the
         // warnings, and a service still saying "installing" is asked again.
         installOutcome_ = outcome;
         Reload();
      }

      private AdministratorUpdate.Outcome installOutcome_;

      private void ShowInstallOutcome_()
      {
         if (installOutcome_ == null || WarningsPanel == null)
            return;
         WarningsPanel.Children.Insert(0, new InlineNotice
         {
            Level = AdministratorUpdate.LevelOf(installOutcome_),
            Text = AdministratorUpdate.OutcomeText(installOutcome_),
            Margin = new Thickness(0, 0, 0, 8)
         });
      }

      // True when this click was an administrator's install - started, or refused
      // with its reason; false for the service's own install.
      private bool TryAdministratorInstall_()
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return false;

         bool? needsAdministrator = null;
         int state = 0;
         string version = "", releaseUrl = "";
         int waitSeconds = 180;
         try
         {
            dynamic status = app.Status;
            state = (int)status.UpdateState;
            version = (string)status.AvailableVersion;
            releaseUrl = (string)status.AvailableVersionUrl;
            try
            {
               needsAdministrator = (bool)status.UpdateNeedsAdministrator;
            }
            catch (Exception older) when (!ExceptionPolicy.IsFatal(older))
            {
               // A server before 6.3.6: it says itself what to do.
               needsAdministrator = null;
            }
            ServerSession.Release(status);
            try
            {
               dynamic settings = app.Settings;
               waitSeconds = int.TryParse((string)settings.GetIniSetting("UpdateServiceWaitSeconds"), out int wait) ? wait : 180; // no-loc: a setting's name
               ServerSession.Release(settings);
            }
            catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
            {
               waitSeconds = 180;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return false;
         }

         string caption = L("Install update");
         switch (AdministratorUpdate.Decide(ServerSession.IsLocalSession, needsAdministrator, state, version))
         {
            case AdministratorUpdate.Decision.ServiceInstalls:
               return false;
            case AdministratorUpdate.Decision.Remote:
               MessageBox.Show(AdministratorUpdate.RemoteText(ServerSession.Current.Host, ReadServiceAccount_(app)), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
               return true;
            case AdministratorUpdate.Decision.NotReady:
               MessageBox.Show(L("No verified installer is waiting; check for updates and download it first."), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
               return true;
            case AdministratorUpdate.Decision.UnderWay:
               MessageBox.Show(AdministratorUpdate.UnderWayText(), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
               return true;
            case AdministratorUpdate.Decision.BadVersion:
               MessageBox.Show(F("The release on offer, {0}, is not a version this Control Panel installs.", version), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
               return true;
         }

         // The installation, as the service manager has it: the stage re-derives it
         // the same way once elevated.
         WindowsServiceInfo service = WindowsServiceInfo.Query();
         if (!AdministratorUpdate.InstallationFrom(WindowsServiceInfo.ExecutableFrom(service.PathName), out string installation))
         {
            MessageBox.Show(AdministratorUpdate.MissingFileText("Bin\\hMailServer.exe", version, releaseUrl), caption, MessageBoxButton.OK, MessageBoxImage.Warning); // no-loc: a file name
            return true;
         }
         foreach (string file in AdministratorUpdate.RequiredFiles(installation))
         {
            if (!System.IO.File.Exists(file))
            {
               MessageBox.Show(AdministratorUpdate.MissingFileText(file, version, releaseUrl), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
               return true;
            }
         }
         string own = Environment.ProcessPath ?? "";
         string expected = System.IO.Path.Combine(installation, "ControlPanel", "hMailCP.exe");
         if (!string.Equals(System.IO.Path.GetFullPath(own), System.IO.Path.GetFullPath(expected), StringComparison.OrdinalIgnoreCase))
         {
            MessageBox.Show(AdministratorUpdate.NotInstalledCopyText(own, expected), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
            return true;
         }

         // Another administrator's install under way - by a mark written within the
         // time one can last; an older one is an Updater's that did not finish,
         // and the stage, which holds the busy file, is the judge of that.
         string inProgressPath = System.IO.Path.Combine(installation, "AdminUpdate", "in-progress");
         string inProgress = AdministratorUpdate.ReadShared(inProgressPath);
         if (inProgress != null && AdministratorUpdate.InProgressMarkIsLive(System.IO.File.GetLastWriteTimeUtc(inProgressPath), DateTime.UtcNow))
         {
            MessageBox.Show(AdministratorUpdate.BusyText(AdministratorUpdate.ParseKeyValues(inProgress)), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
            return true;
         }

         // The permission rule, before Windows is asked: the one check that cannot
         // live in the program it protects.
         Mouse.OverrideCursor = Cursors.Wait;
         InstallationPermissions.Finding finding;
         try
         {
            finding = InstallationPermissions.CheckInstallation(installation, ReadTrustFiles_(installation));
         }
         finally
         {
            Mouse.OverrideCursor = null;
         }
         if (finding.Failed)
         {
            MessageBox.Show(AdministratorUpdate.PermissionText(finding), caption, MessageBoxButton.OK, MessageBoxImage.Warning);
            return true;
         }

         var window = new UpdateInstallWindow(Window.GetWindow(this), installation, version, ReadRunningVersion_(app), waitSeconds);
         window.ShowDialog();
         // Closed for the installer, the Control Panel is ending: the page is not
         // read again.
         if (!window.ClosedForInstaller)
            Reload();
         return true;
      }

      private static string ReadServiceAccount_(dynamic app)
      {
         try
         {
            WindowsServiceInfo service = WindowsServiceInfo.Query();
            return service.StartName;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return "";
         }
      }

      private static string ReadRunningVersion_(dynamic app)
      {
         try
         {
            string version = (string)app.Version;
            int dash = version.IndexOf('-');
            return dash > 0 ? version.Substring(0, dash) : version;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return "";
         }
      }

      // The two Sigstore trust files hMailServer.INI may name, which the install
      // reads elevated and so must be the administrators' alone.
      private static IEnumerable<string> ReadTrustFiles_(string installation)
      {
         var files = new List<string>();
         string ini = System.IO.Path.Combine(installation, "Bin", "hMailServer.INI");
         foreach (string key in new[] { "UpdateTrustRootsFile", "UpdateLogPublicKeyFile" }) // no-loc: setting names
         {
            string value = ProfileApi.ReadString("Settings", key, "", ini, 4096); // no-loc: the section's name
            if (!string.IsNullOrWhiteSpace(value))
               files.Add(value.Trim());
         }
         return files;
      }

      // The three update steps, on the server: a check reads the feed, a download
      // fetches and verifies the installer, an install hands it to the helper. Each
      // answers when it is done, so the cursor waits with it.
      private void RunUpdateAction_(string action)
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null)
            return;

         if (action == "install" && !MessageBox.Confirm(
                L("Install the update now?\n\nThe verified installer is handed to the update helper: the service stops, the new version is installed, and the service starts again. If it does not come back, the previous version is reinstalled, unless the new version has already changed the database. This Control Panel loses its connection while that happens; reconnect afterwards to see the outcome here.") + "\n\n" +
                L("This Control Panel closes as soon as the helper has started, so that the installer can replace its files; open it again in a minute to see the outcome."),
                L("Control Panel"), L("_Install the update"), cancelByDefault: true, decline: L("_Later")))
            return;

         Mouse.OverrideCursor = Cursors.Wait;
         try
         {
            dynamic status = app.Status;
            bool done = action == "check" ? (bool)status.CheckForUpdate()
               : action == "download" ? (bool)status.DownloadUpdate()
               : (bool)status.InstallUpdate();
            string error = (string)status.UpdateLastError;
            ServerSession.Release(status);
            if (!done && !string.IsNullOrEmpty(error))
               MessageBox.Show(error, L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Warning);
            else if (done && action == "install")
            {
               // The helper is running, and the installer it starts replaces this
               // program's files - silent, it aborts at the first one it cannot. The
               // helper ends a Control Panel it finds running; closing here is the
               // same outcome without the surprise, and without the "RPC server is
               // unavailable" a page left open across the service's stop would show.
               MessageBox.Show(L("The update helper has started. This Control Panel closes now so that the installer can replace its files; open it again in a minute to see the outcome."),
                  L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Information);
               Application.Current.Shutdown();
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("The update step could not be run: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"), MessageBoxButton.OK, MessageBoxImage.Error);
         }
         finally
         {
            Mouse.OverrideCursor = null;
         }
         Reload();
      }

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

         // The update check's verdict, kept by the server; the row says what the
         // scheduled check (UpdateCheckEnabled) or an on-demand one last found.
         try
         {
            dynamic status = app.Status;
            int updateState = (int)status.UpdateState;
            string availableVersion = (string)status.AvailableVersion;
            string applyOutcome = (string)status.UpdateApplyOutcome;
            string preparationStep = null;
            if (updateState == 6)
            {
               try
               {
                  preparationStep = AdministratorUpdate.ParsePreparation((string)status.UpdatePreparation)?.Step;
               }
               catch (Exception older) when (!ExceptionPolicy.IsFatal(older))
               {
                  preparationStep = null;
               }
            }
            SetValue_(UpdateValue, L("Update status"), UpdateStatusText.For(updateState, availableVersion,
               (string)status.AvailableVersionPublished, (string)status.UpdateLastChecked, (string)status.UpdateLastError, applyOutcome, preparationStep));
            ServerSession.Release(status);
            // Download once a newer release is known (a failed download can be retried);
            // install once one is verified and waiting.
            DownloadUpdateButton.IsEnabled = updateState == 2 || (updateState == 5 && !string.IsNullOrEmpty(availableVersion));
            InstallUpdateButton.IsEnabled = updateState == 3;
            SettleAfterInstall_(updateState);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            SetValue_(UpdateValue, L("Update status"), NotRead_(fatalCheck));
            DownloadUpdateButton.IsEnabled = false;
            InstallUpdateButton.IsEnabled = false;
         }

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
      }

      /// <summary>
      /// A read that failed, said once under the header at the Critical level with
      /// Try again, and Pause and the three update buttons disabled while it shows:
      /// with the link down every readout said "-", and the only explanation was an
      /// Information line at the foot of the warnings card. A read that worked
      /// hides it; the buttons' own reads have set them already, except Check for
      /// updates, which needs nothing read.
      /// </summary>
      private void ShowReadFailure_()
      {
         if (readError_ == null)
         {
            ReadFailure.Clear(ReadNotice);
            CheckUpdateButton.IsEnabled = true;
            return;
         }

         ReadFailure.Show(ReadNotice, null, null, L("Could not read the server: "), readError_, Reload,
            PauseButton, CheckUpdateButton, DownloadUpdateButton, InstallUpdateButton);
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
            dynamic settings = app.Settings;
            try
            {
               // Each check names the page that puts it right, and opens it.
               if (((string)settings.HostName).Length == 0)
                  count += AddWarning(StatusLevel.Warning, L("No public host name is configured in the SMTP settings."), "protocols");

               if ((bool)settings.DenyMailFromNull)
                  count += AddWarning(StatusLevel.Warning, L("Mail from an empty sender address is denied. Many servers send bounces from <>, which will be rejected."), "protocols");

               dynamic ranges = settings.SecurityRanges;
               int autoban = 0;
               int rangeCount = (int)ranges.Count;
               for (int i = 0; i < rangeCount; i++)
               {
                  dynamic range = ranges.Item[i];
                  try
                  {
                     if ((bool)range.AllowDeliveryFromRemoteToRemote && !(bool)range.RequireSMTPAuthExternalToExternal)
                        count += AddWarning(StatusLevel.Critical, F("IP range '{0}' allows external-to-external delivery without authentication (open relay risk).", (string)range.Name), "ipranges");

                     if ((string)range.LowerIP == "127.0.0.1" && (string)range.UpperIP == "127.0.0.1" && (bool)range.Expires)
                        count += AddWarning(StatusLevel.Warning, L("Localhost is currently banned in the IP ranges."), "ipranges");

                     if ((bool)range.Expires)
                        autoban++;
                  }
                  finally
                  {
                     ServerSession.Release(range);
                  }
               }
               ServerSession.Release(ranges);

               // A count, which is a statistic and not a finding: it was filed as
               // an Information "warning", the one item in that card on most
               // servers.
               SetValue_(AutobanValue, L("Auto-ban IP ranges"), autoban.ToString("N0"));
               AutobanLink.Visibility = autoban > 0 ? Visibility.Visible : Visibility.Collapsed;
            }
            finally
            {
               ServerSession.Release(settings);
            }
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

         ShowInstallOutcome_();
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
