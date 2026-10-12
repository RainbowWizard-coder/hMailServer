// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using hMailServer;
using NUnit.Framework;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   /// What a session is told of the folder it has selected, and in what order - two of the
   /// findings of upstream's UIDPLUS work (#666) that this server shared:
   ///
   ///  * A COPY into the selected folder was never reported to the session that made it:
   ///    the other sessions are told of a change, the one that made it is not, and COPY
   ///    sent no EXISTS of its own as APPEND and MOVE do. Its client saw neither the copies
   ///    nor a count that held them until it selected the folder again.
   ///  * The changes other sessions made were reported with the flag changes first and the
   ///    EXISTS after, so a flag change on a message just added to the mailbox came as a
   ///    FETCH with a number past the end of the mailbox the client knew.
   /// </summary>
   [TestFixture]
   public class SelectedFolderReports : TestFixtureBase
   {
      private Account _account;

      [SetUp]
      public void CreateAccountWithAMessage()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "selected-reports@example.test", "test");

         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "First", "Body");
         CustomAsserts.AssertFolderMessageCount(_account.IMAPFolders.get_ItemByName("INBOX"), 1);
      }

      private ImapClientSimulator SelectInbox()
      {
         var simulator = new ImapClientSimulator();
         Assert.IsTrue(simulator.ConnectAndLogon(_account.Address, "test"));
         Assert.IsTrue(simulator.SelectFolder("INBOX"));
         return simulator;
      }

      private static void AssertExistsBeforeTheTaggedOk(string response, string exists, string tag)
      {
         int existsAt = response.IndexOf(exists, StringComparison.Ordinal);
         int okAt = response.IndexOf(tag + " OK", StringComparison.Ordinal);

         Assert.IsTrue(okAt >= 0, response);
         Assert.IsTrue(existsAt >= 0 && existsAt < okAt,
            "The copy into the selected folder was not reported to the session that made it: " + response);
      }

      [Test]
      [Description("COPY into the selected folder answers \"* 2 EXISTS\" before its OK, and the copy is then " +
                   "message 2 to the session, with \\Recent, as an APPEND or a MOVE into it is. Nothing was " +
                   "said and the session had no message 2 (upstream #666).")]
      public void CopyIntoTheSelectedFolderIsReportedToTheSessionThatMadeIt()
      {
         var simulator = SelectInbox();

         string copy = simulator.SendSingleCommand("A01 COPY 1 INBOX");
         AssertExistsBeforeTheTaggedOk(copy, "* 2 EXISTS", "A01");

         string fetch = simulator.SendSingleCommand("A02 FETCH 2 (FLAGS)");
         StringAssert.Contains("* 2 FETCH (FLAGS (", fetch, "The copy is not message 2 of the session: " + fetch);
         StringAssert.Contains("\\Recent", fetch, "The copy is not the session's \\Recent: " + fetch);

         simulator.Disconnect();
      }

      [Test]
      [Description("UID COPY into the selected folder answers \"* 2 EXISTS\" before its OK, as COPY does.")]
      public void UidCopyIntoTheSelectedFolderIsReportedToTheSessionThatMadeIt()
      {
         var simulator = SelectInbox();

         string copy = simulator.SendSingleCommand("A01 UID COPY 1:* INBOX");
         AssertExistsBeforeTheTaggedOk(copy, "* 2 EXISTS", "A01");

         simulator.Disconnect();
      }

      [Test]
      [Description("A message another session appends and then flags is reported to a session with the " +
                   "folder open as \"* 2 EXISTS\" first and its flags after. The FETCH came first, numbered " +
                   "past the end of the mailbox the client knew (upstream #666).")]
      public void AFlagChangeOnANewMessageComesAfterItsExists()
      {
         var watcher = SelectInbox();

         var other = new ImapClientSimulator();
         Assert.IsTrue(other.ConnectAndLogon(_account.Address, "test"));
         StringAssert.Contains("B01 OK", other.SendSingleCommandWithLiteral("B01 APPEND INBOX {4}", "ABCD"));
         Assert.IsTrue(other.SelectFolder("INBOX"));
         StringAssert.Contains("B02 OK", other.SendSingleCommand("B02 STORE 2 +FLAGS (\\Flagged)"));
         other.Disconnect();

         string noop = watcher.SendSingleCommand("A01 NOOP");

         int existsAt = noop.IndexOf("* 2 EXISTS", StringComparison.Ordinal);
         int fetchAt = noop.IndexOf("* 2 FETCH", StringComparison.Ordinal);

         Assert.IsTrue(existsAt >= 0, "The new message was not reported: " + noop);
         Assert.IsTrue(fetchAt >= 0, "The flag change was not reported: " + noop);
         Assert.IsTrue(existsAt < fetchAt, "The flag change came before the EXISTS that numbers its message: " + noop);

         watcher.Disconnect();
      }
   }
}
