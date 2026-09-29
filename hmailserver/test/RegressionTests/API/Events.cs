// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   [TestFixture]
   public class Events : TestFixtureBase
   {
      [Test]
      public void TestOnAcceptMessageJScript()
      {
         LogHandler.DeleteEventLog();

         _settings.Scripting.Language = "JScript";
         // First set up a script
         var script =
            @"function OnAcceptMessage(oClient, message)
                           {
                              message.HeaderValue('X-SpamResult') = 'TEST';
                              message.Save();
                           }";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         SmtpClientSimulator.StaticSend(account1.Address, account1.Address, "Test", "SampleBody");

         // Check that the message exists
         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsNotEmpty(message);

         Assert.Less(0, message.IndexOf("X-SpamResult: TEST"));
      }

      [Test]
      public void TestOnAcceptMessageVBScript()
      {
         LogHandler.DeleteEventLog();

         // First set up a script
         var script =
            @"Sub OnAcceptMessage(oClient, message)
                               message.HeaderValue(""X-SpamResult"") = ""TEST""
                               message.Save()
                               EventLog.Write(""Port: "" & oClient.Port)
                               EventLog.Write(""Address: "" & oClient.IPAddress)
                               EventLog.Write(""Username: "" & oClient.Username)
                               EventLog.Write(""SessionId: "" & oClient.SessionID)
                              End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         SmtpClientSimulator.StaticSend(account1.Address, account1.Address, "Test", "SampleBody");

         // Check that the message exists
         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsNotEmpty(message);

         Assert.Less(0, message.IndexOf("X-SpamResult: TEST"));


         // Check that the message exists
         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());

         Assert.IsNotEmpty(eventLogText);
         Assert.IsTrue(eventLogText.Contains("Port: 25"));
         Assert.IsTrue(eventLogText.Contains("Address: 127"));
         Assert.IsTrue(
            eventLogText.Contains("Username: \"")); // Should be empty, Username isn't available at this time.
         StringAssert.IsMatch(".*\"SessionId: \\d+\"", eventLogText);
      }

      [Test]
      public void TestOnRecipientUnknownVBScript()
      {
         var eventLogFile = _settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // First set up a script
         var script =
            @"Sub OnRecipientUnknown(oClient, oMessage)
                               EventLog.Write(""OnRecipientUnknown "")
                              End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         try
         {
            SmtpClientSimulator.StaticSend(account1.Address, "nonexistent@example.test", "Test", "SampleBody");
         }
         catch (DeliveryFailedException)
         {
            // Expected, since recipient does not exist.
         }

         // Check that the event was triggered
         var message = TestSetup.ReadExistingTextFile(eventLogFile);
         Assert.IsTrue(message.Contains("OnRecipientUnknown"));
      }

      [Test]
      public void TestOnTooManyInvalidCommands()
      {
         var maxInvalid = 5;

         _settings.MaxNumberOfInvalidCommands = maxInvalid;
         _settings.DisconnectInvalidClients = true;

         var eventLogFile = _settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // First set up a script
         var script =
            @"Sub OnTooManyInvalidCommands(oClient, oMessage)
                               EventLog.Write(""OnTooManyInvalidCommands "")
                              End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var client = new SmtpClientSimulator();
         client.Connect();
         client.Receive(); // Welcome banner
         var ehloResponse = client.SendAndReceive("EHLO example.com\r\n");

         for (var i = 0; i < maxInvalid + 1; i++) client.SendAndReceive("MAIL FROM\r\n");

         // Check that the event was triggered
         var message = TestSetup.ReadExistingTextFile(eventLogFile);
         Assert.IsTrue(message.Contains("OnTooManyInvalidCommands"));
      }


      [Test]
      public void TestOnBackupCompletedJScript()
      {
         LogHandler.DeleteEventLog();

         var scripting = _settings.Scripting;
         scripting.Language = "JScript";

         // First set up a script
         var script =
            @"function OnBackupCompleted()
                           {
                               EventLog.Write('Backup process completed')
                           }";


         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         var back = new BackupRestore();
         back.InitializeBackupSettings();
         back.TestWithMessages();


         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("Backup process completed"));
      }

      [Test]
      public void TestOnBackupCompletedVBScript()
      {
         LogHandler.DeleteEventLog();

         // First set up a script
         var script =
            @"Sub OnBackupCompleted()
                               EventLog.Write(""Backup process completed"")
                           End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         var back = new BackupRestore();
         back.InitializeBackupSettings();
         back.TestWithMessages();


         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("Backup process completed"));
      }

      [Test]
      public void TestOnBackupFailedJScript()
      {
         LogHandler.DeleteEventLog();

         var scripting = _settings.Scripting;
         scripting.Language = "JScript";

         // First set up a script
         var script =
            @"function OnBackupFailed(reason)     
                           {
                               EventLog.Write('Failed: ' + reason)
                           }";


         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         var back = new BackupRestore();
         back.InitializeBackupSettings();
         back.SetBackupDir(@"C:\some-non-existant-directory");
         Assert.IsFalse(back.Execute());

         CustomAsserts.AssertReportedError("BACKUP ERROR: The specified backup directory is not accessible:");
         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("The specified backup directory is not accessible"));
      }

      [Test]
      public void TestOnBackupFailedVBScript()
      {
         LogHandler.DeleteEventLog();

         // First set up a script
         var script =
            @"Sub OnBackupFailed(reason)
                               EventLog.Write(""Failed: "" & reason)
                           End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         var back = new BackupRestore();
         back.InitializeBackupSettings();
         back.SetBackupDir(@"C:\some-non-existant-directory");
         Assert.IsFalse(back.Execute());

         CustomAsserts.AssertReportedError("BACKUP ERROR: The specified backup directory is not accessible:");
         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("The specified backup directory is not accessible"));
      }

      [Test]
      public void TestOnClientConnectJScript()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         scripting.Language = "JScript";

         var script = "function OnClientConnect(oClient) " + Environment.NewLine +
                      "{" + Environment.NewLine +
                      " EventLog.Write('Port: ' + oClient.Port); " + Environment.NewLine +
                      " EventLog.Write('Address: ' + oClient.IPAddress); " + Environment.NewLine +
                      " EventLog.Write('Username: ' + oClient.Username); " + Environment.NewLine +
                      "}" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         var socket = new TcpConnection();
         Assert.IsTrue(socket.IsPortOpen(25));

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("Port: 25"));
         Assert.IsTrue(message.Contains("Address: 127"));
         Assert.IsTrue(message.Contains("Username: \"")); // Should be empty, Username isn't available at this time.
      }

      [Test]
      public void TestOnClientConnectVBScript()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnClientConnect(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"Port: \" & oClient.Port) " + Environment.NewLine +
                      " EventLog.Write(\"Address: \" & oClient.IPAddress) " + Environment.NewLine +
                      " EventLog.Write(\"Username: \" & oClient.Username) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var socket = new TcpConnection();
         Assert.IsTrue(socket.IsPortOpen(25));

         // Check that the message exists
         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());

         Assert.IsNotEmpty(eventLogText);
         Assert.IsTrue(eventLogText.Contains("Port: 25"));
         Assert.IsTrue(eventLogText.Contains("Address: 127"));
         Assert.IsTrue(
            eventLogText.Contains("Username: \"")); // Should be empty, Username isn't available at this time.
      }

      [Test]
      public void TestOnDeliverMessageJScript()
      {
         LogHandler.DeleteEventLog();

         var scripting = _settings.Scripting;
         scripting.Language = "JScript";
         // First set up a script
         var script =
            @"function OnDeliverMessage(message)
                           {
                               message.HeaderValue('X-SpamResult') = 'TEST2';
                               message.Body = 'This is the body text.';
                               message.Save();
                           }";


         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         SmtpClientSimulator.StaticSend(account1.Address, account1.Address, "Test", "SampleBody");

         // Check that the message exists
         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsNotEmpty(message);

         StringAssert.Contains("X-SpamResult: TEST2", message);
         StringAssert.Contains("This is the body text.", message);
      }

      [Test]
      public void TestOnDeliveryFailedJScript()
      {
         LogHandler.DeleteEventLog();

         var scripting = _settings.Scripting;
         scripting.Language = "JScript";

         // First set up a script
         var script = "function OnDeliveryFailed(message, sRecipient, sErrorMessage) {" + Environment.NewLine +
                      " EventLog.Write('File: ' + message.FileName); " + Environment.NewLine +
                      " EventLog.Write('Recipient: ' + sRecipient); " + Environment.NewLine +
                      " EventLog.Write('Error: ' + sErrorMessage); " + Environment.NewLine +
                      "}";


         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account1.Address, "user@dummy.example.com", "Test", "SampleBody");

         // Make sure that the message is deliverd and bounced.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("File: "), eventLogText);
         Assert.IsTrue(eventLogText.Contains("Recipient: user@dummy.example.com"), eventLogText);
         Assert.IsTrue(eventLogText.Contains("No mail servers appear to exists") ||
                       eventLogText.Contains("Unable to find the recipient's email server"), eventLogText);
      }

      [Test]
      public void TestOnDeliveryFailedVBScript()
      {
         LogHandler.DeleteEventLog();

         // First set up a script
         var script = "Sub OnDeliveryFailed(message, sRecipient, sErrorMessage)" + Environment.NewLine +
                      " EventLog.Write(\"File: \" & message.FileName) " + Environment.NewLine +
                      " EventLog.Write(\"Recipient: \" & sRecipient) " + Environment.NewLine +
                      " EventLog.Write(\"Error: \" & sErrorMessage) " + Environment.NewLine +
                      " End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         // Add an account and send a message to it.
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account1.Address, "user@dummy.example.com", "Test", "SampleBody");

         // Make sure that the message is deliverd and bounced.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("File: "));
         Assert.IsTrue(eventLogText.Contains("Recipient: user@dummy.example.com"));
         Assert.IsTrue(eventLogText.Contains("No mail servers appear to exists") ||
                       eventLogText.Contains("Unable to find the recipient's email server"), eventLogText);
      }

      [Test]
      public void TestOnDeliveryStartVBScript()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnDeliveryStart(message) " + Environment.NewLine +
                      " EventLog.Write(\"Delivering message: \" & message.FileName) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         // Wait for the message to be delivered.
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         var eventLogText = TestSetup.ReadExistingTextFile(app.Settings.Logging.CurrentEventLog);
         Assert.IsTrue(eventLogText.Contains("Delivering message"));
      }

      [Test]
      public void TestOnDeliveryStart_SetHtmlBodyEmpty()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnDeliveryStart(message) " + Environment.NewLine +
                      " message.HTMLBody = \"\" " + Environment.NewLine +
                      " EventLog.Write(\"HTMLBody: '\" & message.HTMLBody & \"'\")" + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         // Wait for the message to be delivered.
         Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         var eventLogText = TestSetup.ReadExistingTextFile(app.Settings.Logging.CurrentEventLog);
         StringAssert.Contains("HTMLBody: ''", eventLogText);
      }

      [Test]
      public void TestOnErrorJScript()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;
         scripting.Language = "JScript";
         var script = "function OnError(iSeverity, iError, sSource, sDescription) {" + Environment.NewLine +
                      " EventLog.Write('Severity: ' + iSeverity) " + Environment.NewLine +
                      " EventLog.Write('Error: ' + iError) " + Environment.NewLine +
                      " EventLog.Write('Source: ' + sSource) " + Environment.NewLine +
                      " EventLog.Write('Description: ' + sDescription) " + Environment.NewLine +
                      "}" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         var inbox = account.IMAPFolders.get_ItemByName("Inbox");


         var deletedMessageText = app.Settings.ServerMessages.get_ItemByName("MESSAGE_FILE_MISSING").Text;
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         CustomAsserts.AssertFolderMessageCount(inbox, 1);
         var message = inbox.Messages[0];
         File.Delete(message.Filename);
         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains(deletedMessageText.Replace("%MACRO_FILE%", message.Filename)));
         CustomAsserts.AssertReportedError("Message retrieval failed because message file");

         var eventLogText = TestSetup.ReadExistingTextFile(app.Settings.Logging.CurrentEventLog);
         Assert.IsTrue(eventLogText.Contains("Description: Message retrieval failed"));
      }

      [Test]
      public void TestOnErrorVBScript()
      {
         LogHandler.DeleteEventLog();

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnError(iSeverity, iError, sSource, sDescription) " + Environment.NewLine +
                      " EventLog.Write(\"Severity: \" & iSeverity) " + Environment.NewLine +
                      " EventLog.Write(\"Error: \" & iError) " + Environment.NewLine +
                      " EventLog.Write(\"Source: \" & sSource) " + Environment.NewLine +
                      " EventLog.Write(\"Description: \" & sDescription) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         var inbox = account.IMAPFolders.get_ItemByName("Inbox");


         var deletedMessageText = app.Settings.ServerMessages.get_ItemByName("MESSAGE_FILE_MISSING").Text;
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");

         CustomAsserts.AssertFolderMessageCount(inbox, 1);
         var message = inbox.Messages[0];
         File.Delete(message.Filename);
         var text = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(text.Contains(deletedMessageText.Replace("%MACRO_FILE%", message.Filename)));
         CustomAsserts.AssertReportedError("Message retrieval failed because message file");

         var eventLogText = TestSetup.ReadExistingTextFile(app.Settings.Logging.CurrentEventLog);
         Assert.IsTrue(eventLogText.Contains("Description: Message retrieval failed"));
      }

      [Test]
      public void TestOnExternalAccountDownload()
      {
         LogHandler.DeleteCurrentDefaultLog();

         var messages = new List<string>();

         messages.Add("From: Martin@example1.com\r\n" +
                      "To: Martin@example.com\r\n" +
                      "Subject: Message 1\r\n" +
                      "\r\n" +
                      "Message 1!");

         messages.Add("From: Martin@example2.com\r\n" +
                      "To: Martin@example.com\r\n" +
                      "Subject: Message 2\r\n" +
                      "\r\n" +
                      "Message 2!");

         messages.Add("From: Martin@example3.com\r\n" +
                      "To: Martin@example.com\r\n" +
                      "Subject: Message 3\r\n" +
                      "\r\n" +
                      "Message 3!");


         // The second message should be deleted after 5 days.
         var script = "Sub OnExternalAccountDownload(oFetchAccount, message, sRemoteUID)" + Environment.NewLine +
                      " EventLog.Write(\"UID: \" & sRemoteUID) " + Environment.NewLine +
                      " EventLog.Write(\"FetchAccount: \" & oFetchAccount.Name) " + Environment.NewLine +
                      " If Not message Is Nothing Then " + Environment.NewLine +
                      "   EventLog.Write(\"From: \" & message.FromAddress) " + Environment.NewLine +
                      "   EventLog.Write(\"Filename: \" & message.FileName) " + Environment.NewLine +
                      " Else " + Environment.NewLine +
                      "   EventLog.Write(\"Message details missing\") " + Environment.NewLine +
                      " End If" + Environment.NewLine +
                      " if sRemoteUID = \"UniqueID-" + messages[1].GetHashCode() + "\" Then " +
                      Environment.NewLine +
                      "   Result.Value = 2  " + Environment.NewLine +
                      "   Result.Parameter = 5  " + Environment.NewLine +
                      " End If " + Environment.NewLine +
                      " End Sub";

         var scripting = _settings.Scripting;
         var file = scripting.CurrentScriptFile;
         File.WriteAllText(file, script);
         scripting.Enabled = true;
         scripting.Reload();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "user@example.test", "test");

         FetchAccount fa;

         var port = TestSetup.GetNextFreePort();
         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
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
            fa.Port = port;
            fa.ProcessMIMERecipients = false;
            fa.DaysToKeepMessages = -1;
            fa.Save();
            fa.DownloadNow();

            pop3Server.WaitForCompletion();

            var eventLogFile = _settings.Logging.CurrentEventLog;
            var logContents = TestSetup.ReadExistingTextFile(eventLogFile);

            Assert.IsTrue(logContents.Contains("FetchAccount: " + fa.Name));

            Assert.IsTrue(logContents.Contains("From: Martin@example1.com"));
            Assert.IsTrue(logContents.Contains("From: Martin@example2.com"));
            Assert.IsTrue(logContents.Contains("From: Martin@example3.com"));

            var appLogContent = LogHandler.ReadCurrentDefaultLog();

            Assert.IsTrue(pop3Server.DeletedMessages.Contains(1));
            Assert.IsFalse(pop3Server.DeletedMessages.Contains(2));
            Assert.IsTrue(pop3Server.DeletedMessages.Contains(3));

            Assert.IsTrue(pop3Server.RetrievedMessages.Contains(1));
            Assert.IsTrue(pop3Server.RetrievedMessages.Contains(2));
            Assert.IsTrue(pop3Server.RetrievedMessages.Contains(3));

            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);
         }

         using (var pop3Server = new Pop3ServerSimulator(1, port, messages))
         {
            pop3Server.StartListen();

            // Download again...
            fa.DownloadNow();

            pop3Server.WaitForCompletion();

            // Make sure that no messages are deleted.
            Assert.AreEqual(0, pop3Server.DeletedMessages.Count);
            Assert.AreEqual(0, pop3Server.RetrievedMessages.Count);

            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);
         }
      }

      [Test]
      public void TestOnClientLogon_POP3()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnClientLogon(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"OnClientLogin-POP3\")" + Environment.NewLine +
                      " EventLog.Write(\"IsAuthenticated: \" & oClient.Authenticated) " + Environment.NewLine +
                      " EventLog.Write(\"Username: \" & oClient.Username) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // Log on once to trigger event.
         Pop3ClientSimulator.AssertMessageCount("test@example.test", "test", 0);

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("OnClientLogin-POP3"));
         Assert.IsTrue(message.Contains("IsAuthenticated: True"));
         Assert.IsTrue(
            message.Contains("Username: test@example.test")); // Should be empty, Username isn't available at this time.
      }

      [Test]
      public void TestOnClientLogon_IMAP()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnClientLogon(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"OnClientLogin-IMAP\")" + Environment.NewLine +
                      " EventLog.Write(\"IsAuthenticated: \" & oClient.Authenticated) " + Environment.NewLine +
                      " EventLog.Write(\"Username: \" & oClient.Username) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // Log on once to trigger event.
         ImapClientSimulator.AssertMessageCount("test@example.test", "test", "Inbox", 0);

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("OnClientLogin-IMAP"));
         Assert.IsTrue(message.Contains("IsAuthenticated: True"));
         Assert.IsTrue(
            message.Contains("Username: test@example.test")); // Should be empty, Username isn't available at this time.
      }

      [Test]
      public void TestOnClientLogon_SMTP()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnClientLogon(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"OnClientLogin-SMTP\")" + Environment.NewLine +
                      " EventLog.Write(\"IsAuthenticated: \" & oClient.Authenticated) " + Environment.NewLine +
                      " EventLog.Write(\"Username: \" & oClient.Username) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // Log on once to trigger event.

         var base64Username = Convert.ToBase64String(Encoding.ASCII.GetBytes("test@example.test"));
         var base64Password = Convert.ToBase64String(Encoding.ASCII.GetBytes("test"));

         var clientSimulator = new SmtpClientSimulator();
         string errMsg;
         clientSimulator.ConnectAndLogon(base64Username, base64Password, out errMsg);

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("OnClientLogin-SMTP"));
         Assert.IsTrue(message.Contains("IsAuthenticated: True"));
         Assert.IsTrue(
            message.Contains("Username: test@example.test")); // Should be empty, Username isn't available at this time.
      }

      [Test]
      public void TestOnHelo_WithHelo()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnHelo(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"OnHelo\")" + Environment.NewLine +
                      " EventLog.Write(\"HeloHost: \" & oClient.HELO) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // Log on once to trigger event.
         var clientSimulator = new SmtpClientSimulator();
         clientSimulator.Connect();
         clientSimulator.SendAndReceive("HELO WORLD\r\n");

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("OnHelo"));
         Assert.IsTrue(message.Contains("HeloHost: WORLD"));
      }

      [Test]
      public void TestOnHelo_WithEhlo()
      {
         var domain = SingletonProvider<TestSetup>.Instance.AddTestDomain();
         SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script = "Sub OnHelo(oClient) " + Environment.NewLine +
                      " EventLog.Write(\"OnHelo\")" + Environment.NewLine +
                      " EventLog.Write(\"HeloHost: \" & oClient.HELO) " + Environment.NewLine +
                      "End Sub" + Environment.NewLine + Environment.NewLine;

         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         var eventLogFile = app.Settings.Logging.CurrentEventLog;
         if (File.Exists(eventLogFile))
            File.Delete(eventLogFile);

         // Log on once to trigger event.
         var clientSimulator = new SmtpClientSimulator();
         clientSimulator.Connect();
         clientSimulator.SendAndReceive("EHLO WORLD2\r\n");

         // Check that the message exists
         var message = TestSetup.ReadExistingTextFile(eventLogFile);

         Assert.IsNotEmpty(message);
         Assert.IsTrue(message.Contains("OnHelo"));
         Assert.IsTrue(message.Contains("HeloHost: WORLD2"));
      }

      [Test]
      public void TestOnClientValidatePasswordVBScript_ValidPassword()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         // First verify log on works with proper password ("test") but fails with incorrect ("MySecretPassword")
         Assert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "test"));
         Assert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));

         // Create a script which override password validation to allow MySecretPassword as valid password
         var app = SingletonProvider<TestSetup>.Instance.GetApp();
         var scripting = app.Settings.Scripting;

         var script =
            @"Sub OnClientValidatePassword(account, password)  
                 EventLog.Write(""Account: "" & account.Address)
                 EventLog.Write(""Password: "" & password)
                 If password = ""MySecretPassword"" Then
                   Result.Value = 0
                 Else
                   Result.Value = 1
                 End If
              End Sub";


         File.WriteAllText(scripting.CurrentScriptFile, script);

         scripting.Enabled = true;
         scripting.Reload();

         // Now verify we can log on using the new password
         Assert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));
         Assert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "test"));

         var eventLogText = TestSetup.ReadExistingTextFile(app.Settings.Logging.CurrentEventLog);
         Assert.IsTrue(eventLogText.Contains("Account: test@example.test"));
         Assert.IsTrue(eventLogText.Contains("Password: MySecretPassword"));
      }

      // ------------------------------------------------------------------ the language in force, and the one stored

      // Leaves no script behind for the tests after these, whose setup switches
      // scripting off and puts VBScript back but loads nothing: both files emptied,
      // and an empty VBScript script loaded.
      private void LeaveAnEmptyVBScriptScriptLoaded()
      {
         var scripting = SingletonProvider<TestSetup>.Instance.GetApp().Settings.Scripting;
         scripting.Enabled = false;

         scripting.Language = "JScript";
         File.WriteAllText(scripting.CurrentScriptFile, string.Empty);

         scripting.Language = "VBScript";
         File.WriteAllText(scripting.CurrentScriptFile, string.Empty);

         scripting.Reload();
      }

      [Test]
      [Description("GET /api/v1/status names VBScript and JScript as the languages this build runs, and says it has an engine")]
      public void TheStatusNamesVBScriptAndJScriptAsTheLanguagesThisBuildRuns()
      {
         ClassicAssert.IsTrue(ScriptLanguages.ListReported, "GET /api/v1/status has no build.script_languages.");
         ClassicAssert.AreEqual("VBScript, JScript", ScriptLanguages.Describe(), "build.script_languages");
         ClassicAssert.IsTrue(ScriptLanguages.ScriptEngine, "build.script_engine is false on a build that runs VBScript and JScript.");
         ClassicAssert.IsFalse(ScriptLanguages.ServerRuns("JavaScript"), "A Windows build does not run JavaScript.");
      }

      [Test]
      [Description("Storing JavaScript reloads nothing, and the JScript OnClientValidatePassword in force goes on deciding logons - before the reload and after it, since the reload is refused with the reason. Against the build before this, the call was built for the stored language: empty, the handler never asked, and the stored password let the client in.")]
      public void StoringJavaScriptLeavesTheJScriptPasswordHandlerDeciding()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            scripting.Language = "JScript";
            File.WriteAllText(scripting.CurrentScriptFile,
               "function OnClientValidatePassword(account, password) {\r\n" +
               "   EventLog.Write('Password: ' + password);\r\n" +
               "   Result.Value = password == 'MySecretPassword' ? 0 : 1;\r\n" +
               "}\r\n");
            scripting.Enabled = true;
            scripting.Reload();

            // The handler decides: its password lets the client in, the stored one does not.
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));
            ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "test"));

            // The setting says JavaScript; nothing has been reloaded.
            scripting.Language = "JavaScript";
            LogHandler.DeleteEventLog();

            ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "test"),
               "The JScript handler in force was not asked, and the stored password let the client in.");
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));
            StringAssert.Contains("Password: MySecretPassword", TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName()));

            // A reload in JavaScript is refused on this build, saying why, and the
            // JScript script stays in force - handler and all.
            scripting.Reload();
            CustomAsserts.AssertReportedError("HM5710", "the engine the Linux builds");

            ClassicAssert.IsFalse(ImapClientSimulator.ValidatePassword("test@example.test", "test"),
               "After the refused reload the JScript handler was no longer asked.");
            ClassicAssert.IsTrue(ImapClientSimulator.ValidatePassword("test@example.test", "MySecretPassword"));
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      [Test]
      [Description("A reload into a language this build does not run is refused with the ERROR line even when there is no file for it, and the VBScript handler in force keeps running. Against the build before this, the missing file read as an empty script, the load went through without a word and every handler was off.")]
      public void AReloadIntoJavaScriptWithNoFileIsRefusedAndTheVBScriptHandlerStays()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            File.WriteAllText(scripting.CurrentScriptFile,
               "Sub OnAcceptMessage(oClient, oMessage)\r\n" +
               "   oMessage.HeaderValue(\"X-Handler\") = \"vbscript\"\r\n" +
               "   oMessage.Save\r\n" +
               "End Sub\r\n");
            scripting.Enabled = true;
            scripting.Reload();

            scripting.Language = "JavaScript";

            // No EventHandlers.js at all: the case that used to pass in silence.
            var javaScriptFile = scripting.CurrentScriptFile;
            if (File.Exists(javaScriptFile))
               File.Delete(javaScriptFile);

            scripting.Reload();
            CustomAsserts.AssertReportedError("HM5710", "the engine the Linux builds");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");
            StringAssert.Contains("X-Handler: vbscript", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      [Test]
      [Description("A JavaScript file - here one that is valid JScript too - is refused at load on this build and never run as JScript, the ProgID Windows registers for it; loaded while scripting was off, it is refused again when scripting is switched on, and the rule function that reaches it is reported once, not in silence and not once per message")]
      public void AJavaScriptScriptIsNeverRunAsJScriptAndARuleFunctionAgainstItIsReportedOnce()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            // A known script in force first: VBScript, empty.
            File.WriteAllText(scripting.CurrentScriptFile, string.Empty);
            scripting.Enabled = true;
            scripting.Reload();

            // Both functions are JScript as much as JavaScript. If "JavaScript" ever
            // reached CoCreateInstance, the subjects would carry the marks.
            scripting.Language = "JavaScript";
            File.WriteAllText(scripting.CurrentScriptFile,
               "function OnAcceptMessage(oClient, oMessage) {\r\n" +
               "   oMessage.Subject = '[accept ran] ' + oMessage.Subject;\r\n" +
               "   oMessage.Save();\r\n" +
               "}\r\n" +
               "function MarkTheSubject(oMessage) {\r\n" +
               "   oMessage.Subject = '[rule ran] ' + oMessage.Subject;\r\n" +
               "   oMessage.Save();\r\n" +
               "}\r\n");

            scripting.Reload();
            CustomAsserts.AssertReportedError("the engine the Linux builds");
            StringAssert.Contains("the engine the Linux builds", scripting.CheckSyntax());

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "First", "SampleBody");
            var first = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            StringAssert.Contains("Subject: First", first);
            StringAssert.DoesNotContain("[accept ran]", first);

            // Loaded while scripting is off: no engine runs, nothing is refused then,
            // and nothing is registered. Switching scripting on loads the script
            // (Configuration::SetUseScriptServer), which refuses it at once, saying
            // why - and the JavaScript text stays in force, registering nothing.
            scripting.Enabled = false;
            scripting.Reload();
            CustomAsserts.AssertNoReportedError();

            scripting.Enabled = true;
            CustomAsserts.AssertReportedError("HM5710", "the engine the Linux builds", "loaded while scripting was off");

            var rule = _application.Rules.Add();
            rule.Name = "Runs a script function";
            rule.Active = true;

            var criteria = rule.Criterias.Add();
            criteria.UsePredefined = true;
            criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
            criteria.MatchType = eRuleMatchType.eMTGreaterThan;
            criteria.MatchValue = "0";
            criteria.Save();

            var action = rule.Actions.Add();
            action.Type = eRuleActionType.eRARunScriptFunction;
            action.ScriptFunction = "MarkTheSubject";
            action.Save();

            rule.Save();

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Second", "SampleBody");
            var second = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            StringAssert.Contains("Subject: Second", second);
            StringAssert.DoesNotContain("[rule ran]", second);
            CustomAsserts.AssertReportedError("HM5018", "MarkTheSubject", "which this Windows build does not run");

            // Once per load: the next message the rule matches is refused as quietly
            // as it should be, since the administrator has been told.
            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Third", "SampleBody");
            var third = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            StringAssert.DoesNotContain("[rule ran]", third);
            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      // ------------------------------------------------------------------ scripting off runs no engine

      [Test]
      [Description("With scripting off, neither a reload nor a reinitialise runs the script: one that does not compile is not reported. Switching scripting on loads it at once - the refusal is reported then, and a rule function against the text that was never compiled is refused rather than handed to the engine. Against the build before this, every load compiled the script and searched it for fifteen handlers whatever the switch said, and reported this one at the reload and at the reinitialise.")]
      public void WithScriptingOffNoLoadRunsTheScriptAndSwitchingOnLoadsIt()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            scripting.Enabled = false;
            File.WriteAllText(scripting.CurrentScriptFile, "Sub ThisWillNotCompile(\r\n");

            // Neither load compiles it, so neither can report it.
            scripting.Reload();
            _application.Reinitialize();
            CustomAsserts.AssertNoReportedError();

            // The positive control: loaded with scripting on, it is refused, and the
            // refusal says what is in force.
            scripting.Enabled = true;
            CustomAsserts.AssertReportedError("HM5710", "could not be compiled", "loaded while scripting was off");

            // The text in force was never compiled, so a rule's function is refused
            // with the reason, once, and the message goes on unchanged.
            AddScriptFunctionRule(_application.Rules, "MarkTheSubject");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "First", "SampleBody");
            StringAssert.Contains("Subject: First", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
            CustomAsserts.AssertReportedError("HM5018", "MarkTheSubject", "has not been compiled since");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Second", "SampleBody");
            StringAssert.Contains("Subject: Second", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      [Test]
      [Description("A handler written while scripting was off runs as soon as scripting is switched on, with no reload: switching on loads the script. Negative control: while scripting is off the handler does not run.")]
      public void SwitchingScriptingOnMakesTheHandlersRunWithoutAReload()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            scripting.Enabled = false;
            scripting.Reload();

            // Written after the last load, so that only a load made by switching
            // scripting on can have found it.
            File.WriteAllText(scripting.CurrentScriptFile,
               "Sub OnAcceptMessage(oClient, oMessage)\r\n" +
               "   oMessage.HeaderValue(\"X-Switched-On\") = \"yes\"\r\n" +
               "   oMessage.Save\r\n" +
               "End Sub\r\n");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Off", "SampleBody");
            StringAssert.DoesNotContain("X-Switched-On", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));

            scripting.Enabled = true;

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "On", "SampleBody");
            StringAssert.Contains("X-Switched-On: yes", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      // ------------------------------------------------------------------ what a value may do to the call text

      // Logs on over SMTP AUTH LOGIN, which carries the password in base64 and so
      // byte for byte; true when the server let the client in.
      private static bool LogsOnOverSmtp(string address, string password)
      {
         var client = new SmtpClientSimulator();

         try
         {
            string errorMessage;
            client.ConnectAndLogon(Convert.ToBase64String(Encoding.UTF8.GetBytes(address)),
               Convert.ToBase64String(Encoding.UTF8.GetBytes(password)), out errorMessage);
            return true;
         }
         catch (System.Security.Authentication.AuthenticationException)
         {
            return false;
         }
      }

      // The text's UTF-16 code units, comma-separated: what a handler that writes
      // password.charCodeAt(i) for each i writes, so the comparison is exact and
      // nothing the event log does to a quote or a line break can hide a difference.
      private static string CharacterCodes(string text)
      {
         var codes = new List<string>();

         foreach (var character in text)
            codes.Add(((int) character).ToString());

         return string.Join(",", codes);
      }

      private static hMailServer.Rule AddScriptFunctionRule(hMailServer.Rules rules, string functionName)
      {
         var rule = rules.Add();
         rule.Name = "Runs a script function";
         rule.Active = true;

         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRARunScriptFunction;
         action.ScriptFunction = functionName;
         action.Save();

         rule.Save();
         return rule;
      }

      [Test]
      [Description("A password reaches a JScript OnClientValidatePassword as the value it is, and nothing in it runs. The JScript call used to escape the apostrophe and not the backslash, so a client that sent the password x\\'); followed by a statement - before any logon, for any existing account - ran that statement inside the server's script engine. Negative control: the same statement, run by the handler itself, puts the marker in the log.")]
      public void APasswordCannotEndItsStringInTheJScriptCallAndRunCode()
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            scripting.Language = "JScript";
            File.WriteAllText(scripting.CurrentScriptFile,
               "function OnClientValidatePassword(account, password) {\r\n" +
               "   var codes = [];\r\n" +
               "   for (var i = 0; i < password.length; i++)\r\n" +
               "      codes.push(password.charCodeAt(i));\r\n" +
               "   EventLog.Write('pw=[' + password + '] codes=' + codes.join(','));\r\n" +
               "   if (password == 'control')\r\n" +
               "      EventLog.Write(\"INJ\" + \"ECTED\");\r\n" +
               "   Result.Value = 1;\r\n" +
               "}\r\n");
            scripting.Enabled = true;
            scripting.Reload();

            // The negative control. The handler is reached, is read back, and when it
            // runs the attack's statement itself the marker is in the log - so its
            // absence below means the statement did not run, not that the log could
            // not have shown it.
            LogHandler.DeleteEventLog();
            ClassicAssert.IsFalse(LogsOnOverSmtp("test@example.test", "control"), "The handler refuses every password.");

            var controlLog = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
            StringAssert.Contains("pw=[control] codes=" + CharacterCodes("control"), controlLog);
            StringAssert.Contains("INJECTED", controlLog);

            // The attack: a backslash before the apostrophe. Escaping the apostrophe
            // alone made that 'x\\'); - an escaped backslash, the string closed by the
            // apostrophe after it, and the rest of the password statements, the
            // comment swallowing the call's own closing quote and parenthesis.
            const string attack = "x\\'); EventLog.Write(\"INJ\" + \"ECTED\"); //";

            LogHandler.DeleteEventLog();
            ClassicAssert.IsFalse(LogsOnOverSmtp("test@example.test", attack), "The handler refuses every password.");

            var attackLog = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
            StringAssert.Contains("codes=" + CharacterCodes(attack), attackLog,
               "The handler was not handed the password it was sent, byte for byte.");
            StringAssert.DoesNotContain("INJECTED", attackLog,
               "A statement carried in a password ran inside the server's script engine.");
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      [Test]
      [Description("A rule's Run function action is refused unless it names a plain function, so its name cannot carry statements into the script engine; a plain name runs the function (the negative control). The name was written into the call as it was, and domain and account rules are written by a domain's administrator too.")]
      public void ARuleFunctionNameCannotCarryCodeIntoTheScript()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         var scripting = _settings.Scripting;

         try
         {
            scripting.Language = "JScript";
            File.WriteAllText(scripting.CurrentScriptFile,
               "function MarkTheSubject(oMessage) {\r\n" +
               "   oMessage.Subject = '[rule ran] ' + oMessage.Subject;\r\n" +
               "   oMessage.Save();\r\n" +
               "}\r\n");
            scripting.Enabled = true;
            scripting.Reload();

            // The negative control: named plainly, the function runs.
            var plain = AddScriptFunctionRule(_application.Rules, "MarkTheSubject");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "First", "SampleBody");
            StringAssert.Contains("Subject: [rule ran] First", Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test"));

            plain.Delete();

            // A name that is a call, then a statement, then a comment: written into
            // the call as it was, all three ran.
            AddScriptFunctionRule(_application.Rules, "MarkTheSubject(HMAILSERVER_MESSAGE); EventLog.Write(\"INJ\" + \"ECTED\"); //");

            LogHandler.DeleteEventLog();

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Second", "SampleBody");
            var second = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
            StringAssert.Contains("Subject: Second", second);
            StringAssert.DoesNotContain("[rule ran]", second);

            CustomAsserts.AssertReportedError("HM5018", "not a plain function name");
            StringAssert.DoesNotContain("INJECTED", LogHandler.ReadEventLog(),
               "A statement carried in a rule's function name ran inside the server's script engine.");
         }
         finally
         {
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      [Test]
      [Description("An event script uses the COM Utilities members a client in another process must sign in for - the " +
                   "legacy Blowfish transform, the resolver, the Sieve parser and interpreter, the rule matcher - without " +
                   "signing in, as it always could: through a Utilities object it creates, and through an Application it " +
                   "creates and never authenticates. Nothing else is given to those objects: the same Application is still " +
                   "refused its Settings.")]
      public void AnEventScriptUsesTheUtilitiesHelpersWithoutSigningIn()
      {
         // With no administrator password every COM object answers as the server
         // administrator, a script's included, and the script below would prove
         // nothing about an unauthenticated one.
         object administrator = new Application().Authenticate("Administrator", "");
         if (administrator != null)
            Assert.Ignore("This server has no administrator password, so an event script's COM objects answer as the " +
                          "server administrator and are never unauthenticated.");

         const string mailDomain = "scripthost.example.test";
         const string mailExchange = "mx.scripthost.example.test";

         // TEST-NET-1 (RFC 5737): asked about, never connected to.
         const string mailExchangeAddress = "192.0.2.27";

         SuiteDns.Zone
            .WithMx(mailDomain, 10, mailExchange)
            .WithA(mailExchange, mailExchangeAddress);

         try
         {
            // Every answer, or the error in its place, goes into a header of the
            // message the handler is given, so that what the script saw is read
            // back from the delivered message rather than from a log.
            var script =
               "Sub OnAcceptMessage(oClient, oMessage)\r\n" +
               "   Call Report(oMessage, \"Direct\", CreateObject(\"hMailServer.Utilities\"))\r\n" +
               "   Dim oApp\r\n" +
               "   Set oApp = CreateObject(\"hMailServer.Application\")\r\n" +
               "   Call Report(oMessage, \"Application\", oApp.Utilities)\r\n" +
               "   Dim sHostName\r\n" +
               "   On Error Resume Next\r\n" +
               "   sHostName = oApp.Settings.HostName\r\n" +
               "   Call Note(oMessage, \"Application\", \"Settings\", \"answered\", Err.Number, Err.Description)\r\n" +
               "   On Error Goto 0\r\n" +
               "   oMessage.Save\r\n" +
               "End Sub\r\n" +
               "\r\n" +
               "Sub Report(oMessage, sVia, oUtilities)\r\n" +
               "   Dim r\r\n" +
               "   On Error Resume Next\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.BlowfishEncrypt(\"Test\")\r\n" +
               "   Call Note(oMessage, sVia, \"BlowfishEncrypt\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.BlowfishDecrypt(\"08d04d2ef52d64ce\")\r\n" +
               "   Call Note(oMessage, sVia, \"BlowfishDecrypt\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.ResolveMXRecords(\"" + mailDomain + "\")\r\n" +
               "   Call Note(oMessage, sVia, \"ResolveMXRecords\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.GetMailServer(\"someone@" + mailDomain + "\")\r\n" +
               "   Call Note(oMessage, sVia, \"GetMailServer\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.IsLocalHost(\"127.0.0.1\")\r\n" +
               "   Call Note(oMessage, sVia, \"IsLocalHost\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.CheckSieveSyntax(\"keep;\")\r\n" +
               "   Call Note(oMessage, sVia, \"CheckSieveSyntax\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.EvaluateSieveScript(\"keep;\", \"Subject: x\" & vbCrLf & vbCrLf & \"body\")\r\n" +
               "   Call Note(oMessage, sVia, \"EvaluateSieveScript\", r, Err.Number, Err.Description)\r\n" +
               "   Err.Clear\r\n" +
               "   r = oUtilities.CriteriaMatch(\"Test\", 1, \"Test\")\r\n" +
               "   Call Note(oMessage, sVia, \"CriteriaMatch\", r, Err.Number, Err.Description)\r\n" +
               "End Sub\r\n" +
               "\r\n" +
               "Sub Note(oMessage, sVia, sMember, vValue, iError, sError)\r\n" +
               "   Dim sText\r\n" +
               "   If iError <> 0 Then\r\n" +
               "      sText = \"error \" & sError\r\n" +
               "   ElseIf VarType(vValue) = vbBoolean Then\r\n" +
               "      If vValue Then sText = \"True\" Else sText = \"False\"\r\n" +
               "   Else\r\n" +
               "      sText = Replace(CStr(vValue), vbTab, \" \")\r\n" +
               "   End If\r\n" +
               "   oMessage.HeaderValue(\"X-\" & sVia & \"-\" & sMember) = \"[\" & sText & \"]\"\r\n" +
               "End Sub\r\n";

            var scripting = _settings.Scripting;
            File.WriteAllText(scripting.CurrentScriptFile, script);
            scripting.Enabled = true;
            scripting.Reload();

            var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

            SmtpClientSimulator.StaticSend(account.Address, account.Address, "Test", "SampleBody");
            var message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

            foreach (var via in new[] { "Direct", "Application" })
            {
               AssertAnswered(message, via, "BlowfishEncrypt", "08d04d2ef52d64ce");
               AssertAnswered(message, via, "BlowfishDecrypt", "Test");
               AssertAnswered(message, via, "GetMailServer", mailExchangeAddress);

               // The exchange's name as the resolver hands it back, then its address.
               StringAssert.IsMatch("(?i)X-" + via + @"-ResolveMXRecords: \[" + System.Text.RegularExpressions.Regex.Escape(mailExchange) +
                  @"\.? " + System.Text.RegularExpressions.Regex.Escape(mailExchangeAddress) + @"\]", message,
                  "ResolveMXRecords, reached " + via + ", did not give an event script that never signed in the answer " +
                  "it gives an authenticated caller. Message: " + message);
               AssertAnswered(message, via, "CheckSieveSyntax", "");
               AssertAnswered(message, via, "EvaluateSieveScript", "keep");
               AssertAnswered(message, via, "CriteriaMatch", "True");

               // Whether 127.0.0.1 is one of the addresses the server listens on
               // depends on its ports; that the member answered is what is asked.
               StringAssert.IsMatch("X-" + via + @"-IsLocalHost: \[(True|False)\]", message,
                  "IsLocalHost, reached " + via + ", did not answer the event script. Message: " + message);
            }

            StringAssert.IsMatch(@"X-Application-Settings:\s+\[error ", message,
               "The unauthenticated Application an event script creates was given its Settings: the script host's " +
               "exemption reached beyond the Utilities helpers. Message: " + message);
         }
         finally
         {
            SuiteDns.Reset();
            LeaveAnEmptyVBScriptScriptLoaded();
         }
      }

      private static void AssertAnswered(string message, string via, string member, string expected)
      {
         StringAssert.Contains("X-" + via + "-" + member + ": [" + expected + "]", message,
            member + ", reached " + via + ", did not give an event script that never signed in the answer it gives " +
            "an authenticated caller. Message: " + message);
      }
   }
}