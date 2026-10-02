// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Security
{
   /// <summary>
   ///    An auto-ban widened to its reverse-name neighbours (GitLab work item #55),
   ///    on the Windows side: Settings.AutoBanWidenByReverseName over COM, the one
   ///    Windows Defender Firewall rule a widened ban becomes, and AutoBanCommand
   ///    told "ban" and "unban" for each address of the run, one at a time, as it
   ///    always was, and a hook that runs past ExternalProcessTimeout not run again
   ///    in the same reconciliation, so that a run costs one timeout, not one per
   ///    address. The behaviour of the walk itself is held over REST and SMTP by
   ///    AutoBanWiden, which both suites run; this is what only Windows has, so it is
   ///    not linked into the Linux suite.
   ///
   ///    The property is reached through dynamic, as AutoBanNeverBanNamesCom reaches
   ///    its own members: the suite's interop is generated from the registered type
   ///    library, and naming it early-bound would not compile against a bench whose
   ///    server predates it.
   ///
   ///    The ban is a spam trap hit from an address of 198.18.0.0/15 that XCLIENT
   ///    moves the session to, with the suite's DNS zone answering the run's reverse
   ///    names - the same arrangement as AutoBanWiden, over COM and raw sockets. The
   ///    firewall rule blocks addresses nothing on the bench uses; every test deletes
   ///    the ban in its finally block, which takes the rule with it.
   /// </summary>
   [TestFixture]
   public class AutoBanWidenCom : TestFixtureBase
   {
      private const string Domain = "widen-com.test";
      private const string TrapLocalPart = "never-was-real-widen-com";
      private const string RangeName = "widen com test net";
      private const string RuleGroup = "hMailServer auto-ban";

      private int banMinutesBefore_;

      [SetUp]
      public void ArrangeXclientAndTrap()
      {
         dynamic settings = _settings;
         banMinutesBefore_ = (int) settings.SpamTrapBanMinutes;
         settings.SpamTrapBanMinutes = 5;
         settings.AutoBanWidenByReverseName = false;

         IniFileSetting.Write("SMTPXClientEnabled", "1");
         IniFileSetting.Write("SMTPXClientTrustedIPs", "127.0.0.1");
         _application.Reinitialize();

         TakeBackOurs();

         hMailServer.SecurityRange range = _settings.SecurityRanges.Add();
         range.Name = RangeName;
         range.LowerIP = "198.18.0.0";
         range.UpperIP = "198.19.255.255";
         range.Priority = 20;
         range.AllowSMTPConnections = true;
         range.AllowDeliveryFromRemoteToLocal = true;
         range.RequireAuthForDeliveryToLocal = false;
         range.EnableSpamProtection = true;
         range.Save();

         dynamic domain = _domain;
         domain.AddSpamTrap(TrapLocalPart + "@" + _domain.Name);
      }

      [TearDown]
      public void TakeItBack()
      {
         try
         {
            TakeBackOurs();

            dynamic settings = _settings;
            settings.SpamTrapBanMinutes = banMinutesBefore_;
            settings.AutoBanWidenByReverseName = false;
         }
         finally
         {
            SuiteDns.Reset();
            IniFileSetting.Delete("AutoBanCommand");
            IniFileSetting.Delete("AutoBanFirewall");
            IniFileSetting.Delete("SMTPXClientEnabled");
            IniFileSetting.Delete("SMTPXClientTrustedIPs");
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("Settings.AutoBanWidenByReverseName reads and writes the setting, and a value written over COM is the one the server acts on.")]
      public void ThePropertyIsReadAndWrittenOverCom()
      {
         dynamic settings = _settings;

         settings.AutoBanWidenByReverseName = true;
         ClassicAssert.IsTrue((bool) settings.AutoBanWidenByReverseName, "The property did not read back on.");

         settings.AutoBanWidenByReverseName = false;
         ClassicAssert.IsFalse((bool) settings.AutoBanWidenByReverseName, "The property did not read back off.");

         // A fresh Settings object reads the store, not a copy this one kept.
         dynamic fresh = _application.Settings;
         ClassicAssert.IsFalse((bool) fresh.AutoBanWidenByReverseName, "Another Settings object did not read the value written.");
      }

      [Test]
      [Description("With AutoBanFirewall on, a widened ban is ONE Windows Defender Firewall rule named for its run, whose remote addresses are the run; the single address's rule goes, and deleting the ban takes the run's rule with it.")]
      public void AWidenedBanIsOneFirewallRuleForTheRun()
      {
         const string net = "198.18.62.";
         string runRule = "hMailServer auto-ban " + net + "17-" + net + "23";

         try
         {
            IniFileSetting.Write("AutoBanFirewall", "1");
            dynamic settings = _settings;
            settings.AutoBanWidenByReverseName = true;
            Scanners(net, "mx20.fw." + Domain);

            TrapHitFrom(net + "20");

            hMailServer.SecurityRange ban = WaitForRun(net + "17", net + "23", TimeSpan.FromSeconds(30));
            ClassicAssert.IsNotNull(ban, "The ban of " + net + "20 was not widened to " + net + "17-" + net + "23.");

            object rule = WaitForRule(runRule, true, TimeSpan.FromSeconds(20));
            ClassicAssert.IsNotNull(rule, "No firewall rule named '" + runRule + "' appeared. Error log: " + SafeErrorLog());
            StringAssert.Contains(net + "17-" + net + "23", (string) Property(rule, "RemoteAddresses"), "The run's rule does not block the run.");
            ClassicAssert.AreEqual(RuleGroup, (string) Property(rule, "Grouping"), "The run's rule is not in the auto-ban group.");
            ClassicAssert.AreEqual(0, (int) Property(rule, "Action"), "The run's rule does not block.");

            ClassicAssert.IsNull(WaitForRule("hMailServer auto-ban " + net + "20", false, TimeSpan.FromSeconds(20)),
               "The single address's rule stayed beside the run's.");

            ban.Delete();

            ClassicAssert.IsNull(WaitForRule(runRule, false, TimeSpan.FromSeconds(20)),
               "The ban was deleted but the run's firewall rule stayed - a lockout of seven addresses that outlives the ban. Error log: " + SafeErrorLog());
         }
         finally
         {
            TakeBackOurs();
            IniFileSetting.Delete("AutoBanFirewall");
            LogHandler.DeleteErrorLog();
         }
      }

      [Test]
      [Description("AutoBanCommand is told 'ban <address> <minutes> <ports>' for each address of a widened run, one at a time, and 'unban <address>' for each when the ban is deleted - never a range it cannot read.")]
      public void TheHookIsToldEachAddressOfTheRun()
      {
         const string net = "198.18.63.";

         string folder = Path.Combine(Path.GetTempPath(), "hmailserver-autoban-widen-hook-test");
         Directory.CreateDirectory(folder);
         string script = Path.Combine(folder, "hook.cmd");
         string log = Path.Combine(folder, "hook.log");
         File.Delete(log);
         File.WriteAllText(script, "@echo %* >> \"" + log + "\"\r\n");

         try
         {
            IniFileSetting.Write("AutoBanCommand", "cmd.exe /c \"" + script + "\"");
            dynamic settings = _settings;
            settings.AutoBanWidenByReverseName = true;
            Scanners(net, "mx20.hook." + Domain);

            TrapHitFrom(net + "20");

            hMailServer.SecurityRange ban = WaitForRun(net + "17", net + "23", TimeSpan.FromSeconds(30));
            ClassicAssert.IsNotNull(ban, "The ban of " + net + "20 was not widened to " + net + "17-" + net + "23.");

            string content = WaitForFileContaining(log, "ban " + net + "23 ", TimeSpan.FromSeconds(20));
            for (int octet = 17; octet <= 23; octet++)
               StringAssert.Contains("ban " + net + octet + " ", content, "The hook was not told to ban " + net + octet + ". Hook log: " + content);

            StringAssert.DoesNotContain(net + "17-", content, "The hook was handed a range. Hook log: " + content);
            StringAssert.DoesNotContain("ban " + net + "16 ", content, "The hook was told to ban an address outside the run. Hook log: " + content);

            ban.Delete();

            content = WaitForFileContaining(log, "unban " + net + "23", TimeSpan.FromSeconds(20));
            for (int octet = 17; octet <= 23; octet++)
               StringAssert.Contains("unban " + net + octet, content, "The hook was not told to unban " + net + octet + ". Hook log: " + content);
         }
         finally
         {
            TakeBackOurs();
            IniFileSetting.Delete("AutoBanCommand");
            LogHandler.DeleteErrorLog();
            try
            {
               Directory.Delete(folder, true);
            }
            catch (IOException)
            {
               // The service may still hold the log for a moment; it is temp space.
            }
         }
      }

      [Test]
      [Description("A hook that runs past ExternalProcessTimeout is not run again in the same reconciliation: a widened run costs one timeout of a hanging hook, not one per address. Its negative control: a hook that fails at once, with an exit code, is still run for every address of the run.")]
      public void AHookThatHangsCostsOneTimeoutNotOnePerAddress()
      {
         // Spins in cmd.exe itself, with no child process to outlive it, until the
         // server stops it at the one-second timeout.
         string hung = HookTold(66, "@echo %* >> \"{0}\"\r\n:spin\r\n@goto spin\r\n", 1);

         // The run's first address is told by whichever reconciliation first sees
         // the widened row, and the ban itself, .20, by the one before it unless
         // the widening overtook that: two runs of the hook at most. Had the pass
         // gone on, the hook would have been run for one address a second.
         StringAssert.Contains("ban 198.18.66.17 ", hung, "The first address of the widened run was not told to the hook. Hook log: " + hung);
         ClassicAssert.LessOrEqual(Told(hung, "198.18.66."), 2,
            "A hook that had run past the timeout in a reconciliation was run again in it for another address of the run. Hook log: " + hung);

         // The negative control: a failure that is not the timeout goes on being
         // tried address by address, as it always was.
         string failing = HookTold(67, "@echo %* >> \"{0}\"\r\n@exit /b 1\r\n", 0);

         ClassicAssert.AreEqual(7, Told(failing, "198.18.67."),
            "A hook that failed with an exit code was not run for every address of the run - only a timeout ends the pass. Hook log: " + failing);
      }

      /// <summary>How many addresses of the run .17-.23 of the net a hook log says "ban" for.</summary>
      private static int Told(string hookLog, string net)
      {
         int told = 0;
         for (int octet = 17; octet <= 23; octet++)
         {
            if (hookLog.Contains("ban " + net + octet + " "))
               told++;
         }
         return told;
      }

      // ------------------------------------------------------------------- helpers

      /// <summary>
      ///    What AutoBanCommand, a cmd script made from body ({0} the log it
      ///    appends its arguments to), was told while a ban from 198.18.&lt;third&gt;.20
      ///    was written and widened to .17-.23 - ExternalProcessTimeout set to
      ///    timeoutSeconds unless that is 0 - read once the run's last address has
      ///    had a moment to be told. Everything it set is taken back.
      /// </summary>
      private string HookTold(int third, string body, int timeoutSeconds)
      {
         string net = "198.18." + third + ".";
         string folder = Path.Combine(Path.GetTempPath(), "hmailserver-autoban-widen-hook-timeout-" + third);
         Directory.CreateDirectory(folder);
         string script = Path.Combine(folder, "hook.cmd");
         string log = Path.Combine(folder, "hook.log");
         File.Delete(log);
         File.WriteAllText(script, string.Format(body, log));

         try
         {
            if (timeoutSeconds > 0)
               IniFileSetting.Write("ExternalProcessTimeout", timeoutSeconds.ToString());
            IniFileSetting.Write("AutoBanCommand", "cmd.exe /c \"" + script + "\"");
            dynamic settings = _settings;
            settings.AutoBanWidenByReverseName = true;
            Scanners(net, "mx20.timeout" + third + "." + Domain);

            TrapHitFrom(net + "20");

            hMailServer.SecurityRange ban = WaitForRun(net + "17", net + "23", TimeSpan.FromSeconds(30));
            ClassicAssert.IsNotNull(ban, "The ban of " + net + "20 was not widened to " + net + "17-" + net + "23.");

            // The reconciliation the widening asked for runs on the name lookup
            // queue: it has told the hook about .17 within a few seconds, and
            // three more is time enough for it to have run the six others, at a
            // second each, had it gone on.
            WaitForFileContaining(log, "ban " + net + "17 ", TimeSpan.FromSeconds(20));
            Thread.Sleep(TimeSpan.FromSeconds(3));
            return WaitForFileContaining(log, "ban " + net + "17 ", TimeSpan.FromSeconds(1));
         }
         finally
         {
            // The command first, so that taking the ban back does not run it.
            IniFileSetting.Delete("AutoBanCommand");
            if (timeoutSeconds > 0)
               IniFileSetting.Delete("ExternalProcessTimeout");
            TakeBackOurs();
            LogHandler.DeleteErrorLog();
            try
            {
               Directory.Delete(folder, true);
            }
            catch (IOException)
            {
               // The service may still hold the log for a moment; it is temp space.
            }
         }
      }

      /// <summary>
      ///    The run .17-.23 named h&lt;n&gt;.widen-com.test, the banned .20 named
      ///    seedName and resolving back to itself, and .16 under another domain.
      /// </summary>
      private static void Scanners(string net, string seedName)
      {
         for (int octet = 17; octet <= 23; octet++)
         {
            string host = octet == 20 ? seedName : "h" + octet + "." + Domain;
            SuiteDns.Zone.WithRaw(octet + "." + ReverseZone(net), 12, DnsWire.Name(host));
         }

         SuiteDns.Zone.WithRaw("16." + ReverseZone(net), 12, DnsWire.Name("mail16.other-isp.test"));
         SuiteDns.Zone.WithA(seedName, net + "20");
      }

      private static string ReverseZone(string net)
      {
         string[] octets = net.TrimEnd('.').Split('.');
         return octets[2] + "." + octets[1] + "." + octets[0] + ".in-addr.arpa";
      }

      private void TrapHitFrom(string address)
      {
         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(25), "Could not connect to SMTP.");
            socket.ReadUntil("220");

            socket.Send("EHLO upstream.widen-com.test\r\n");
            StringAssert.Contains("XCLIENT", socket.ReadUntil("250 "), "XCLIENT is not offered to 127.0.0.1.");

            socket.Send("XCLIENT ADDR=" + address + "\r\n");
            StringAssert.StartsWith("220", socket.ReadUntil("\r\n"), "XCLIENT to " + address + " was refused before the trap was hit.");

            socket.Send("EHLO client.widen-com.test\r\n");
            socket.ReadUntil("250 ");

            socket.Send("MAIL FROM:<harvester@widen-com-spammer.example>\r\n");
            socket.ReadUntil("250");

            socket.Send("RCPT TO:<" + TrapLocalPart + "@" + _domain.Name + ">\r\n");
            StringAssert.StartsWith("550", socket.ReadUntil("\r\n"), "The trap was not refused.");

            socket.Send("QUIT\r\n");
         }
      }

      /// <summary>The auto-ban range that reads lower..upper, polled until the deadline; null if none does.</summary>
      private hMailServer.SecurityRange WaitForRun(string lower, string upper, TimeSpan deadline)
      {
         DateTime until = DateTime.UtcNow + deadline;

         while (true)
         {
            _settings.SecurityRanges.Refresh();

            for (int i = 0; i < _settings.SecurityRanges.Count; i++)
            {
               hMailServer.SecurityRange range = _settings.SecurityRanges[i];
               if (range.Name.StartsWith("Auto-ban: ", StringComparison.Ordinal) && range.LowerIP == lower && range.UpperIP == upper)
                  return range;
            }

            if (DateTime.UtcNow > until)
               return null;

            Thread.Sleep(250);
         }
      }

      /// <summary>The fixture's range, its bans on 198.18.0.0/15, its trap and the record of its hits.</summary>
      private void TakeBackOurs()
      {
         _settings.SecurityRanges.Refresh();

         for (int i = _settings.SecurityRanges.Count - 1; i >= 0; i--)
         {
            hMailServer.SecurityRange range = _settings.SecurityRanges[i];
            bool ours = range.Name == RangeName ||
                        (range.Name.StartsWith("Auto-ban: ", StringComparison.Ordinal) &&
                         (range.LowerIP.StartsWith("198.18.", StringComparison.Ordinal) || range.LowerIP.StartsWith("198.19.", StringComparison.Ordinal)));

            if (ours)
               range.Delete();
         }

         try
         {
            dynamic domain = _domain;
            domain.RemoveSpamTrap(TrapLocalPart + "@" + _domain.Name);
         }
         catch (COMException)
         {
            // Not declared: nothing to take back.
         }
      }

      private static string SafeErrorLog()
      {
         if (!File.Exists(LogHandler.GetErrorLogFileName()))
            return "(no error log was written)";
         return LogHandler.ReadErrorLog();
      }

      private static object Property(object comObject, string name)
      {
         return comObject.GetType().InvokeMember(name, BindingFlags.GetProperty, null, comObject, null);
      }

      private static object FindRule(string name)
      {
         Type type = Type.GetTypeFromProgID("HNetCfg.FwPolicy2");
         ClassicAssert.IsNotNull(type, "Windows Defender Firewall's COM policy object is not registered on this machine.");
         object policy = Activator.CreateInstance(type);
         object rules = Property(policy, "Rules");

         try
         {
            return rules.GetType().InvokeMember("Item", BindingFlags.InvokeMethod, null, rules, new object[] { name });
         }
         catch (TargetInvocationException)
         {
            return null;
         }
         catch (FileNotFoundException)
         {
            return null;
         }
         catch (COMException)
         {
            return null;
         }
      }

      private static object WaitForRule(string name, bool present, TimeSpan timeout)
      {
         DateTime deadline = DateTime.UtcNow + timeout;
         object rule = FindRule(name);
         while ((rule != null) != present && DateTime.UtcNow < deadline)
         {
            Thread.Sleep(250);
            rule = FindRule(name);
         }
         return rule;
      }

      private static string WaitForFileContaining(string path, string text, TimeSpan timeout)
      {
         DateTime deadline = DateTime.UtcNow + timeout;
         string content = "";
         while (DateTime.UtcNow < deadline)
         {
            if (File.Exists(path))
            {
               using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
               using (var reader = new StreamReader(stream))
                  content = reader.ReadToEnd();
               if (content.Contains(text))
                  return content;
            }
            Thread.Sleep(250);
         }
         return content;
      }
   }
}
