// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    OnIMAPMessageMoved (GitLab work item #109): the event script's handler called
   ///    once for each message a committed IMAP MOVE or UID MOVE moved between two of the
   ///    signed-in account's own folders, after the command and off the IMAP thread,
   ///    while Scripting.IMAPMessageMovedEvents is on (IMAP/IMAPMoveDispatch.h).
   ///
   ///    The handlers write one line to the event log per event, every argument in it,
   ///    and the tests read them back. The events are fired by one task, in the order
   ///    they were queued, so a test that wants to show that a command raised nothing
   ///    makes a MOVE after it that does raise one and waits for that: an event of the
   ///    first command would have come before it.
   /// </summary>
   [TestFixture]
   public class ImapMoveEvents : TestFixtureBase
   {
      private const string Password = "test";
      private const string Marker = "MOVED|";
      private Account _account;

      private const string VBScriptHandler =
         "Sub OnIMAPMessageMoved(oAccount, sEventId, sOperation, sUser, iSourceFolderId, sSourceFolder, iSourceUidValidity, iSourceUid, " +
         "iDestinationFolderId, sDestinationFolder, iDestinationUidValidity, iDestinationUid, iSourceMessageId, iMessageId)\r\n" +
         "   EventLog.Write(\"MOVED|\" & oAccount.Address & \"|\" & sEventId & \"|\" & sOperation & \"|\" & sUser & \"|\" & iSourceFolderId & \"|\" & sSourceFolder & \"|\" & " +
         "iSourceUidValidity & \"|\" & iSourceUid & \"|\" & iDestinationFolderId & \"|\" & sDestinationFolder & \"|\" & iDestinationUidValidity & \"|\" & " +
         "iDestinationUid & \"|\" & iSourceMessageId & \"|\" & iMessageId & \"|\" & TypeName(iSourceUid))\r\n" +
         "End Sub\r\n";

      private const string JScriptHandler =
         "function OnIMAPMessageMoved(account, eventId, operation, user, sourceFolderId, sourceFolder, sourceUidValidity, sourceUid, " +
         "destinationFolderId, destinationFolder, destinationUidValidity, destinationUid, sourceMessageId, messageId)\r\n" +
         "{\r\n" +
         "   EventLog.Write('MOVED|' + [account.Address, eventId, operation, user, sourceFolderId, sourceFolder, sourceUidValidity, sourceUid, " +
         "destinationFolderId, destinationFolder, destinationUidValidity, destinationUid, sourceMessageId, messageId, typeof sourceUid].join('|'));\r\n" +
         "}\r\n";

      [SetUp]
      public void CreateAccount()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "mover@example.test", Password);
         LogHandler.DeleteEventLog();
      }

      [TearDown]
      public void LeaveTheEventOff()
      {
         var scripting = SingletonProvider<TestSetup>.Instance.GetApp().Settings.Scripting;
         scripting.IMAPMessageMovedEvents = false;
         scripting.Enabled = false;

         scripting.Language = "JScript";
         File.WriteAllText(scripting.CurrentScriptFile, string.Empty);

         scripting.Language = "VBScript";
         File.WriteAllText(scripting.CurrentScriptFile, string.Empty);

         scripting.Reload();
      }

      private void LoadHandler(string language, string script, bool eventsOn)
      {
         var scripting = _settings.Scripting;
         scripting.Language = language;
         File.WriteAllText(scripting.CurrentScriptFile, script);
         scripting.Enabled = true;
         scripting.Reload();
         ClassicAssert.AreEqual(string.Empty, scripting.CheckSyntax(), "The handler compiles.");
         scripting.IMAPMessageMovedEvents = eventsOn;
      }

      private static ImapClientSimulator SignIn(string address)
      {
         var client = new ImapClientSimulator();
         ClassicAssert.IsTrue(client.ConnectAndLogon(address, Password));
         return client;
      }

      private static void Append(ImapClientSimulator client, string folder, int count)
      {
         for (int i = 0; i < count; i++)
         {
            string message = "From: sender@example.test\r\nSubject: Message " + i + "\r\n\r\nBody " + i + "\r\n";
            StringAssert.Contains("A0" + i + " OK", client.SendSingleCommandWithLiteral("A0" + i + " APPEND \"" + folder + "\" {" + message.Length + "}", message));
         }
      }

      private static long UidValidity(ImapClientSimulator client, string folder)
      {
         var match = Regex.Match(client.Status(folder, "UIDVALIDITY"), @"UIDVALIDITY (\d+)");
         ClassicAssert.IsTrue(match.Success, "STATUS gives the folder's UIDVALIDITY.");
         return long.Parse(match.Groups[1].Value);
      }

      // The ids of the folder's messages, in UID order, as COM reads them.
      private List<long> MessageIds(string folder)
      {
         var ids = new List<long>();
         var messages = _account.IMAPFolders.get_ItemByName(folder).Messages;
         for (int i = 0; i < messages.Count; i++)
            ids.Add(messages[i].ID);
         return ids;
      }

      // Every event line written since the test began, each split into its fields:
      // the account's address, then the thirteen values, then the type of sourceUid.
      private static List<string[]> EventLines()
      {
         var lines = new List<string[]>();
         string log;
         try
         {
            log = LogHandler.ReadEventLog();
         }
         catch (IOException)
         {
            return lines;
         }

         foreach (var line in log.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries))
         {
            var start = line.IndexOf(Marker, StringComparison.Ordinal);
            if (start < 0)
               continue;

            var text = line.Substring(start + Marker.Length);
            if (text.EndsWith("\""))
               text = text.Substring(0, text.Length - 1);
            lines.Add(text.Split('|'));
         }

         return lines;
      }

      private static List<string[]> WaitForEvents(int count)
      {
         var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(30);
         while (true)
         {
            var lines = EventLines();
            if (lines.Count >= count || DateTime.UtcNow >= deadline)
               return lines;
            System.Threading.Thread.Sleep(200);
         }
      }

      private static string CopyUid(string response)
      {
         var match = Regex.Match(response, @"\[COPYUID (\d+) ([0-9:,]+) ([0-9:,]+)\]");
         ClassicAssert.IsTrue(match.Success, "The response has a COPYUID: " + response);
         return match.Value;
      }

      private void AssertEvent(string[] fields, string operation, long sourceFolderId, string sourceFolder, long sourceValidity, long sourceUid,
                               long destinationFolderId, string destinationFolder, long destinationValidity, long destinationUid,
                               long sourceMessageId, long messageId, string numberType)
      {
         ClassicAssert.AreEqual(15, fields.Length, "The account and thirteen values, and the type: " + string.Join("|", fields));
         ClassicAssert.AreEqual(_account.Address, fields[0], "account.Address");
         StringAssert.IsMatch(@"^\{[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}\}$", fields[1], "eventId is a braced UUID");
         ClassicAssert.AreEqual(operation, fields[2], "operation");
         ClassicAssert.AreEqual(_account.Address, fields[3], "user");
         ClassicAssert.AreEqual(sourceFolderId.ToString(), fields[4], "sourceFolderId");
         ClassicAssert.AreEqual(sourceFolder, fields[5], "sourceFolder");
         ClassicAssert.AreEqual(sourceValidity.ToString(), fields[6], "sourceUidValidity");
         ClassicAssert.AreEqual(sourceUid.ToString(), fields[7], "sourceUid");
         ClassicAssert.AreEqual(destinationFolderId.ToString(), fields[8], "destinationFolderId");
         ClassicAssert.AreEqual(destinationFolder, fields[9], "destinationFolder");
         ClassicAssert.AreEqual(destinationValidity.ToString(), fields[10], "destinationUidValidity");
         ClassicAssert.AreEqual(destinationUid.ToString(), fields[11], "destinationUid");
         ClassicAssert.AreEqual(sourceMessageId.ToString(), fields[12], "sourceMessageId");
         ClassicAssert.AreEqual(messageId.ToString(), fields[13], "messageId");
         // VBScript reads a literal that fits in 16 bits as an Integer, JScript any as a number.
         ClassicAssert.AreEqual(numberType, fields[14], "a UID is given as a number");
      }

      [Test]
      [Description("A MOVE of two messages raises OnIMAPMessageMoved twice, in VBScript, each with the account, its own event id, MOVE, the user, both folders by id, name and UIDVALIDITY, the UIDs COPYUID reports and the message's ids before and after.")]
      public void AMoveRaisesOneEventPerMessageInVBScript()
      {
         LoadHandler("VBScript", VBScriptHandler, true);

         var client = SignIn(_account.Address);
         ClassicAssert.IsTrue(client.CreateFolder("Archive"));
         Append(client, "INBOX", 2);
         long inboxValidity = UidValidity(client, "INBOX");
         long archiveValidity = UidValidity(client, "Archive");
         var before = MessageIds("INBOX");

         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));
         string response = client.SendSingleCommand("A10 MOVE 1:2 Archive");
         StringAssert.Contains("A10 OK MOVE completed", response);
         StringAssert.Contains("[COPYUID " + archiveValidity + " 1,2 1,2]", response);
         client.Disconnect();

         var events = WaitForEvents(2);
         ClassicAssert.AreEqual(2, events.Count, "one event per message moved");

         var after = MessageIds("Archive");
         long inboxId = _account.IMAPFolders.get_ItemByName("INBOX").ID;
         long archiveId = _account.IMAPFolders.get_ItemByName("Archive").ID;

         AssertEvent(events[0], "MOVE", inboxId, "INBOX", inboxValidity, 1, archiveId, "Archive", archiveValidity, 1, before[0], after[0], "Integer");
         AssertEvent(events[1], "MOVE", inboxId, "INBOX", inboxValidity, 2, archiveId, "Archive", archiveValidity, 2, before[1], after[1], "Integer");
         ClassicAssert.AreNotEqual(events[0][1], events[1][1], "each event has an id of its own");
      }

      [Test]
      [Description("A UID MOVE raises the event in JScript, named UID MOVE, out of a subfolder whose name holds an apostrophe, which the call quotes.")]
      public void AUidMoveRaisesTheEventInJScript()
      {
         LoadHandler("JScript", JScriptHandler, true);

         var client = SignIn(_account.Address);
         ClassicAssert.IsTrue(client.CreateFolder("Projects"));
         ClassicAssert.IsTrue(client.CreateFolder("Projects.It's done"));
         Append(client, "Projects.It's done", 3);
         long sourceValidity = UidValidity(client, "Projects.It's done");
         long inboxValidity = UidValidity(client, "INBOX");
         var sourceFolder = _account.IMAPFolders.get_ItemByName("Projects").SubFolders.get_ItemByName("It's done");
         var sourceIds = new List<long>();
         for (int i = 0; i < sourceFolder.Messages.Count; i++)
            sourceIds.Add(sourceFolder.Messages[i].ID);

         ClassicAssert.IsTrue(client.SelectFolder("Projects.It's done"));
         string response = client.SendSingleCommand("A10 UID MOVE 2 INBOX");
         StringAssert.Contains("A10 OK UID MOVE completed", response);
         StringAssert.Contains("[COPYUID " + inboxValidity + " 2 1]", response);
         client.Disconnect();

         var events = WaitForEvents(1);
         ClassicAssert.AreEqual(1, events.Count, "one event for the one message");

         var inbox = MessageIds("INBOX");
         AssertEvent(events[0], "UID MOVE", sourceFolder.ID, "Projects.It's done", sourceValidity, 2,
            _account.IMAPFolders.get_ItemByName("INBOX").ID, "INBOX", inboxValidity, 1, sourceIds[1], inbox[0], "number");
      }

      [Test]
      [Description("A COPY, a MOVE that fails part way over the quota and a MOVE made with the setting off raise nothing; a MOVE after them does, and its event is the only one.")]
      public void ACopyAFailedMoveAndAMoveWithTheSettingOffRaiseNothing()
      {
         // 1 MB: two messages of 300 KB and one copy of one fit, and a second copy does not.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "mover-quota@example.test", Password, 1);
         LoadHandler("VBScript", VBScriptHandler, false);

         var client = SignIn(account.Address);
         ClassicAssert.IsTrue(client.CreateFolder("Archive"));

         var body = new System.Text.StringBuilder();
         for (int line = 0; line < 3072; line++)
            body.Append(new string('x', 98)).Append("\r\n");
         for (int i = 0; i < 2; i++)
         {
            string message = "From: " + account.Address + "\r\nSubject: Large " + i + "\r\n\r\n" + body;
            StringAssert.Contains("A0" + i + " OK", client.SendSingleCommandWithLiteral("A0" + i + " APPEND INBOX {" + message.Length + "}", message));
         }

         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));

         // The setting off: a MOVE that succeeds raises nothing.
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Archive"));
         ClassicAssert.IsTrue(client.SelectFolder("Archive"));
         StringAssert.Contains("A11 OK", client.SendSingleCommand("A11 MOVE 1 INBOX"));
         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));

         _settings.Scripting.IMAPMessageMovedEvents = true;

         // A COPY that succeeds raises nothing; its copy is taken out again with a
         // STORE and an EXPUNGE, which is not a MOVE either.
         StringAssert.Contains("A12 OK", client.SendSingleCommand("A12 COPY 1 Archive"));
         ClassicAssert.IsTrue(client.SelectFolder("Archive"));
         StringAssert.Contains("A13 OK", client.SendSingleCommand("A13 STORE 1 +FLAGS (\\Deleted)"));
         StringAssert.Contains("A14 OK", client.SendSingleCommand("A14 EXPUNGE"));
         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));

         // A MOVE and a UID MOVE that fail at the second copy: NO, and nothing raised.
         StringAssert.Contains("A15 NO", client.SendSingleCommand("A15 MOVE 1:2 Archive"));
         StringAssert.Contains("A16 NO", client.SendSingleCommand("A16 UID MOVE 1:* Archive"));
         StringAssert.Contains("(MESSAGES 0)", client.Status("Archive", "MESSAGES"));

         // And one MOVE that succeeds: its event is the first and the only one.
         string response = client.SendSingleCommand("A17 MOVE 2 Archive");
         StringAssert.Contains("A17 OK MOVE completed", response);
         client.Disconnect();

         var events = WaitForEvents(1);
         System.Threading.Thread.Sleep(1000);
         events = EventLines();
         ClassicAssert.AreEqual(1, events.Count, "only the last MOVE raised the event: " + LogHandler.ReadEventLog());
         ClassicAssert.AreEqual(account.Address, events[0][0]);
         ClassicAssert.AreEqual("MOVE", events[0][2]);
         // INBOX holds UID 2, and UID 3, the first message moved out and back.
         ClassicAssert.AreEqual("3", events[0][7], "the source UID of the message the last MOVE moved");
      }

      [Test]
      [Description("A handler that fails is reported as a Script Error and changes nothing of the move: the MOVE answers OK, the messages are moved, and the next message's event is still called.")]
      public void AHandlerThatFailsChangesNothingOfTheMove()
      {
         var script =
            "Sub OnIMAPMessageMoved(oAccount, sEventId, sOperation, sUser, iSourceFolderId, sSourceFolder, iSourceUidValidity, iSourceUid, " +
            "iDestinationFolderId, sDestinationFolder, iDestinationUidValidity, iDestinationUid, iSourceMessageId, iMessageId)\r\n" +
            "   If iSourceUid = 1 Then Err.Raise 5, \"OnIMAPMessageMoved\", \"move_handler_failed_on_purpose\"\r\n" +
            "   EventLog.Write(\"MOVED|\" & iSourceUid)\r\n" +
            "End Sub\r\n";
         LoadHandler("VBScript", script, true);

         var client = SignIn(_account.Address);
         ClassicAssert.IsTrue(client.CreateFolder("Archive"));
         Append(client, "INBOX", 2);
         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));
         StringAssert.Contains("A10 OK MOVE completed", client.SendSingleCommand("A10 MOVE 1:2 Archive"));
         client.Disconnect();

         var events = WaitForEvents(1);
         ClassicAssert.AreEqual(1, events.Count, "the second message's event is called after the first's failed");
         ClassicAssert.AreEqual("2", events[0][0]);

         CustomAsserts.AssertReportedError("move_handler_failed_on_purpose");
         CustomAsserts.AssertFolderMessageCount(_account.IMAPFolders.get_ItemByName("Archive"), 2);
         CustomAsserts.AssertFolderMessageCount(_account.IMAPFolders.get_ItemByName("INBOX"), 0);
      }

      [Test]
      [Description("The client is answered the same with the event on as off: the same untagged and tagged lines, in the same order, for a MOVE of two messages.")]
      public void TheReplyToAMoveIsTheSameWithTheEventOnAsOff()
      {
         LoadHandler("VBScript", VBScriptHandler, false);
         string off = MoveTwoAndAnswer("mover-off@example.test");

         _settings.Scripting.IMAPMessageMovedEvents = true;
         string on = MoveTwoAndAnswer("mover-on@example.test");

         ClassicAssert.AreEqual(off, on, "the MOVE's reply, its UIDVALIDITY aside");
         ClassicAssert.AreEqual(2, WaitForEvents(2).Count, "and with the event on, it was raised");
      }

      [Test]
      [Description("When the IMAP master user opened the account's mailbox, the event names the account as the owner and the master user as the user who moved the message.")]
      public void TheUserIsTheMasterUserWhenItOpenedTheMailbox()
      {
         const string master = "move-master@example.test";
         bool sasl = _settings.IMAPSASLPlainEnabled;
         try
         {
            _settings.IMAPSASLPlainEnabled = true;
            _settings.IMAPMasterUser = master;
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, master, Password);
            LoadHandler("VBScript", VBScriptHandler, true);

            var owner = SignIn(_account.Address);
            ClassicAssert.IsTrue(owner.CreateFolder("Archive"));
            Append(owner, "INBOX", 1);
            owner.Disconnect();

            var socket = new TcpConnection();
            ClassicAssert.IsTrue(socket.Connect(143), "Could not connect to the IMAP server on port 143.");
            socket.ReadUntil("* OK");
            socket.Send("A01 AUTHENTICATE PLAIN\r\n");
            StringAssert.StartsWith("+", socket.Receive());
            socket.Send(Convert.ToBase64String(Encoding.UTF8.GetBytes(_account.Address + "\0" + master + "\0" + Password)) + "\r\n");
            StringAssert.Contains("A01 OK", socket.ReadUntil("A01 "));
            socket.Send("A02 SELECT INBOX\r\n");
            StringAssert.Contains("A02 OK", socket.ReadUntil("A02 "));
            socket.Send("A03 MOVE 1 Archive\r\n");
            StringAssert.Contains("A03 OK", socket.ReadUntil("A03 "));
            socket.Disconnect();

            var events = WaitForEvents(1);
            ClassicAssert.AreEqual(1, events.Count, "the master user's MOVE in the account's own folders raises the event");
            ClassicAssert.AreEqual(_account.Address, events[0][0], "the account is the owner of the folders");
            ClassicAssert.AreEqual(master, events[0][3], "the user is the master user that signed in");
         }
         finally
         {
            _settings.IMAPMasterUser = string.Empty;
            _settings.IMAPSASLPlainEnabled = sasl;
         }
      }

      private string MoveTwoAndAnswer(string address)
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         var client = SignIn(address);
         ClassicAssert.IsTrue(client.CreateFolder("Archive"));
         Append(client, "INBOX", 2);
         ClassicAssert.IsTrue(client.SelectFolder("INBOX"));
         string response = client.SendSingleCommand("A10 MOVE 1:2 Archive");
         client.Disconnect();

         CopyUid(response);
         return Regex.Replace(response, @"\[COPYUID \d+ ", "[COPYUID <validity> ");
      }
   }
}
