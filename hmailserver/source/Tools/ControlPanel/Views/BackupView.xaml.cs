// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using hMailServer.ControlPanel.Services;
using hMailServer.ControlPanel.Views.Scaffold;
using MessageBox = hMailServer.ControlPanel.Views.Dialogs;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Views
{
   public partial class BackupView : UserControl, IPageLifecycle
   {
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
      }

      public void OnEnter()
      {
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
            MessagesDbOnlyRow.Hint = L("hMailServer.ini was not found on this machine, so BackupMessagesDBOnly can only be changed on the server itself.");
            CheckVerifyRestore.IsEnabled = false;
            VerifyRestoreRow.Hint = L("hMailServer.ini was not found on this machine, so BackupVerifyRestore can only be changed on the server itself.");

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

            CheckEncrypt.IsChecked = encryptionPassphraseSet_;
            PassphraseBox.Password = "";
            PassphraseAgainBox.Password = "";
            ShowEncryptionState_();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            ShowNotice_(StatusLevel.Critical, F("Could not read the backup settings: {0}", ex.Message));
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
         EncryptionNotice.Level = encryptionPassphraseSet_ ? StatusLevel.Good : StatusLevel.Information;
         EncryptionNotice.Text = encryptionPassphraseSet_
            ? L("A passphrase is set: the archive is encrypted as it is written, file names included, and a restore asks for the passphrase.")
            : L("No passphrase is set: the archive is not encrypted, and anyone who can reach the destination folder can read every mailbox in it.");
      }

      /// <summary>What the last action said, at its level, above the cards.</summary>
      private void ShowNotice_(StatusLevel level, string text)
      {
         Notice.Level = level;
         Notice.Text = text;
         Notice.Visibility = Visibility.Visible;
      }

      public void OnLeave()
      {
      }

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
            MessageBox.Show(F("Could not save the backup settings: {0}", ex.Message), L("Control Panel"));
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
         string file = PathPicker.PickFile(BackupFileBox.Text, L("Backup files (*.xml)|*.xml|All files (*.*)|*.*"));
         if (file != null)
            BackupFileBox.Text = file;
      }

      private void SaveSettings_Click(object sender, RoutedEventArgs e)
      {
         if (SaveBackupSettings())
         {
            // The COM properties are live immediately; the INI one is read when
            // the service starts, so don't claim both took effect.
            ShowNotice_(StatusLevel.Good, iniStore_.IsAvailable
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
            ShowNotice_(StatusLevel.Good, F("Backup started {0} - runs in the background on the server.", DateTime.Now.ToLongTimeString()));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the backup: {0}", ex.Message), L("Control Panel"));
         }
      }

      private void StartRestore_Click(object sender, RoutedEventArgs e)
      {
         string backupFile = BackupFileBox.Text.Trim();
         if (backupFile.Length == 0)
         {
            MessageBox.Show(L("Enter the path of the backup XML file on the server."), L("Control Panel"));
            return;
         }

         if (MessageBox.Show(
             L("Restoring replaces current data for the selected categories with the backup contents.\n\nContinue?"),
             L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
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
            ShowNotice_(StatusLevel.Good, L("Restore started - runs in the background on the server."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the restore: {0}", ex.Message), L("Control Panel"));
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

      private void ShowMailboxRestoreOutcome_(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;

            string text = F("Job {0} is {1}: {2} folders, {3} messages added, {4} skipped as still in the mailbox, {5} failed.",
               Number_(root, "job"), Text_(root, "state"), Number_(root, "folders"), Number_(root, "added"), Number_(root, "skipped"), Number_(root, "failed"));

            string error = Text_(root, "error");
            if (error.Length > 0)
               text += " " + error;

            MailboxRestoreOutcome.Text = text;
         }
         catch (JsonException)
         {
            MailboxRestoreOutcome.Text = json;
         }

         MailboxRestoreStatusButton.IsEnabled = lastMailboxRestoreJob_ > 0;
      }

      private static string Text_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static long Number_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number ? value.GetInt64() : 0;

      private void RestoreMailbox_Click(object sender, RoutedEventArgs e)
      {
         if (!ReadMailboxRestoreInputs_(out string archive, out string address))
            return;

         bool replace = MailboxModeReplace.IsChecked is true;

         string question = replace
            ? F("Replace the mailbox of {0} with the one in the archive?\n\nEvery folder is emptied first: mail that arrived since the backup is gone. It is kept only for RecoverDeletedDays, which is off by default.", address)
            : F("Merge the folders and messages of the archive into the mailbox of {0}?\n\nNothing already there is removed.", address);

         if (MessageBox.Show(question, L("Control Panel"), MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            int job = (int)manager.RestoreMailbox(archive, address, replace ? "replace" : "merge", MailboxPassphraseBox.Password ?? "");
            lastMailboxRestoreJob_ = job;
            string status = (string)manager.MailboxRestoreStatus(job);
            ServerSession.Release(manager);

            ShowMailboxRestoreOutcome_(status);
            ShowNotice_(StatusLevel.Good, F("Mailbox restore {0} started - runs in the background on the server.", job));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the mailbox restore: {0}", ex.Message), L("Control Panel"));
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
            ShowMailboxRestoreOutcome_(status);
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not read the outcome: {0}", ex.Message), L("Control Panel"));
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

            var lines = new StringBuilder();

            using (JsonDocument document = JsonDocument.Parse(json))
            {
               JsonElement root = document.RootElement;

               if (root.TryGetProperty("folders", out JsonElement folders) && folders.ValueKind == JsonValueKind.Array)
               {
                  foreach (JsonElement folder in folders.EnumerateArray())
                  {
                     if (!folder.TryGetProperty("messages", out JsonElement messages) || messages.ValueKind != JsonValueKind.Array)
                        continue;

                     foreach (JsonElement message in messages.EnumerateArray())
                     {
                        lines.Append(Number_(message, "id").ToString(CultureInfo.InvariantCulture).PadRight(10));
                        lines.Append(Text_(message, "date")).Append("  ");
                        lines.Append(Text_(message, "folder")).Append("  ");
                        lines.Append(Text_(message, "from")).Append("  ");
                        lines.Append(Text_(message, "subject"));
                        lines.AppendLine();
                     }
                  }
               }

               if (lines.Length == 0)
                  lines.AppendLine(L("The archive holds no messages for this account."));

               string note = Text_(root, "subjects_note");
               if (note.Length > 0)
                  lines.AppendLine(note);
            }

            ArchivedMessagesBox.Text = lines.ToString();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not list the messages: {0}", ex.Message), L("Control Panel"));
         }
      }

      private void RestoreArchivedMessage_Click(object sender, RoutedEventArgs e)
      {
         if (!ReadMailboxRestoreInputs_(out string archive, out string address))
            return;

         if (!long.TryParse(ArchivedMessageIdBox.Text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out long messageId) || messageId <= 0)
         {
            MessageBox.Show(L("Enter the id of the message, as the listing shows it."), L("Control Panel"));
            return;
         }

         if (MessageBox.Show(F("Put message {0} back into the mailbox of {1}?", messageId, address), L("Control Panel"),
                             MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;

         try
         {
            dynamic manager = ServerSession.Current.Application.BackupManager;
            int job = (int)manager.RestoreMessage(archive, address, messageId, ArchivedMessageFolderBox.Text.Trim(), MailboxPassphraseBox.Password ?? "");
            lastMailboxRestoreJob_ = job;
            string status = (string)manager.MailboxRestoreStatus(job);
            ServerSession.Release(manager);

            ShowMailboxRestoreOutcome_(status);
            ShowNotice_(StatusLevel.Good, F("Message restore {0} started - runs in the background on the server.", job));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            MessageBox.Show(F("Could not start the message restore: {0}", ex.Message), L("Control Panel"));
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
            MessageBox.Show(F("Could not write to hMailServer.ini: {0}", ex.Message), L("Control Panel"));
            return;
         }

         // Show what was actually stored, normalised (an empty count box was 0).
         ScheduleTimeBox.Text = time;
         IntervalBox.Text = intervalMinutes.ToString(CultureInfo.InvariantCulture);
         KeepCountBox.Text = keepCount.ToString(CultureInfo.InvariantCulture);
         MaxAgeBox.Text = maxAgeDays.ToString(CultureInfo.InvariantCulture);

         ShowNotice_(StatusLevel.Good, L("Backup schedule saved - the service reads these settings when it starts, so they apply after a service restart."));
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
            scheduleText = L("Cannot tell whether a schedule is configured: hMailServer.ini was not found on this machine. Check ScheduledBackupTime and ScheduledBackupIntervalMinutes on the server itself.");
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
               scheduleText = F("ScheduledBackupTime '{0}' is not a valid 24-hour HH:MM time, so the server falls back to the interval: one backup every {1} minute(s).", time, intervalMinutes);
            }
            else if (time.Length > 0)
            {
               scheduleLevel = StatusLevel.Critical;
               scheduleText = F("ScheduledBackupTime '{0}' is not a valid 24-hour HH:MM time and no interval is set, so no scheduled backup will ever run.", time);
            }
            else if (intervalMinutes > 0)
            {
               scheduleConfigured = true;
               expectedMinutes = intervalMinutes;
               scheduleLevel = StatusLevel.Good;
               scheduleText = F("A schedule is configured: one backup every {0} minute(s).", intervalMinutes);
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
                  .Select(name => (Name: name, IsArchive: TryParseArchiveName(name, out DateTime created), Created: created))
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
               lastText = F("Cannot tell when the last backup ran: {0} could not be read from this machine. Check the folder on the server, or in the backup log on the Logs page.", destination);
            }
            else if (newestName == null)
            {
               lastLevel = scheduleConfigured ? StatusLevel.Critical : StatusLevel.Warning;
               lastText = F("No backup has ever completed into {0} - there is no HMBackup archive there.", destination) +
                          (scheduleConfigured
                             ? L(" If the schedule was only just set up, re-check after its first run was due; otherwise the backup log on the Logs page says why runs are being skipped.")
                             : "");
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
                  lastText = described + L(" That is older than the schedule allows, so runs are failing or being skipped - the backup log on the Logs page says why.");
               }
               else if (scheduleConfigured)
               {
                  lastLevel = StatusLevel.Good;
                  lastText = described;
               }
               else
               {
                  lastLevel = StatusLevel.Information;
                  lastText = described + L(" With no schedule configured, nothing will take the next one.");
               }
            }
         }

         ApplyStatus(LastBackupNotice, lastLevel, lastText);
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
      /// Mirrors BackupRetention::ParseArchiveName: "HMBackup YYYY-MM-DD HHMMSS.7z",
      /// 29 characters, case-insensitive prefix and extension, digits in the digit
      /// positions, and a date that actually exists. The name is generated in
      /// exactly one place (BackupExecuter::StartBackup) and parsed by the server's
      /// own retention and scheduling code with these same rules - if the format
      /// ever changes, all of them change together.
      /// </summary>
      private static bool TryParseArchiveName(string fileName, out DateTime created)
      {
         created = DateTime.MinValue;

         if (fileName == null || fileName.Length != 29)
            return false;

         if (!fileName.StartsWith("HMBackup ", StringComparison.OrdinalIgnoreCase))
            return false;

         if (!fileName.EndsWith(".7z", StringComparison.OrdinalIgnoreCase))
            return false;

         if (fileName[13] != '-' || fileName[16] != '-' || fileName[19] != ' ')
            return false;

         int[] dateDigitOffsets = { 9, 10, 11, 12, 14, 15, 17, 18 };
         if (!dateDigitOffsets.All(offset => char.IsAsciiDigit(fileName[offset])))
            return false;

         for (int offset = 20; offset <= 25; offset++)
            if (!char.IsAsciiDigit(fileName[offset]))
               return false;

         int year = int.Parse(fileName.Substring(9, 4), CultureInfo.InvariantCulture);
         int month = int.Parse(fileName.Substring(14, 2), CultureInfo.InvariantCulture);
         int day = int.Parse(fileName.Substring(17, 2), CultureInfo.InvariantCulture);
         int hour = int.Parse(fileName.Substring(20, 2), CultureInfo.InvariantCulture);
         int minute = int.Parse(fileName.Substring(22, 2), CultureInfo.InvariantCulture);
         int second = int.Parse(fileName.Substring(24, 2), CultureInfo.InvariantCulture);

         try
         {
            // The archive name carries the server's local time.
            created = new DateTime(year, month, day, hour, minute, second, DateTimeKind.Local);
         }
         catch (ArgumentOutOfRangeException)
         {
            // 2026-02-30 and friends: digits in the right places that do not
            // describe a real instant are not a name the server wrote.
            return false;
         }

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
            return minutes == 1 ? L("1 minute ago") : F("{0} minutes ago", minutes);
         }

         if (age.TotalDays < 2)
         {
            int hours = (int)age.TotalHours;
            return hours == 1 ? L("1 hour ago") : F("{0} hours ago", hours);
         }

         return F("{0} days ago", (int)age.TotalDays);
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
