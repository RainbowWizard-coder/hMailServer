// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Mail;
using System.Reflection;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.POP3;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    The event handlers in JavaScript: the engine the Linux builds embed
   ///    (Common/Scripting/ScriptBinding, hmailserver/docs/EventScripts.md).
   ///
   ///    Three groups of tests, all with scriptlanguage=JavaScript and an
   ///    EventHandlers.js:
   ///
   ///      - one for each event API/Events.cs tests, with a handler that mirrors the
   ///        VBScript or JScript one there - the same members read and written, the
   ///        same assertions. The two JScript handlers that assign to
   ///        message.HeaderValue('X'), which is a syntax error in JavaScript, use
   ///        message.SetHeaderValue('X', v), the binding's spelling of it;
   ///      - what the engine promises beyond those: the timeout and the memory
   ///        ceiling stop a handler and the message still arrives, a syntax error is
   ///        named with its line, CreateObject refuses clearly, Result refuses a
   ///        message, an exception is logged with its stack, values arrive unquoted,
   ///        and a handler may be written in the JavaScript of this decade;
   ///      - JavaScript counterparts of fifteen tests elsewhere in the suite that the
   ///        Linux project skips: their handlers are VBScript (one is JScript), and
   ///        on a build that runs JavaScript those originals still skip, with the
   ///        language as the reason. These run beside them; they do not replace them.
   ///
   ///    Compiled into both projects, and run only against a server whose build
   ///    reports JavaScript in GET /api/v1/status's build.script_languages - a Linux
   ///    build. A Windows build runs VBScript and JScript and refuses a JavaScript
   ///    script at load, so there the whole fixture is skipped with that reason; in
   ///    the Linux project a server that does not report JavaScript fails it
   ///    instead (ScriptLanguages.JavaScriptRequired).
   ///
   ///    The event log is read from a mark taken at SetUp and again wherever a test
   ///    calls LogHandler.DeleteEventLog: on the Windows bench that call deletes the
   ///    file, and on a Linux server, whose event log is one file for the server's
   ///    whole life, it marks where the file ends. Either way a test reads only what
   ///    was written after it.
   /// </summary>
   [TestFixture]
   public class EventsJavaScript : TestFixtureBase
   {
      private const string JavaScript = "JavaScript";

      // The row's ports are 10070-10079; 10070 is ScriptLanguages' on Windows.
      private const int FetchPort = 10071;

      [OneTimeSetUp]
      public void RunOnlyWhereJavaScriptRuns()
      {
         if (!ScriptLanguages.ServerRuns(JavaScript))
         {
            var why = "This server runs event scripts in " + ScriptLanguages.Describe() +
                      " (GET /api/v1/status, build.script_languages); JavaScript is the engine the Linux builds embed.";

            // The Linux project runs against a Linux build, and every Linux build runs
            // JavaScript: one that does not say so has lost its engine, and forty
            // skips would leave the job green over it.
            if (ScriptLanguages.JavaScriptRequired)
               Assert.Fail(why);

            Assert.Ignore(why);
         }

         // Asking may have reinitialised the server; the settings object is read again.
         _settings = _application.Settings;
      }

      // Named apart from the base class's SetUp, which runs first and which a
      // method of the same name would hide.
      [SetUp]
      public void StartTheEventLogAfresh()
      {
         LogHandler.DeleteEventLog();
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

      private void InstallScript(string script)
      {
         var scripting = _application.Settings.Scripting;
         scripting.Language = JavaScript;
         File.WriteAllText(scripting.CurrentScriptFile, script);
         scripting.Enabled = true;
         scripting.Reload();
      }

      /// The event log written since the last mark (LogHandler.ReadEventLog: since
      /// SetUp, or since the test's own DeleteEventLog), polled until it holds the
      /// text or the wait runs out; returned either way so that an assertion can
      /// print it.
      private static string WaitForEventLog(string expected)
      {
         return WaitForEventLog(expected, TimeSpan.FromSeconds(20));
      }

      private static string WaitForEventLog(string expected, TimeSpan bound)
      {
         var contents = string.Empty;
         var tries = Math.Max(1, (int) (bound.TotalMilliseconds / 250));

         for (var i = 0; i < tries; i++)
         {
            try
            {
               contents = LogHandler.ReadEventLog();
            }
            catch (IOException)
            {
               // The server may be mid-write; try again.
            }
            catch (UnauthorizedAccessException)
            {
               // Or holding the file a moment; try again.
            }

            if (contents.Contains(expected))
               return contents;

            Thread.Sleep(250);
         }

         return contents;
      }

      /// Runs the body with the backup settings it changes put back afterwards,
      /// whatever they were: the Linux per-test baseline does not cover the backup
      /// group, so a destination or a switch left here would stay for the rest of
      /// the run. Read before anything is written.
      private void WithTheBackupSettingsPutBack(Action body)
      {
         var backup = _settings.Backup;
         var destination = backup.Destination;
         var backupSettings = backup.BackupSettings;
         var backupDomains = backup.BackupDomains;
         var backupMessages = backup.BackupMessages;
         var compress = backup.CompressDestinationFiles;

         try
         {
            body();
         }
         finally
         {
            backup.Destination = destination;
            backup.BackupSettings = backupSettings;
            backup.BackupDomains = backupDomains;
            backup.BackupMessages = backupMessages;
            backup.CompressDestinationFiles = compress;
         }
      }

      private static string Lines(params string[] lines)
      {
         return string.Join("\n", lines) + "\n";
      }

      // ------------------------------------------------------------------ the events, as API/Events.cs has them

      [Test]
      [Description("GET /api/v1/status names JavaScript as the language this build runs, and says it has an engine")]
      public void TheStatusNamesJavaScriptAsTheLanguageThisBuildRuns()
      {
         ClassicAssert.IsTrue(ScriptLanguages.ListReported, "GET /api/v1/status has no build.script_languages.");
         ClassicAssert.AreEqual(JavaScript, ScriptLanguages.Describe(), "build.script_languages");
         ClassicAssert.IsTrue(ScriptLanguages.ScriptEngine, "build.script_engine is false on a build that runs JavaScript.");
      }

      [Test]
      [Description("OnAcceptMessage: a header written and saved, the client read")]
      public void OnAcceptMessageWritesAHeaderAndReadsTheClient()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   message.SetHeaderValue('X-SpamResult', 'TEST');",
            "   message.Save();",
            "   EventLog.Write('Port: ' + oClient.Port);",
            "   EventLog.Write('Address: ' + oClient.IPAddress);",
            "   EventLog.Write('Username: ' + oClient.Username);",
            "   EventLog.Write('SessionId: ' + oClient.SessionID);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains("X-SpamResult: TEST", message);

         var eventLogText = WaitForEventLog("SessionId: ");
         StringAssert.Contains("Port: " + TestPorts.Actual(25), eventLogText);
         StringAssert.IsMatch("Address: [0-9a-fA-F.:]+", eventLogText);
         // Empty: the user name is not known when a message is accepted.
         StringAssert.Contains("Username: \"", eventLogText);
         StringAssert.IsMatch(".*\"SessionId: \\d+\"", eventLogText);
      }

      [Test]
      [Description("OnRecipientUnknown fires for a recipient the server does not have")]
      public void OnRecipientUnknownFires()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnRecipientUnknown(oClient, oMessage) {",
            "   EventLog.Write('OnRecipientUnknown ');",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         try
         {
            SmtpClientSimulator.StaticSend(account.Address, "nonexistent@example.test", "Test", "SampleBody");
         }
         catch (DeliveryFailedException)
         {
            // Expected, since the recipient does not exist.
         }

         StringAssert.Contains("OnRecipientUnknown", WaitForEventLog("OnRecipientUnknown"));
      }

      [Test]
      [Description("OnTooManyInvalidCommands fires when a client passes the limit")]
      public void OnTooManyInvalidCommandsFires()
      {
         var maxInvalid = 5;

         _settings.MaxNumberOfInvalidCommands = maxInvalid;
         _settings.DisconnectInvalidClients = true;

         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnTooManyInvalidCommands(oClient, oMessage) {",
            "   EventLog.Write('OnTooManyInvalidCommands ');",
            "}"));

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var client = new SmtpClientSimulator();
         client.Connect();
         client.Receive();
         client.SendAndReceive("EHLO example.com\r\n");

         for (var i = 0; i < maxInvalid + 1; i++)
            client.SendAndReceive("MAIL FROM\r\n");

         StringAssert.Contains("OnTooManyInvalidCommands", WaitForEventLog("OnTooManyInvalidCommands"));
      }

      [Test]
      [Description("OnBackupCompleted fires when a backup completes")]
      public void OnBackupCompletedFires()
      {
         // The failure handler is there so that a backup which fails - on a Linux
         // server without 7-Zip, which writes the archive and which the packages
         // only recommend - says why in the assertion rather than timing out mute.
         InstallScript(Lines(
            "function OnBackupCompleted() {",
            "   EventLog.Write('Backup process completed');",
            "}",
            "function OnBackupFailed(reason) {",
            "   EventLog.Write('Backup failed: ' + reason);",
            "}"));

         WithTheBackupSettingsPutBack(() =>
         {
            var backup = _settings.Backup;
            var directory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
            Directory.CreateDirectory(directory);

            try
            {
               backup.Destination = directory;
               backup.BackupSettings = true;
               backup.BackupDomains = true;
               backup.BackupMessages = false;
               backup.CompressDestinationFiles = false;

               // Started where this server starts one: over COM on Windows, POST
               // /api/v1/backup on Linux (ScriptLanguages.cs, EventScriptBackups).
               EventScriptBackups.Start(_application);

               var eventLog = WaitForEventLog("Backup process completed");
               StringAssert.Contains("Backup process completed", eventLog, "The backup did not complete. The event log since the test began: " + eventLog);
               StringAssert.DoesNotContain("Backup failed: ", eventLog);
            }
            finally
            {
               try
               {
                  Directory.Delete(directory, true);
               }
               catch (IOException)
               {
                  // Best effort: a temporary directory the backup may still be closing.
               }
            }
         });
      }

      [Test]
      [Description("OnBackupFailed is handed the reason")]
      public void OnBackupFailedHandsOverTheReason()
      {
         InstallScript(Lines(
            "function OnBackupFailed(reason) {",
            "   EventLog.Write('Failed: ' + reason);",
            "}"));

         var missing = Paths.Combine(Path.GetTempPath(), "some-non-existant-directory-" + TestSetup.UniqueString());

         WithTheBackupSettingsPutBack(() =>
         {
            var backup = _settings.Backup;
            backup.Destination = missing;
            backup.BackupSettings = true;

            EventScriptBackups.Start(_application);

            // The reason as a value, whole: the sentence and the directory it names.
            StringAssert.Contains("Failed: The specified backup directory is not accessible: " + missing,
               WaitForEventLog("The specified backup directory is not accessible"));
            CustomAsserts.AssertReportedError("BACKUP ERROR: The specified backup directory is not accessible:");
         });
      }

      [Test]
      [Description("OnClientConnect reads the client")]
      public void OnClientConnectReadsTheClient()
      {
         InstallScript(Lines(
            "function OnClientConnect(oClient) {",
            "   EventLog.Write('Port: ' + oClient.Port);",
            "   EventLog.Write('Address: ' + oClient.IPAddress);",
            "   EventLog.Write('Username: ' + oClient.Username);",
            "}"));

         LogHandler.DeleteEventLog();

         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.IsPortOpen(25));

         var eventLogText = WaitForEventLog("Username: ");
         StringAssert.Contains("Port: " + TestPorts.Actual(25), eventLogText);
         StringAssert.IsMatch("Address: [0-9a-fA-F.:]+", eventLogText);
         // Empty: nobody has logged on when a client connects.
         StringAssert.Contains("Username: \"", eventLogText);
      }

      [Test]
      [Description("An OnClientConnect handler still running on one of the TCP/IP threads when a reinitialise has given " +
                   "up on that thread and gone on without it leaves its port to the servers the reinitialise starts: " +
                   "they greet on that port while the handler still runs, and again once it has ended (as API/Events.cs " +
                   "has it in VBScript). The handler runs for one client address alone, 127.0.0.2, so that the " +
                   "greetings looked for are not held as well.")]
      public void AnOnClientConnectThatOutlastsAReinitialiseLeavesItsPortToTheServersItStarts()
      {
         // Past the stop's give-ups - ten seconds for the listeners' close, ten for
         // the TCP/IP threads - and the start after them, and inside ScriptTimeout's
         // sixty seconds.
         const int holdSeconds = 45;

         InstallScript(Lines(
            "function OnClientConnect(oClient) {",
            "   if (oClient.IPAddress !== '127.0.0.2') return;",
            "   EventLog.Write('OnClientConnect holding');",
            "   var until = Date.now() + " + (holdSeconds * 1000) + ";",
            "   while (Date.now() < until) { }",
            "   EventLog.Write('OnClientConnect released');",
            "}"));

         LogHandler.DeleteEventLog();

         using (var held = new System.Net.Sockets.TcpClient(new System.Net.IPEndPoint(System.Net.IPAddress.Parse("127.0.0.2"), 0)))
         {
            held.Connect(TestPorts.HostAddress, TestPorts.Actual(25));

            StringAssert.Contains("OnClientConnect holding", WaitForEventLog("OnClientConnect holding"),
               "OnClientConnect did not begin for the connection from 127.0.0.2.");

            _application.Reinitialize();

            ClassicAssert.IsFalse(LogHandler.ReadEventLog().Contains("OnClientConnect released"),
               "OnClientConnect had ended before the reinitialise returned, so the reinitialise did not go on without its thread and nothing was tested.");

            string seen;
            ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
               "The servers the reinitialise started did not greet on port 25 within twenty seconds, while the " +
               "OnClientConnect handler the reinitialise gave up on was still running; last seen: " + seen + ".");

            StringAssert.Contains("OnClientConnect released", WaitForEventLog("OnClientConnect released", TimeSpan.FromSeconds(holdSeconds + 15)),
               "OnClientConnect did not end within its span.");

            ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
               "The servers did not greet on port 25 within twenty seconds once the OnClientConnect handler the " +
               "reinitialise gave up on had ended; last seen: " + seen + ".");
         }
      }

      [Test]
      [Description("An OnAcceptMessage handler still running on one of the asynchronous-task queue's threads, finalising " +
                   "a message, when a reinitialise has given up on that thread and gone on without it ends in a server " +
                   "that is still there: the servers the reinitialise started greet on port 25 while the handler still " +
                   "runs, and once it has ended they greet again and take and deliver a message (as API/Events.cs has " +
                   "it in VBScript). Only the message with the subject the handler looks for is held.")]
      public void AnOnAcceptMessageThatOutlastsAReinitialiseLeavesTheServersRunning()
      {
         // As the OnClientConnect test's: past the stop's ten seconds for the
         // asynchronous-task queue's threads and the start after it, and inside
         // ScriptTimeout's sixty seconds.
         const int holdSeconds = 45;
         const string heldSubject = "Held across a reinitialise";

         InstallScript(Lines(
            "function OnAcceptMessage(oClient, oMessage) {",
            "   if (oMessage.Subject !== '" + heldSubject + "') return;",
            "   EventLog.Write('OnAcceptMessage holding');",
            "   var until = Date.now() + " + (holdSeconds * 1000) + ";",
            "   while (Date.now() < until) { }",
            "   EventLog.Write('OnAcceptMessage released');",
            "}"));

         var held = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "held@example.test", "test");
         var after = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "after@example.test", "test");
         string heldAddress = held.Address;

         LogHandler.DeleteEventLog();

         // Not waited for: its "250" is to go out on a connection of the TCP/IP
         // service the reinitialise stops. What becomes of the message is not
         // asserted; that the server is still there afterwards is.
         var sender = System.Threading.Tasks.Task.Run(() =>
         {
            try
            {
               new SmtpClientSimulator().Send(heldAddress, heldAddress, heldSubject, "Body");
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // Deliberately ignored: the session is cut off by the reinitialise, which is the scenario.
            }
         });

         StringAssert.Contains("OnAcceptMessage holding", WaitForEventLog("OnAcceptMessage holding"),
            "OnAcceptMessage did not begin for the message held across the reinitialise.");

         _application.Reinitialize();

         ClassicAssert.IsFalse(LogHandler.ReadEventLog().Contains("OnAcceptMessage released"),
            "OnAcceptMessage had ended before the reinitialise returned, so the reinitialise did not go on without its thread and nothing was tested.");

         string seen;
         ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
            "The servers the reinitialise started did not greet on port 25 within twenty seconds, while the " +
            "OnAcceptMessage handler the reinitialise gave up on was still running; last seen: " + seen + ".");

         StringAssert.Contains("OnAcceptMessage released", WaitForEventLog("OnAcceptMessage released", TimeSpan.FromSeconds(holdSeconds + 15)),
            "OnAcceptMessage did not end within its span.");

         // The rest of the finalisation runs after the handler: a few seconds for it.
         Thread.Sleep(3000);

         ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
            "The servers did not greet on port 25 within twenty seconds once the OnAcceptMessage handler the " +
            "reinitialise gave up on had ended; last seen: " + seen + ".");

         SmtpClientSimulator.StaticSend(after.Address, after.Address, "After the reinitialise", "Body");
         Pop3ClientSimulator.AssertGetFirstMessageText(after.Address, "test");

         sender.Wait(TimeSpan.FromSeconds(1));
      }

      [Test]
      [Description("OnDeliverMessage: a header and the body written and saved")]
      public void OnDeliverMessageWritesAHeaderAndTheBody()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnDeliverMessage(message) {",
            "   message.SetHeaderValue('X-SpamResult', 'TEST2');",
            "   message.Body = 'This is the body text.';",
            "   message.Save();",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains("X-SpamResult: TEST2", message);
         StringAssert.Contains("This is the body text.", message);
      }

      [Test]
      [Description("OnDeliveryFailed is handed the message, the recipient and the error")]
      public void OnDeliveryFailedHandsOverTheRecipientAndTheError()
      {
         LogHandler.DeleteEventLog();

         // Filename, not FileName: JavaScript's property names are the IDL's, and
         // case-sensitive, where JScript reached COM without regard to case.
         InstallScript(Lines(
            "function OnDeliveryFailed(message, sRecipient, sErrorMessage) {",
            "   EventLog.Write('File: ' + message.Filename);",
            "   EventLog.Write('Recipient: ' + sRecipient);",
            "   EventLog.Write('Error: ' + sErrorMessage);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, "user@dummy.example.com", "Test", "SampleBody");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var eventLogText = WaitForEventLog("Error: ");
         StringAssert.Contains("File: ", eventLogText);
         StringAssert.Contains("Recipient: user@dummy.example.com", eventLogText);
         ClassicAssert.IsTrue(eventLogText.Contains("No mail servers appear to exists") ||
                              eventLogText.Contains("Unable to find the recipient's email server"), eventLogText);
      }

      [Test]
      [Description("OnDeliveryStart reads the message")]
      public void OnDeliveryStartReadsTheMessage()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnDeliveryStart(message) {",
            "   EventLog.Write('Delivering message: ' + message.Filename);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         StringAssert.Contains("Delivering message", WaitForEventLog("Delivering message"));
      }

      [Test]
      [Description("OnDeliveryStart can empty the HTML body")]
      public void OnDeliveryStartCanEmptyTheHtmlBody()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnDeliveryStart(message) {",
            "   message.HTMLBody = '';",
            "   EventLog.Write(\"HTMLBody: '\" + message.HTMLBody + \"'\");",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         StringAssert.Contains("HTMLBody: ''", WaitForEventLog("HTMLBody: "));
      }

      [Test]
      [Description("OnError is handed the error")]
      public void OnErrorHandsOverTheError()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnError(iSeverity, iError, sSource, sDescription) {",
            "   EventLog.Write('Severity: ' + iSeverity);",
            "   EventLog.Write('Error: ' + iError);",
            "   EventLog.Write('Source: ' + sSource);",
            "   EventLog.Write('Description: ' + sDescription);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         var inbox = account.IMAPFolders.get_ItemByName("Inbox");

         var deletedMessageText = _application.Settings.ServerMessages.get_ItemByName("MESSAGE_FILE_MISSING").Text;
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         CustomAsserts.AssertFolderMessageCount(inbox, 1);
         var message = inbox.Messages[0];
         File.Delete(message.Filename);

         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains(deletedMessageText.Replace("%MACRO_FILE%", message.Filename), text);
         CustomAsserts.AssertReportedError("Message retrieval failed because message file");

         StringAssert.Contains("Description: Message retrieval failed", WaitForEventLog("Description: Message retrieval failed"));
      }

      [Test]
      [Description("OnExternalAccountDownload decides, through Result, what is kept on the remote server")]
      public void OnExternalAccountDownloadDecidesWhatIsKept()
      {
         LogHandler.DeleteCurrentDefaultLog();

         var messages = new List<string>
         {
            "From: Martin@example1.com\r\nTo: Martin@example.com\r\nSubject: Message 1\r\n\r\nMessage 1!",
            "From: Martin@example2.com\r\nTo: Martin@example.com\r\nSubject: Message 2\r\n\r\nMessage 2!",
            "From: Martin@example3.com\r\nTo: Martin@example.com\r\nSubject: Message 3\r\n\r\nMessage 3!",
         };

         // The second message is kept for five days; the others are deleted as they
         // are downloaded. A message argument that is not there is null.
         InstallScript(Lines(
            "function OnExternalAccountDownload(oFetchAccount, message, sRemoteUID) {",
            "   EventLog.Write('UID: ' + sRemoteUID);",
            "   EventLog.Write('FetchAccount: ' + oFetchAccount.Name);",
            "   if (message !== null) {",
            "      EventLog.Write('From: ' + message.FromAddress);",
            "      EventLog.Write('Filename: ' + message.Filename);",
            "   } else {",
            "      EventLog.Write('Message details missing');",
            "   }",
            "   if (sRemoteUID === 'UniqueID-" + messages[1].GetHashCode() + "') {",
            "      Result.Value = 2;",
            "      Result.Parameter = 5;",
            "   }",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", "test");

         FetchAccount fa;

         using (var pop3Server = new Pop3ServerSimulator(1, FetchPort, messages))
         {
            pop3Server.StartListen();

            fa = account.FetchAccounts.Add();
            fa.Enabled = true;
            fa.MinutesBetweenFetch = 10;
            fa.Name = "TestFA";
            fa.Username = "test@example.com";
            fa.Password = "test";
            fa.UseSSL = false;
            fa.ServerAddress = "localhost";
            fa.Port = FetchPort;
            fa.ProcessMIMERecipients = false;
            fa.DaysToKeepMessages = -1;
            fa.Save();
            fa.DownloadNow();

            pop3Server.WaitForCompletion();

            var logContents = WaitForEventLog("From: Martin@example3.com");
            StringAssert.Contains("FetchAccount: " + fa.Name, logContents);
            StringAssert.Contains("From: Martin@example1.com", logContents);
            StringAssert.Contains("From: Martin@example2.com", logContents);
            StringAssert.Contains("From: Martin@example3.com", logContents);

            ClassicAssert.IsTrue(pop3Server.DeletedMessages.Contains(1));
            ClassicAssert.IsFalse(pop3Server.DeletedMessages.Contains(2));
            ClassicAssert.IsTrue(pop3Server.DeletedMessages.Contains(3));

            ClassicAssert.IsTrue(pop3Server.RetrievedMessages.Contains(1));
            ClassicAssert.IsTrue(pop3Server.RetrievedMessages.Contains(2));
            ClassicAssert.IsTrue(pop3Server.RetrievedMessages.Contains(3));

            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);
         }

         using (var pop3Server = new Pop3ServerSimulator(1, FetchPort, messages))
         {
            pop3Server.StartListen();

            fa.DownloadNow();
            pop3Server.WaitForCompletion();

            // Nothing is downloaded or deleted twice.
            ClassicAssert.AreEqual(0, pop3Server.DeletedMessages.Count);
            ClassicAssert.AreEqual(0, pop3Server.RetrievedMessages.Count);

            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);
         }
      }

      private const string LogonScript =
         "function OnClientLogon(oClient) {\n" +
         "   EventLog.Write('OnClientLogin-' + oClient.Port);\n" +
         "   EventLog.Write('IsAuthenticated: ' + (oClient.Authenticated ? 'True' : 'False'));\n" +
         "   EventLog.Write('Username: ' + oClient.Username);\n" +
         "}\n";

      [Test]
      [Description("OnClientLogon fires for POP3")]
      public void OnClientLogonFiresForPop3()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(LogonScript);
         LogHandler.DeleteEventLog();

         Pop3ClientSimulator.AssertMessageCount("test@example.test", "test", 0);

         var eventLogText = WaitForEventLog("Username: test@example.test");
         StringAssert.Contains("OnClientLogin-", eventLogText);
         StringAssert.Contains("IsAuthenticated: True", eventLogText);
         StringAssert.Contains("Username: test@example.test", eventLogText);
      }

      [Test]
      [Description("OnClientLogon fires for IMAP")]
      public void OnClientLogonFiresForImap()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(LogonScript);
         LogHandler.DeleteEventLog();

         ImapClientSimulator.AssertMessageCount("test@example.test", "test", "Inbox", 0);

         var eventLogText = WaitForEventLog("Username: test@example.test");
         StringAssert.Contains("IsAuthenticated: True", eventLogText);
         StringAssert.Contains("Username: test@example.test", eventLogText);
      }

      [Test]
      [Description("OnClientLogon fires for SMTP")]
      public void OnClientLogonFiresForSmtp()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(LogonScript);
         LogHandler.DeleteEventLog();

         var base64Username = Convert.ToBase64String(Encoding.ASCII.GetBytes("test@example.test"));
         var base64Password = Convert.ToBase64String(Encoding.ASCII.GetBytes("test"));

         var clientSimulator = new SmtpClientSimulator();
         string errMsg;
         clientSimulator.ConnectAndLogon(base64Username, base64Password, out errMsg);

         var eventLogText = WaitForEventLog("Username: test@example.test");
         StringAssert.Contains("IsAuthenticated: True", eventLogText);
         StringAssert.Contains("Username: test@example.test", eventLogText);
      }

      private const string HeloScript =
         "function OnHELO(oClient) {\n" +
         "   EventLog.Write('OnHelo');\n" +
         "   EventLog.Write('HeloHost: ' + oClient.HELO);\n" +
         "}\n";

      [Test]
      [Description("OnHELO reads the host a HELO names")]
      public void OnHeloReadsTheHeloHost()
      {
         InstallScript(HeloScript);
         LogHandler.DeleteEventLog();

         var clientSimulator = new SmtpClientSimulator();
         clientSimulator.Connect();
         clientSimulator.SendAndReceive("HELO WORLD\r\n");

         // The closing quote of the event log's field: HELO's WORLD, not EHLO's WORLD2.
         StringAssert.Contains("HeloHost: WORLD\"", WaitForEventLog("HeloHost: WORLD"));
      }

      [Test]
      [Description("OnHELO reads the host an EHLO names")]
      public void OnHeloReadsTheEhloHost()
      {
         InstallScript(HeloScript);
         LogHandler.DeleteEventLog();

         var clientSimulator = new SmtpClientSimulator();
         clientSimulator.Connect();
         clientSimulator.SendAndReceive("EHLO WORLD2\r\n");

         StringAssert.Contains("HeloHost: WORLD2\"", WaitForEventLog("HeloHost: WORLD2"));
      }

      [Test]
      [Description("OnClientValidatePassword accepts and denies a logon through Result.Value")]
      public void OnClientValidatePasswordAcceptsAndDenies()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "test"));
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));

         // 0 lets the client in, 1 turns it away, whatever the stored password says.
         InstallScript(Lines(
            "function OnClientValidatePassword(account, password) {",
            "   EventLog.Write('Account: ' + account.Address);",
            "   EventLog.Write('Password: ' + password);",
            "   Result.Value = password === 'MySecretPassword' ? 0 : 1;",
            "}"));

         ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));
         ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "test"));

         var eventLogText = WaitForEventLog("Password: MySecretPassword");
         StringAssert.Contains("Account: test@example.test", eventLogText);
         StringAssert.Contains("Password: MySecretPassword", eventLogText);
      }

      // ------------------------------------------------------------------ what the engine promises

      [Test]
      [Description("A handler that never ends is interrupted at ScriptTimeout, and the message still arrives")]
      public void AHandlerThatNeverEndsIsInterruptedAndTheMessageStillArrives()
      {
         // The bench's own value, if it has one, to put back: Delete alone would
         // drop it, and a Write left behind by a Reinitialize that threw before the
         // try would give every later scripting test a two-second limit.
         var previous = _settings.GetIniSetting("ScriptTimeout");

         try
         {
            IniFileSetting.Write("ScriptTimeout", "2");
            _application.Reinitialize();

            InstallScript(Lines(
               "function OnAcceptMessage(client, message) {",
               "   for (;;) {",
               "      try { for (;;) {} } catch (e) { }",
               "   }",
               "}"));

            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loop@example.test", "test");
            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Loop", "SampleBody");

            Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            CustomAsserts.AssertReportedError("HM7341", "OnAcceptMessage", "did not complete within 2 seconds");
         }
         finally
         {
            if (string.IsNullOrEmpty(previous))
               IniFileSetting.Delete("ScriptTimeout");
            else
               IniFileSetting.Write("ScriptTimeout", previous);

            _application.Reinitialize();
         }
      }

      [Test]
      [Description("A handler that allocates past the 64 MB ceiling is stopped and reported, and the message still arrives")]
      public void AHandlerThatAllocatesPastTheCeilingIsStoppedAndTheMessageStillArrives()
      {
         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   const kept = [];",
            "   for (;;) kept.push('x'.repeat(4096) + kept.length);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "grow@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Grow", "SampleBody");

         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         CustomAsserts.AssertReportedError("HM7342", "memory ceiling of 64 MB");
      }

      [Test]
      [Description("A syntax error is named with its line when the script is loaded")]
      public void ASyntaxErrorIsNamedWithItsLineAtLoad()
      {
         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   var fine = 1;",
            "   var broken = ;",
            "}"));

         var check = _application.Settings.Scripting.CheckSyntax();
         StringAssert.Contains("SyntaxError", check);
         StringAssert.Contains("Line: 3", check);

         // The load was refused and said so, with the same line.
         CustomAsserts.AssertReportedError("could not be compiled", "SyntaxError", "Line: 3");
      }

      [Test]
      [Description("CreateObject and new ActiveXObject throw an error that says why")]
      public void CreateObjectThrowsAClearError()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   try { CreateObject('WScript.Shell'); EventLog.Write('CreateObject made an object'); }",
            "   catch (e) { EventLog.Write('CreateObject: ' + e.message); }",
            "   try { new ActiveXObject('Scripting.FileSystemObject'); EventLog.Write('ActiveXObject made an object'); }",
            "   catch (e) { EventLog.Write('ActiveXObject: ' + e.name); }",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "com@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Com", "SampleBody");
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         var eventLogText = WaitForEventLog("ActiveXObject: ");
         StringAssert.Contains("CreateObject: CreateObject and ActiveXObject are not available to JavaScript event scripts", eventLogText);
         StringAssert.Contains("ActiveXObject: TypeError", eventLogText);
         StringAssert.DoesNotContain("made an object", eventLogText);
      }

      [Test]
      [Description("Result.Value 2 with Result.Message refuses a message in OnAcceptMessage with that text")]
      public void ResultRefusesAMessageInOnAcceptMessage()
      {
         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   Result.Value = 2;",
            "   Result.Message = 'Refused by a JavaScript handler';",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "refused@example.test", "test");

         var refusal = Assert.Throws<DeliveryFailedException>(() =>
            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Refused", "SampleBody"));

         StringAssert.Contains("554", refusal.Message);
         StringAssert.Contains("Refused by a JavaScript handler", refusal.Message);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("An exception a handler does not catch is logged with its line and stack, and the event goes on")]
      public void AnExceptionInAHandlerIsLoggedWithItsStackAndTheMessageStillArrives()
      {
         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   var nothing = null;",
            "   return nothing.property;",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "throws@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Throws", "SampleBody");

         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         CustomAsserts.AssertReportedError("Script Error: Source: JavaScript", "TypeError", "Line: 3", "Stack:", "OnAcceptMessage");
      }

      [Test]
      [Description("A message's subject, body and headers edited from JavaScript are written, and read back")]
      public void MessageEditsFromJavaScriptAreWritten()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnAcceptMessage(client, message) {",
            "   message.Subject = '[js] ' + message.Subject;",
            "   message.Body = message.Body + ' - signed in JavaScript';",
            "   message.SetHeaderValue('X-Edited-By', 'JavaScript');",
            "   EventLog.Write('Header read back: ' + message.HeaderValue('X-Edited-By'));",
            "   EventLog.Write('Recipients: ' + message.Recipients.Count + ' ' + message.Recipients.Item(0).Address);",
            "   message.Save();",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "edits@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Hello", "SampleBody");

         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains("Subject: [js] Hello", text);
         StringAssert.Contains("SampleBody", text);
         StringAssert.Contains(" - signed in JavaScript", text);
         StringAssert.Contains("X-Edited-By: JavaScript", text);

         var eventLogText = WaitForEventLog("Recipients: ");
         StringAssert.Contains("Header read back: JavaScript", eventLogText);
         StringAssert.Contains("Recipients: 1 edits@example.test", eventLogText);
      }

      [Test]
      [Description("A handler may be written in ES2023: a class with a private field, an arrow, optional chaining")]
      public void Es2023SyntaxRuns()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "class Stamp {",
            "   #who;",
            "   constructor(who) { this.#who = who; }",
            "   get text() { return `stamped by ${this.#who}`; }",
            "}",
            "const port = (client) => client?.Port ?? 'no client';",
            "const OnAcceptMessage = (client, message) => {",
            "   const seen = [1, 2, 3].findLast((n) => n < 3);",
            "   EventLog.Write(`${new Stamp('es2023').text} on ${port(client)}, ${seen}, ${port(null)}`);",
            "};"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "modern@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Modern", "SampleBody");
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         StringAssert.Contains("stamped by es2023 on " + TestPorts.Actual(25) + ", 2, no client", WaitForEventLog("stamped by es2023"));
      }

      [Test]
      [Description("A value is handed to the handler as it is: a password with quotes and a backslash")]
      public void APasswordWithQuotesAndABackslashReachesTheHandlerAsItIs()
      {
         // A value is passed to a JavaScript handler, not pasted into its source, so
         // there is nothing for a quote to end. Sent over SMTP AUTH LOGIN, in base64,
         // so that the protocol carries the password byte for byte.
         var password = "it's \"quoted\" \\ ');EventLog.Write('injected";

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quotes@example.test", "test");

         InstallScript(Lines(
            "function OnClientValidatePassword(account, password) {",
            "   EventLog.Write('Password length: ' + password.length);",
            "   Result.Value = 1;",
            "}"));

         LogHandler.DeleteEventLog();

         // The handler's Result.Value 1 refuses the logon, and ConnectAndLogon throws
         // when it is refused.
         var clientSimulator = new SmtpClientSimulator();
         Assert.Catch(() => clientSimulator.ConnectAndLogon(Convert.ToBase64String(Encoding.ASCII.GetBytes("quotes@example.test")),
            Convert.ToBase64String(Encoding.ASCII.GetBytes(password)), out _));

         var eventLogText = WaitForEventLog("Password length: ");
         StringAssert.Contains("Password length: " + password.Length, eventLogText);
         StringAssert.DoesNotContain("injected", eventLogText);
      }

      // ------------------------------------------------------------------ ported: API/Basics.cs

      [Test]
      [Description("EventLog.Write writes the event log as UTF-16 with its byte order mark on Windows, and as UTF-8 without one elsewhere (Basics.TestEventLog)")]
      public void EventLogIsWrittenAsUtf16()
      {
         InstallScript(Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   EventLog.Write('HOWDY');",
            "}"));

         var eventLogFile = _settings.Logging.CurrentEventLog;
         LogHandler.DeleteEventLog();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         StringAssert.Contains("HOWDY", WaitForEventLog("HOWDY"));

         byte[] bytes;
         using (var stream = new FileStream(eventLogFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
         using (var copy = new MemoryStream())
         {
            stream.CopyTo(copy);
            bytes = copy.ToArray();
         }

         if (Environment.OSVersion.Platform == PlatformID.Win32NT)
         {
            // UTF-16LE after its byte order mark, as it always was on Windows.
            ClassicAssert.AreEqual(255, bytes[0]);
            ClassicAssert.AreEqual(254, bytes[1]);
         }
         else
         {
            // UTF-8 with no mark elsewhere, as the application log is, from 6.3.6
            // (Logger.cpp, WritesUtf16_): no FF FE and no EF BB BF at the start, and
            // not a NUL second byte under a first that is not, which is how the
            // server itself tells a UTF-16 log without its mark (ConvertUtf16Log_).
            // The file is the server's whole life's event log here (DeleteEventLog
            // only takes a mark), so its first bytes are those of its first line.
            ClassicAssert.IsTrue(bytes.Length >= 2, "the event log is shorter than two bytes");
            ClassicAssert.IsFalse(bytes[0] == 0xFF && bytes[1] == 0xFE, "the event log starts with UTF-16's byte order mark");
            ClassicAssert.IsFalse(bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF, "the event log starts with a UTF-8 byte order mark");
            ClassicAssert.IsFalse(bytes[0] != 0 && bytes[1] == 0, "the event log starts as UTF-16 without its mark");
            StringAssert.Contains("HOWDY", Encoding.UTF8.GetString(bytes));
         }
      }

      [Test]
      [Description("Message.InternalDate is the time the server stored the message (Basics.TestInternalDateCombinedWithOnDeliverMessage)")]
      public void InternalDateIsTheTimeTheMessageWasStored()
      {
         LogHandler.DeleteEventLog();

         InstallScript(Lines(
            "function OnDeliverMessage(message) {",
            "   EventLog.Write(message.InternalDate);",
            "}"));

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         var text = WaitForEventLog(DateTime.Now.Year.ToString());
         var columns = text.Split('\t');
         ClassicAssert.AreEqual(3, columns.Length, "Wrong number of columns: " + text);

         var lastColumn = columns[columns.Length - 1];
         ClassicAssert.IsFalse(lastColumn.Contains("00:00:00"), lastColumn);
         ClassicAssert.IsTrue(lastColumn.Contains(DateTime.Now.Year.ToString()), lastColumn);
         ClassicAssert.IsTrue(lastColumn.Contains(DateTime.Now.Month.ToString()), lastColumn);
         ClassicAssert.IsTrue(lastColumn.Contains(DateTime.Now.Day.ToString()), lastColumn);
      }

      // ------------------------------------------------------------------ ported: API/Message.cs

      private const string Signature = "MySignature";

      private static string SignatureScript()
      {
         return Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   EventLog.Write('Subject:' + message.Subject);",
            "   EventLog.Write('Date:' + message.Date);",
            "   EventLog.Write('Body:' + message.Body);",
            "   message.Body = message.Body + '" + Signature + "';",
            "   message.Save();",
            "}");
      }

      [Test]
      [Description("Text added to the body while a message is sent is written (Message.TestAddTextDuringSending)")]
      public void TextAddedToTheBodyWhileAMessageIsSentIsWritten()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(SignatureScript());
         ClassicAssert.IsEmpty(_application.Settings.Scripting.CheckSyntax());

         SmtpClientSimulator.StaticSend("test@example.test", new List<string> { "test@example.test" }, "Hej", "Välkommen till verkligheten");

         var firstMessageText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains(Signature, firstMessageText);
         StringAssert.Contains("Hej", firstMessageText);
      }

      private void SendWithAnAttachment(string body)
      {
         var assembly = Assembly.GetExecutingAssembly();

         using var mail = new MailMessage();
         mail.From = new MailAddress("test@test.se");
         mail.To.Add("test@example.test");
         mail.Subject = "Automatiskt servertest";
         mail.Body = body;
         mail.BodyEncoding = Encoding.UTF8;
         mail.SubjectEncoding = Encoding.UTF8;
         mail.Attachments.Add(new System.Net.Mail.Attachment(assembly.Location));

         using var client = new SmtpClient(TestPorts.Host, TestPorts.Actual(25));
         client.Send(mail);
      }

      [Test]
      [Description("Text added to the body of a message with an attachment is written (Message.TestAddTextDuringSendingAttachment)")]
      public void TextAddedToTheBodyOfAMessageWithAnAttachmentIsWritten()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(SignatureScript());

         SendWithAnAttachment("Detta är ett automatiskt test av epostservern.");

         var firstMessageText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains(Signature, firstMessageText, "The message retrieved from POP3 server does not contain signature.");
      }

      [Test]
      [Description("Text added to an empty body of a message with an attachment is written (Message.TestAddTextToEmptyBody)")]
      public void TextAddedToAnEmptyBodyIsWritten()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(SignatureScript());

         SendWithAnAttachment("");

         var firstMessageText = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains(Signature, firstMessageText, "The message retrieved from POP3 server does not contain signature.");
      }

      [Test]
      [Description("A subject is updated on a message with no message-wide character set (Message.TestUpdateSubjectOnMessageWithNoMessageWideCharacterSet)")]
      public void ASubjectIsUpdatedOnAMessageWithNoMessageWideCharacterSet()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "encode@example.test", "test");

         InstallScript(Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   message.Subject = '[ov]' + message.Subject;",
            "   message.Save();",
            "}"));
         ClassicAssert.IsEmpty(_application.Settings.Scripting.CheckSyntax());

         var body = "From: <test@example.com>\r\n" +
                    "Subject: =?windows-1251?B?yuDr7Pvq7uLzIMji4O3zIC0g7/Do7OXwICLy5fXt6Pfl8eru4+4g8OX4?=\r\n" +
                    "   =?windows-1251?B?5e3o/yIgW0Z3ZDog0tAg4uXw8ejoIDEuMl0=?=\r\n" +
                    "\r\n" +
                    "Hej!\r\n";

         SmtpClientSimulator.StaticSendRaw("encode@example.test", "encode@example.test", body);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);
         CustomAsserts.AssertFolderMessageCount(account.IMAPFolders[0], 1);

         // What the original reads: the rewritten subject, stored in UTF-8 because
         // the message named no character set of its own, has to be readable - and,
         // which the original never asked, has to be the rewritten one.
         StringAssert.StartsWith("[ov]", account.IMAPFolders[0].Messages[0].Subject);
      }

      // ------------------------------------------------------------------ ported: IMAP/MessageUids.cs

      private void CreateMessageModificationRule(hMailServer.Rules ruleContainer)
      {
         var rule = ruleContainer.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRARunScriptFunction;
         ruleAction.ScriptFunction = "ModifyMessage";
         ruleAction.Save();

         rule.Save();

         // The rule's function, found by name and called with the message.
         InstallScript(Lines(
            "function ModifyMessage(message) {",
            "   message.Subject = '[Spam] ' + message.Subject;",
            "   message.Save();",
            "}"));
      }

      private static void CreateMoveRule(hMailServer.Rules ruleContainer, string foldername)
      {
         var rule = ruleContainer.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = foldername;
         ruleAction.Save();

         rule.Save();
      }

      [Test]
      [Description("A message a rule's JavaScript function saved and an account rule moved gets a UID of the new folder (MessageUids)")]
      public void ARuleFunctionSavesAMessageThatAnAccountRuleMoves()
      {
         var testAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "Test'Account@example.test", "test");

         var sim = new ImapClientSimulator();
         ClassicAssert.IsTrue(sim.ConnectAndLogon(testAccount.Address, "test"));

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");
         Pop3ClientSimulator.AssertMessageCount(testAccount.Address, "test", 1);
         var inboxFolder = testAccount.IMAPFolders[0];
         ClassicAssert.AreEqual(1, inboxFolder.CurrentUID);
         ClassicAssert.AreEqual(1, inboxFolder.Messages[0].UID);
         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 2"));

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");
         Pop3ClientSimulator.AssertMessageCount(testAccount.Address, "test", 2);
         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 3"));
         ClassicAssert.AreEqual(2, inboxFolder.CurrentUID);
         ClassicAssert.AreEqual(2, inboxFolder.Messages[1].UID);

         CreateMessageModificationRule(testAccount.Rules);
         CreateMoveRule(testAccount.Rules, "TestFolder");

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");

         CustomAsserts.AssertFolderExists(testAccount.IMAPFolders, "TestFolder");

         var testFolder = testAccount.IMAPFolders.get_ItemByName("TestFolder");
         CustomAsserts.AssertFolderMessageCount(testFolder, 1);

         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 3"));
         ClassicAssert.IsTrue(sim.Status("TestFolder", "UIDNEXT").Contains("UIDNEXT 2"));
         ClassicAssert.AreEqual(1, testFolder.Messages.Count);
         ClassicAssert.AreEqual(1, testFolder.CurrentUID);
         ClassicAssert.AreEqual(1, testFolder.Messages[0].UID);

         // And the function ran.
         StringAssert.StartsWith("[Spam] ", testFolder.Messages[0].Subject);
      }

      [Test]
      [Description("A message a rule's JavaScript function saved and a global rule moved gets a UID of the new folder (MessageUids)")]
      public void ARuleFunctionSavesAMessageThatAGlobalRuleMoves()
      {
         var testAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "Test'Account@example.test", "test");

         var sim = new ImapClientSimulator();
         ClassicAssert.IsTrue(sim.ConnectAndLogon(testAccount.Address, "test"));

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");
         Pop3ClientSimulator.AssertMessageCount(testAccount.Address, "test", 1);
         var inboxFolder = testAccount.IMAPFolders[0];
         ClassicAssert.AreEqual(1, inboxFolder.CurrentUID);
         ClassicAssert.AreEqual(1, inboxFolder.Messages[0].UID);
         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 2"));

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");
         Pop3ClientSimulator.AssertMessageCount(testAccount.Address, "test", 2);
         ClassicAssert.AreEqual(2, inboxFolder.CurrentUID);
         ClassicAssert.AreEqual(2, inboxFolder.Messages[1].UID);
         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 3"));

         CreateMessageModificationRule(_application.Rules);
         CreateMoveRule(_application.Rules, "TestFolder");

         SmtpClientSimulator.StaticSend(testAccount.Address, testAccount.Address, "Test", "Test");

         CustomAsserts.AssertFolderExists(testAccount.IMAPFolders, "TestFolder");
         var testFolder = testAccount.IMAPFolders.get_ItemByName("TestFolder");
         CustomAsserts.AssertFolderMessageCount(testFolder, 1);

         ClassicAssert.IsTrue(sim.Status("INBOX", "UIDNEXT").Contains("UIDNEXT 3"));

         var status = sim.Status("TestFolder", "UIDNEXT");
         ClassicAssert.IsTrue(status.Contains("UIDNEXT 2"), status);
         ClassicAssert.AreEqual(1, testFolder.CurrentUID);
         ClassicAssert.AreEqual(1, testFolder.Messages[0].UID);

         StringAssert.StartsWith("[Spam] ", testFolder.Messages[0].Subject);
      }

      // ------------------------------------------------------------------ ported: API/ScriptReload.cs

      private const string VersionHeader = "X-Script-Version";

      private static string AcceptMessageScript(string version)
      {
         return Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   message.SetHeaderValue('" + VersionHeader + "', '" + version + "');",
            "   message.Save();",
            "}");
      }

      // The closing brace left off - the shape a reload typo usually takes. It still
      // mentions OnAcceptMessage, so a delivered message carrying "second" would say
      // the broken script had been installed rather than merely advertised.
      private static string BrokenAcceptMessageScript()
      {
         return Lines(
            "function OnAcceptMessage(oClient, message) {",
            "   message.SetHeaderValue('" + VersionHeader + "', 'second');",
            "   message.Save();");
      }

      private static string LogonScriptVersion(string version)
      {
         return Lines(
            "function OnClientLogon(oClient) {",
            "   EventLog.Write('logon-handler-" + version + "');",
            "}");
      }

      private static string BrokenLogonScript()
      {
         return Lines(
            "function OnClientLogon(oClient) {",
            "   EventLog.Write('logon-handler-second');");
      }

      [Test]
      [Description("A reload that fails to compile leaves the previous OnAcceptMessage handler working (ScriptReload)")]
      public void RejectedReloadKeepsThePreviousAcceptHandlerRunning()
      {
         var before = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "before@example.test", "test");
         var after = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "after@example.test", "test");

         InstallScript(AcceptMessageScript("first"));

         SmtpClientSimulator.StaticSend(before.Address, before.Address, "Test", "SampleBody");
         StringAssert.Contains(VersionHeader + ": first", Pop3ClientSimulator.AssertGetFirstMessageText(before.Address, "test"));

         InstallScript(BrokenAcceptMessageScript());
         ClassicAssert.IsFalse(string.IsNullOrEmpty(_application.Settings.Scripting.CheckSyntax()),
            "the broken script was expected not to compile");
         CustomAsserts.AssertReportedError("HM5710");

         SmtpClientSimulator.StaticSend(after.Address, after.Address, "Test", "SampleBody");
         var secondMessage = Pop3ClientSimulator.AssertGetFirstMessageText(after.Address, "test");

         StringAssert.Contains(VersionHeader + ": first", secondMessage);
         StringAssert.DoesNotContain(VersionHeader + ": second", secondMessage);
      }

      [Test]
      [Description("A reload that fails to compile leaves the previous OnClientLogon handler working (ScriptReload)")]
      public void RejectedReloadKeepsTheLogonHandlerRunning()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         InstallScript(LogonScriptVersion("first"));

         Pop3ClientSimulator.AssertMessageCount("test@example.test", "test", 0);
         StringAssert.Contains("logon-handler-first", WaitForEventLog("logon-handler-first"));
         LogHandler.DeleteEventLog();

         InstallScript(BrokenLogonScript());
         ClassicAssert.IsFalse(string.IsNullOrEmpty(_application.Settings.Scripting.CheckSyntax()),
            "the broken script was expected not to compile");
         CustomAsserts.AssertReportedError("HM5710");

         Pop3ClientSimulator.AssertMessageCount("test@example.test", "test", 0);

         StringAssert.Contains("logon-handler-first", WaitForEventLog("logon-handler-first"));
      }

      [Test]
      [Description("A rejected reload is reported as HM5710 (ScriptReload)")]
      public void RejectedReloadIsReported()
      {
         InstallScript(AcceptMessageScript("first"));

         LogHandler.DeleteErrorLog();

         InstallScript(BrokenAcceptMessageScript());

         CustomAsserts.AssertReportedError("HM5710");
      }

      [Test]
      [Description("A VBScript script is refused at load on this build with the sentence that says to set JavaScript, and the JavaScript script in force keeps running")]
      public void AVBScriptScriptIsRefusedAtLoadSayingToSetJavaScript()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "vbscript@example.test", "test");

         InstallScript(AcceptMessageScript("first"));

         // Scripting is on from InstallScript; the language changes under it, as an
         // administrator carrying a Windows configuration over would change it.
         var scripting = _application.Settings.Scripting;
         scripting.Language = "VBScript";
         File.WriteAllText(scripting.CurrentScriptFile,
            "Sub OnAcceptMessage(oClient, oMessage)\r\n" +
            "   oMessage.HeaderValue(\"" + VersionHeader + "\") = \"vbscript\"\r\n" +
            "   oMessage.Save\r\n" +
            "End Sub\r\n");
         scripting.Reload();

         CustomAsserts.AssertReportedError("HM5710", "runs event scripts in JavaScript only", "Set the script language to JavaScript");

         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");
         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         StringAssert.Contains(VersionHeader + ": first", text);
         StringAssert.DoesNotContain(VersionHeader + ": vbscript", text);
      }

      [Test]
      [Description("A reload that does compile replaces the previous script (ScriptReload)")]
      public void GoodReloadReplacesThePreviousScript()
      {
         var before = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "before@example.test", "test");
         var after = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "after@example.test", "test");

         InstallScript(AcceptMessageScript("first"));

         SmtpClientSimulator.StaticSend(before.Address, before.Address, "Test", "SampleBody");
         StringAssert.Contains(VersionHeader + ": first", Pop3ClientSimulator.AssertGetFirstMessageText(before.Address, "test"));

         InstallScript(AcceptMessageScript("second"));

         SmtpClientSimulator.StaticSend(after.Address, after.Address, "Test", "SampleBody");
         StringAssert.Contains(VersionHeader + ": second", Pop3ClientSimulator.AssertGetFirstMessageText(after.Address, "test"));
      }

      // ------------------------------------------------------------------ ported: Infrastructure/SessionLimitingTests.cs

      [Test]
      [Description("OnClientConnect's Result.Value 1 disconnects the client and the session count falls back (SessionLimitingTests)")]
      public void OnClientConnectResultDisconnectsAndTheSessionCountFalls()
      {
         // POP3's count: the connection below is to port 110. The original counts
         // SMTP sessions, which a POP3 connection never changes.
         var countBefore = _application.Status.get_SessionCount(eSessionType.eSTPOP3);

         InstallScript(Lines(
            "function OnClientConnect(oClient) {",
            "   Result.Value = 1;",
            "}"));

         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(110));
         ClassicAssert.IsEmpty(socket.Receive());

         Infrastructure.SessionLimitingTests.AssertMaxSessionCount(eSessionType.eSTPOP3, countBefore);
      }

      // ------------------------------------------------------------------ ported: POP3/ProtocolConformance.cs

      [Test]
      [Description("A failed SCRAM-SHA-256 logon fires OnClientLogon with Authenticated false (ProtocolConformance)")]
      public void FailedScramLogonFiresOnClientLogon()
      {
         var settings = _application.Settings;
         var originalAutoBan = settings.AutoBanOnLogonFailure;
         settings.AutoBanOnLogonFailure = false;
         settings.ClearLogonFailureList();

         try
         {
            SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "scramevent@example.test", "correct horse");

            InstallScript(Lines(
               "function OnClientLogon(oClient) {",
               "   EventLog.Write('POP3-Logon-Event');",
               "   EventLog.Write('IsAuthenticated: ' + (oClient.Authenticated ? 'True' : 'False'));",
               "   EventLog.Write('Username: ' + oClient.Username);",
               "}"));

            LogHandler.DeleteEventLog();

            var tc = new TcpConnection();
            ClassicAssert.IsTrue(tc.Connect(110));
            tc.ReadUntil("+OK");

            var final = Pop3SaslTestClient.AuthenticateScram(tc, "scramevent@example.test", "wrong password");
            ClassicAssert.IsTrue(final.TrimStart().StartsWith("-ERR"), "SCRAM with a wrong password must be rejected. Got: " + final);
            tc.Disconnect();

            var log = WaitForEventLog("Username: scramevent@example.test");
            StringAssert.Contains("POP3-Logon-Event", log);
            StringAssert.Contains("IsAuthenticated: False", log);
            StringAssert.Contains("Username: scramevent@example.test", log);

            LogHandler.DeleteEventLog();

            var tc2 = new TcpConnection();
            ClassicAssert.IsTrue(tc2.Connect(110));
            tc2.ReadUntil("+OK");
            var ok = Pop3SaslTestClient.AuthenticateScram(tc2, "scramevent@example.test", "correct horse");
            ClassicAssert.IsTrue(ok.TrimStart().StartsWith("+OK"), "SCRAM with the correct password must succeed. Got: " + ok);
            tc2.Send("QUIT\r\n");
            tc2.Disconnect();

            var successLog = WaitForEventLog("IsAuthenticated: True");
            StringAssert.Contains("IsAuthenticated: True", successLog);
            StringAssert.DoesNotContain("IsAuthenticated: False", successLog);
         }
         finally
         {
            settings.AutoBanOnLogonFailure = originalAutoBan;
            settings.ClearLogonFailureList();
         }
      }

      // ------------------------------------------------------------------ ported: Infrastructure/ErrorManagerDiagnosticsTests.cs

      [Test]
      [Description("An error whose description spans lines still reaches the OnError handler (ErrorManagerDiagnosticsTests)")]
      public void AMultiLineErrorDescriptionReachesTheOnErrorHandler()
      {
         const string marker = "ONERROR_MARKER";

         LogHandler.DeleteEventLog();

         // The script in force when the error happens.
         InstallScript(Lines(
            "function OnError(iSeverity, iCode, sSource, sDescription) {",
            "   EventLog.Write('" + marker + " code=' + iCode + ' source=' + sSource + ' description=' + sDescription);",
            "}"));

         // A script that cannot compile: the reload fails, it is not installed, and
         // the failure - HM5710, whose description carries a line break - is
         // reported, which fires OnError in the script above.
         var scripting = _application.Settings.Scripting;
         File.WriteAllText(scripting.CurrentScriptFile, Lines(
            "function OnError(iSeverity, iCode, sSource, sDescription) {",
            "}",
            "",
            "function ThisWillNotCompile("));
         scripting.Reload();

         var eventLog = WaitForEventLog(marker);
         StringAssert.Contains(marker, eventLog, "The OnError handler was never reached.");
         StringAssert.Contains("code=5710", eventLog);
         StringAssert.Contains("source=ScriptServer::LoadScripts", eventLog);
         StringAssert.Contains("[nl]", eventLog);
         StringAssert.Contains("could not be compiled", eventLog);

         CustomAsserts.AssertReportedError("HM5710");
      }
   }
}
