// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   /// Runs the error handling that three sweeps in August 2026 added, by making one
   /// named database statement fail.
   ///
   /// Those sweeps fixed 51 places where the result of a database operation was
   /// discarded. Every defect they found lived in error handling that had never once
   /// executed — a `return true` on a failed delete, a String passed through a variadic
   /// %s in the very line that reports a failed restore, a UID counter read back after
   /// an unchecked increment. The fixes then added 51 more branches which, without
   /// something like this, would also never execute. Untested error handling is exactly
   /// the category that was just found to be full of defects, so shipping 51 more
   /// unexercised branches would have repeated the mistake at one remove.
   ///
   /// `[Settings] SimulateDatabaseFailureFor` names a substring of a statement. Only
   /// statements containing it fail, which is what makes this usable at all: delivery
   /// threads, scheduled tasks and other sessions keep working normally while one write
   /// is broken. It is empty out of the box, cannot be set over COM, and the server
   /// reports HM6119 for as long as it is set.
   ///
   /// The tests below are the four most serious failures of the fifty-one, not all of
   /// them. What matters is that each one now runs the branch rather than reasoning
   /// about it.
   /// </summary>
   [TestFixture]
   public class DatabaseFailureHandling : TestFixtureBase
   {
      [TearDown]
      public void StopSimulatingTheFailure()
      {
         try
         {
            IniFileSetting.Write("SimulateDatabaseFailureFor", "");
            _application.Reinitialize();

            // A delivery the simulation refused bounces, and the bounce is a message
            // of its own in the delivery queue. Left there, it is delivered during the
            // next fixture's setup - which has just deleted the domain it is addressed
            // to - and the HM5207/HM5205/HM6081 trio it provokes lands in a fresh error
            // log for that setup to find. Seen in the 6.2.25 release gate, on the
            // fixture's fourth test, after the wait for HM6081 in the first test had
            // closed the earlier version of the same window. With the simulation off
            // the bounce delivers cleanly; waiting for the queue to empty is what makes
            // the log cleared below the last word.
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         }
         finally
         {
            // These tests exist to provoke error records, so they are cleared here
            // rather than left to fail whichever fixture runs next. HM6119 is in there
            // too, from the injector announcing itself.
            LogHandler.DeleteErrorLog();
         }
      }

      /// <summary>
      /// The setting is cached by IniFileSettings::InitInstance; Reinitialize re-reads
      /// it, and Stop()/Start() does not.
      /// </summary>
      private void FailStatementsContaining(string fragment)
      {
         IniFileSetting.Write("SimulateDatabaseFailureFor", fragment);
         _application.Reinitialize();
      }

      [Test]
      [Description("A message is refused rather than stored with a duplicate UID when the folder's UID " +
                   "counter cannot be incremented. GetUniqueMessageID read the counter back after an " +
                   "unchecked increment, so a failed UPDATE returned the previous message's UID.")]
      public void AMessageIsRefusedWhenItsUidCannotBeAllocated()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "uidfail@example.test", "test");

         // One delivered message first, so the folder exists and the counter has a
         // value to hand out twice.
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "First", "BodyOne");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         var firstUid = account.IMAPFolders.get_ItemByName("INBOX").Messages[0].UID;

         // The increment specifically. The bare column name also appears in the folder
         // SELECT and in every folder save, so failing that would break reading the
         // mailbox and the test would pass without ever reaching the branch it is for.
         FailStatementsContaining("foldercurrentuid + 1");

         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Second", "BodyTwo");

         // The second message must not arrive carrying the first one's UID. An IMAP
         // client that already has UID N will not fetch a different message with UID N,
         // so the delivered-but-invisible outcome is the one being ruled out here.
         //
         // HM6081 is the delivery thread's last word - the message could not be saved
         // and its file was removed - and it is written a moment after HM5205. Waiting
         // for both before the error log is consumed is what keeps that last line from
         // landing in a fresh log after the consume, where the next test's setup would
         // find it and fail for an error that was expected here. Seen once in a full
         // gate on 5 September 2026.
         CustomAsserts.AssertReportedError("HM5205", "HM6081");

         var folder = account.IMAPFolders.get_ItemByName("INBOX");

         ClassicAssert.AreEqual(1, folder.Messages.Count,
            "The message must be refused rather than stored, because storing it would duplicate a UID.");

         ClassicAssert.AreEqual(firstUid, folder.Messages[0].UID,
            "The message that was already there must be untouched.");
      }

      [Test]
      [Description("Auto-ban: when a failed logon cannot be recorded the server says so (HM6114). The " +
                   "count never rises, so the threshold is never crossed and no ban is ever created - " +
                   "brute-force protection that is enabled, configured and doing nothing.")]
      public void AFailedLogonThatCannotBeRecordedIsReported()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "banme@example.test", "test");

         // TestSetup switches auto-ban off before every test, and RegisterFailedLogin
         // returns immediately when it is off - so without this the failure is never
         // recorded in the first place and the test would pass having exercised nothing.
         _settings.AutoBanOnLogonFailure = true;

         FailStatementsContaining("hm_logon_failures");

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsFalse(pop3.ConnectAndLogon("banme@example.test", "the-wrong-password"),
            "The logon must still be refused.");

         CustomAsserts.AssertReportedError("HM6114");
      }

      [Test]
      [Description("Auto-ban: when the range that bans an address cannot be saved (HM6099), the address's " +
                   "spent logon failures are kept, so that its next failed logon crosses the threshold " +
                   "again and the range is written then. Up to 6.3.7 they were cleared before the save, " +
                   "and an address whose range was refused was given a whole new set of attempts.")]
      public void AnAutoBanThatCannotBeSavedIsTriedAgainOnTheNextFailure()
      {
         const string userName = "banretry@example.test";

         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 3;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;
         _settings.ClearLogonFailureList();

         try
         {
            // The range's INSERT alone - the check for a name in use is a SELECT -
            // and the save's second attempt under a GUID name is the same INSERT.
            FailStatementsContaining("INSERT INTO hm_securityranges");

            for (var i = 1; i <= 3; i++)
               ClassicAssert.IsFalse(FailLogon(userName), "Failed logon " + i + " was not refused.");

            CustomAsserts.AssertReportedError("HM6099");
            StringAssert.StartsWith("+OK", new Pop3ClientSimulator().GetWelcomeMessage(),
               "The address was banned although its range could not be saved.");

            FailStatementsContaining("");

            // One failure more, and no more: the three kept and this one cross the
            // threshold. Had the three been cleared, this would be the first of three.
            ClassicAssert.IsFalse(FailLogon(userName), "The failed logon after the refused range was not refused.");

            ClassicAssert.AreEqual("", new Pop3ClientSimulator().GetWelcomeMessage(),
               "The next failed logon from the address did not ban it: its spent failures were not kept.");
         }
         finally
         {
            var ranges = _settings.SecurityRanges;
            for (var i = ranges.Count - 1; i >= 0; i--)
            {
               var range = ranges[i];
               if (range.Name.StartsWith("Auto-ban: "))
                  ranges.DeleteByDBID(range.ID);
            }

            _settings.ClearLogonFailureList();
            _settings.AutoBanOnLogonFailure = false;
         }
      }

      [Test]
      [Description("Auto-ban: the check for a range's name in use binds the name as a parameter. With every " +
                   "statement that holds a range's name as a literal failing, a range standing under the name " +
                   "a ban takes is still found, and the ban is written under the next name. Up to 6.3.7 the " +
                   "check wrote the name into the statement, which SQL Server reads in the database's code " +
                   "page: a name with characters outside it was not found, and the ban was refused (HM6099).")]
      public void TheCheckForANameInUseBindsTheName()
      {
         const string userName = "bindcheck@example.test";

         try
         {
            StandAutoBanRange("Auto-ban: " + userName);
            FailStatementsContaining("rangename = '");

            FailThreshold(userName);

            ClassicAssert.AreEqual("", new Pop3ClientSimulator().GetWelcomeMessage(),
               "The address was not banned: the check for the name in use did not find the range standing under it.");
            ClassicAssert.Contains("Auto-ban: " + userName + " (1)", RangeNames(), "The ban was not written under the next name.");
            AssertNotInErrorLog("HM6099");
         }
         finally
         {
            RemoveAutoBans();
         }
      }

      [Test]
      [Description("Auto-ban: a range's name that the unique index refuses after the name was chosen as free - " +
                   "as another server sharing the database can write it in between - is written again under " +
                   "a GUID name, and the refusal is not reported. The check for a name in use is made to fail, " +
                   "so the name a range stands under is chosen, and the index alone refuses it.")]
      public void ANameTheIndexRefusesIsWrittenAgainUnderAGuid()
      {
         const string userName = "racecheck@example.test";

         try
         {
            StandAutoBanRange("Auto-ban: " + userName);
            FailStatementsContaining("count(*) as c from hm_securityranges where rangename");

            FailThreshold(userName);

            ClassicAssert.AreEqual("", new Pop3ClientSimulator().GetWelcomeMessage(),
               "The address was not banned after the index refused the name chosen.");

            var names = RangeNames();
            ClassicAssert.IsTrue(names.Exists(n => n.StartsWith("Auto-ban: " + userName + " {") && n.EndsWith("}")),
               "The ban was not written again under a GUID name. The ranges: " + string.Join(" | ", names));
            AssertNotInErrorLog("HM6099");
            AssertNotInErrorLog("HM5032");
         }
         finally
         {
            RemoveAutoBans();
         }
      }

      private void StandAutoBanRange(string name)
      {
         _settings.AutoBanOnLogonFailure = true;
         _settings.MaxInvalidLogonAttempts = 3;
         _settings.MaxInvalidLogonAttemptsWithin = 5;
         _settings.AutoBanMinutes = 3;
         _settings.ClearLogonFailureList();

         var range = _settings.SecurityRanges.Add();
         range.Name = name;
         range.LowerIP = "192.0.2.78";
         range.UpperIP = "192.0.2.78";
         range.Priority = 100;
         range.AllowSMTPConnections = false;
         range.AllowPOP3Connections = false;
         range.AllowIMAPConnections = false;
         range.Expires = true;
         range.ExpiresTime = DateTime.Now.AddMinutes(30);
         range.Save();

         ClassicAssert.Contains(name, RangeNames(), "The range the test stands on was not saved.");
      }

      private static void FailThreshold(string userName)
      {
         for (var i = 1; i <= 3; i++)
            ClassicAssert.IsFalse(FailLogon(userName), "Failed logon " + i + " was not refused.");
      }

      private System.Collections.Generic.List<string> RangeNames()
      {
         var names = new System.Collections.Generic.List<string>();
         var ranges = _settings.SecurityRanges;
         for (var i = 0; i < ranges.Count; i++)
            names.Add(ranges[i].Name);
         return names;
      }

      private void RemoveAutoBans()
      {
         var ranges = _settings.SecurityRanges;
         for (var i = ranges.Count - 1; i >= 0; i--)
         {
            var range = ranges[i];
            if (range.Name.StartsWith("Auto-ban: "))
               ranges.DeleteByDBID(range.ID);
         }

         _settings.ClearLogonFailureList();
         _settings.AutoBanOnLogonFailure = false;
      }

      // The error log as it stands holds no line with the code given. The
      // injector's HM6119 is always in it while a fragment is set (and the
      // tear-down clears it), so there is a log to read.
      private static void AssertNotInErrorLog(string code)
      {
         var text = LogHandler.ReadErrorLog();
         ClassicAssert.IsFalse(text.Contains(code), code + " was reported: " + text);
      }

      private static bool FailLogon(string userName)
      {
         var pop3 = new Pop3ClientSimulator();
         var signedIn = pop3.ConnectAndLogon(userName, "the-wrong-password");
         pop3.Disconnect();
         return signedIn;
      }

      [Test]
      [Description("IMAP SUBSCRIBE answers NO when the subscription cannot be saved. It used to answer " +
                   "OK, and a client that is told OK does not ask again.")]
      public void SubscribeAnswersNoWhenItCannotBeSaved()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "subfail@example.test", "test");

         // Created over COM rather than over IMAP, because injecting the failure goes
         // through Reinitialize, and Reinitialize stops and restarts the servers - which
         // drops every open connection. An IMAP session opened before the injection is
         // dead by the time the SUBSCRIBE is sent, and the test fails on a closed socket
         // having proved nothing.
         var folder = account.IMAPFolders.Add("SubscribeFail");
         folder.Save();

         // The assignment, not the column name, and no spaces around the "=".
         //
         // IMAPFolders::Refresh selects an explicit column list - "folderid,
         // folderparentid, foldername, folderissubscribed, ..." - so the bare name breaks
         // the folder *load*, and SUBSCRIBE is then refused with "Folder could not be
         // found": a different refusal that would have let this test pass without ever
         // reaching the branch it exists for. SQLStatement generates an update as
         // "name=@name_1" with no spaces, so this fragment appears only in the write.
         FailStatementsContaining("folderissubscribed=");

         var simulator = new ImapClientSimulator();
         ClassicAssert.IsTrue(simulator.ConnectAndLogon(account.Address, "test"));

         var result = simulator.Send("A10 SUBSCRIBE SubscribeFail");

         // The specific wording, not just "NO": failing every hm_imapfolders statement
         // could also make the folder lookup fail, which would refuse the command for a
         // different reason and let this pass without exercising the branch it is for.
         StringAssert.Contains("The subscription could not be saved", result,
            "SUBSCRIBE must be refused because the save failed. Got: " + result);

         simulator.Disconnect();
      }

      [Test]
      [Description("Restoring the default TCP/IP ports reports failure rather than claiming success. The " +
                   "existing ports are deleted first, so a silent failure leaves the server listening on " +
                   "fewer ports than it started with - possibly none.")]
      public void RestoringDefaultPortsReportsFailureRatherThanClaimingSuccess()
      {
         FailStatementsContaining("hm_tcpipports");

         // The COM call now answers with an error instead of S_OK. Caught by hand rather
         // than with a typed Assert.Throws, because which exception the interop layer
         // surfaces for a failed HRESULT is not what is being tested - that the call is
         // not silent is.
         //
         // Failing every hm_tcpipports statement also fails the deletes, so the ports
         // survive this test; and PerformBasicSetup calls SetDefault before every test
         // in any case, so a half-cleared port list could not outlive the fixture.
         bool reportedFailure = false;

         try
         {
            _settings.TCPIPPorts.SetDefault();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            reportedFailure = true;
         }

         ClassicAssert.IsTrue(reportedFailure,
            "SetDefault must report that it could not restore the ports, rather than answering S_OK for ports it deleted and did not put back.");

         CustomAsserts.AssertReportedError("HM6079");
      }
   }
}
