// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    An un-ban AutoBanCommand was not run for to an answer is asked for again.
   ///
   ///    A reconciliation runs a command that has run past ExternalProcessTimeout no
   ///    more in the same pass (a widened ban is up to 65 addresses, under the lock
   ///    every ban takes). Until 6.3.6 an un-ban it was therefore not run for, and
   ///    the one it was stopped in, were forgotten: with iptables or a command of
   ///    the administrator's - nothing else lifts such a block - those addresses
   ///    stayed blocked below the server for good. They are owed now, and asked for
   ///    by the next pass; one that runs past the timeout again waits a minute, then
   ///    longer, so that a hook which hangs for good costs one timeout an hour and
   ///    not one a minute.
   ///
   ///    The bans are auto-ban ranges the fixture writes itself, on 198.18.72.0/24,
   ///    which nothing on the bench uses. A range saved starts no reconciliation and
   ///    a range deleted does, so the fixture decides when one runs: it saves the
   ///    changes it wants and then deletes a range of its own. The hook logs what it
   ///    is told and, while a marker file exists, hangs on "unban" until the
   ///    one-second timeout stops it. Both servers: cmd on Windows, sh elsewhere.
   /// </summary>
   [TestFixture]
   public class AutoBanHookRetry : TestFixtureBase
   {
      private const string Net = "198.18.72.";
      private const string BanPrefix = "Auto-ban: hook retry ";
      private const string TriggerName = "hook retry trigger";

      private string folder_;
      private string log_;
      private string hang_;

      [SetUp]
      public void WriteTheHook()
      {
         TakeBackOurs();

         folder_ = Path.Combine(Path.GetTempPath(), "hmailserver-autoban-hook-retry");
         Directory.CreateDirectory(folder_);
         log_ = Path.Combine(folder_, "hook.log");
         hang_ = Path.Combine(folder_, "hang");
         File.Delete(log_);
         File.Delete(hang_);

         string command;
         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
         {
            string script = Path.Combine(folder_, "hook.cmd");
            File.WriteAllText(script,
               "@echo %* >> \"" + log_ + "\"\r\n" +
               "@if /i \"%1\"==\"unban\" if exist \"" + hang_ + "\" goto spin\r\n" +
               "@exit /b 0\r\n" +
               ":spin\r\n" +
               "@goto spin\r\n");
            command = "cmd.exe /c \"" + script + "\"";
         }
         else
         {
            // exec, so that the process the timeout stops is the one that waits.
            string script = Path.Combine(folder_, "hook.sh");
            File.WriteAllText(script,
               "#!/bin/sh\n" +
               "echo \"$*\" >> '" + log_ + "'\n" +
               "if [ \"$1\" = unban ] && [ -e '" + hang_ + "' ]; then exec sleep 30; fi\n" +
               "exit 0\n");
            command = "/bin/sh " + script;
         }

         IniFileSetting.Write("ExternalProcessTimeout", "1");
         IniFileSetting.Write("AutoBanCommand", command);
      }

      [TearDown]
      public void TakeTheHookBack()
      {
         // The command first, so that taking the bans back does not run it.
         IniFileSetting.Delete("AutoBanCommand");
         IniFileSetting.Delete("ExternalProcessTimeout");
         TakeBackOurs();
         LogHandler.DeleteErrorLog();

         try
         {
            Directory.Delete(folder_, true);
         }
         catch (IOException)
         {
            // The service may still hold the log for a moment; it is temp space.
         }
         catch (UnauthorizedAccessException)
         {
            // Likewise.
         }
      }

      [Test]
      [Description("An un-ban the hook was not run for because it ran past ExternalProcessTimeout is asked for again: at the next reconciliation, then - while it still hangs - not before a minute has passed, and once the hook answers, every address owed is un-banned.")]
      public void AnUnbanAHangingHookWasNotRunForIsAskedForAgain()
      {
         hMailServer.SecurityRange first = AddBan(Net + "1");
         hMailServer.SecurityRange second = AddBan(Net + "2");

         Reconcile();
         string told = WaitForLog(l => Count(l, "ban " + Net + "1 ") >= 1 && Count(l, "ban " + Net + "2 ") >= 1, TimeSpan.FromSeconds(20));
         ClassicAssert.AreEqual(0, Count(told, "unban " + Net), "Nothing was un-banned yet. Hook log: " + told);

         // Both bans go in one reconciliation, with the hook hanging on un-ban: the
         // first is stopped at the timeout and the second is not run at all.
         File.WriteAllText(hang_, "hang");
         Lift(first);
         Lift(second);
         Reconcile();

         // The expiry pass, once a minute, may be one of these reconciliations: the
         // counts below hold whichever runs them.
         string hung = WaitForLog(l => Count(l, "unban " + Net + "1") >= 1, TimeSpan.FromSeconds(20));
         ClassicAssert.GreaterOrEqual(Count(hung, "unban " + Net + "1"), 1, "The first un-ban was not run. Hook log: " + hung);
         ClassicAssert.AreEqual(0, Count(hung, "unban " + Net + "2"), "The pass went on running a command that had run past the timeout. Hook log: " + hung);

         // The next reconciliation asks again. Until 6.3.6 the first un-ban was
         // forgotten here and the second never run.
         Reconcile();
         string again = WaitForLog(l => Count(l, "unban " + Net + "1") >= 2, TimeSpan.FromSeconds(20));
         ClassicAssert.GreaterOrEqual(Count(again, "unban " + Net + "1"), 2,
            "An un-ban stopped at the timeout was not asked for again by the next reconciliation: the address stays blocked below the server. Hook log: " + again);

         // Run past the timeout a second time, the owed un-bans wait a minute: a
         // reconciliation now runs nothing, so a hook that hangs for good is not
         // run at every pass under the lock every ban takes.
         int retried = Count(again, "unban " + Net + "1");
         Reconcile();
         Thread.Sleep(TimeSpan.FromSeconds(3));
         string waiting = ReadLog();
         ClassicAssert.AreEqual(retried, Count(waiting, "unban " + Net + "1"), "A hook that had run past the timeout twice was run again at once. Hook log: " + waiting);
         ClassicAssert.AreEqual(0, Count(waiting, "unban " + Net + "2"), "Hook log: " + waiting);

         // The hook answers again: once the wait is over, every address owed is
         // un-banned - a reconciliation every ten seconds, the expiry pass's among
         // them, until both are.
         File.Delete(hang_);
         string done = WaitForLog(l => Count(l, "unban " + Net + "2") >= 1 && Count(l, "unban " + Net + "1") > retried,
            TimeSpan.FromSeconds(150), TimeSpan.FromSeconds(10));
         StringAssert.Contains("unban " + Net + "2", done, "The un-ban the hanging hook was not run for was never asked for. Hook log: " + done);
         ClassicAssert.Greater(Count(done, "unban " + Net + "1"), retried, "Hook log: " + done);

         // Owed no more: a later reconciliation runs nothing for either.
         int settled = Count(done, "unban " + Net);
         Reconcile();
         Thread.Sleep(TimeSpan.FromSeconds(2));
         string after = ReadLog();
         ClassicAssert.AreEqual(settled, Count(after, "unban " + Net), "An un-ban that had run was asked for again. Hook log: " + after);
      }

      // ------------------------------------------------------------------ helpers

      private hMailServer.SecurityRange AddBan(string address)
      {
         hMailServer.SecurityRange range = _settings.SecurityRanges.Add();
         range.Name = BanPrefix + address;
         range.LowerIP = address;
         range.UpperIP = address;
         range.Priority = 20;
         range.AllowSMTPConnections = false;
         range.AllowIMAPConnections = false;
         range.AllowPOP3Connections = false;
         range.Expires = true;
         range.ExpiresTime = DateTime.Now.AddMinutes(30);
         range.Save();
         return range;
      }

      /// <summary>
      ///    The ban is lifted without a reconciliation: the range stops being one
      ///    (no expiry, so no auto-ban), which a save does not announce.
      /// </summary>
      private static void Lift(hMailServer.SecurityRange range)
      {
         range.Expires = false;
         range.Save();
      }

      /// <summary>A reconciliation, now: a range of the fixture's own made and deleted.</summary>
      private void Reconcile()
      {
         hMailServer.SecurityRange trigger = _settings.SecurityRanges.Add();
         trigger.Name = TriggerName;
         trigger.LowerIP = Net + "250";
         trigger.UpperIP = Net + "250";
         trigger.Priority = 5;
         trigger.Save();
         trigger.Delete();
      }

      private string ReadLog()
      {
         if (!File.Exists(log_))
            return "";

         using (var stream = new FileStream(log_, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
         using (var reader = new StreamReader(stream))
            return reader.ReadToEnd();
      }

      /// <summary>
      ///    The hook's log once it satisfies the condition, or as it is at the
      ///    deadline; with a period, a reconciliation of the fixture's own each time.
      /// </summary>
      private string WaitForLog(Func<string, bool> condition, TimeSpan deadline, TimeSpan? period = null)
      {
         DateTime until = DateTime.UtcNow + deadline;
         DateTime nextReconcile = period.HasValue ? DateTime.UtcNow + period.Value : DateTime.MaxValue;

         while (true)
         {
            string content = ReadLog();
            if (condition(content) || DateTime.UtcNow > until)
               return content;

            if (DateTime.UtcNow > nextReconcile)
            {
               Reconcile();
               nextReconcile = DateTime.UtcNow + period.Value;
            }

            Thread.Sleep(250);
         }
      }

      /// <summary>How many lines of the hook's log begin with the text.</summary>
      private static int Count(string log, string start)
      {
         int count = 0;
         foreach (string line in log.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries))
         {
            if (line.TrimStart().StartsWith(start, StringComparison.Ordinal))
               count++;
         }
         return count;
      }

      /// <summary>The fixture's bans and its trigger range, whatever state a test left them in.</summary>
      private void TakeBackOurs()
      {
         _settings.SecurityRanges.Refresh();

         for (int i = _settings.SecurityRanges.Count - 1; i >= 0; i--)
         {
            hMailServer.SecurityRange range = _settings.SecurityRanges[i];
            if (range.Name.StartsWith(BanPrefix, StringComparison.Ordinal) || range.Name == TriggerName)
               range.Delete();
         }
      }
   }
}
