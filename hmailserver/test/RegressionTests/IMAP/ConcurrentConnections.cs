// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Threading.Tasks;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   [TestFixture]
   public class ConcurrentConnections : TestFixtureBase
   {
      [Test]
      [Description("Bug: SendCachedNotifications passed lastExists to SendRECENT_ instead of lastRecent")]
      public void NoopRecentCountReflectsRecentMessagesNotExistsCount()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         // SELECT an empty inbox; the connection's recent-messages set starts empty (size=0).
         var sim = new ImapClientSimulator();
         sim.ConnectAndLogon(account.Address, "test");
         sim.SelectFolder("INBOX");

         // Deliver two messages after SELECT. This session is the first to be told of
         // them, so they are \Recent to it (RFC 3501 2.3.2, as imaptest holds since the
         // IMAP compliance row) - and to no other session.
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test 1", "Body 1");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test 2", "Body 2");

         // Wait for delivery without opening a second IMAP SELECT (which would claim \Recent).
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         // NOOP flushes cached notifications: 2 EXISTS, and the two are this session's
         // recent messages.
         var response = sim.NOOP();
         Assert.IsTrue(response.Contains("* 2 EXISTS"), response);
         Assert.IsTrue(response.Contains("* 2 RECENT"), response);

         // A second session is not the first to see them: RECENT counts \Recent
         // messages, never merely the messages that exist.
         var second = new ImapClientSimulator();
         second.ConnectAndLogon(account.Address, "test");
         string selected = second.SendSingleCommand("A2 SELECT INBOX");
         Assert.IsTrue(selected.Contains("* 2 EXISTS"), selected);
         Assert.IsTrue(selected.Contains("* 0 RECENT"), selected);
         second.Disconnect();

         sim.Disconnect();
      }


      [Test]
      public void ChangingFlagShouldAffectAllConnections()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "Test");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         var sim1 = new ImapClientSimulator();
         Assert.IsTrue(sim1.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(sim1.SelectFolder("Inbox"));

         var sim2 = new ImapClientSimulator();
         Assert.IsTrue(sim2.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(sim2.SelectFolder("Inbox"));

         sim1.SetFlagOnMessage(1, true, "\\Deleted");

         var flags1 = sim1.GetFlags(1);
         var flags2 = sim2.GetFlags(1);

         Assert.IsTrue(flags2.Contains(@"* 1 FETCH (FLAGS (\Deleted))"), flags2);
         Assert.IsTrue(flags2.Contains(@"* 1 FETCH (FLAGS (\Deleted) UID 1)"), flags2);
      }
      [Test]
      [Description("Two STOREs on one message from two sessions at once: neither change may be lost, in memory or in the database")]
      public void ConcurrentStoresOnOneMessageLoseNoChange()
      {
         // STORE used to change the shared message, read its flags and keywords
         // back and write them over it again with the row, three steps with no
         // lock between them: a session that read just before the other's
         // change wrote back what it had read, and that change was gone. Each
         // session here adds keywords of its own, one STORE at a time, at the
         // same moment as the other; at the end every one of them must be on
         // the message, as the sessions see it and as the database has it.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "Test");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         const int perSession = 40;

         var sessions = new[] { new ImapClientSimulator(), new ImapClientSimulator() };
         foreach (var session in sessions)
         {
            Assert.IsTrue(session.ConnectAndLogon(account.Address, "test"));
            Assert.IsTrue(session.SelectFolder("INBOX"));
         }

         var prefixes = new[] { "a", "b" };
         var stores = new Task[2];
         for (int s = 0; s < 2; s++)
         {
            var session = sessions[s];
            var prefix = prefixes[s];
            stores[s] = Task.Run(() =>
            {
               for (int k = 1; k <= perSession; k++)
               {
                  string answer = session.SendSingleCommand(prefix.ToUpper() + k + " STORE 1 +FLAGS (" + prefix + k + ")");
                  StringAssert.Contains(prefix.ToUpper() + k + " OK", answer);
               }
            });
         }
         Task.WaitAll(stores);

         foreach (var session in sessions)
            session.Disconnect();

         AssertEveryKeyword(account.Address, perSession, "as the sessions see it");

         // The same from the rows: a restart empties the message cache, so the
         // folder is read afresh from hm_messages.
         _application.Stop();
         _application.Start();

         AssertEveryKeyword(account.Address, perSession, "as the database has it");
      }

      private static void AssertEveryKeyword(string address, int perSession, string where)
      {
         var reader = new ImapClientSimulator();
         Assert.IsTrue(reader.ConnectAndLogon(address, "test"));
         Assert.IsTrue(reader.SelectFolder("INBOX"));
         string flags = reader.SendSingleCommand("F1 FETCH 1 (FLAGS)");
         reader.Disconnect();

         // The message's own flags - not the untagged FLAGS line of the
         // mailbox, which lists every keyword the mailbox has ever seen.
         const string marker = "* 1 FETCH (FLAGS (";
         int start = flags.IndexOf(marker);
         Assert.GreaterOrEqual(start, 0, flags);
         start += marker.Length;
         int end = flags.IndexOf(")", start);
         var present = new System.Collections.Generic.HashSet<string>(
            flags.Substring(start, end - start).Split(new[] { ' ' }, System.StringSplitOptions.RemoveEmptyEntries));

         foreach (var prefix in new[] { "a", "b" })
         {
            for (int k = 1; k <= perSession; k++)
               Assert.IsTrue(present.Contains(prefix + k), "Keyword " + prefix + k + " was lost, " + where + ": " + flags);
         }
      }

   }
}