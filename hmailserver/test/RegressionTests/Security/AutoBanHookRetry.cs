// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    An un-ban AutoBanCommand was not run for to an answer is asked for again,
   ///    and one a hook still owes when the service stops is asked for after it
   ///    starts.
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
   ///    What a hook holds was also in memory only, so a start forgot it: an un-ban
   ///    owed when the service stopped, or a ban that ended while it was stopped,
   ///    was never asked of the hook. Since 6.3.6 AutoBanCommand's account is kept
   ///    in a ledger in the temp folder (autoban-command.ledger) and read at a
   ///    start, and the packaged Linux hook is told "reset" at a start, before the
   ///    bans in force are told to it again (AutoBanFirewall::SynchroniseAtStart).
   ///    A start here is RestartServerAndReacquireCom: the service restarted on
   ///    Windows, a reinitialise on Linux, which runs the same start-up pass and
   ///    forgets what the process knew of the hooks as a new process would.
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
      private const string LedgerHeader = "hMailServer auto-ban command ledger 1";

      private string folder_;
      private string log_;
      private string hang_;
      private string command_;

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

         command_ = WriteHook("hook", log_);

         // A ledger a test before this one left - planted, or written by a server
         // that did not take it back - is not this test's. Removed before the
         // command is set, so the server has written nothing for it yet.
         File.Delete(LedgerPath());

         IniFileSetting.Write("ExternalProcessTimeout", "1");
         IniFileSetting.Write("AutoBanCommand", command_);
      }

      [TearDown]
      public void TakeTheHookBack()
      {
         // The command first, so that taking the bans back does not run it.
         IniFileSetting.Delete("AutoBanCommand");
         IniFileSetting.Delete("ExternalProcessTimeout");
         TakeBackOurs();

         // And a reconciliation with no command, now: one that finds the command
         // cleared while it held an address names those addresses in the error
         // log (HM6523), which is for this fixture to clear, not the next one's.
         Reconcile();

         // And the ledger, whoever wrote it. That reconciliation removes the
         // server's own, but one a test planted for a server that reads no
         // ledger - a build before 6.3.6, a negative control - is still there,
         // and the next start of a server that reads one would name its
         // addresses in the error log, under whatever test came first: on
         // bench150 that was Security.AutoBan's SetUp, after a run against
         // 8f2dec98b had left 198.18.72.12 and .13 in the temp folder.
         File.Delete(LedgerPath());
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

      [Test]
      [Description("What AutoBanCommand is told to ban and not yet to un-ban - an un-ban owed after a timeout included - is kept in its ledger, autoban-command.ledger in the temp folder, under the SHA-256 of the command line and never its text; an address leaves it once its un-ban has run.")]
      public void WhatTheCommandHoldsIsKeptInItsLedger()
      {
         hMailServer.SecurityRange ban = AddBan(Net + "21");
         Reconcile();
         WaitForLog(l => Count(l, "ban " + Net + "21 ") >= 1, TimeSpan.FromSeconds(20));

         string[] held = WaitForLedger(lines => lines.Contains(Net + "21"), TimeSpan.FromSeconds(10));
         ClassicAssert.GreaterOrEqual(held.Length, 3, "The command's ledger, " + LedgerPath() + ", does not hold the address it was told to ban: [" + string.Join(" | ", held) + "]");
         ClassicAssert.AreEqual(LedgerHeader, held[0], "The ledger's first line. Ledger: [" + string.Join(" | ", held) + "]");
         ClassicAssert.AreEqual("command " + Digest(command_), held[1], "The ledger does not name the command by the SHA-256 of its command line. Ledger: [" + string.Join(" | ", held) + "]");
         CollectionAssert.Contains(held, Net + "21");
         StringAssert.DoesNotContain(folder_, string.Join("\n", held), "The ledger carries the command line's text, which can hold a credential.");

         // The un-ban runs past the timeout: owed, and still held.
         File.WriteAllText(hang_, "hang");
         Lift(ban);
         Reconcile();
         WaitForLog(l => Count(l, "unban " + Net + "21") >= 1, TimeSpan.FromSeconds(20));
         Thread.Sleep(TimeSpan.FromSeconds(2));
         CollectionAssert.Contains(LedgerLines(), Net + "21", "An un-ban owed after a timeout left the ledger: a stop now would forget it.");

         // Answered: gone from the ledger.
         File.Delete(hang_);
         string[] after = WaitForLedger(lines => lines.Length >= 2 && !lines.Contains(Net + "21"), TimeSpan.FromSeconds(150), TimeSpan.FromSeconds(10));
         CollectionAssert.DoesNotContain(after, Net + "21", "An address whose un-ban ran is still in the ledger. Hook log: " + ReadLog());
      }

      [Test]
      [Description("An address AutoBanCommand was left holding by an earlier run - a ban that ended while the service was stopped, an un-ban still owed when it stopped - is un-banned at the next start, from the command's ledger; a ban still in force is announced again, and not un-banned.")]
      public void WhatTheCommandWasLeftHoldingIsUnbannedAtTheNextStart()
      {
         AddBan(Net + "13");
         Reconcile();
         WaitForLog(l => Count(l, "ban " + Net + "13 ") >= 1, TimeSpan.FromSeconds(20));
         WaitForLedger(lines => lines.Contains(Net + "13"), TimeSpan.FromSeconds(10));

         // What the run before this start left: .12 told "ban" and never "unban".
         // A reconciliation of the running server finds nothing to change - it
         // holds .13, which is still banned - so nothing writes over this before
         // the start reads it.
         WriteLedger(Digest(command_), Net + "12", Net + "13");

         RestartServerAndReacquireCom();

         string log = WaitForLog(l => Count(l, "unban " + Net + "12") >= 1 && Count(l, "ban " + Net + "13 ") >= 2, TimeSpan.FromSeconds(30));
         ClassicAssert.GreaterOrEqual(Count(log, "unban " + Net + "12"), 1,
            "The start did not un-ban the address the command was left holding: it stays blocked below the server for good. Hook log: " + log);
         ClassicAssert.AreEqual(0, Count(log, "unban " + Net + "13"), "A ban still in force was un-banned at the start. Hook log: " + log);
         ClassicAssert.GreaterOrEqual(Count(log, "ban " + Net + "13 "), 2, "A ban in force was not announced again at the start. Hook log: " + log);

         string[] ledger = WaitForLedger(lines => !lines.Contains(Net + "12"), TimeSpan.FromSeconds(10));
         CollectionAssert.DoesNotContain(ledger, Net + "12", "The address un-banned at the start is still in the ledger.");
         CollectionAssert.Contains(ledger, Net + "13", "The ban in force left the ledger.");
      }

      [Test]
      [Description("A ledger written for another command - AutoBanCommand changed while the service was stopped - is not acted on with the command configured now: its addresses are named in the error log, for the administrator to lift where that command put them.")]
      public void AnotherCommandsLedgerIsNamedNotRun()
      {
         WriteLedger(Digest("/opt/an-earlier/firewall-command"), Net + "31");

         RestartServerAndReacquireCom();

         CustomAsserts.AssertReportedError("not the one configured now", Net + "31");
         Reconcile();
         Thread.Sleep(TimeSpan.FromSeconds(2));
         ClassicAssert.AreEqual(0, Count(ReadLog(), "unban " + Net + "31"), "The command configured now was run for an address another command was told to ban. Hook log: " + ReadLog());
      }

      [Test]
      [Description("AutoBanCommand replaced while the service runs: the new command is told every ban in force, and what the old one was told and not taken back is named in the error log - the old command is not run again.")]
      public void ACommandReplacedLeavesItsAddressesNamedAndTheNewOneIsToldEveryBan()
      {
         AddBan(Net + "51");
         Reconcile();
         WaitForLog(l => Count(l, "ban " + Net + "51 ") >= 1, TimeSpan.FromSeconds(20));

         string secondLog = Path.Combine(folder_, "second.log");
         IniFileSetting.Write("AutoBanCommand", WriteHook("second", secondLog));
         Reconcile();

         CustomAsserts.AssertReportedError("since replaced or cleared", Net + "51");
         string second = WaitForFile(secondLog, l => Count(l, "ban " + Net + "51 ") >= 1, TimeSpan.FromSeconds(20));
         ClassicAssert.GreaterOrEqual(Count(second, "ban " + Net + "51 "), 1, "The new command was not told the ban in force. Its log: " + second);
         ClassicAssert.AreEqual(0, Count(ReadLog(), "unban " + Net + "51"), "The replaced command was run again. Its log: " + ReadLog());
      }

      [Test]
      [Description("A reconciliation that cannot read the IP ranges changes nothing below the server: the command is not told to un-ban a ban in force because the read failed. Until 6.3.6 a failed read was taken for no bans at all, and every address was un-banned below the server - every Windows rule removed too - until a later pass put them back.")]
      public void APassThatCannotReadTheRangesLiftsNothing()
      {
         AddBan(Net + "61");
         Reconcile();
         WaitForLog(l => Count(l, "ban " + Net + "61 ") >= 1, TimeSpan.FromSeconds(20));

         // The ranges made unreadable for one expiry pass: their table renamed.
         // The pass first fails to delete the expired ranges, which it reports
         // (HM6112), and then reconciles. It is due a minute after the last, and
         // the scheduler wakes a minute after its last round, so a pass due a
         // moment after a round waits for the next: up to two minutes apart.
         LogHandler.DeleteErrorLog();
         RenameRangesTable(RangesTable, RangesTableAside);
         string errors;
         try
         {
            DateTime until = DateTime.UtcNow + TimeSpan.FromSeconds(150);
            while (true)
            {
               errors = File.Exists(LogHandler.GetErrorLogFileName()) ? LogHandler.ReadErrorLog() : "";
               if (errors.Contains(ExpiryPassFailed) || DateTime.UtcNow > until)
                  break;
               Thread.Sleep(500);
            }

            // The reconciliation follows the report at once; a hook it ran
            // would have written its line within this.
            Thread.Sleep(TimeSpan.FromSeconds(5));
         }
         finally
         {
            RenameRangesTable(RangesTableAside, RangesTable);
         }

         StringAssert.Contains(ExpiryPassFailed, errors, "No expiry pass ran while the IP ranges could not be read, so this test proved nothing. Error log: " + errors);
         string log = ReadLog();
         ClassicAssert.AreEqual(0, Count(log, "unban " + Net + "61"),
            "A reconciliation that could not read the IP ranges told the command to un-ban a ban in force: a database that falters lifts every ban below the server. Hook log: " + log);

         LogHandler.DeleteErrorLog();
      }

      [Test]
      [Description("Linux: the packaged hook is told 'reset' at a start, before the bans in force are told to it again, and when AutoBanFirewall is switched off; and at a start with the setting off, wherever the hook is installed. Under iptables nothing else lifts a ban that ended while the service was stopped.")]
      public void ThePackagedHookIsResetAtAStartAndWhenSwitchedOff()
      {
         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
            Assert.Ignore("The packaged hook is the Linux server's; Windows' rules are enumerated at a start (AutoBanFirewall).");

         string packagedLog = Path.Combine(folder_, "packaged.log");
         string hook = PutPackagedStandIn(packagedLog);

         try
         {
            AddBan(Net + "41");
            IniFileSetting.Write("AutoBanFirewall", "1");

            RestartServerAndReacquireCom();

            string started = WaitForFile(packagedLog, l => ResetThenBan(l, Net + "41"), TimeSpan.FromSeconds(30));
            ClassicAssert.IsTrue(ResetThenBan(started, Net + "41"),
               "The packaged hook was not told 'reset' at the start and then the ban in force: a ban that ended while the service was stopped stays in its iptables chain. Its log: " + started);

            int resets = Count(started, "reset");
            IniFileSetting.Delete("AutoBanFirewall");
            Reconcile();
            string off = WaitForFile(packagedLog, l => Count(l, "reset") > resets, TimeSpan.FromSeconds(20));
            ClassicAssert.Greater(Count(off, "reset"), resets, "Switching AutoBanFirewall off did not reset the packaged hook: what it blocked stays. Its log: " + off);

            // Off, at a start: reset, wherever it is installed, and nothing told.
            int bans = Count(off, "ban ");
            resets = Count(off, "reset");
            RestartServerAndReacquireCom();
            string offStart = WaitForFile(packagedLog, l => Count(l, "reset") > resets, TimeSpan.FromSeconds(30));
            ClassicAssert.Greater(Count(offStart, "reset"), resets, "A start with AutoBanFirewall off did not reset the installed hook. Its log: " + offStart);
            ClassicAssert.AreEqual(bans, Count(offStart, "ban "), "A hook switched off was told a ban. Its log: " + offStart);
         }
         finally
         {
            IniFileSetting.Delete("AutoBanFirewall");
            TakePackagedStandInBack(hook);
         }
      }

      [Test]
      [Description("Linux: a command that starts the server short of its listeners, run beside a running server - hmailserver --apply-environment, which the container runs at every start, --sample-domain, --apply-profile - leaves what is below that server alone: the packaged hook is not told 'reset', and neither hook is told anything. Run from such a command, the start's reset lifted every ban below the running server until each was told again - and, had the command stopped in between, for the rest of each ban, since the running server believes the hook still holds them and never tells them again.")]
      public void ACommandRunBesideTheServerLeavesWhatIsBelowItAlone()
      {
         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
            Assert.Ignore("The packaged hook, and its reset, are the Linux server's; a command on Windows starts the server through the same InitInstance.");

         string packagedLog = Path.Combine(folder_, "packaged.log");
         string hook = PutPackagedStandIn(packagedLog);

         try
         {
            // The running server holds a ban below itself, through both hooks.
            AddBan(Net + "71");
            IniFileSetting.Write("AutoBanFirewall", "1");
            Reconcile();
            WaitForFile(packagedLog, l => Count(l, "ban " + Net + "71 ") >= 1, TimeSpan.FromSeconds(20));
            WaitForLog(l => Count(l, "ban " + Net + "71 ") >= 1, TimeSpan.FromSeconds(20));

            // Nothing more is owed to either: a pass of the running server, the
            // expiry pass's included, tells neither anything from here on.
            Thread.Sleep(TimeSpan.FromSeconds(2));
            string packagedBefore = ReadFile(packagedLog);
            string commandBefore = ReadLog();
            StringAssert.Contains("ban " + Net + "71 ", packagedBefore, "The packaged hook was never told the ban, so this test proves nothing. Its log: " + packagedBefore);

            (int exitCode, string output) run = RunServerCommand("--apply-environment");
            ClassicAssert.AreEqual(0, run.exitCode, "hmailserver --apply-environment, with no HM_ variable: " + run.output);

            string packagedAfter = ReadFile(packagedLog);
            string commandAfter = ReadLog();
            ClassicAssert.AreEqual(Count(packagedBefore, "reset"), Count(packagedAfter, "reset"),
               "A command run beside the running server told the packaged hook 'reset': every ban below the running server was lifted until the command told it again, which the running server cannot know. Its log: " + packagedAfter);
            ClassicAssert.AreEqual(packagedBefore, packagedAfter, "A command run beside the running server told the packaged hook something. Its log: " + packagedAfter);
            ClassicAssert.AreEqual(commandBefore, commandAfter, "A command run beside the running server told AutoBanCommand something. Its log: " + commandAfter);
         }
         finally
         {
            IniFileSetting.Delete("AutoBanFirewall");
            TakePackagedStandInBack(hook);
         }
      }

      // ------------------------------------------------------------------ helpers

      private const string RangesTable = "hm_securityranges";
      private const string RangesTableAside = "hm_securityranges_hmtest_aside";
      private const string ExpiryPassFailed = "Expired IP ranges could not be removed";

      /// <summary>
      ///    The IP ranges' table renamed, through the server's database on Windows
      ///    (ExecuteSQL) and with psql against the suite's PostgreSQL on Linux,
      ///    whose server answers no SQL over REST. Not on SQL Server Compact,
      ///    which refuses to rename a table a cursor still reads ("Table is in
      ///    use"), and the service reads this one at every connection: a rename
      ///    back that failed would leave the bench with no IP ranges at all.
      /// </summary>
      private void RenameRangesTable(string from, string to)
      {
         if (Environment.OSVersion.Platform != PlatformID.Win32NT)
         {
            string host = Environment.GetEnvironmentVariable("HM_PG_HOST") ?? "postgres";
            string database = Environment.GetEnvironmentVariable("HM_PG_DATABASE") ?? "hmtest";
            var start = new ProcessStartInfo("psql",
               "-h " + host + " -d " + database + " -v ON_ERROR_STOP=1 -Atc \"ALTER TABLE " + from + " RENAME TO " + to + "\"")
            {
               UseShellExecute = false,
               RedirectStandardOutput = true,
               RedirectStandardError = true
            };

            Process psql;
            try
            {
               psql = Process.Start(start);
            }
            catch (System.ComponentModel.Win32Exception e)
            {
               Assert.Ignore("psql, which this test renames the IP ranges' table with on Linux, cannot be run here: " + e.Message);
               return;
            }

            using (psql)
            {
               string said = psql.StandardOutput.ReadToEnd() + psql.StandardError.ReadToEnd();
               psql.WaitForExit();
               if (psql.ExitCode != 0)
               {
                  // Taking the table aside failed: nothing to put back, and the
                  // suite's database is not one this test can reach.
                  if (from == RangesTable)
                     Assert.Ignore("The IP ranges' table cannot be renamed with psql on " + host + "/" + database + " (HM_PG_HOST, HM_PG_DATABASE): " + said);
                  Assert.Fail("The IP ranges' table could not be put back: " + said);
               }
            }
            return;
         }

         hMailServer.Database db = _application.Database;
         switch (db.DatabaseType)
         {
            case hMailServer.eDBtype.hDBTypeMSSQLCE:
               Assert.Ignore("SQL Server Compact refuses to rename a table a cursor still reads, and the service reads this one at every connection: a rename back that failed would leave the bench with no IP ranges. The Linux suite holds this.");
               return;
            case hMailServer.eDBtype.hDBTypeMSSQL:
               db.ExecuteSQL("EXEC sp_rename '" + from + "', '" + to + "'");
               return;
            case hMailServer.eDBtype.hDBTypeMySQL:
               db.ExecuteSQL("RENAME TABLE " + from + " TO " + to);
               return;
            default:
               db.ExecuteSQL("ALTER TABLE " + from + " RENAME TO " + to);
               return;
         }
      }

      /// <summary>
      ///    A hook in the fixture's folder that appends what it is told to the log
      ///    and hangs on "unban" while the marker exists; the command line that
      ///    runs it.
      /// </summary>
      private string WriteHook(string name, string log)
      {
         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
         {
            string script = Path.Combine(folder_, name + ".cmd");
            File.WriteAllText(script,
               "@echo %* >> \"" + log + "\"\r\n" +
               "@if /i \"%1\"==\"unban\" if exist \"" + hang_ + "\" goto spin\r\n" +
               "@exit /b 0\r\n" +
               ":spin\r\n" +
               "@goto spin\r\n");
            return "cmd.exe /c \"" + script + "\"";
         }

         // exec, so that the process the timeout stops is the one that waits.
         string shell = Path.Combine(folder_, name + ".sh");
         File.WriteAllText(shell,
            "#!/bin/sh\n" +
            "echo \"$*\" >> '" + log + "'\n" +
            "if [ \"$1\" = unban ] && [ -e '" + hang_ + "' ]; then exec sleep 30; fi\n" +
            "exit 0\n");
         return "/bin/sh " + shell;
      }

      private static void MakeExecutable(string path)
      {
         using (Process chmod = Process.Start(new ProcessStartInfo("chmod", "755 \"" + path + "\"") { UseShellExecute = false }))
         {
            chmod.WaitForExit();
            if (chmod.ExitCode != 0)
               throw new IOException("chmod 755 " + path + " exited " + chmod.ExitCode);
         }
      }

      /// <summary>
      ///    A stand-in for the packaged hook that appends what it is told to the
      ///    log, where the build installs it and the server runs it from
      ///    (AutoBanFirewall::PackagedHook, CMAKE_INSTALL_PREFIX/lib/hmailserver):
      ///    /usr/lib for the CI's build, which is what this suite runs. Whatever
      ///    was there is kept beside it, for TakePackagedStandInBack.
      /// </summary>
      private static string PutPackagedStandIn(string log)
      {
         string hook = Environment.GetEnvironmentVariable("HMTEST_AUTOBAN_HOOK") ?? "/usr/lib/hmailserver/autoban-hook";
         string saved = hook + ".hmtest-saved";

         try
         {
            Directory.CreateDirectory(Path.GetDirectoryName(hook));
            if (File.Exists(hook) && !File.Exists(saved))
               File.Move(hook, saved);
            File.WriteAllText(hook, "#!/bin/sh\necho \"$*\" >> '" + log + "'\nexit 0\n");
            MakeExecutable(hook);
         }
         catch (Exception e) when (e is UnauthorizedAccessException || e is IOException)
         {
            Assert.Ignore("A stand-in for the packaged hook cannot be put at " + hook + " (" + e.Message + "): run as root, as the CI's suite is, or name another path the server runs it from in HMTEST_AUTOBAN_HOOK.");
         }

         return hook;
      }

      private static void TakePackagedStandInBack(string hook)
      {
         string saved = hook + ".hmtest-saved";
         File.Delete(hook);
         if (File.Exists(saved))
            File.Move(saved, hook);
      }

      /// <summary>
      ///    The server's own program, run as a command beside the running server -
      ///    the binary in the program folder the server reports, with the suite's
      ///    configuration (HMTEST_SERVER_INI) - with no HM_ variable in its
      ///    environment, so that --apply-environment finds nothing to store. One
      ///    that has not finished in three minutes is stopped.
      /// </summary>
      private (int exitCode, string output) RunServerCommand(string arguments)
      {
         string binary = Path.Combine(_settings.Directories.ProgramDirectory, "hmailserver");
         string configuration = Environment.GetEnvironmentVariable("HMTEST_SERVER_INI");
         if (!File.Exists(binary) || string.IsNullOrEmpty(configuration))
            Assert.Ignore("The server's program (" + binary + ") or its configuration (HMTEST_SERVER_INI) is not where the Linux suite puts them, so a command cannot be run beside it here.");

         var start = new ProcessStartInfo(binary, "--config \"" + configuration + "\" " + arguments)
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         using (Process process = Process.Start(start))
         {
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(180 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(arguments + " did not finish within three minutes, and was stopped.");
            }
            process.WaitForExit();
            return (process.ExitCode, output.Result + error.Result);
         }
      }

      /// <summary>A "reset" line, and after it a "ban" of the address.</summary>
      private static bool ResetThenBan(string log, string address)
      {
         string[] lines = log.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries).Select(l => l.Trim()).ToArray();
         int reset = Array.FindIndex(lines, l => l == "reset");
         return reset >= 0 && lines.Skip(reset + 1).Any(l => l.StartsWith("ban " + address + " ", StringComparison.Ordinal));
      }

      private string LedgerPath()
      {
         return Paths.Combine(_application.Settings.Directories.TempDirectory, "autoban-command.ledger");
      }

      /// <summary>What the server records a command by: the SHA-256 of its UTF-8, lower-case hex.</summary>
      private static string Digest(string command)
      {
         using (SHA256 sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(command))).Replace("-", "").ToLowerInvariant();
      }

      private string[] LedgerLines()
      {
         string path = LedgerPath();
         if (!File.Exists(path))
            return new string[0];

         try
         {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new StreamReader(stream))
               return reader.ReadToEnd().Replace("\r", "").Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
         }
         catch (IOException)
         {
            // Being renamed into place this moment.
            return new string[0];
         }
      }

      /// <summary>A ledger as a run of the server writes one, here for a run before this start.</summary>
      private void WriteLedger(string digest, params string[] addresses)
      {
         File.WriteAllText(LedgerPath(), LedgerHeader + "\n" + "command " + digest + "\n" + string.Concat(addresses.Select(a => a + "\n")));
      }

      /// <summary>The ledger's lines once they satisfy the condition, or as they are at the deadline; with a period, a reconciliation of the fixture's own each time.</summary>
      private string[] WaitForLedger(Func<string[], bool> condition, TimeSpan deadline, TimeSpan? period = null)
      {
         DateTime until = DateTime.UtcNow + deadline;
         DateTime nextReconcile = period.HasValue ? DateTime.UtcNow + period.Value : DateTime.MaxValue;

         while (true)
         {
            string[] lines = LedgerLines();
            if (condition(lines) || DateTime.UtcNow > until)
               return lines;

            if (DateTime.UtcNow > nextReconcile)
            {
               Reconcile();
               nextReconcile = DateTime.UtcNow + period.Value;
            }

            Thread.Sleep(250);
         }
      }

      private static string ReadFile(string path)
      {
         if (!File.Exists(path))
            return "";

         using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
         using (var reader = new StreamReader(stream))
            return reader.ReadToEnd();
      }

      private static string WaitForFile(string path, Func<string, bool> condition, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;
         while (true)
         {
            string content = ReadFile(path);
            if (condition(content) || DateTime.UtcNow > until)
               return content;
            Thread.Sleep(250);
         }
      }

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
         return ReadFile(log_);
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
