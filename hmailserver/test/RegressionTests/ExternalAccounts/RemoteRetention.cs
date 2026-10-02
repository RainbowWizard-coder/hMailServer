// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.ExternalAccounts
{
   /// <summary>
   ///    What an external account leaves on the remote server: FetchAccount.DaysToKeepMessages,
   ///    one meaning for both fetchers (Server/Common/BO/FetchAccount.h) - a negative number
   ///    deletes each message there in the session that collects it, 0 leaves every message
   ///    there, N deletes it N days after it was collected - and the OnExternalAccountDownload
   ///    event's answers, which map onto the same three (1 deletes now, 2 after Parameter days,
   ///    3 never).
   ///    <para />
   ///    From 6.2.25 to 6.3.5 the IMAP fetcher read 0 as "delete" and a negative number as
   ///    "keep", the other way round from the POP3 fetcher, the COM API and the event: an IMAP
   ///    account at 0 - the default of COM, the Control Panel and the REST API - had its remote
   ///    INBOX emptied as it was collected, and a script that answered 3 ("never delete") for an
   ///    IMAP message had it deleted. The IMAP tests below each failed against that fetcher;
   ///    the POP3 ones hold the meaning the POP3 fetcher has always had, and both fetchers now
   ///    ask the same two questions of FetchAccount.
   ///    <para />
   ///    Ports 9603-9609 are this fixture's.
   /// </summary>
   [TestFixture]
   public class RemoteRetention : TestFixtureBase
   {
      private const int PortPop3DeletesAtOnce = 9603;
      private const int PortPop3KeepsAll = 9604;
      private const int PortImapDeletesAtOnce = 9605;
      private const int PortImapKeepsAll = 9606;
      private const int PortImapScriptNever = 9607;
      private const int PortImapScriptNow = 9608;
      private const int PortPop3ScriptNever = 9609;

      private const string Password = "test";

      private Account _account;

      [SetUp]
      public void SetUpTest()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "retention@example.test", Password);
      }

      [TearDown]
      public void LeaveNoScriptBehind()
      {
         var scripting = _application.Settings.Scripting;
         scripting.Enabled = false;

         var file = scripting.CurrentScriptFile;
         if (File.Exists(file))
            File.WriteAllText(file, string.Empty);

         scripting.Reload();
      }

      // ------------------------------------------------------------------ helpers

      private static string MessageText(string subject)
      {
         return "From: sender@dummy-example.com\r\n" +
                "To: retention@example.test\r\n" +
                "Subject: " + subject + "\r\n" +
                "\r\n" +
                "Body of " + subject + ".";
      }

      private static ScriptedImapServer.RemoteMailbox ImapMailbox(params string[] subjects)
      {
         var mailbox = new ScriptedImapServer.RemoteMailbox();
         for (var i = 0; i < subjects.Length; i++)
            mailbox.Add(new ScriptedImapServer.RemoteMessage(101 + i, MessageText(subjects[i])));
         return mailbox;
      }

      private static List<ScriptedPop3Server.RemoteMessage> Pop3Mailbox(params string[] subjects)
      {
         var messages = new List<ScriptedPop3Server.RemoteMessage>();
         for (var i = 0; i < subjects.Length; i++)
            messages.Add(new ScriptedPop3Server.RemoteMessage("retention-uid-" + (i + 1), MessageText(subjects[i])));
         return messages;
      }

      private FetchAccount CreateFetchAccount(int serverType, int port, int daysToKeepMessages)
      {
         var fetchAccount = _account.FetchAccounts.Add();
         fetchAccount.Enabled = true;
         // Long enough that the scheduler never starts a fetch of its own during a test:
         // every fetch here is driven by DownloadNow.
         fetchAccount.MinutesBetweenFetch = 60;
         fetchAccount.Name = serverType == 1 ? "Retention IMAP" : "Retention POP3";
         fetchAccount.ServerType = serverType;
         fetchAccount.Username = "remote@dummy-example.com";
         fetchAccount.Password = "far-side-secret";
         fetchAccount.UseSSL = false;
         fetchAccount.ServerAddress = "localhost";
         fetchAccount.Port = port;
         fetchAccount.ProcessMIMERecipients = false;
         fetchAccount.DaysToKeepMessages = daysToKeepMessages;
         fetchAccount.UseAntiSpam = false;
         fetchAccount.UseAntiVirus = false;
         fetchAccount.Save();

         Assert.AreEqual(daysToKeepMessages, fetchAccount.DaysToKeepMessages, "The value is stored as it was given.");
         return fetchAccount;
      }

      /// <summary>
      ///    A fetch is asynchronous: DownloadNow only queues it. The account stays locked for
      ///    the duration, so waiting for the lock to clear is what tells us the whole session -
      ///    the UID bookkeeping and the deletions among it - has finished.
      /// </summary>
      private static void WaitForFetchToFinish(FetchAccount fetchAccount)
      {
         var timeoutTime = DateTime.Now.Add(TimeSpan.FromSeconds(60));
         while (DateTime.Now < timeoutTime)
         {
            if (!fetchAccount.IsLocked)
               return;
            Thread.Sleep(100);
         }

         Assert.Fail("The external account was still locked after 60 seconds. Log:\r\n" +
                     LogHandler.ReadCurrentDefaultLog());
      }

      private static void Poll(ScriptedImapServer server, FetchAccount fetchAccount)
      {
         server.StartListen();
         fetchAccount.DownloadNow();
         WaitForFetchToFinish(fetchAccount);
         server.WaitForCompletion();
      }

      private static void Poll(ScriptedPop3Server server, FetchAccount fetchAccount)
      {
         server.StartListen();
         fetchAccount.DownloadNow();
         server.WaitForCompletion();
         WaitForFetchToFinish(fetchAccount);
      }

      private void AssertDeliveredMessageCount(int expectedCount)
      {
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(_account.Address, Password, expectedCount);
      }

      /// <summary>
      ///    An OnExternalAccountDownload handler that answers <paramref name="resultValue"/>
      ///    for every message, in the script language the server runs: JavaScript on Linux,
      ///    JScript on Windows - the same text in both.
      /// </summary>
      private void InstallDownloadHandler(int resultValue)
      {
         var language = ScriptLanguages.ServerRuns("JavaScript") ? "JavaScript" : "JScript";

         // Asking may have reinitialised the server; the settings object is read again.
         _settings = _application.Settings;
         var scripting = _settings.Scripting;
         scripting.Language = language;
         File.WriteAllText(scripting.CurrentScriptFile,
            "function OnExternalAccountDownload(oFetchAccount, message, sRemoteUID) {\r\n" +
            "   Result.Value = " + resultValue + ";\r\n" +
            "}\r\n");
         scripting.Enabled = true;
         scripting.Reload();
      }

      // ------------------------------------------------------------------ POP3

      [Test]
      [Description("POP3 at -1: each message is deleted on the remote server in the session that collects it.")]
      public void Pop3MinusOneDeletesEachMessageOnceCollected()
      {
         var messages = Pop3Mailbox("First", "Second");
         var fetchAccount = CreateFetchAccount(0, PortPop3DeletesAtOnce, -1);

         using (var server = new ScriptedPop3Server(PortPop3DeletesAtOnce, messages))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEquivalent(new[] { 1, 2 }, server.RetrievedMessages);
            CollectionAssert.AreEquivalent(new[] { 1, 2 }, server.DeletedMessages, "-1 deletes what was collected, in the same session.");
         }

         Assert.AreEqual(0, messages.Count, "The remote mailbox is empty after the session.");
         AssertDeliveredMessageCount(2);
      }

      [Test]
      [Description("POP3 at 0: every message stays on the remote server, and the next poll collects none of them again.")]
      public void Pop3ZeroLeavesEveryMessage()
      {
         var messages = Pop3Mailbox("Kept one", "Kept two");
         var fetchAccount = CreateFetchAccount(0, PortPop3KeepsAll, 0);

         using (var server = new ScriptedPop3Server(PortPop3KeepsAll, messages))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEquivalent(new[] { 1, 2 }, server.RetrievedMessages);
            CollectionAssert.IsEmpty(server.DeletedMessages, "0 never deletes.");
         }
         AssertDeliveredMessageCount(2);

         using (var server = new ScriptedPop3Server(PortPop3KeepsAll, messages))
         {
            Poll(server, fetchAccount);
            CollectionAssert.IsEmpty(server.RetrievedMessages, "Both are on record, so neither is collected again.");
            CollectionAssert.IsEmpty(server.DeletedMessages, "And still nothing is deleted.");
         }

         Assert.AreEqual(2, messages.Count, "Both messages are still on the remote server.");
         AssertDeliveredMessageCount(2);
      }

      [Test]
      [Description("POP3 at -1 with a handler that answers 3 (never delete): nothing is deleted on the remote server.")]
      public void Pop3HandlerAnsweringNeverKeepsTheMessages()
      {
         InstallDownloadHandler(3);

         var messages = Pop3Mailbox("Script keeps one", "Script keeps two");
         var fetchAccount = CreateFetchAccount(0, PortPop3ScriptNever, -1);

         using (var server = new ScriptedPop3Server(PortPop3ScriptNever, messages))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEquivalent(new[] { 1, 2 }, server.RetrievedMessages);
            CollectionAssert.IsEmpty(server.DeletedMessages, "The handler's 3 overrides the account's -1.");
         }

         Assert.AreEqual(2, messages.Count);
         AssertDeliveredMessageCount(2);
      }

      // ------------------------------------------------------------------ IMAP

      [Test]
      [Description("IMAP at -1: each collected message is flagged \\Deleted and expunged in the same session, as POP3 deletes it.")]
      public void ImapMinusOneDeletesEachMessageOnceCollected()
      {
         var mailbox = ImapMailbox("Take me", "And me");
         var fetchAccount = CreateFetchAccount(1, PortImapDeletesAtOnce, -1);

         using (var server = new ScriptedImapServer(PortImapDeletesAtOnce, mailbox))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.FetchedUids);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.StoredDeletedUids,
               "-1 deletes what was collected. Before 6.3.6 the IMAP fetcher read -1 as \"keep\".");
            Assert.AreEqual(1, server.ExpungeCount, "One EXPUNGE at the end of the session.");
         }

         Assert.AreEqual(0, mailbox.Count, "The far side is empty after the expunge.");
         AssertDeliveredMessageCount(2);
      }

      [Test]
      [Description("IMAP at 0 - the default - leaves every message on the remote server, poll after poll, and collects each once.")]
      public void ImapZeroLeavesEveryMessage()
      {
         var mailbox = ImapMailbox("Kept one", "Kept two");
         var fetchAccount = CreateFetchAccount(1, PortImapKeepsAll, 0);

         using (var server = new ScriptedImapServer(PortImapKeepsAll, mailbox))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.FetchedUids);
            CollectionAssert.IsEmpty(server.StoredDeletedUids,
               "0 never deletes. Before 6.3.6 the IMAP fetcher read 0 as \"delete\" and emptied the remote INBOX.");
            Assert.AreEqual(0, server.ExpungeCount, "Nothing to expunge.");
         }
         AssertDeliveredMessageCount(2);

         using (var server = new ScriptedImapServer(PortImapKeepsAll, mailbox))
         {
            Poll(server, fetchAccount);
            CollectionAssert.IsEmpty(server.FetchedUids, "Both are on record, so neither is collected again.");
            CollectionAssert.IsEmpty(server.StoredDeletedUids, "And the cleanup of what is on record deletes nothing either.");
         }

         Assert.AreEqual(2, mailbox.Count, "Both messages are still on the remote server.");
         AssertDeliveredMessageCount(2);
      }

      [Test]
      [Description("IMAP at -1 with a handler that answers 3 (never delete): nothing is deleted on the remote server.")]
      public void ImapHandlerAnsweringNeverKeepsTheMessages()
      {
         InstallDownloadHandler(3);

         var mailbox = ImapMailbox("Script keeps one", "Script keeps two");
         var fetchAccount = CreateFetchAccount(1, PortImapScriptNever, -1);

         using (var server = new ScriptedImapServer(PortImapScriptNever, mailbox))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.FetchedUids);
            CollectionAssert.IsEmpty(server.StoredDeletedUids,
               "The handler's 3 means never delete, for IMAP as for POP3. Before 6.3.6 the IMAP fetcher deleted on it.");
         }

         Assert.AreEqual(2, mailbox.Count);
         AssertDeliveredMessageCount(2);
      }

      [Test]
      [Description("IMAP at 0 with a handler that answers 1 (delete now): each message is deleted in the session that collects it.")]
      public void ImapHandlerAnsweringNowDeletesTheMessages()
      {
         InstallDownloadHandler(1);

         var mailbox = ImapMailbox("Script takes one", "Script takes two");
         var fetchAccount = CreateFetchAccount(1, PortImapScriptNow, 0);

         using (var server = new ScriptedImapServer(PortImapScriptNow, mailbox))
         {
            Poll(server, fetchAccount);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.FetchedUids);
            CollectionAssert.AreEqual(new[] { 101, 102 }, server.StoredDeletedUids,
               "The handler's 1 means delete now, for IMAP as for POP3. Before 6.3.6 the IMAP fetcher kept the message on it.");
         }

         Assert.AreEqual(0, mailbox.Count);
         AssertDeliveredMessageCount(2);
      }
   }
}
