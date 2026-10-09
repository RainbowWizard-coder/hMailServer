// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// What the Backup &amp; restore page reads and says about archives and restore
   /// jobs, without WPF, so the tests hold it (review finding F257).
   ///
   /// The page put every outcome in one notice above its first card, a screen or more
   /// above the buttons that caused it; it printed a mailbox restore's state as the
   /// server's raw token ("Job 7 is running: …"), and the server's JSON itself when it
   /// could not read it; and its restores wanted an archive's path or name and a
   /// message's id typed by hand, copied from a monospace text box. Here are the three
   /// things the page now reads instead: the archives in the destination, named as the
   /// server names them and newest first; a restore job's outcome as a level and a
   /// sentence; and the messages of an account in an archive as rows to choose from.
   /// </summary>
   public static class BackupRestoreReport
   {
      /// <summary>One archive in the backup destination.</summary>
      public sealed record Archive(string Name, string Path, DateTime Taken, long Size)
      {
         /// <summary>When it was taken, in the reader's own date format; the name carries the server's local time.</summary>
         public string TakenText => Taken.ToString("g", CultureInfo.CurrentCulture);

         /// <summary>Its size in megabytes, to one place, in the reader's own number format.</summary>
         public string SizeText => Megabytes(Size);

         /// <summary>What a screen reader says for the row: the archive's name, not this record's.</summary>
         public override string ToString() => Name + ", " + TakenText;
      }

      /// <summary>
      /// Mirrors BackupRetention::ParseArchiveName: "HMBackup YYYY-MM-DD HHMMSS.7z",
      /// 29 characters, case-insensitive prefix and extension, digits in the digit
      /// positions, and a date that actually exists. The name is generated in exactly
      /// one place (BackupExecuter::StartBackup) and parsed by the server's own
      /// retention, scheduling and archive listing with these same rules - if the
      /// format ever changes, all of them change together.
      /// </summary>
      public static bool TryParseArchiveName(string fileName, out DateTime created)
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
         {
            if (!char.IsAsciiDigit(fileName[offset]))
               return false;
         }

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
      /// The archives among a folder's files, newest first, as the server's own
      /// listing (GET /api/v1/backup/archives) answers them: only names this server
      /// generates. An administrator's own copies in the same folder are not listed.
      /// </summary>
      public static List<Archive> Archives(string destination, IEnumerable<(string Name, long Size)> files)
      {
         var archives = new List<Archive>();
         if (files == null)
            return archives;

         string folder = (destination ?? "").TrimEnd('\\', '/');
         foreach ((string name, long size) in files)
         {
            if (TryParseArchiveName(name, out DateTime taken))
               archives.Add(new Archive(name, folder + "\\" + name, taken, size));
         }

         return archives.OrderByDescending(a => a.Taken).ThenBy(a => a.Name, StringComparer.OrdinalIgnoreCase).ToList();
      }

      /// <summary>A size in megabytes to one place, never "0.0" for a file that has bytes in it.</summary>
      public static string Megabytes(long bytes)
      {
         if (bytes <= 0)
            return 0.0.ToString("N1", CultureInfo.CurrentCulture);

         double megabytes = Math.Max(0.1, bytes / (1024.0 * 1024.0));
         return megabytes.ToString("N1", CultureInfo.CurrentCulture);
      }

      /// <summary>A restore job's outcome: its level, the sentence that says it, and the job's number (0 when unknown).</summary>
      public sealed record JobOutcome(StatusLevel Level, string Text, int Job);

      /// <summary>
      /// A mailbox or one-message restore job, from the JSON MailboxRestoreStatus
      /// answers: queued and running at Information, done at Good - or at Warning when
      /// some messages could not be put back - and failed at Critical with the
      /// server's reason. The server's state tokens and its JSON are never shown.
      /// </summary>
      public static JobOutcome Job(string json)
      {
         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
               return Unreadable_();

            int job = (int)Number_(root, "job");
            long folders = Number_(root, "folders");
            long added = Number_(root, "added");
            long skipped = Number_(root, "skipped");
            long failed = Number_(root, "failed");
            string error = Text_(root, "error").Trim();

            switch (Text_(root, "state").Trim().ToLowerInvariant())
            {
               case "queued":
                  return new JobOutcome(StatusLevel.Information,
                     F("Restore job {0} is queued: it starts as soon as no backup or other restore is running.", job), job);

               case "running":
                  return new JobOutcome(StatusLevel.Information,
                     F("Restore job {0} is running. So far - folders: {1}; messages added: {2}, skipped as still in the mailbox: {3}, failed: {4}.", job, folders, added, skipped, failed), job);

               case "done":
                  return failed > 0
                     ? new JobOutcome(StatusLevel.Warning,
                        F("Restore job {0} is done, but some messages could not be put back. Folders: {1}; messages added: {2}, skipped as still in the mailbox: {3}, failed: {4}. The backup log says why.", job, folders, added, skipped, failed), job)
                     : new JobOutcome(StatusLevel.Good,
                        F("Restore job {0} is done. Folders: {1}; messages added: {2}, skipped as still in the mailbox: {3}.", job, folders, added, skipped), job);

               case "failed":
                  return new JobOutcome(StatusLevel.Critical, error.Length > 0
                     ? F("Restore job {0} failed: {1}", job, error)
                     : F("Restore job {0} failed. The backup log says why.", job), job);

               default:
                  return new JobOutcome(StatusLevel.Warning,
                     F("Restore job {0} reports a state this Control Panel does not know: {1}.", job, Text_(root, "state")), job);
            }
         }
         catch (JsonException)
         {
            return Unreadable_();
         }
      }

      private static JobOutcome Unreadable_()
         => new(StatusLevel.Critical, L("The server's answer about the restore could not be read."), 0);

      /// <summary>One message of an account as an archive holds it.</summary>
      public sealed record ArchivedMessage(long Id, string Date, string Folder, string From, string Subject)
      {
         /// <summary>What a screen reader says for the row.</summary>
         public override string ToString() => string.Join(", ", new[] { Subject, From, Folder, Date }.Where(s => !string.IsNullOrWhiteSpace(s)));
      }

      /// <summary>
      /// What Put the message back asks before it puts <paramref name="message"/> back
      /// into the mailbox of <paramref name="address"/>: the message named as the grid
      /// shows it - its subject, or its sender where the subject is empty or was not
      /// read (the server reads subjects for a listing of at most 500 messages), then
      /// its folder and its date - where the question named it by its id, a number
      /// the grid does not show (review finding F257). A message with neither is
      /// named by its date and folder alone.
      /// </summary>
      public static string PutBackQuestion(ArchivedMessage message, string address)
      {
         string name = !string.IsNullOrWhiteSpace(message.Subject) ? message.Subject : message.From;
         return string.IsNullOrWhiteSpace(name)
            ? F("Put the message dated {0} in {1} back into the mailbox of {2}?", message.Date, message.Folder, address)
            : F("Put \"{0}\" ({1}, {2}) back into the mailbox of {3}?", name, message.Folder, message.Date, address);
      }

      /// <summary>
      /// The messages of an account in an archive, from the JSON DescribeMailbox
      /// answers, in the order the archive lists them; <paramref name="note"/> is the
      /// server's note on the subjects (read for at most 500 messages), or "". Null,
      /// with <paramref name="error"/>, when the answer cannot be read.
      /// </summary>
      public static List<ArchivedMessage> ArchivedMessages(string json, out string note, out string error)
      {
         note = "";
         error = null;
         var messages = new List<ArchivedMessage>();

         try
         {
            using JsonDocument document = JsonDocument.Parse(json ?? "");
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
               error = L("The server's answer about the archive could not be read.");
               return null;
            }

            if (root.TryGetProperty("folders", out JsonElement folders) && folders.ValueKind == JsonValueKind.Array)
            {
               foreach (JsonElement folder in folders.EnumerateArray())
               {
                  if (!folder.TryGetProperty("messages", out JsonElement list) || list.ValueKind != JsonValueKind.Array)
                     continue;

                  foreach (JsonElement message in list.EnumerateArray())
                  {
                     messages.Add(new ArchivedMessage(Number_(message, "id"), LocalDate(Text_(message, "date")),
                        Text_(message, "folder"), Text_(message, "from"), Text_(message, "subject")));
                  }
               }
            }

            note = Text_(root, "subjects_note");
            return messages;
         }
         catch (JsonException)
         {
            error = L("The server's answer about the archive could not be read.");
            return null;
         }
      }

      /// <summary>
      /// A date the server writes as "yyyy-MM-dd HH:mm:ss" in its own local time, in
      /// the reader's format; anything else as it came.
      /// </summary>
      public static string LocalDate(string serverDate)
      {
         string text = (serverDate ?? "").Trim();
         return DateTime.TryParseExact(text, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime parsed)
            ? parsed.ToString("g", CultureInfo.CurrentCulture)
            : text;
      }

      private static string Text_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static long Number_(JsonElement element, string name)
         => element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out long number) ? number : 0;
   }
}
