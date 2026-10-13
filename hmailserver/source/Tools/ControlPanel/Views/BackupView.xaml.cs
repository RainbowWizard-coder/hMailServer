// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class BackupView : UserControl, IPageLifecycle, IRevealSetting
   {
      // The migrations' card, the server's settings for moving mailboxes in from
      // another server or a PST file (review finding F273), made as the page is first
      // entered, with its own Save, captioned in this page's scope.
      private ServerSettingsView migrations_;

      // BackupMessagesDBOnly changes what a backup and a restore do, but it lives
      // in hMailServer.ini rather than in the COM backup settings, so it needs its
      // own store alongside the COM properties on this page. The four schedule and
      // retention settings below live in the same file, for the same reason.
      private readonly IniFeatureStore iniStore_ = new IniFeatureStore();

      // Whether the server says a backup passphrase is set. It is the only thing
      // about the passphrase that can be read at all - EncryptionPassphrase is
      // write-only over COM, as it is over REST - so it is what the check box and
      // the notice are drawn from, and what decides whether an empty box means
      // "leave it alone" or "there is nothing to leave alone".
      private bool encryptionPassphraseSet_;

      public BackupView()
      {
         InitializeComponent();

         // Ctrl+S saves the card the keyboard is in - the backup's settings or the
         // schedule - as each card's own Save does (Views/PageCommands, which
         // presses the marked button nearest the keyboard). The settings card's
         // Save has no name, and is found in its card by its caption once the
         // card is drawn. F5 is not this page's: it has no Reload, and reading the
         // settings again would throw what was typed away without asking.
         PageCommands.SetCommand(SaveScheduleButton, PageCommand.Save);

         // The two pickers, captioned Browse… where they were a 40-pixel "…"
         // (review finding F324), are named by the caption and the field each
         // fills, so that the two are told apart.
         ControlHelp.NameLink(BrowseDestination, L("Browse…"), LabelInName.Shown(L("Destination _folder (on the server)")));
         ControlHelp.NameLink(BrowseBackupFile, L("Browse…"), LabelInName.Shown(L("Back_up archive (HMBackup*.7z on the server)")));
         Loaded += (_, _) =>
         {
            string saveSettings = L("Save s_ettings");
            foreach (System.Windows.Controls.Primitives.ButtonBase button in PageCommands.Descendants_(BackupCard).OfType<System.Windows.Controls.Primitives.ButtonBase>())
            {
               if (button.Content as string == saveSettings)
                  PageCommands.SetCommand(button, PageCommand.Save);
            }
         };

         // Put the message back acts on the message chosen in the list.
         SelectionGate.Bind(ArchivedMessageGrid, PutMessageBackButton);

         // Nothing is listed until List the messages in the archive reads them.
         ForgetArchivedMessages_();
      }

      public void OnEnter()
      {
         EnterMigrations_();

         // What the buttons did on a last visit is not news on this one.
         foreach (InlineNotice outcome in new[] { BackupOutcome, ScheduleOutcome, RestoreOutcome, MailboxOutcome, MessageOutcome })
            outcome.Visibility = Visibility.Collapsed;

         // The Browse… buttons open the Windows file pickers of the machine this
         // Control Panel runs on, which are the server's own disks only when it
         // runs on the server: connected to another machine they browsed the
         // wrong one (review finding F257). The boxes take a path as the server
         // sees it either way.
         bool local = ServerSession.IsLocalSession;
         foreach (Wpf.Ui.Controls.Button browse in new[] { BrowseDestination, BrowseBackupFile })
         {
            browse.IsEnabled = local;
            browse.ToolTip = local
               ? (browse == BrowseDestination ? L("Browse for a folder") : L("Browse for a file"))
               : L("Browsing works on the server itself; connected to another machine, type the path as the server sees it.");
            ToolTipService.SetShowOnDisabled(browse, true);
         }

         // When a save of each row kept in the settings store reaches the running
         // server, as the server marks it (Services/SettingApplies). All six are
         // marked now: the backup reads its two switches when it runs, and the
         // schedule task reads the time and the interval on every tick - a schedule
         // set on a server that started without one is registered by the save.
         // Every mark in one call (IniFeatureStore.LoadMarks).
         IniFeatureStore.LoadMarks();
         MessagesDbOnlyRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("BackupMessagesDBOnly"));
         VerifyRestoreRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("BackupVerifyRestore"));
         ScheduleTimeRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("ScheduledBackupTime"));
         IntervalRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("ScheduledBackupIntervalMinutes"));
         KeepCountRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("ScheduledBackupKeepCount"));
         MaxAgeRow.Applies = SettingApplies.Caption(IniFeatureStore.AppliesFor("ScheduledBackupMaxAgeDays"));

         if (iniStore_.IsAvailable)
         {
            CheckMessagesDbOnly.IsChecked = iniStore_.ReadBool("BackupMessagesDBOnly", false);
            CheckVerifyRestore.IsChecked = iniStore_.ReadBool("BackupVerifyRestore", true);

            // The schedule and retention settings, with the server's own defaults:
            // time "" and everything else 0, i.e. no schedule and no pruning
            // (IniFileSettings.cpp reads them exactly so).
            ScheduleTimeBox.Text = iniStore_.ReadString("ScheduledBackupTime", "").Trim();
            IntervalBox.Text = iniStore_.ReadString("ScheduledBackupIntervalMinutes", "0").Trim();
            KeepCountBox.Text = iniStore_.ReadString("ScheduledBackupKeepCount", "0").Trim();
            MaxAgeBox.Text = iniStore_.ReadString("ScheduledBackupMaxAgeDays", "0").Trim();
         }
         else
         {
            CheckMessagesDbOnly.IsEnabled = false;
            MessagesDbOnlyRow.Hint = L("hMailServer.ini was not found on this machine, so this can only be changed on the server itself.");
            CheckVerifyRestore.IsEnabled = false;
            VerifyRestoreRow.Hint = L("hMailServer.ini was not found on this machine, so this can only be changed on the server itself.");

            // An editor that cannot read the value back must not write it either -
            // it would misreport its own state on the next visit.
            ScheduleTimeBox.IsEnabled = false;
            IntervalBox.IsEnabled = false;
            KeepCountBox.IsEnabled = false;
            MaxAgeBox.IsEnabled = false;
            SaveScheduleButton.IsEnabled = false;
         }

         try
         {
            dynamic backup = ServerSession.Current.Application.Settings.Backup;
            DestinationBox.Text = (string)backup.Destination ?? "";
            CheckDomains.IsChecked = (bool)backup.BackupDomains;
            CheckMessages.IsChecked = (bool)backup.BackupMessages;
            CheckSettings.IsChecked = (bool)backup.BackupSettings;
            CheckCompress.IsChecked = (bool)backup.CompressDestinationFiles;
            encryptionPassphraseSet_ = (bool)backup.EncryptionPassphraseSet;
            ServerSession.Release(backup);

            ReadFailure.Clear(Notice, BackupCard);
            EncryptionNotice.Visibility = Visibility.Visible;
            CheckEncrypt.IsChecked = encryptionPassphraseSet_;
            PassphraseBox.Password = "";
            PassphraseAgainBox.Password = "";
            ShowEncryptionState_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            // The card showed the values of the last read, or ticked defaults, with
            // Save settings and Start backup now live - neither of which is a fact
            // this page has: the editors are cleared and the card disabled until a
            // read works, and the notice says what happened, with Try again
            // (Services/ReadFailure). The two hMailServer.ini boxes go too: with
            // the link lost, what they hold is the file's copy or the default -
            // BackupVerifyRestore's is on - and a default drawn as a setting is
            // the same claim, under the same notice.
            DestinationBox.Text = "";
            CheckDomains.IsChecked = CheckMessages.IsChecked = CheckSettings.IsChecked = CheckCompress.IsChecked = false;
            CheckMessagesDbOnly.IsChecked = CheckVerifyRestore.IsChecked = false;
            encryptionPassphraseSet_ = false;
            CheckEncrypt.IsChecked = false;
            PassphraseBox.Password = "";
            PassphraseAgainBox.Password = "";
            EncryptionNotice.Visibility = Visibility.Collapsed;
            ReadFailure.Show(Notice, null, null, L("Could not read the backup settings: {0}"), ex, OnEnter, BackupCard);
         }

         RefreshScheduleStatus();
      }

      /// <summary>
      ///    Which of the two states the server is in, in the words that matter: a
      ///    passphrase that is set is a restore that will ask for it, and one that
      ///    is not is an archive anybody who reaches the folder can read.
      /// </summary>
      private void ShowEncryptionState_()
      {
         // Warning, not Information: an archive anyone who reaches the folder can
         // read is a state to act on, not a fact to note (review finding F257).
         EncryptionNotice.Level = encryptionPassphraseSet_ ? StatusLevel.Good : StatusLevel.Warning;
         EncryptionNotice.Text = encryptionPassphraseSet_
            ? L("A passphrase is set: the archive is encrypted as it is written, file names included, and a restore asks for the passphrase.")
            : L("No passphrase is set: the archive is not encrypted, and anyone who can reach the destination folder can read every mailbox in it.");
      }

      /// <summary>
      /// What an action did, at its level, in the footer of the card whose button
      /// did it. It was said in one notice above the first card, a screen or more
      /// above the restore cards' buttons, and only Save settings raised a toast
      /// (review finding F257).
      /// </summary>
      private static void ShowOutcome_(InlineNotice outcome, StatusLevel level, string text)
      {
         outcome.Level = level;
         outcome.Text = text;
         outcome.Visibility = Visibility.Visible;

         // Said again when the words are the same as the last time.
         Announce.Request(outcome);
      }

      public void OnLeave()
      {
      }

      /// <summary>
      /// The migrations' card, read as the page is entered: a section of the settings
      /// pages embedded here, with its own Save (review finding F273).
      /// </summary>
      private void EnterMigrations_()
      {
         if (migrations_ == null)
         {
            migrations_ = new ServerSettingsView(ServerSettingsView.Section.Migrations, embedded: true) { SaveCaption = L("Save _migration settings") };
            System.Windows.Automation.AutomationProperties.SetAutomationId(migrations_, "backup-migrations");
            MigrationsHost.Content = migrations_;
         }

         migrations_.OnEnter();
      }

      /// <summary>
      /// An edit of the migrations' card is the page's unsaved change: the backup's own
      /// cards save themselves with their own buttons and keep no unsaved state.
      /// </summary>
      public bool HasUnsavedChanges => migrations_?.HasUnsavedChanges == true;

      /// <summary>Saves the migrations' card, as its own Save does.</summary>
      public bool SaveChanges() => migrations_ == null || migrations_.SaveChanges();

      /// <summary>A setting of the migrations' card, found by Ctrl+K, is shown on it.</summary>
      public bool RevealSetting(string key, string label) => migrations_ != null && migrations_.RevealSetting(key, label);

      /// <summary>
      ///    The passphrase to write, decided before anything is saved: null when
      ///    there is nothing to change, "" when the administrator has turned
      ///    encryption off, and the passphrase itself when one was typed. False
      ///    means the page has already said why it will not save.
      /// </summary>
      private bool ReadEncryptionChange_(out string passphrase)
      {
         passphrase = null;

         bool encrypt = CheckEncrypt.IsChecked is true;
         string typed = PassphraseBox.Password ?? "";
         string again = PassphraseAgainBox.Password ?? "";

         if (!encrypt)
         {
            if (encryptionPassphraseSet_)
               passphrase = "";

            return true;
         }

         if (typed.Length == 0)
         {
            if (encryptionPassphraseSet_)
               return true;

            MessageBox.Show(L("Enter the passphrase the archive is to be encrypted with, and type it again below."), L("Control Panel"));
            return false;
         }

         // A backup passphrase that is not what the person meant to type cannot be
         // found out later: nothing reads it back, and the archive it writes opens
         // for nobody. So it is typed twice or it is not accepted.
         if (typed != again)
         {
            MessageBox.Show(L("The two passphrases are not the same. Nothing has been saved."), L("Control Panel"));
            return false;
         }

         // The server refuses this combination when the backup runs, which would
         // be hours later and unattended; refusing it here is the same rule said
         // in time to act on it.
         if (CheckMessages.IsChecked is true && CheckCompress.IsChecked is not true)
         {
            MessageBox.Show(L("With a passphrase set and compression off, the message files are copied beside the archive instead of into it, where nothing encrypts them - and the backup refuses to run. Switch compression on, or clear the passphrase."), L("Control Panel"));
            return false;
         }

         passphrase = typed;
         return true;
      }

      private bool SaveBackupSettings()
      {
         if (!ReadEncryptionChange_(out string passphrase))
            return false;

         try
         {
            if (iniStore_.IsAvailable)
            {
               iniStore_.WriteBool("BackupMessagesDBOnly", CheckMessagesDbOnly.IsChecked is true);
               iniStore_.WriteBool("BackupVerifyRestore", CheckVerifyRestore.IsChecked is true);
            }

            dynamic backup = ServerSession.Current.Application.Settings.Backup;
            // IInterfaceBackupSettings has no Save method: each property setter
            // writes straight through to the server's settings store, so the
            // assignments above are already persisted.
            backup.Destination = DestinationBox.Text.Trim();
            backup.BackupDomains = CheckDomains.IsChecked is true;
            backup.BackupMessages = CheckMessages.IsChecked is true;
            backup.BackupSettings = CheckSettings.IsChecked is true;
            backup.CompressDestinationFiles = CheckCompress.IsChecked is true;

            // Last, and only when there is something to change: the setter is the
            // one on this page that can refuse (a passphrase too short, or one
            // with a character an archive would not survive being moved with), and
            // its refusal is the server's own sentence.
            if (passphrase != null)
            {
               backup.EncryptionPassphrase = passphrase;
               encryptionPassphraseSet_ = passphrase.Length > 0;
            }

            ServerSession.Release(backup);

            PassphraseBox.Password = "";
            PassphraseAgainBox.Password = "";
            CheckEncrypt.IsChecked = encryptionPassphraseSet_;
            ShowEncryptionState_();

            return true;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not save the backup settings: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return false;
         }
      }

      private void BrowseDestination_Click(object sender, RoutedEventArgs e)
      {
         string folder = PathPicker.PickFolder(DestinationBox.Text);
         if (folder != null)
            DestinationBox.Text = folder;
      }

      private void BrowseBackupFile_Click(object sender, RoutedEventArgs e)
      {
         string file = PathPicker.PickFile(BackupFileBox.Text, L("Backup archives (HMBackup*.7z)|HMBackup*.7z|All files (*.*)|*.*"));
         if (file != null)
            BackupFileBox.Text = file;
      }

      private void SaveSettings_Click(object sender, RoutedEventArgs e)
      {
         if (SaveBackupSettings())
         {
            // The COM properties are live immediately, and so are the two ini
            // switches now: both are marked now, and a save publishes them
            // (docs/SettingsApply.md). A server from before that reads them when it
            // starts, and is told so as it was.
            ShowOutcome_(BackupOutcome, StatusLevel.Good, IniFeatureStore.ServerMarksSettings == false && iniStore_.IsAvailable
               ? L("Backup settings saved - the message-metadata-only switch applies after a service restart.")
               : L("Backup settings saved."));
            Services.Toast.Success(L("Backup settings saved."));

            // The destination may have changed, and the destination is where the
            // "last backup" state is read from.
            RefreshScheduleStatus();
         }
      }

      private void StartBackup_Click(object sender, RoutedEventArgs e)
      {
         if (!SaveBackupSettings())
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            manager.StartBackup();
            ServerSession.Release(manager);
            string started = F("Backup started {0} - runs in the background on the server.", DateTime.Now.ToLongTimeString());
            ShowOutcome_(BackupOutcome, StatusLevel.Good, started);
            Services.Toast.Success(started, L("Backup & restore"));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the backup: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      private void StartRestore_Click(object sender, RoutedEventArgs e)
      {
         string backupFile = BackupFileBox.Text.Trim();
         if (backupFile.Length == 0)
         {
            MessageBox.Show(L("Choose an archive in the list above, or enter the path of one on the server."), L("Control Panel"));
            return;
         }

         if (!MessageBox.Confirm(
             L("Replace the server's data in the ticked categories with what the backup holds? What is there now is overwritten, and there is no undo."),
             L("Control Panel"), L("_Replace data"), destructive: true))
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;

            // With a passphrase, the archive is opened with it; without one, as
            // before. Either way the opening is where a wrong passphrase, a
            // damaged archive or one from a newer hMailServer is refused - with
            // the server's own sentence, and before anything here is touched.
            string passphrase = RestorePassphraseBox.Password ?? "";
            dynamic backup = passphrase.Length > 0
               ? manager.LoadEncryptedBackup(backupFile, passphrase)
               : manager.LoadBackup(backupFile);

            backup.RestoreDomains = RestoreDomains.IsChecked is true;
            backup.RestoreMessages = RestoreMessages.IsChecked is true;
            backup.RestoreSettings = RestoreSettings.IsChecked is true;
            backup.StartRestore();
            ServerSession.Release(backup);
            ServerSession.Release(manager);

            RestorePassphraseBox.Password = "";
            ShowOutcome_(RestoreOutcome, StatusLevel.Good, L("Restore started - runs in the background on the server."));
            Services.Toast.Success(L("Restore started - runs in the background on the server."), L("Backup & restore"));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the restore: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      // ---- One mailbox, or one message, from an archive ------------------------
      //
      // BackupManager.RestoreMailbox, RestoreMessage, DescribeMailbox and
      // MailboxRestoreStatus over COM: the same operation the Deck reaches over
      // REST. The archive is named as the destination lists it, or by its path
      // inside the destination; anything else the server refuses with its own
      // sentence, before anything is touched. What became of a job is read back
      // from the server and shown under the button.

      private int lastMailboxRestoreJob_;

      // The card whose job the last one is - the mailbox's or the one message's -
      // so that Re-read the outcome says it where it was asked for.
      private InlineNotice lastJobOutcome_;

      // The archive and the account the messages in the grid were listed from, or
      // null before a listing. A message's id is the one it has in its archive, so
      // Put the message back puts the one chosen back from these, whatever the boxes
      // say by then, and the listing goes once the boxes name another archive or
      // account (review finding F257): the id of a row listed from one archive,
      // sent with another chosen in the archive list, put back that archive's
      // message of the same id, or none.
      private string listedArchive_;
      private string listedAddress_;

      private bool ReadMailboxRestoreInputs_(out string archive, out string address)
      {
         archive = MailboxArchiveBox.Text.Trim();
         address = MailboxAddressBox.Text.Trim();

         if (archive.Length == 0)
         {
            MessageBox.Show(L("Name the archive as the backup destination lists it (HMBackup YYYY-MM-DD HHMMSS.7z), or by its path inside the destination."), L("Control Panel"));
            return false;
         }

         if (address.Length == 0)
         {
            MessageBox.Show(L("Enter the address of the account whose mailbox to restore."), L("Control Panel"));
            return false;
         }

         return true;
      }

      /// <summary>
      /// What became of a restore job, at its level and in words, in the card that
      /// started it: queued and running at Information, done at Good (Warning when
      /// some messages could not be put back), failed at Critical with the server's
      /// reason. The server's state token was printed here as it came, and its JSON
      /// when it could not be read (Services/BackupRestoreReport).
      /// </summary>
      private void ShowJobOutcome_(InlineNotice outcome, string json)
      {
         BackupRestoreReport.JobOutcome job = BackupRestoreReport.Job(json);
         ShowOutcome_(outcome, job.Level, job.Text);
         lastJobOutcome_ = outcome;
         MailboxRestoreStatusButton.IsEnabled = lastMailboxRestoreJob_ > 0;
      }

      private void RestoreMailbox_Click(object sender, RoutedEventArgs e)
      {
         if (!ReadMailboxRestoreInputs_(out string archive, out string address))
            return;

         bool replace = MailboxModeReplace.IsChecked is true;

         string question = replace
            ? F("Replace the mailbox of {0} with the one in the archive?\n\nEvery folder is emptied first: mail that arrived since the backup is gone. The person can put it back only if the Webmail page keeps deleted messages for them, which is off by default.", address)
            : F("Merge the folders and messages of the archive into the mailbox of {0}?\n\nNothing already there is removed.", address);

         // Replace empties the mailbox first and is drawn as the destructive act it
         // is; Merge removes nothing, and its button is an ordinary one.
         bool chosen = replace
            ? MessageBox.Confirm(question, L("Control Panel"), L("_Replace mailbox"), destructive: true)
            : MessageBox.Confirm(question, L("Control Panel"), L("_Merge"));
         if (!chosen)
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            int job = (int)manager.RestoreMailbox(archive, address, replace ? "replace" : "merge", MailboxPassphraseBox.Password ?? "");
            lastMailboxRestoreJob_ = job;
            string status = (string)manager.MailboxRestoreStatus(job);
            ServerSession.Release(manager);

            ShowJobOutcome_(MailboxOutcome, status);
            Services.Toast.Success(F("Mailbox restore {0} started - runs in the background on the server.", job), L("Backup & restore"));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the mailbox restore: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      private void RefreshMailboxRestore_Click(object sender, RoutedEventArgs e)
      {
         if (lastMailboxRestoreJob_ <= 0)
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            string status = (string)manager.MailboxRestoreStatus(lastMailboxRestoreJob_);
            ServerSession.Release(manager);
            ShowJobOutcome_(lastJobOutcome_ ?? MailboxOutcome, status);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not read the outcome: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      private void ListArchivedMessages_Click(object sender, RoutedEventArgs e)
      {
         if (!ReadMailboxRestoreInputs_(out string archive, out string address))
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            string json = (string)manager.DescribeMailbox(archive, address, MailboxPassphraseBox.Password ?? "");
            ServerSession.Release(manager);

            // Rows to choose from: the one selected is the one Put the message
            // back puts back, where its id was copied by hand out of a monospace
            // listing (review finding F257).
            List<BackupRestoreReport.ArchivedMessage> messages = BackupRestoreReport.ArchivedMessages(json, out string note, out string error);
            ArchivedMessageGrid.ItemsSource = messages;
            listedArchive_ = archive;
            listedAddress_ = address;
            MessageOutcome.Visibility = Visibility.Collapsed;
            if (messages == null)
            {
               ArchivedMessagesEmpty.Visibility = Visibility.Collapsed;
               ShowOutcome_(MessageOutcome, StatusLevel.Critical, error);
            }
            else
            {
               StatusText.Show(ArchivedMessagesEmpty, null, messages.Count, null, L("The archive holds no messages for this account."));
            }

            ArchivedMessagesNote.Text = note;
            ArchivedMessagesNote.Visibility = string.IsNullOrEmpty(note) ? Visibility.Collapsed : Visibility.Visible;
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not list the messages: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      /// <summary>
      /// The messages listed go, with what was said about them and about the last
      /// message put back, once the archive or the account named above is no longer
      /// the one they were listed from - another archive chosen in the list above, or
      /// either box typed in (review finding F257).
      /// </summary>
      private void MailboxNames_TextChanged(object sender, TextChangedEventArgs e)
      {
         if (listedArchive_ == null)
            return;

         if (!string.Equals(MailboxArchiveBox.Text.Trim(), listedArchive_, StringComparison.Ordinal)
             || !string.Equals(MailboxAddressBox.Text.Trim(), listedAddress_, StringComparison.Ordinal))
            ForgetArchivedMessages_();
      }

      /// <summary>
      /// The message card with nothing listed - before a listing, and once the one
      /// there was goes - says so in the grid's place and what lists them, where the
      /// grid was a blank under its headers.
      /// </summary>
      private void ForgetArchivedMessages_()
      {
         listedArchive_ = null;
         listedAddress_ = null;
         ArchivedMessageGrid.ItemsSource = null;
         StatusText.Show(ArchivedMessagesEmpty, null, 0, null, L("No messages listed yet. List the messages in the archive reads them from the archive and the account named above."));
         ArchivedMessagesNote.Visibility = Visibility.Collapsed;
         MessageOutcome.Visibility = Visibility.Collapsed;
      }

      private void RestoreArchivedMessage_Click(object sender, RoutedEventArgs e)
      {
         // Enabled only while a message is selected (SelectionGate), and a message
         // is listed only with the archive and the account it was listed from.
         if (listedArchive_ == null || ArchivedMessageGrid.SelectedItem is not BackupRestoreReport.ArchivedMessage selected || selected.Id <= 0)
            return;

         long messageId = selected.Id;

         if (!MessageBox.Confirm(BackupRestoreReport.PutBackQuestion(selected, listedAddress_), L("Control Panel"), L("_Put the message back")))
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            int job = (int)manager.RestoreMessage(listedArchive_, listedAddress_, messageId, ArchivedMessageFolderBox.Text.Trim(), MailboxPassphraseBox.Password ?? "");
            lastMailboxRestoreJob_ = job;
            string status = (string)manager.MailboxRestoreStatus(job);
            ServerSession.Release(manager);

            ShowJobOutcome_(MessageOutcome, status);
            Services.Toast.Success(F("Message restore {0} started - runs in the background on the server.", job), L("Backup & restore"));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the message restore: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
         }
      }

      // ---- Schedule & retention (hMailServer.ini) -----------------------------

      private void SaveSchedule_Click(object sender, RoutedEventArgs e)
      {
         if (!iniStore_.IsAvailable)
            return;

         string time = ScheduleTimeBox.Text.Trim();
         if (time.Length > 0 && !TryParseTimeOfDay(time, out _, out _))
         {
            // The server is deliberately strict (BackupScheduleTask::ParseTimeOfDay
            // refuses anything it would have to guess at), so refuse here too
            // rather than store a value the service will reject at start-up.
            MessageBox.Show(F("'{0}' is not a 24-hour HH:MM time. The server accepts values like 02:00 or 23:45 and refuses anything else rather than guess. Leave the box empty for no daily backup.", time),
               L("Control Panel"));
            return;
         }

         if (!TryReadCount(IntervalBox, L("Backup interval in minutes"), out int intervalMinutes))
            return;
         if (!TryReadCount(KeepCountBox, L("Archives to keep"), out int keepCount))
            return;
         if (!TryReadCount(MaxAgeBox, L("Delete archives older than this many days"), out int maxAgeDays))
            return;

         try
         {
            iniStore_.WriteString("ScheduledBackupTime", time);
            iniStore_.WriteString("ScheduledBackupIntervalMinutes", intervalMinutes.ToString(CultureInfo.InvariantCulture));
            iniStore_.WriteString("ScheduledBackupKeepCount", keepCount.ToString(CultureInfo.InvariantCulture));
            iniStore_.WriteString("ScheduledBackupMaxAgeDays", maxAgeDays.ToString(CultureInfo.InvariantCulture));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not write to hMailServer.ini: {0}", ServerSession.DescribeComError(ex)), L("Control Panel"));
            return;
         }

         // Show what was actually stored, normalised (an empty count box was 0).
         ScheduleTimeBox.Text = time;
         IntervalBox.Text = intervalMinutes.ToString(CultureInfo.InvariantCulture);
         KeepCountBox.Text = keepCount.ToString(CultureInfo.InvariantCulture);
         MaxAgeBox.Text = maxAgeDays.ToString(CultureInfo.InvariantCulture);

         ShowOutcome_(ScheduleOutcome, StatusLevel.Good, IniFeatureStore.ServerMarksSettings == false
            ? L("Backup schedule saved - the service reads these settings when it starts, so they apply after a service restart.")
            : L("Backup schedule saved."));
         Services.Toast.Success(L("Backup schedule saved."));
         RefreshScheduleStatus();
      }

      private void RefreshScheduleStatus_Click(object sender, RoutedEventArgs e)
      {
         RefreshScheduleStatus();
      }

      private static bool TryReadCount(Wpf.Ui.Controls.TextBox box, string label, out int value)
      {
         if (!NumericField.TryValidate(box.Text, label, 0, int.MaxValue, out value, out bool hasValue, out string error))
         {
            MessageBox.Show(error, L("Control Panel"));
            return false;
         }

         // An empty box means the shipped default, which for all three counts is 0.
         if (!hasValue)
            value = 0;

         return true;
      }

      /// <summary>
      /// Recomputes the two state lines: whether a schedule is configured (from
      /// hMailServer.ini, the file the service reads at start-up) and how old the
      /// newest archive in the destination is (from the destination directory
      /// itself, which is also where the server recovers this fact from after a
      /// restart - see BackupScheduleTask::Seed_).
      /// </summary>
      private void RefreshScheduleStatus()
      {
         bool scheduleConfigured = false;
         int expectedMinutes = 0;

         StatusLevel scheduleLevel;
         string scheduleText;

         if (!iniStore_.IsAvailable)
         {
            scheduleLevel = StatusLevel.Warning;
            scheduleText = L("Cannot tell whether a schedule is configured: hMailServer.ini was not found on this machine. Check the daily backup time and the backup interval on the server itself.");
         }
         else
         {
            string time = iniStore_.ReadString("ScheduledBackupTime", "").Trim();
            int intervalMinutes = ParseServerInt(iniStore_.ReadString("ScheduledBackupIntervalMinutes", "0"));

            // These four branches mirror BackupScheduleTask: a valid daily time
            // wins over the interval; an invalid time falls back to the interval
            // if there is one; and with neither, no task is created at all.
            if (time.Length > 0 && TryParseTimeOfDay(time, out int hour, out int minute))
            {
               scheduleConfigured = true;
               expectedMinutes = 24 * 60;
               scheduleLevel = StatusLevel.Good;
               scheduleText = F("A schedule is configured: daily at {0:00}:{1:00}, server local time.", hour, minute);
            }
            else if (time.Length > 0 && intervalMinutes > 0)
            {
               scheduleConfigured = true;
               expectedMinutes = intervalMinutes;
               scheduleLevel = StatusLevel.Warning;
               scheduleText = PluralF(intervalMinutes,
                  "The daily backup time '{0}' is not a valid 24-hour HH:MM time, so the server falls back to the interval: one backup every {1} minute.",
                  "The daily backup time '{0}' is not a valid 24-hour HH:MM time, so the server falls back to the interval: one backup every {1} minutes.",
                  time, intervalMinutes);
            }
            else if (time.Length > 0)
            {
               scheduleLevel = StatusLevel.Critical;
               scheduleText = F("The daily backup time '{0}' is not a valid 24-hour HH:MM time and no interval is set, so no scheduled backup will ever run.", time);
            }
            else if (intervalMinutes > 0)
            {
               scheduleConfigured = true;
               expectedMinutes = intervalMinutes;
               scheduleLevel = StatusLevel.Good;
               scheduleText = Plural(intervalMinutes, "A schedule is configured: one backup every {0} minute.", "A schedule is configured: one backup every {0} minutes.");
            }
            else
            {
               scheduleLevel = StatusLevel.Warning;
               scheduleText = L("No schedule is configured, so no backup will ever run on its own. Backups happen only when someone clicks Start backup now.");
            }
         }

         ApplyStatus(ScheduleNotice, scheduleLevel, scheduleText);

         StatusLevel lastLevel;
         string lastText;

         string destination = TryReadDestination();

         if (destination == null)
         {
            lastLevel = StatusLevel.Warning;
            lastText = L("Cannot tell when the last backup ran: the backup settings could not be read from the server.");
         }
         else if (destination.Length == 0)
         {
            lastLevel = StatusLevel.Warning;
            lastText = scheduleConfigured
               ? L("No destination folder is configured, so the schedule above will skip every run until one is set.")
               : L("No destination folder is configured, so a backup has nowhere to go.");
         }
         else
         {
            string newestName = null;
            DateTime newestTime = DateTime.MinValue;
            bool destinationReadable;

            try
            {
               var newest = System.IO.Directory.EnumerateFiles(destination)
                  .Select(System.IO.Path.GetFileName)
                  .Select(name => (Name: name, IsArchive: BackupRestoreReport.TryParseArchiveName(name, out DateTime created), Created: created))
                  .Where(archive => archive.IsArchive)
                  .MaxBy(archive => archive.Created);
               if (newest.Name != null)
               {
                  newestName = newest.Name;
                  newestTime = newest.Created;
               }
               destinationReadable = true;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               destinationReadable = false;
            }

            if (!destinationReadable)
            {
               lastLevel = scheduleConfigured ? StatusLevel.Warning : StatusLevel.Information;
               lastText = F("Cannot tell when the last backup ran: {0} could not be read from this machine. Check the folder on the server, or in the backup log on the Live logs page.", destination);
            }
            else if (newestName == null)
            {
               lastLevel = scheduleConfigured ? StatusLevel.Critical : StatusLevel.Warning;
               lastText = Sentences(F("No backup has ever completed into {0} - there is no HMBackup archive there.", destination),
                  scheduleConfigured
                     ? L("If the schedule was only just set up, re-check after its first run was due; otherwise the backup log on the Live logs page says why runs are being skipped.")
                     : null);
            }
            else
            {
               TimeSpan age = DateTime.Now - newestTime;
               string described = F("Last backup: {0} ({1}).", DescribeAge(age), newestName);

               // 2.0 rather than 2, so the doubling happens in floating point. As an
               // int multiply this overflows for any expectedMinutes above
               // int.MaxValue / 2 - reachable, because ScheduledBackupIntervalMinutes
               // is read from the INI and nothing bounds it - and an overflow lands
               // NEGATIVE. age.TotalMinutes > (negative) is then true for every backup,
               // so the card would announce that backups are failing on a server whose
               // backups are fine, which is the one thing a status card must not do.
               if (scheduleConfigured && expectedMinutes > 0 && age.TotalMinutes > expectedMinutes * 2.0)
               {
                  lastLevel = StatusLevel.Critical;
                  lastText = Sentences(described, L("That is older than the schedule allows, so runs are failing or being skipped - the backup log on the Live logs page says why."));
               }
               else if (scheduleConfigured)
               {
                  lastLevel = StatusLevel.Good;
                  lastText = described;
               }
               else
               {
                  lastLevel = StatusLevel.Information;
                  lastText = Sentences(described, L("With no schedule configured, nothing will take the next one."));
               }
            }
         }

         ApplyStatus(LastBackupNotice, lastLevel, lastText);

         ShowArchives_(destination);
      }

      /// <summary>
      /// The archives in the destination, newest first, read from this machine by
      /// the server's own naming rule, as the last-backup line above is; the three
      /// restore cards take the one chosen, where its path and its name were typed
      /// by hand (review finding F257). Connected to another machine the folder is
      /// not this machine's to read, and the card says so.
      /// </summary>
      private void ShowArchives_(string destination)
      {
         ArchiveGrid.ItemsSource = null;
         ArchivesEmpty.Visibility = Visibility.Collapsed;
         ArchivesNotice.Visibility = Visibility.Collapsed;

         if (destination == null)
         {
            ShowOutcome_(ArchivesNotice, StatusLevel.Warning, L("The archives cannot be listed: the backup settings could not be read from the server."));
            return;
         }

         if (!ServerSession.IsLocalSession)
         {
            ShowOutcome_(ArchivesNotice, StatusLevel.Information, L("This Control Panel is connected to another machine, so the archives in the destination cannot be listed from here. Name the archive in the cards below as the destination lists it."));
            return;
         }

         if (destination.Length == 0)
         {
            StatusText.Show(ArchivesEmpty, null, 0, null, L("No destination folder is set, so there are no archives to list."));
            return;
         }

         List<BackupRestoreReport.Archive> archives;
         try
         {
            archives = BackupRestoreReport.Archives(destination,
               new System.IO.DirectoryInfo(destination).EnumerateFiles().Select(file => (file.Name, file.Length)).ToList());
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ShowOutcome_(ArchivesNotice, StatusLevel.Warning, F("{0} could not be read from this machine: {1}", destination, ex.Message));
            return;
         }

         ArchiveGrid.ItemsSource = archives;
         StatusText.Show(ArchivesEmpty, null, archives.Count, null, F("No backup archives in {0} yet. Start backup now writes the first.", destination));
      }

      private void ArchiveGrid_SelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (ArchiveGrid.SelectedItem is not BackupRestoreReport.Archive archive)
            return;

         // The whole restore opens the archive by its path; the mailbox and the
         // one-message restores name it as the destination lists it, and the
         // messages listed from another archive go (MailboxNames_TextChanged).
         BackupFileBox.Text = archive.Path;
         MailboxArchiveBox.Text = archive.Name;
      }

      /// <summary>The saved destination, "" when none is set, or null when COM could not be read.</summary>
      private static string TryReadDestination()
      {
         try
         {
            dynamic backup = ServerSession.Current.Application.Settings.Backup;
            string destination = (string)backup.Destination ?? "";
            ServerSession.Release(backup);
            return destination.Trim();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
      }

      /// <summary>The notice draws the level's colour, shape and word, and puts the
      /// word first in its accessible name.</summary>
      private static void ApplyStatus(InlineNotice notice, StatusLevel level, string message)
      {
         notice.Level = level;
         notice.Text = message;
      }

      /// <summary>
      /// Mirrors BackupScheduleTask::ParseTimeOfDay exactly: strict 24-hour
      /// "HH:MM", one-or-two-digit hour, exactly-two-digit minute, nothing else.
      /// The server refuses "26:00" and "2" rather than guessing, so the editor
      /// must refuse the same values or it would claim a schedule the service will
      /// not run.
      /// </summary>
      private static bool TryParseTimeOfDay(string value, out int hour, out int minute)
      {
         hour = 0;
         minute = 0;

         string trimmed = (value ?? "").Trim();

         int separator = trimmed.IndexOf(':');
         if (separator < 1)
            return false;

         string hourPart = trimmed.Substring(0, separator);
         string minutePart = trimmed.Substring(separator + 1);

         if (hourPart.Length > 2 || minutePart.Length != 2)
            return false;

         if (!hourPart.All(char.IsAsciiDigit))
            return false;
         if (!minutePart.All(char.IsAsciiDigit))
            return false;

         int parsedHour = int.Parse(hourPart, CultureInfo.InvariantCulture);
         int parsedMinute = int.Parse(minutePart, CultureInfo.InvariantCulture);

         if (parsedHour > 23 || parsedMinute > 59)
            return false;

         hour = parsedHour;
         minute = parsedMinute;
         return true;
      }

      /// <summary>
      /// How the running service would read a numeric INI value that this page did
      /// not write: anything unparseable counts as 0, i.e. off.
      /// </summary>
      private static int ParseServerInt(string value)
      {
         return int.TryParse((value ?? "").Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed)
            ? parsed
            : 0;
      }

      private static string DescribeAge(TimeSpan age)
      {
         if (age < TimeSpan.Zero)
            age = TimeSpan.Zero;

         if (age.TotalMinutes < 1)
            return L("less than a minute ago");

         if (age.TotalHours < 1)
         {
            int minutes = (int)age.TotalMinutes;
            return Plural(minutes, "{0} minute ago", "{0} minutes ago");
         }

         if (age.TotalDays < 2)
         {
            int hours = (int)age.TotalHours;
            return Plural(hours, "{0} hour ago", "{0} hours ago");
         }

         return Plural((int)age.TotalDays, "{0} day ago", "{0} days ago");
      }
   }

   /// <summary>
   /// String-typed access to hMailServer.ini for the backup page. These exist as
   /// named ReadString/WriteString methods - rather than calls to the store's
   /// plain Read/Write - so that the settings-index generator and the test that
   /// audits it can both see, by scanning this source, exactly which INI keys the
   /// page edits. The same convention the ReadBool/WriteBool calls above follow.
   /// </summary>
   internal static class BackupIniStringAccess
   {
      public static string ReadString(this IniFeatureStore store, string key, string defaultValue)
      {
         return store.Read(key, defaultValue);
      }

      public static void WriteString(this IniFeatureStore store, string key, string value)
      {
         store.Write(key, value);
      }
   }
}
