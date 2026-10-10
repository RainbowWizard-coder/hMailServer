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
   /// The Updates page's own card (FeatureSettingsView.xaml's UpdateCard), above
   /// the update policy: the version this server runs, the update check's verdict,
   /// and the three steps - Check for updates, Download update, Install update - one at a
   /// time, the one the state calls for as the Primary, run off the UI thread.
   ///
   /// They were on Server status, under a row of its Server card, and the policy
   /// that drives them - the daily check, the channel, the download, the
   /// unattended window, the signature checks - was a card on API &amp; monitoring;
   /// nothing was called Updates, and Ctrl+K "update" found only setting rows
   /// (review finding F272). The page puts the two together, and Server status,
   /// the Dashboard and About say in one line when a release is waiting
   /// (<see cref="UpdateNotice"/>). The code is Server status's, moved.
   /// </summary>
   public partial class FeatureSettingsView
   {
      // Asking the service again while it still says an install that has ended is
      // under way (AdministratorUpdate.ShouldAskAgainAfterInstall).
      private System.Windows.Threading.DispatcherTimer settle_;
      private int settleAttempts_;

      private AdministratorUpdate.Outcome installOutcome_;

      private bool IsUpdatesPage_ => section_ == Section.Updates;

      /// <summary>
      /// The outcome of an administrator's install, at the top of the card: set by
      /// the Control Panel that opened again after it (MainWindow, --after-update).
      /// </summary>
      public void ShowInstallOutcome(AdministratorUpdate.Outcome outcome)
      {
         // Read again with the outcome known: the notice is drawn with the
         // readouts, and a service still saying "installing" is asked again.
         installOutcome_ = outcome;
         ReloadUpdate_();
      }

      private void EnterUpdates_()
      {
         UpdateCard.Visibility = IsUpdatesPage_ ? Visibility.Visible : Visibility.Collapsed;
         if (IsUpdatesPage_)
            ReloadUpdate_();
      }

      private void LeaveUpdates_() => settle_?.Stop();

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
               ReloadUpdate_();
            };
         }
         settle_.Start();
      }

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
      /// The version and the update check's verdict, kept by the server; the row
      /// says what the scheduled check (UpdateCheckEnabled) or an on-demand one
      /// last found. A read that failed is said once, in the card, at the level
      /// of its cause, with Try again, and the three steps held while it shows:
      /// Check for updates as well as Download and Install, since the service that
      /// runs them did not answer - with the link lost it offered Check, and
      /// pressing it brought "The update step could not be run". Where the page
      /// has already said under its header that the link is lost - Connection
      /// lost, and why - the card says it no second time: its readouts say Not
      /// read and its steps wait for the server, where a lost link put a second
      /// Critical notice, of the same cause, under the first.
      /// </summary>
      private void ReloadUpdate_()
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null || !IsUpdatesPage_)
            return;

         string readError = null;

         try
         {
            SetUpdateValue_(InstalledVersionValue, L("Installed version"),
               (string)app.Version + " (" + (string)app.VersionArchitecture + ")");
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            readError ??= ServerSession.DescribeComError(fatalCheck);
            SetUpdateValue_(InstalledVersionValue, L("Installed version"), L("Not read"));
         }

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
            SetUpdateValue_(UpdateValue, L("Update status"), UpdateStatusText.For(updateState, availableVersion,
               (string)status.AvailableVersionPublished, (string)status.UpdateLastChecked, (string)status.UpdateLastError, applyOutcome, preparationStep));
            ServerSession.Release(status);
            // The row says the last apply's outcome, which Server status and the
            // Dashboard said until it was shown here (Views/UpdateNotice).
            UpdateNotice.MarkSeen(applyOutcome);
            // Download once a newer release is known (a failed download can be retried);
            // install once one is verified and waiting; one of the three at a time.
            shownVersion_ = availableVersion;
            ShowUpdateSteps_(UpdateSteps.Next(updateState, availableVersion));
            SettleAfterInstall_(updateState);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            readError ??= ServerSession.DescribeComError(fatalCheck);
            SetUpdateValue_(UpdateValue, L("Update status"), L("Not read"));
            ShowUpdateSteps_(UpdateStep.Check);
            DownloadUpdateButton.IsEnabled = false;
            InstallUpdateButton.IsEnabled = false;
         }

         UpdateOutcomePanel.Children.Clear();
         if (installOutcome_ != null)
         {
            UpdateOutcomePanel.Children.Add(new InlineNotice
            {
               Level = AdministratorUpdate.LevelOf(installOutcome_),
               Text = AdministratorUpdate.OutcomeText(installOutcome_),
               Margin = new Thickness(0, 0, 0, 8)
            });
         }

         if (readError == null)
         {
            ReadFailure.Clear(UpdateReadNotice);
            CheckUpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep.Check, shownStep_, updateRunning_);
            return;
         }

         if (saveState_.State == SettingsPageState.NotConnected)
         {
            ReadFailure.Clear(UpdateReadNotice);
            ReadFailure.Show(null, null, null, null, readError, null, CheckUpdateButton, DownloadUpdateButton, InstallUpdateButton);
            return;
         }

         ReadFailure.Show(UpdateReadNotice, null, null, L("Could not read the update status: "), readError, ReloadUpdate_,
            CheckUpdateButton, DownloadUpdateButton, InstallUpdateButton);
      }

      // The release the card read and the step it offers, for the busy line's words and
      // for Check's being pressable once a read worked; and whether a step is under way,
      // which holds every step however often the card is read meanwhile.
      private string shownVersion_;
      private UpdateStep shownStep_ = UpdateStep.Check;
      private bool updateRunning_;

      /// <summary>
      /// The one step the state calls for, as the Primary, and Check again as the
      /// secondary before it while a release is known (review finding F246): the three
      /// were equal buttons, the two that did not apply greyed with no reason given. A
      /// step that does not apply is not shown, and while an install is under way none
      /// is offered and Check waits too.
      /// </summary>
      private void ShowUpdateSteps_(UpdateStep step)
      {
         shownStep_ = step;
         bool checkIsTheStep = step == UpdateStep.Check;
         CheckUpdateButton.Content = checkIsTheStep ? L("Check for _updates") : L("Check _again");
         CheckUpdateButton.Appearance = checkIsTheStep ? Wpf.Ui.Controls.ControlAppearance.Primary : Wpf.Ui.Controls.ControlAppearance.Secondary;
         CheckUpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep.Check, step, updateRunning_);
         // The step's appearance is set here, not in the XAML, where two Primaries in one
         // row - only one of them ever shown - read as a Primary before another button.
         DownloadUpdateButton.Appearance = Wpf.Ui.Controls.ControlAppearance.Primary;
         InstallUpdateButton.Appearance = Wpf.Ui.Controls.ControlAppearance.Primary;
         DownloadUpdateButton.Visibility = step == UpdateStep.Download ? Visibility.Visible : Visibility.Collapsed;
         DownloadUpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep.Download, step, updateRunning_);
         InstallUpdateButton.Visibility = step == UpdateStep.Install ? Visibility.Visible : Visibility.Collapsed;
         InstallUpdateButton.IsEnabled = UpdateSteps.CanPress(UpdateStep.Install, step, updateRunning_);
      }

      /// <summary>
      /// A step under way: the line that says which, and every step held until it is
      /// done - through updateRunning_, which ShowUpdateSteps_ and ReloadUpdate_ read,
      /// so that the page's Reload during a download, which reads the card again, does
      /// not enable the steps under "Downloading 6.3.7..." - while the rest of the page
      /// can be used meanwhile.
      /// </summary>
      private void SetUpdateBusy_(string text)
      {
         bool busy = text != null;
         updateRunning_ = busy;
         UpdateBusy.Text = text ?? "";
         UpdateBusy.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
         if (busy)
         {
            CheckUpdateButton.IsEnabled = false;
            DownloadUpdateButton.IsEnabled = false;
            InstallUpdateButton.IsEnabled = false;
         }
      }

      /// <summary>
      /// A readout and the accessible name that goes with it: the caption and the
      /// value are two TextBlocks WPF infers no relationship between, so the value
      /// is named "caption, value", as Server status names its readouts.
      /// </summary>
      private static void SetUpdateValue_(TextBlock value, string label, string text)
      {
         value.Text = text;
         AutomationProperties.SetName(value, label + ", " + text);
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
            ReloadUpdate_();
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
      // answers when it is done, which for a download of tens of megabytes is a while:
      // off the UI thread, with the card's busy line saying which step is under way,
      // where the window waited on a wait cursor and its tool tip said that the Control
      // Panel waits while it does (review finding F246).
      private async void RunUpdateAction_(string action)
      {
         dynamic app = ServerSession.Current?.Application;
         if (app == null || updateRunning_)
            return;

         if (action == "install" && !MessageBox.Confirm(
                L("Install the update now?\n\nThe verified installer is handed to the update helper: the service stops, the new version is installed, and the service starts again. If it does not come back, the previous version is reinstalled, unless the new version has already changed the database. This Control Panel loses its connection while that happens; reconnect afterwards to see the outcome here.") + "\n\n" +
                L("This Control Panel closes as soon as the helper has started, so that the installer can replace its files; open it again in a minute to see the outcome."),
                L("Control Panel"), L("_Install the update"), cancelByDefault: true, decline: L("_Later")))
            return;

         string version = shownVersion_ ?? "";
         SetUpdateBusy_(action == "check" ? L("Checking for updates…")
            : action == "download" ? F("Downloading {0}…", version)
            : F("Handing {0} to the update helper…", version));
         try
         {
            // The application as an object, so that the step is a typed call and not a
            // dynamic one, whose result could not be taken apart.
            object application = app;
            (bool done, string error) = await System.Threading.Tasks.Task.Run(() => RunUpdateStep_(application, action));
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
            SetUpdateBusy_(null);
         }
         ReloadUpdate_();
      }

      /// <summary>One update step on the server, off the UI thread: whether it was done, and the error it left.</summary>
      private static (bool Done, string Error) RunUpdateStep_(object application, string action)
      {
         dynamic app = application;
         dynamic status = app.Status;
         try
         {
            bool done = action == "check" ? (bool)status.CheckForUpdate()
               : action == "download" ? (bool)status.DownloadUpdate()
               : (bool)status.InstallUpdate();
            return (done, (string)status.UpdateLastError);
         }
         finally
         {
            ServerSession.Release((object)status);
         }
      }
   }
}
