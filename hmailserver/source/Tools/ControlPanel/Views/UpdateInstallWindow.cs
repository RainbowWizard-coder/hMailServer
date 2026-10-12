// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Threading;
using hMailServer.ControlPanel.Services;
using Wpf.Ui.Controls;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   /// <summary>
   /// Install update, on a server whose service cannot run an installer: the whole
   /// install, after one click and Windows' own prompt (docs/ServiceAccount.md, "The
   /// live update"; Services/AdministratorUpdate.cs).
   ///
   /// In order, each a step on screen:
   ///   1. Windows' permission: {app}\Bin\hMailServer.exe /InstallUpdate, asked for
   ///      with ShellExecuteEx "runas" and this window as the prompt's owner. The
   ///      prompt IS the confirmation; declined, nothing happened anywhere.
   ///   2. Checking the installer: the elevated stage copies the service's download
   ///      into a folder only administrators can write and checks it there - the
   ///      publisher, the Sigstore log, the version, the architecture. Its
   ///      status.txt, which this user may read, says how far it got.
   ///   3. Backing up and 4. the version to go back to: once the stage has verified
   ///      the installer, the service is asked over COM to take the backup and fetch
   ///      the rollback image (Status.PrepareUpdateForAdministrator); its document
   ///      says how far it got. A failed backup asks whether to go on without one.
   ///      When the service is ready, the stage is told (the ready event it handed
   ///      to this process), checks the rollback image itself and hands over to the
   ///      Updater.
   ///   5. Stopping new connections: Status.BeginUpdateForAdministrator drains the
   ///      server and issues the database upgrade's token.
   ///   6. Installing: the sign-in is handed over (AfterUpdateHandoff), and the
   ///      Control Panel closes with the go code so that the installer can replace
   ///      it; the Updater starts it again afterwards.
   /// Cancel stays available until the drain; closing the window is Cancel.
   /// </summary>
   public sealed class UpdateInstallWindow : FluentDialogWindow
   {
      private enum StepState { Pending, Working, Done, Skipped, Failed }

      private sealed class Step
      {
         public string Title;
         public StepState State;

         /// <summary>
         /// The step's status mark, in its level's shape and fill-text colour (review
         /// finding F246): a hollow circle for every state but the four symbols, all in
         /// the text colour, looked like a radio button and coloured a failed step as a
         /// finished one.
         /// </summary>
         public System.Windows.Shapes.Path Mark;
         public System.Windows.Controls.TextBlock Line;
         public FrameworkElement Row;
      }

      private readonly string app_;
      private readonly string version_;
      private readonly string running_;
      private readonly int waitSeconds_;
      private readonly string workId_ = AdministratorUpdate.NewWorkId();
      private readonly string work_;

      private readonly Step permission_, check_, backup_, rollback_, drain_, install_;
      private readonly System.Windows.Controls.TextBlock summary_;
      private readonly Wpf.Ui.Controls.Button cancelButton_;
      private readonly DispatcherTimer timer_;

      private IntPtr stage_ = IntPtr.Zero;
      private bool prepareAsked_;
      private bool permissionNamed_;
      private bool readySet_;
      private bool begun_;
      private bool finished_;

      /// <summary>
      /// True once this window has shut the Control Panel down with the go code for
      /// the installer: nothing of the page that opened it may run after that, since
      /// the application is ending (a page reloaded in that moment threw from WPF's
      /// own resource lookup, on the smoke VM on 1 October 2026).
      /// </summary>
      public bool ClosedForInstaller { get; private set; }
      private bool cancelWanted_;
      private bool backupQuestionAsked_;
      private bool asking_;
      private string cancelReason_ = "";
      private Dictionary<string, string> status_ = new Dictionary<string, string>();

      public UpdateInstallWindow(Window owner, string app, string version, string running, int waitSeconds)
      {
         Owner = owner;
         app_ = app;
         version_ = version;
         running_ = running;
         waitSeconds_ = waitSeconds;
         work_ = AdministratorUpdate.WorkFolder(app, workId_);
         Title = F("Installing hMailServer {0}", version);

         var steps = new StackPanel();
         permission_ = AddStep_(steps, L("Windows' permission"));
         check_ = AddStep_(steps, L("Checking the installer"));
         backup_ = AddStep_(steps, L("Backing up"));
         rollback_ = AddStep_(steps, L("The version to go back to"));
         drain_ = AddStep_(steps, L("Stopping new connections"));
         install_ = AddStep_(steps, L("Installing"));

         summary_ = new System.Windows.Controls.TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, DesignTokens.Space.Md, 0, 0) };
         steps.Children.Add(summary_);

         cancelButton_ = new Wpf.Ui.Controls.Button { Content = L("Cancel"), MinWidth = 88 };
         cancelButton_.Click += (s, e) => AskToCancel_();

         UseFrame(Title, steps, null, cancelButton_,
            F("hMailServer {0} replaces {1} on this server. Windows asks for an administrator's permission once; after that everything happens by itself. The server stops accepting mail for a minute or two while it restarts, and if {0} does not start, {1} is put back, unless {0} has already changed the database.", version, running),
            640);

         timer_ = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(500) };
         timer_.Tick += (s, e) => Tick_();

         Loaded += (s, e) => Dispatcher.BeginInvoke(new Action(Start_), DispatcherPriority.ApplicationIdle);
         Closing += OnClosing_;
      }

      private static Step AddStep_(StackPanel parent, string title)
      {
         var step = new Step { Title = title };
         var grid = new Grid { Margin = new Thickness(0, 0, 0, DesignTokens.Space.Sm) };
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
         grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
         step.Mark = new System.Windows.Shapes.Path
         {
            Width = 12,
            Height = 12,
            Stretch = System.Windows.Media.Stretch.Fill,
            Margin = new Thickness(2, 4, DesignTokens.Space.Sm + 2, 0),
            VerticalAlignment = VerticalAlignment.Top
         };
         ApplyMark_(step.Mark, StepState.Pending);
         var text = new StackPanel();
         text.Children.Add(new System.Windows.Controls.TextBlock { Text = title, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap });
         step.Line = new System.Windows.Controls.TextBlock { TextWrapping = TextWrapping.Wrap, Opacity = 0.85 };
         text.Children.Add(step.Line);
         Grid.SetColumn(step.Mark, 0);
         Grid.SetColumn(text, 1);
         grid.Children.Add(step.Mark);
         grid.Children.Add(text);
         step.Row = grid;
         parent.Children.Add(grid);
         return step;
      }

      /// <summary>A step's level: finished Good, failed Critical, skipped Warning, under way Information, not started Normal.</summary>
      private static StatusLevel LevelOf_(StepState state) => state switch
      {
         StepState.Done => StatusLevel.Good,
         StepState.Failed => StatusLevel.Critical,
         StepState.Skipped => StatusLevel.Warning,
         StepState.Working => StatusLevel.Information,
         _ => StatusLevel.Normal
      };

      private static void ApplyMark_(System.Windows.Shapes.Path mark, StepState state)
      {
         StatusPresentation presentation = StatusSemantics.For(LevelOf_(state));
         ShapeMarkVisuals.ApplyMark(mark, presentation.Shape, presentation.FillTextBrushKey);
      }

      private static void Set_(Step step, StepState state, string line)
      {
         step.State = state;
         ApplyMark_(step.Mark, state);
         if (line != null)
            step.Line.Text = line;
         string word = state switch
         {
            StepState.Working => L("in progress"),
            StepState.Done => L("finished"),
            StepState.Skipped => L("skipped"),
            StepState.Failed => L("failed"),
            _ => L("not started")
         };
         AutomationProperties.SetName(step.Row, step.Title + ", " + word + (string.IsNullOrEmpty(step.Line.Text) ? "" : ", " + step.Line.Text));
      }

      // ---- 1. Windows' permission

      private void Start_()
      {
         Set_(permission_, StepState.Working, L("Waiting for you to allow hMailServer to make changes, in the Windows prompt."));
         string program = Path.Combine(app_, "Bin", "hMailServer.exe");
         string arguments = AdministratorUpdate.StageArguments(version_, workId_, Environment.ProcessId, waitSeconds_);

         var info = new ShellExecuteInfo
         {
            Size = Marshal.SizeOf<ShellExecuteInfo>(),
            Mask = SeeMaskNoCloseProcess | SeeMaskNoAsync | SeeMaskFlagNoUi,
            Window = new WindowInteropHelper(this).Handle,
            Verb = "runas", // no-loc: the shell verb
            File = program,
            Parameters = arguments,
            Directory = Environment.SystemDirectory,
            Show = 0
         };
         if (!ShellExecuteEx(ref info) || info.Process == IntPtr.Zero)
         {
            Finish_(permission_, AdministratorUpdate.ElevationFailedText(Marshal.GetLastWin32Error()), false);
            return;
         }
         stage_ = info.Process;
         // The prompt was answered yes: Windows started the stage. Its status file
         // names the account it runs as once it has written one; a stage that
         // stops before it does is not left looking as if the prompt were open.
         Set_(permission_, StepState.Done, "");
         timer_.Start();
      }

      // ---- 2 to 5, polled

      private void Tick_()
      {
         if (!AdministratorUpdate.InstallMayAdvance(finished_, asking_))
            return;

         string text = AdministratorUpdate.ReadShared(Path.Combine(work_, "status.txt"));
         if (text != null)
            status_ = AdministratorUpdate.ParseKeyValues(text);
         string state = status_.TryGetValue("state", out string s) ? s : "";

         if (state.Length > 0 && !permissionNamed_ && Value_("elevated-by").Length > 0)
         {
            permissionNamed_ = true;
            Set_(permission_, StepState.Done, F("Allowed as {0}.", Value_("elevated-by")));
         }

         // A cancel asked for before the stage had handed this process its events:
         // set as soon as it has.
         if (cancelWanted_ && SetStageEvent_("cancel-event"))
         {
            CancelOnServer_();
            Finish_(null, L("The update was stopped; nothing was installed."), false);
            return;
         }

         if (state == "checking")
            Set_(check_, StepState.Working, F("Checking {0}: its signature, its record in the Sigstore transparency log, its version and its architecture.",
               Path.GetFileName(Value_("installer-source"))));
         else if (state is "installer-verified" or "rollback-verified" or "rollback-none" or "handed-over")
         {
            if (check_.State != StepState.Done)
               Set_(check_, StepState.Done, F("Signed by {0} and recorded in the Sigstore log at {1}; version {2}, {3}.",
                  AdministratorUpdate.CommonNameOf(Value_("signer")), ServerTimeText.Utc(Value_("log-time")), version_, Value_("architecture")));
            if (!prepareAsked_)
               Prepare_(false);
         }

         if (state == "rollback-verified")
            Set_(rollback_, StepState.Done, F("hMailServer {0} is ready to be put back if needed.", running_));
         else if (state == "rollback-none")
            Set_(rollback_, StepState.Skipped, F("None: {0}. If {1} does not start, it will need reinstalling by hand.", Value_("rollback-reason"), version_));

         if (prepareAsked_ && !readySet_)
            PollService_();

         // The stage's end: handed over to the Updater, or stopped.
         if (stage_ != IntPtr.Zero && WaitForSingleObject(stage_, 0) == 0)
         {
            GetExitCodeProcess(stage_, out int exitCode);
            CloseHandle(stage_);
            stage_ = IntPtr.Zero;
            // Read once more now that the stage has ended: the read above may
            // have come before its last write - the reason it stopped.
            string last = AdministratorUpdate.ReadShared(Path.Combine(work_, "status.txt"));
            if (last != null)
               status_ = AdministratorUpdate.ParseKeyValues(last);
            if (exitCode == AdministratorUpdate.StageExit.HandedOver)
               Begin_();
            else
            {
               if (prepareAsked_)
                  CancelOnServer_();
               Step at = check_.State == StepState.Done ? rollback_ : check_;
               Finish_(at, AdministratorUpdate.StageStoppedText(exitCode, status_, version_), false);
            }
         }
      }

      private string Value_(string key) => status_.TryGetValue(key, out string value) ? value : "";

      private dynamic Status_()
      {
         dynamic app = ServerSession.Current?.Application;
         return app?.Status;
      }

      private void Prepare_(bool skipBackup)
      {
         prepareAsked_ = true;
         Set_(backup_, StepState.Working, L("Asking the service to take the backup."));
         try
         {
            dynamic status = Status_();
            bool prepared = (bool)status.PrepareUpdateForAdministrator(version_, skipBackup);
            string error = (string)status.UpdateLastError;
            ServerSession.Release(status);
            if (!prepared)
            {
               SetStageEvent_("cancel-event");
               Finish_(backup_, F("The service did not prepare the install: {0}", error), false);
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            SetStageEvent_("cancel-event");
            Finish_(backup_, F("The service did not prepare the install: {0}", ServerSession.DescribeComError(ex)), false);
         }
      }

      private void PollService_()
      {
         int state;
         string preparationJson, error;
         try
         {
            dynamic status = Status_();
            state = (int)status.UpdateState;
            preparationJson = (string)status.UpdatePreparation;
            error = (string)status.UpdateLastError;
            ServerSession.Release(status);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            return; // the next tick asks again
         }

         AdministratorUpdate.Preparation preparation = AdministratorUpdate.ParsePreparation(preparationJson);
         if (preparation != null)
         {
            string backupText = AdministratorUpdate.BackupText(preparation);
            if (preparation.BackupRunning)
               Set_(backup_, StepState.Working, backupText);
            else if (backupText.Length > 0 && preparation.Step != "backup")
               Set_(backup_, preparation.BackupTaken ? StepState.Done : StepState.Skipped, backupText);
            if (preparation.Step == "rollback")
               Set_(rollback_, StepState.Working, F("Fetching and checking the hMailServer {0} installer, to put back if {1} does not start.", running_, version_));
         }

         if (state == 7)
         {
            if (SetStageEvent_("ready-event"))
            {
               readySet_ = true;
               Set_(rollback_, StepState.Working, AdministratorUpdate.RollbackText(preparation, version_));
            }
            return;
         }

         if (state == 5)
         {
            if (preparation != null && preparation.BackupFailed.Length > 0 && !backupQuestionAsked_)
            {
               backupQuestionAsked_ = true;
               Set_(backup_, StepState.Failed, preparation.BackupFailed);
               if (Ask_(() => MessageBox.Confirm(AdministratorUpdate.BackupFailedQuestion(preparation, version_, running_), Title, L("_Install without a backup"), destructive: true)))
               {
                  Prepare_(true);
                  return;
               }
               cancelReason_ = "the backup failed and the administrator chose not to install without one"; // no-loc: a log line
            }
            SetStageEvent_("cancel-event");
            CancelOnServer_();
            Finish_(backup_, string.IsNullOrEmpty(cancelReason_) ? F("The service did not prepare the install: {0}", error) : L("The update was stopped; nothing was installed."), false);
         }
      }

      // ---- 5. Stopping new connections, and 6. closing for the installer

      private void Begin_()
      {
         cancelButton_.IsEnabled = false;
         try
         {
            dynamic status = Status_();
            bool begun = (bool)status.BeginUpdateForAdministrator(version_);
            string error = (string)status.UpdateLastError;
            ServerSession.Release(status);
            if (!begun)
            {
               SetStageEvent_("cancel-event");
               CancelOnServer_();
               Finish_(drain_, F("The update was not installed: {0}", error), false);
               return;
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            SetStageEvent_("cancel-event");
            CancelOnServer_();
            Finish_(drain_, F("The update was not installed: {0}", ServerSession.DescribeComError(ex)), false);
            return;
         }

         begun_ = true;
         timer_.Stop();
         Set_(drain_, StepState.Done, L("The server no longer accepts new connections; the mail in progress is finishing."));

         // The sign-in, carried to the Control Panel that opens again - unless a
         // second factor guards it, which a one-time code cannot be carried past.
         bool secondFactor = TotpManager.IsConfigured() || (ServerSession.Current?.UsedSecondFactor ?? true);
         if (!secondFactor)
         {
            byte[] entropy = AfterUpdateHandoff.ReadEntropy(work_);
            if (entropy != null)
            {
               ServerSession.Current.WriteAfterUpdateHandoff(workId_, entropy, out _);
               Array.Clear(entropy, 0, entropy.Length);
            }
         }

         Set_(install_, StepState.Working, L("This window closes now so that the installer can replace the Control Panel, and opens again when hMailServer is running, in about two minutes. Do not sign out of Windows until it has."));
         var close = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(3000) };
         close.Tick += (s, e) =>
         {
            close.Stop();
            finished_ = true;
            ClosedForInstaller = true;
            System.Windows.Application.Current.Shutdown(AdministratorUpdate.GoExitCode);
         };
         close.Start();
      }

      // ---- Cancel and the end

      private void AskToCancel_()
      {
         if (finished_)
         {
            Close();
            return;
         }
         if (begun_)
            return;
         if (!Ask_(() => MessageBox.Confirm(L("Stop the update? Nothing has been installed yet."), Title, L("_Stop the update"), destructive: true)))
            return;

         cancelReason_ = "cancelled from the Control Panel"; // no-loc: a log line
         cancelWanted_ = true;
         if (stage_ == IntPtr.Zero && !prepareAsked_)
         {
            Finish_(null, L("The update was stopped; nothing was installed."), false);
            return;
         }
         if (SetStageEvent_("cancel-event"))
         {
            CancelOnServer_();
            Finish_(null, L("The update was stopped; nothing was installed."), false);
            return;
         }
         // Not yet: the stage is still checking and has not handed this process
         // its events. The next tick sets it as soon as it has.
         cancelButton_.IsEnabled = false;
         summary_.Text = L("Stopping…");
      }

      // A question asked while the timer runs: the message loop of a modal box goes
      // on dispatching it, and the install went on underneath - on the installer
      // smoke VM on 2 October 2026 it reached the installer while "Stop the update?"
      // waited for its answer. Nothing advances until the question is answered.
      private bool Ask_(Func<bool> question)
      {
         asking_ = true;
         try
         {
            return question();
         }
         finally
         {
            asking_ = false;
         }
      }

      private void OnClosing_(object sender, CancelEventArgs e)
      {
         if (finished_ || begun_)
            return;
         e.Cancel = true;
         AskToCancel_();
      }

      private void CancelOnServer_()
      {
         if (!prepareAsked_)
            return;
         try
         {
            dynamic status = Status_();
            status.CancelUpdateForAdministrator(string.IsNullOrEmpty(cancelReason_) ? "the install stopped" : cancelReason_); // no-loc: a log line
            ServerSession.Release(status);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The service's own watch returns the state to the downloaded installer.
         }
      }

      private void Finish_(Step at, string text, bool good)
      {
         finished_ = true;
         timer_.Stop();
         if (at != null)
            Set_(at, good ? StepState.Done : StepState.Failed, text);
         summary_.Text = text;
         cancelButton_.Content = L("Close");
         cancelButton_.IsEnabled = true;
         if (stage_ != IntPtr.Zero)
         {
            CloseHandle(stage_);
            stage_ = IntPtr.Zero;
         }
      }

      // The events the stage duplicated into this process, by the handle values
      // status.txt names; each is set once.
      private readonly HashSet<string> set_ = new HashSet<string>();

      private bool SetStageEvent_(string key)
      {
         if (set_.Contains(key))
            return true;
         if (!long.TryParse(Value_(key), out long value) || value <= 0)
            return false;
         if (!SetEvent(new IntPtr(value)))
            return false;
         set_.Add(key);
         return true;
      }

      // ---- Win32

      private const int SeeMaskNoCloseProcess = 0x40, SeeMaskNoAsync = 0x100, SeeMaskFlagNoUi = 0x400;

      [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
      private struct ShellExecuteInfo
      {
         public int Size;
         public int Mask;
         public IntPtr Window;
         [MarshalAs(UnmanagedType.LPWStr)] public string Verb;
         [MarshalAs(UnmanagedType.LPWStr)] public string File;
         [MarshalAs(UnmanagedType.LPWStr)] public string Parameters;
         [MarshalAs(UnmanagedType.LPWStr)] public string Directory;
         public int Show;
         public IntPtr InstanceApp;
         public IntPtr IdList;
         [MarshalAs(UnmanagedType.LPWStr)] public string Class;
         public IntPtr ClassKey;
         public int HotKey;
         public IntPtr IconOrMonitor;
         public IntPtr Process;
      }

      [DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "ShellExecuteExW")]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool ShellExecuteEx(ref ShellExecuteInfo info);

      [DllImport("kernel32.dll", SetLastError = true)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

      [DllImport("kernel32.dll", SetLastError = true)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool GetExitCodeProcess(IntPtr process, out int exitCode);

      [DllImport("kernel32.dll", SetLastError = true)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool CloseHandle(IntPtr handle);

      [DllImport("kernel32.dll", SetLastError = true)]
      [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
      private static extern bool SetEvent(IntPtr handle);
   }
}
