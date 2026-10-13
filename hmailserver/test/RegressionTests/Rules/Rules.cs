// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using System.Linq;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Rules
{
   [TestFixture]
   public class Rules : TestFixtureBase
   {
      private void CreatePrintRecipientCountRule(hMailServer.Rules rules)
      {
         var rule = rules.Add();
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
         ruleAction.ScriptFunction = "PrintRecipientCount";
         ruleAction.Save();

         rule.Save();

         File.WriteAllText(_settings.Scripting.CurrentScriptFile,
            "Sub PrintRecipientCount(message)" + Environment.NewLine +
            " Call EventLog.Write(message.Recipients.Count)" + Environment.NewLine +
            "End Sub");

         _settings.Scripting.Reload();
      }

      [Test]
      public void ActionAccountRuleMoveToExistingPublicFolder()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");
         var publicFolders = _settings.PublicFolders;
         var folder = publicFolders.Add("Share1");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionExpunge, true);
         permission.Save();

         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.Share1";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska hamna i public folder.");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "#public.Share1", 1);
      }

      [Test]
      public void ActionAccountRuleMoveToExistingPublicFolderSubStructureWithCreatePermission()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "account1@example.test", "test");
         var publicFolders = _settings.PublicFolders;
         var folder = publicFolders.Add("Share1");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionCreate, true);
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.Save();

         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.Share1.Sub";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "account1@example.test", "SomeString",
            "This should end up in the #public.share1.sub since user lacks right.");

         ImapClientSimulator.AssertMessageCount("account1@example.test", "test", "#public.Share1.Sub", 1);
      }

      [Test]
      public void ActionAccountRuleMoveToNonExistingPublicFolder()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");
         var publicFolders = _settings.PublicFolders;

         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.Share1";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "This should end up in the inbox since user lacks right.");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "INBOX", 1);
      }

      [Test]
      [Description("Tessts the bind to address rule action.")]
      public void ActionBindToAddress()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = SingletonProvider<TestSetup>.Instance.GetApp().Rules.Add();
         rule.Name = "Global rule test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRABindToAddress;
         ruleAction.Value = "255.254.253.252";
         ruleAction.Save();
         rule.Save();

         // Sent through a ROUTE rather than to a real external domain.
         //
         // This test is about the bind, and the message previously went to
         // chris.holloway@gmail.com - so reaching the bind at all meant an MX lookup of
         // gmail.com first. That made a rules test depend on live DNS, and on
         // 19 August 2026 it failed mid-gate for exactly that reason: the lookup
         // did not complete, the message was deferred rather than failed, and the
         // assertion below saw one message in the queue instead of none, 20 seconds
         // later. A route resolves nothing.
         //
         // The route target is in TEST-NET-1 (RFC 5737) and is never contacted:
         // TCPConnection::Connect binds before it connects, so the bind to
         // 255.254.253.252 fails first, which is the whole point of the test.
         var settings = SingletonProvider<TestSetup>.Instance.GetApp().Settings;
         var route = settings.Routes.Add();
         route.DomainName = "bindtest.test";
         route.TargetSMTPHost = "192.0.2.10";
         route.TargetSMTPPort = 25;
         route.NumberOfTries = 1;
         route.MinutesBetweenTry = 5;
         route.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         smtpClientSimulator.Send("ruletest@example.test", "someone@bindtest.test", "SomeString",
            "This mail should not be delivered - Test ActionBindToAddress.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var errorLog = LogHandler.ReadAndDeleteErrorLog();

         Assert.IsTrue(errorLog.Contains("Failed to bind to IP address 255.254.253.252."));
      }

      [Test]
      [Description("Test to delete using a rule. Confirm that the account folder does not contain the files")]
      public void ActionDelete()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRADeleteEmail;
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString", "Test 1");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "a", "Test 2");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString", "Test 3");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "b", "Test 2");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 2);

         var fileCount = 0;
         var domainDir = Paths.Combine(_application.Settings.Directories.DataDirectory, "example.test");
         var userDir = Paths.Combine(domainDir, "ruletest");

         RetryHelper.TryAction(TimeSpan.FromSeconds(10), () =>
         {
            var dirs = Directory.GetDirectories(userDir);
            fileCount += dirs.Sum(dir => Directory.GetFiles(dir).Length);

            RetryableAssert.AreEqual(2, fileCount);
         });

         RetryHelper.TryAction(TimeSpan.FromSeconds(10), delegate
         {
            var logContent = LogHandler.ReadCurrentDefaultLog();
            var loggedDeletionCount =
               new Regex(Regex.Escape("Delivery to this account was canceled by an account rule")).Matches(logContent)
                  .Count;

            RetryableAssert.AreEqual(2, loggedDeletionCount);
         });
      }

      [Test]
      public void ActionGlobalMoveToIMAPFolder()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            "ActionGlobalMoveToIMAPFolder@example.test",
            "test");

         var rule = SingletonProvider<TestSetup>.Instance.GetApp().Rules.Add();
         rule.Name = "Global rule test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTNotEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.GlobalBox";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ActionGlobalMoveToIMAPFolder@example.test",
            "ActionGlobalMoveToIMAPFolder@example.test", "SomeString",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ActionGlobalMoveToIMAPFolder@example.test",
            "ActionGlobalMoveToIMAPFolder@example.test", "SomeStringA",
            "Detta ska hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ActionGlobalMoveToIMAPFolder@example.test",
            "ActionGlobalMoveToIMAPFolder@example.test", "somestring",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");

         ImapClientSimulator.AssertMessageCount("ActionGlobalMoveToIMAPFolder@example.test", "test", "Inbox.GlobalBox",
            1);
         ImapClientSimulator.AssertMessageCount("ActionGlobalMoveToIMAPFolder@example.test", "test", "Inbox", 2);
      }

      [Test]
      public void ActionGlobalRuleMoveToIMAPFolderPublicFolder()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");
         var publicFolders = _settings.PublicFolders;
         var folder = publicFolders.Add("Share1");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionExpunge, true);
         permission.Save();

         var rule = application.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.Share1";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska hamna i public folder.");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "#public.Share1", 1);
      }

      [Test]
      public void ActionGlobalRuleMoveToIMAPFolderPublicFolderNonExistant()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = application.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.MyFolder";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska hamna i public folder.");

         // Wait for the folder to be created.
         var folder = CustomAsserts.AssertFolderExists(_settings.PublicFolders, "MyFolder");

         // Wait for the message to appear.
         CustomAsserts.AssertFolderMessageCount(folder, 1);

         // Make sure we can't access it.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon("ruletest@example.test", "test"));

         Assert.Throws<ArgumentException>(() => imap.GetMessageCount("#public.MyFolder"));

         // Set permissions on this folder.
         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionExpunge, true);
         permission.Save();

         // Make sure we can access it now.
         imap.GetMessageCount("#public.MyFolder");
      }

      [Test]
      public void ActionMoveToIMAPFolder()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTNotEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.NotEquals";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeStringA",
            "Detta ska hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "somestring",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.NotEquals", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 2);
      }

      [Test]
      public void ActionOverrideMoveToIMAPFolder()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = SingletonProvider<TestSetup>.Instance.GetApp().Rules.Add();
         rule.Name = "Global rule test";
         rule.Active = true;
         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTNotEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();
         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.GlobalBox";
         ruleAction.Save();
         rule.Save();

         // Account-level rule
         var accountRule = account.Rules.Add();
         accountRule.Name = "Criteria test";
         accountRule.Active = true;

         ruleCriteria = accountRule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTNotEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();

         // Add action
         ruleAction = accountRule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Overriden.Test";
         ruleAction.Save();

         // Save the rule in the database
         accountRule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska inte hamna i mappen Inbox.Overriden.Test");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeStringA",
            "Detta ska hamna i mappen Inbox.Overriden.Test");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "somestring",
            "Detta ska inte hamna i mappen Inbox.Overriden.Test");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Overriden.Test", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 2);
      }

      [Test]
      public void ActionSendUsingRoute()
      {
         var smtpServerPort = TestSetup.GetNextFreePort();

         // add an account to send from
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");

         // Add a route so we can conenct to localhost.
         var route = TestSetup.AddRoutePointingAtLocalhost(5, smtpServerPort, false);

         // Add a global send-using-route rule
         var rule = _application.Rules.Add();
         rule.Name = "Send using route";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRASendUsingRoute;
         ruleAction.RouteID = route.ID;
         ruleAction.Save();

         rule.Save();

         // Send message and confirm that the route does not affect it.
         var smtp = new SmtpClientSimulator();
         var recipients = new List<string>();
         recipients.Add("test@example.test");
         smtp.Send("test@example.test", recipients, "Test", "Test message");

         var message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");
         Assert.IsTrue(message.Contains("Test message"));

         // Send a message and confirm that the rule affects it.
         var deliveryResults = new Dictionary<string, int>();
         deliveryResults["test@nonexistantdomain.com"] = 550;


         using (var server = new SmtpServerSimulator(1, smtpServerPort))
         {
            server.AddRecipientResult(deliveryResults);
            server.StartListen();

            // Send the actual message
            recipients = new List<string>();
            recipients.Add("test@nonexistantdomain.com");
            smtp.Send("test@example.test", recipients, "TestString", "Test message");


            server.WaitForCompletion();
         }

         // Submit the bounce message...
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // Download it.
         message = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         Assert.IsTrue(message.Contains("550"));
         Assert.IsTrue(message.Contains("test@nonexistantdomain.com"));
      }

      [Test]
      public void ActionSetHeaderContents()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRASetHeaderValue;
         ruleAction.HeaderName = "SomeHeader";
         ruleAction.Value = "SomeValue";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString", "Test 1");

         var sContents = Pop3ClientSimulator.AssertGetFirstMessageText("ruletest@example.test", "test");

         if (sContents.IndexOf("SomeHeader: SomeValue") <= 0)
            throw new Exception("Message header not set");
      }

      [Test]
      public void ActionSetHeaderContents_MACRO_ORIGINAL_HEADER()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRASetHeaderValue;
         ruleAction.HeaderName = "Subject";
         ruleAction.Value = "Foo: %MACRO_ORIGINAL_HEADER%";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString", "Test 1");

         var sContents = Pop3ClientSimulator.AssertGetFirstMessageText("ruletest@example.test", "test");

         StringAssert.Contains("Foo: TestString", sContents, sContents);
      }

      [Test]
      public void CriteriaContains()
      {
         var watch = new Stopwatch();
         watch.Start();

         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestStri",
            "Detta ska inte hamna Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "VaffeTestStringBaffe",
            "Detta ska hamna i mappen Inbox\\Wildcard");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Wildcard", 2);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 1);


         Trace.WriteLine(watch.ElapsedMilliseconds);
      }

      [Test]
      [Description("2009-12-08: The Body criteria didn't include the HTML body. It should.")]
      public void CriteriaContainsHTMLBody()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTBody;
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "MyHTMLBody";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         var message = "From: Someone <Someone@example.org>" + Environment.NewLine +
                       "Content-Type: text/html; charset=\"Windows-1251\"" + Environment.NewLine +
                       "Content-Transfer-Encoding: quoted-printable" + Environment.NewLine +
                       Environment.NewLine +
                       "<HTML><Center>MyHTMLBody</Center></HTML>" + Environment.NewLine;

         smtpClientSimulator.SendRaw("someone@example.org", account.Address, message);

         // The message should be placed in the Wildcard folder, since the HTML body of the message contains MyHTMLBody.
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox.Wildcard", 1);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 0);
      }

      [Test]
      public void CriteriaEquals()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTEquals;
         ruleCriteria.MatchValue = "TestString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "TestString",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "teststring",
            "Detta ska hamna Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "Testar",
            "Detta ska inte hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "teststring vaffe",
            "Detta ska inte hamna i mappen Inbox\\Wildcard");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Wildcard", 2);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 2);
      }


      [Test]
      public void CriteriaGreaterThan()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "2";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.GreaterThan";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "0",
            "Detta ska inte hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "1",
            "Detta ska inte hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "2",
            "Detta ska inte hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "3",
            "Detta ska hamna i mappen Inbox\\GreaterThan");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "4",
            "Detta ska hamna i mappen Inbox\\GreaterThan");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 3);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.GreaterThan", 2);
      }

      [Test]
      public void CriteriaLessThan()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTLessThan;
         ruleCriteria.MatchValue = "2";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.LessThan";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "0",
            "Detta ska hamna i mappen Inbox\\LessThan");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "2",
            "Detta ska hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "3",
            "Detta ska hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "4",
            "Detta ska hamna i mappen Inbox");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.LessThan", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 3);
      }

      [Test]
      public void CriteriaNotEquals()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTNotEquals;
         ruleCriteria.MatchValue = "SomeString";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.NotEquals";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeStringA",
            "Detta ska hamna i mappen Inbox\\NotEquals");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "somestring",
            "Detta ska inte hamna i mappen Inbox\\NotEquals");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.NotEquals", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 2);
      }

      [Test]
      public void CriteriaRegEx()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTRegExMatch;
         ruleCriteria.MatchValue = "[a-f]*";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.RegEx";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "abc",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "abcdef",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "abcdefghi",
            "Detta ska inte hamna i mappen Inbox\\Wildcard");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.RegEx", 2);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 1);
      }

      [Test]
      public void CriteriaWildcardExactMatch()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTWildcard;
         ruleCriteria.MatchValue = "Exact wildcard";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "Exact wildcard",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "Exact wildcard",
            "Detta ska hamna i mappen Inbox\\Wildcard");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Wildcard", 2);
      }

      [Test]
      public void CriteriaWildcardNoCase()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain,
            "CriteriaWildcardNoCase@example.test",
            "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTWildcard;
         ruleCriteria.MatchValue = "Exact*Match";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("CriteriaWildcardNoCase@example.test", "CriteriaWildcardNoCase@example.test",
            "exact Test match",
            "Detta ska hamna i mappen Inbox\\Wildcard");

         ImapClientSimulator.AssertMessageCount("CriteriaWildcardNoCase@example.test", "test", "Inbox.Wildcard", 1);
      }

      [Test]
      public void CriteriaWildcardPartialMatch()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTWildcard;
         ruleCriteria.MatchValue = "Exact*Match";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Wildcard";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "Exact Test Match",
            "Detta ska hamna i mappen Inbox\\Wildcard");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "ExactMatchArInte",
            "Detta ska inte hamna Inbox\\Wildcard");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Wildcard", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 1);
      }

      [Test]
      public void MovedToRenamedPublicFolder()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         _settings.IMAPPublicFolderName = "Public";

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");
         var publicFolders = _settings.PublicFolders;
         var folder = publicFolders.Add("Share1");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionExpunge, true);
         permission.Save();

         var rule = application.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "Public.Share1";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "Detta ska hamna i public folder.");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Public.Share1", 1);
      }

      [Test]
      public void TestBasics()
      {
         // Fetch the default domain

         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");

         // Add a rule to this account.
         AddSpamRule(account);
         AddCorporateRule(account);
         AddExactMatchRule(account);

         var smtpClientSimulator = new SmtpClientSimulator();


         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "**SPAM** INBOX->SPAM",
            "Detta ska hamna i mappen Inbox\\Spam");

         // Corporate folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "**CORPORATE** INBOX->CORPORATE",
            "Detta ska hamna i mappen Inbox\\Corporate");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "CORPORATE EXACT MATCH",
            "Detta ska hamna i mappen Inbox\\Corporate");

         // Inbox folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "**CORPORATE EXACT MATCH**",
            "Detta ska hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "INBOX",
            "Detta ska hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "INBOX",
            "Detta ska hamna i mappen Inbox");
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "INBOX",
            "Detta ska hamna i mappen Inbox");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Spam", 1);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox.Corporate", 2);
         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "Inbox", 4);

         // Test move to imap with mail with multiple recipients.

         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest-m1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest-m2@example.test", "test");
         AddSpamRule(account1);

         // Send email to both recipients
         var lstRecipients = new List<string> { "ruletest-m1@example.test", "ruletest-m2@example.test" };

         const string sBody = "Test of sending same email to multiple accounts.";

         smtpClientSimulator.Send(account1.Address, lstRecipients, "**SPAM** INBOX->SPAM", sBody);

         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox.Spam", 1);
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 1);

         var sim = new ImapClientSimulator();
         sim.ConnectAndLogon(account2.Address, "test");
         Assert.IsFalse(sim.SelectFolder("Inbox.Spam"));
      }

      [Test]
      [Description("Test that creating a message copy in a rule works using a account rule.")]
      public void TestCreateCopyAccountRule()
      {
         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");
         var account3 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest3@example.test", "test");

         // Set up a rule to forward from account1 to 2 and 3.
         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;
         rule.UseAND = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Only send one copy.
         ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "X-hMailServer-LoopCount";
         ruleCriteria.MatchType = eRuleMatchType.eMTLessThan;
         ruleCriteria.MatchValue = "1";
         ruleCriteria.Save();

         // Set up the actions to forward.
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRACreateCopy;
         ruleAction.To = "ruletest2@example.test";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the message to account 1. Make sure a copy is created by this rule.
         smtpClientSimulator.Send(account1.Address, account1.Address, "Test", "Test message.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);
         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox", 2);

         var firstTemp = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         var secondTemp = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");

         // This is where it gets really ugly. The order of the two deliveries
         // are not defined. The message created by the rule could be delivered
         // before the first message.
         var first = "";
         var second = "";

         if (secondTemp.Contains("X-hMailServer-LoopCount"))
         {
            first = firstTemp;
            second = secondTemp;
         }
         else
         {
            first = secondTemp;
            second = firstTemp;
         }

         Assert.IsFalse(first.Contains("X-hMailServer-LoopCount: 1"), first);
         Assert.IsFalse(first.Contains("X-CopyRule: Criteria test"), first);
         Assert.IsTrue(second.Contains("X-hMailServer-LoopCount"), second);
         Assert.IsTrue(second.Contains("X-CopyRule: Criteria test"), second);
      }

      [Test]
      [Description("Test that creating a message copy in a rule works using a global rule.")]
      public void TestCreateCopyGlobalRule()
      {
         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         // Set up a rule to forward from account1 to 2 and 3.
         var rule = _application.Rules.Add();
         rule.Name = "CriteriaTest";
         rule.Active = true;
         rule.UseAND = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Only send one copy.
         ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "X-hMailServer-LoopCount";
         ruleCriteria.MatchType = eRuleMatchType.eMTLessThan;
         ruleCriteria.MatchValue = "1";
         ruleCriteria.Save();

         // Set up the actions to create a copy
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRACreateCopy;
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the message to account 1. Make sure a copy is created by this rule.
         smtpClientSimulator.Send(account1.Address, new List<string> { account1.Address, account2.Address }, "Test",
            "Test message.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);
         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox", 2);
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 2);

         /*
          * The delivery order is not guaranteed. The copied message may be delivered
          * before the original message. Check both situations.
          * 
          */
         var first = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         var second = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");

         if (first.Contains("X-hMailServer-LoopCount: 1"))
         {
            Assert.IsFalse(second.Contains("X-CopyRule: CriteriaTest"), first);
            Assert.IsTrue(first.Contains("X-hMailServer-LoopCount"), first);
         }
         else
         {
            Assert.IsTrue(second.Contains("X-hMailServer-LoopCount: 1"), second);
            Assert.IsTrue(second.Contains("X-CopyRule: CriteriaTest"), first);
         }

         first = Pop3ClientSimulator.AssertGetFirstMessageText(account2.Address, "test");
         second = Pop3ClientSimulator.AssertGetFirstMessageText(account2.Address, "test");

         if (first.Contains("X-hMailServer-LoopCount: 1"))
         {
            Assert.IsFalse(second.Contains("X-hMailServer-LoopCount"), first);
            Assert.IsTrue(first.Contains("X-CopyRule: CriteriaTest"), first);
         }
         else
         {
            Assert.IsTrue(second.Contains("X-hMailServer-LoopCount: 1"), second);
            Assert.IsTrue(second.Contains("X-CopyRule: CriteriaTest"), first);
         }
      }

      [Test]
      [Description("Test the delivery attempts criteria.")]
      public void TestDeliveryAttempts()
      {
         // Add an account
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "test@example.test", "test");
         var adminAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "admin@example.test", "test");

         // Set up a rule to forward from account1 to 2 and 3.
         var rule = _application.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;
         rule.UseAND = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTDeliveryAttempts;
         ruleCriteria.MatchType = eRuleMatchType.eMTEquals;
         ruleCriteria.MatchValue = "2";
         ruleCriteria.Save();

         // The second time we try to deliver an email, forward a copy to the admin!
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAForwardEmail;
         ruleAction.To = adminAccount.Address;
         ruleAction.Save();

         // And then delete it.
         ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRADeleteEmail;
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();


         var deliveryResults = new Dictionary<string, int>();
         deliveryResults["ahem@dummy-example.com"] = 452;

         var smtpServerPort = TestSetup.GetNextFreePort();
         using (var smtpServer = new SmtpServerSimulator(1, smtpServerPort))
         {
            smtpServer.AddRecipientResult(deliveryResults);
            smtpServer.StartListen();

            // Add a route so we can connect to localhost.
            TestSetup.AddRoutePointingAtLocalhost(2, smtpServerPort, false);

            var smtpClientSimulator = new SmtpClientSimulator();

            // Test to send the message to account 1. Make sure a copy is created by this rule.
            smtpClientSimulator.Send(account.Address, new List<string> { "ahem@dummy-example.com" }, "Test",
               "Test message.");

            smtpServer.WaitForCompletion();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);

         var first = Pop3ClientSimulator.AssertGetFirstMessageText(adminAccount.Address, "test");

         Assert.IsTrue(first.Contains("X-hMailServer-LoopCount: 1"), first);

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("Test that forwarding set up in a rule works.")]
      public void TestForward()
      {
         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");
         var account3 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest3@example.test", "test");

         // Set up a rule to forward from account1 to 2 and 3.
         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Set up the actions to forward.
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAForwardEmail;
         ruleAction.To = "ruletest2@example.test";
         ruleAction.Save();

         var ruleAction2 = rule.Actions.Add();
         ruleAction2.Type = eRuleActionType.eRAForwardEmail;
         ruleAction2.To = "ruletest3@example.test";
         ruleAction2.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the messge to account 1.
         smtpClientSimulator.Send(account1.Address, account1.Address, "Test", "Test message.");

         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(account3.Address, "test", "Inbox", 1);
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 1);
      }

      [Test]
      [Description("Test forward rule when spam flagged.")]
      public void TestForwardAbortSpamFlagged()
      {
         CustomAsserts.AssertSpamAssassinIsRunning();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         // Set up a rule to forward from account1 to 2 and 3.
         var oRule = account1.Rules.Add();
         oRule.Name = "Criteria test";
         oRule.Active = true;

         var oRuleCriteria = oRule.Criterias.Add();
         oRuleCriteria.UsePredefined = true;
         oRuleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         oRuleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         oRuleCriteria.MatchValue = "0";
         oRuleCriteria.Save();

         // Set up the actions to forward.
         var oRuleAction = oRule.Actions.Add();
         oRuleAction.Type = eRuleActionType.eRAForwardEmail;
         oRuleAction.To = "ruletest2@test.com";
         oRuleAction.AbortSpamFlagged = true;
         oRuleAction.Save();

         // Save the rule in the database
         oRule.Save();

         // Set Thresholds
         _settings.AntiSpam.SpamMarkThreshold = 5;
         _settings.AntiSpam.SpamDeleteThreshold = 20;

         // Enable SpamAssassin
         _settings.AntiSpam.SpamAssassinEnabled = true;
         _settings.AntiSpam.SpamAssassinHost = "localhost";
         _settings.AntiSpam.SpamAssassinPort = 783;
         _settings.AntiSpam.SpamAssassinMergeScore = false;
         _settings.AntiSpam.SpamAssassinScore = 5;

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the messge to account 1.
         smtpClientSimulator.Send(account1.Address, account1.Address, "Test message", "This is a test message with spam.\r\n XJS*C4JDBQADN1.NSBN3*2IDNEN*GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X.");

         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox", 1);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var defaultLogText = TestSetup.ReadExistingTextFile(LogHandler.GetDefaultLogFileName());
         Assert.IsTrue(defaultLogText.Contains("RuleApplier::ApplyAction_Forward aborted, message marked as spam"));
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 0);
      }

      [Test]
      [Description("Test to move to a public folder without permission.")]
      public void TestMoveToPublicFolderWithoutPermission()
      {
         var application = SingletonProvider<TestSetup>.Instance.GetApp();

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest@example.test", "test");
         var publicFolders = _settings.PublicFolders;
         var folder = publicFolders.Add("Share1");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionAccountID = account1.ID;
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionExpunge, true);
         permission.Save();

         var rule = account1.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "#public.Share1.Sub";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Spam folder
         smtpClientSimulator.Send("ruletest@example.test", "ruletest@example.test", "SomeString",
            "This should end up in the inbox since user lacks right.");

         ImapClientSimulator.AssertMessageCount("ruletest@example.test", "test", "INBOX", 1);
      }

      [Test]
      [Description("Issue 338, Recipient Count is wrong under certain circumstances")]
      public void TestRecipientCountInAccountLevelRule()
      {
         _settings.Scripting.Enabled = true;

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         CreatePrintRecipientCountRule(account1.Rules);

         SmtpClientSimulator.StaticSend(account1.Address, account1.Address, "SomeString",
            "Detta ska inte hamna i mappen Inbox.Overriden.Test");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         // This should print a single list.
         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         CustomAsserts.AssertDeleteFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("\"1\""), eventLogText);

         // Send message to two recipients. Recipient should still be one, since it's an account-level rule.
         SmtpClientSimulator.StaticSend(account1.Address, new List<string> { account1.Address, account2.Address },
            "SomeString", "Detta ska inte hamna i mappen Inbox.Overriden.Test");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         // This should print a single list.
         eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         CustomAsserts.AssertDeleteFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("\"1\""), eventLogText);
      }

      [Test]
      [Description("Issue 338, Recipient Count is wrong under certain circumstances")]
      public void TestRecipientCountInGlobalRule()
      {
         _settings.Scripting.Enabled = true;

         // Add an account
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         CreatePrintRecipientCountRule(_application.Rules);

         SmtpClientSimulator.StaticSend(account1.Address, account1.Address, "SomeString",
            "Detta ska inte hamna i mappen Inbox.Overriden.Test");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         // This should print a single list.

         var eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         CustomAsserts.AssertDeleteFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("\"1\""), eventLogText);

         // Send message to two recipients. 
         SmtpClientSimulator.StaticSend(account1.Address, new List<string> { account1.Address, account2.Address },
            "SomeString", "Detta ska inte hamna i mappen Inbox.Overriden.Test");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         // This should print a two recipients. Global rule is affected before message reaches recipients.
         eventLogText = TestSetup.ReadExistingTextFile(LogHandler.GetEventLogFileName());
         CustomAsserts.AssertDeleteFile(LogHandler.GetEventLogFileName());
         Assert.IsTrue(eventLogText.Contains("\"2\""), eventLogText);
      }

      [Test]
      [Description("Test that replying using rule works.")]
      public void TestReply()
      {
         // Add accounts
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         // Set up a rule to reply to any message sent to account2.
         var rule = account2.Rules.Add();
         rule.Name = "Criteria test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         // Set up the actions to forward.
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAReply;
         ruleAction.FromAddress = account2.Address;
         ruleAction.FromName = "Rule Test 2";
         ruleAction.Subject = "Autoreply";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the message to account 2.
         smtpClientSimulator.Send(account1.Address, account2.Address, "Test", "Test message.");
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 1);

         // Make sure a reply is sent back to account 1.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         var message = CustomAsserts.AssertGetFirstMessage(account1, "Inbox");

         Assert.AreEqual(string.Empty, message.FromAddress);
         Assert.AreEqual("auto-replied", message.get_HeaderValue("Auto-Submitted"));
      }

      [Test]
      [Description("Auto-reply generated by a rule must use a null envelope-from (Return-Path: <>) to prevent bounce loops per RFC 3834.")]
      public void WhenRuleRepliesEnvelopeFromShouldBeNull()
      {
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replytest-src@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replytest-dst@example.test", "test");

         var rule = account2.Rules.Add();
         rule.Name = "Auto-reply rule";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAReply;
         ruleAction.FromAddress = account2.Address;
         ruleAction.FromName = "Auto Reply";
         ruleAction.Subject = "Autoreply";
         ruleAction.Save();

         rule.Save();

         SmtpClientSimulator.StaticSend(account1.Address, account2.Address, "Test", "Test message.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var replyText = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");
         Assert.IsTrue(replyText.Contains("Return-Path: <>"),
            "Rule-based auto-reply must use null envelope-from to prevent bounce loops.");
      }

      [Test]
      [Description("Rule REPLY body must be QP-wrapped to 76 chars per RFC 2045 section 6.7 (issue #171).")]
      public void WhenRuleReplyBodyExceedsQPLineLimitShouldWrapToRfc2045()
      {
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "qpwrap-src@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "qpwrap-dst@example.test", "test");

         var rule = account2.Rules.Add();
         rule.Name = "QP wrap test";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         const string longLine = "Please note my normal office hours are Monday and Tuesday only.  I will respond to your email when I return.";

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAReply;
         ruleAction.FromAddress = account2.Address;
         ruleAction.Subject = "Autoreply";
         ruleAction.Body = longLine;
         ruleAction.Save();

         rule.Save();

         SmtpClientSimulator.StaticSend(account1.Address, account2.Address, "Test", "Trigger.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var rawMessage = Pop3ClientSimulator.AssertGetFirstMessageText(account1.Address, "test");

         foreach (var line in rawMessage.Split(new[] { "\r\n", "\n" }, StringSplitOptions.None))
            Assert.LessOrEqual(line.Length, 76, $"QP line exceeds 76 chars: {line}");
      }

      [Test]
      [Description("Test reply rule when spam flagged.")]
      public void TestReplyAbortSpamFlagged()
      {
         CustomAsserts.AssertSpamAssassinIsRunning();

         // Add accounts
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest1@example.test", "test");
         var account2 = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruletest2@example.test", "test");

         // Set up a rule to reply to any message sent to account2.
         var oRule = account2.Rules.Add();
         oRule.Name = "Criteria test";
         oRule.Active = true;

         var oRuleCriteria = oRule.Criterias.Add();
         oRuleCriteria.UsePredefined = true;
         oRuleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         oRuleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         oRuleCriteria.MatchValue = "0";
         oRuleCriteria.Save();

         // Set up the actions to forward.
         var oRuleAction = oRule.Actions.Add();
         oRuleAction.Type = eRuleActionType.eRAReply;
         oRuleAction.FromAddress = account2.Address;
         oRuleAction.FromName = "Rule Test 2";
         oRuleAction.Subject = "Autoreply";
         oRuleAction.AbortSpamFlagged = true;
         oRuleAction.Save();

         // Save the rule in the database
         oRule.Save();

         // Set Thresholds
         _settings.AntiSpam.SpamMarkThreshold = 5;
         _settings.AntiSpam.SpamDeleteThreshold = 20;

         // Enable SpamAssassin
         _settings.AntiSpam.SpamAssassinEnabled = true;
         _settings.AntiSpam.SpamAssassinHost = "localhost";
         _settings.AntiSpam.SpamAssassinPort = 783;
         _settings.AntiSpam.SpamAssassinMergeScore = false;
         _settings.AntiSpam.SpamAssassinScore = 5;

         var smtpClientSimulator = new SmtpClientSimulator();

         // Test to send the message to account 2.
         smtpClientSimulator.Send(account1.Address, account2.Address, "Test message", "This is a test message with spam.\r\n XJS*C4JDBQADN1.NSBN3*2IDNEN*GTUBE-STANDARD-ANTI-UBE-TEST-EMAIL*C.34X.");
         ImapClientSimulator.AssertMessageCount(account2.Address, "test", "Inbox", 1);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var defaultLogText = TestSetup.ReadExistingTextFile(LogHandler.GetDefaultLogFileName());
         Assert.IsTrue(defaultLogText.Contains("RuleApplier::ApplyAction_Reply aborted, message marked as spam"));
         // Make sure a reply is not sent back to account 1.
         ImapClientSimulator.AssertMessageCount(account1.Address, "test", "Inbox", 0);

         oRuleAction.AbortSpamFlagged = false;
         oRuleAction.Save();
      }

      /// <summary>
      /// The rule-loop counter is the depth of the chain that produced a message, not a
      /// property of the message. ApplyAction_Reply built a brand-new Message and called
      /// IncreaseRuleLoopCount() on it, and since a message with no
      /// X-hMailServer-LoopCount header counts as zero, the reply always went out
      /// carrying 1 - whatever the message that triggered it carried.
      ///
      /// Against the unfixed server this test fails on the final assertion with
      /// "X-hMailServer-LoopCount: 1" in the reply instead of 3.
      /// </summary>
      [Test]
      [Description("A rule REPLY must carry the triggering message's rule-loop count forward, not restart it at 1.")]
      public void WhenRuleRepliesTheLoopCountShouldBeCarriedForward()
      {
         // Stated rather than assumed: the shipped default is 5 and PerformBasicSetup
         // does not reset it, so a fixture that ran earlier could have left it anywhere.
         _settings.RuleLoopLimit = 5;

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopcarry-src@example.test", "test");
         var replier = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopcarry-dst@example.test", "test");

         var rule = replier.Rules.Add();
         rule.Name = "Loop count carry";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = true;
         ruleCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         ruleCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         ruleCriteria.MatchValue = "0";
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAReply;
         ruleAction.FromAddress = replier.Address;
         ruleAction.FromName = "Loop Carry";
         ruleAction.Subject = "Autoreply";
         ruleAction.Body = "Autoreply body.";
         ruleAction.Save();

         rule.Save();

         // Two hops already spent on this message, so the reply it provokes is the third.
         var message =
            "From: " + sender.Address + "\r\n" +
            "To: " + replier.Address + "\r\n" +
            "Subject: Carry the count\r\n" +
            "X-hMailServer-LoopCount: 2\r\n" +
            "\r\n" +
            "Trigger the reply rule.\r\n";

         var smtpClientSimulator = new SmtpClientSimulator();
         smtpClientSimulator.SendRaw(sender.Address, replier.Address, message);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var replyText = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");

         StringAssert.Contains("X-hMailServer-LoopCount: 3", replyText, replyText);
      }

      /// <summary>
      /// Why the count has to be carried and not merely reported: the Forward action
      /// deliberately does not honour Auto-Submitted (a forward is not a generated
      /// response), so a chain that alternates reply and forward is guarded by the
      /// counter and by nothing else. Restarting the counter at every reply makes that
      /// chain unbounded.
      ///
      /// Here the reply is generated at the loop limit, so the forward that would carry
      /// it onwards must be refused.
      ///
      /// Against the unfixed server the reply carries 1 rather than 5, the forward is
      /// permitted, and this test fails on the observer's message count being 1.
      /// </summary>
      [Test]
      [Description("A reply generated at the rule-loop limit must not then be forwardable by another rule.")]
      public void WhenReplyReachesTheLoopLimitAFollowOnForwardShouldBeRefused()
      {
         _settings.RuleLoopLimit = 5;

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopstop-src@example.test", "test");
         var replier = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopstop-dst@example.test", "test");
         var observer = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopstop-obs@example.test", "test");

         // The replier answers anything it receives.
         var replyRule = replier.Rules.Add();
         replyRule.Name = "Loop stop reply";
         replyRule.Active = true;

         var replyCriteria = replyRule.Criterias.Add();
         replyCriteria.UsePredefined = true;
         replyCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         replyCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         replyCriteria.MatchValue = "0";
         replyCriteria.Save();

         var replyAction = replyRule.Actions.Add();
         replyAction.Type = eRuleActionType.eRAReply;
         replyAction.FromAddress = replier.Address;
         replyAction.FromName = "Loop Stop";
         replyAction.Subject = "Autoreply";
         replyAction.Body = "Autoreply body.";
         replyAction.Save();

         replyRule.Save();

         // The sender forwards anything it receives - including that reply - onwards.
         var forwardRule = sender.Rules.Add();
         forwardRule.Name = "Loop stop forward";
         forwardRule.Active = true;

         var forwardCriteria = forwardRule.Criterias.Add();
         forwardCriteria.UsePredefined = true;
         forwardCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         forwardCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         forwardCriteria.MatchValue = "0";
         forwardCriteria.Save();

         var forwardAction = forwardRule.Actions.Add();
         forwardAction.Type = eRuleActionType.eRAForwardEmail;
         forwardAction.To = observer.Address;
         forwardAction.Save();

         forwardRule.Save();

         // Four hops spent, so the reply is the fifth and the limit is reached exactly at
         // the point the forward would have to be generated.
         var message =
            "From: " + sender.Address + "\r\n" +
            "To: " + replier.Address + "\r\n" +
            "Subject: Stop the chain\r\n" +
            "X-hMailServer-LoopCount: 4\r\n" +
            "\r\n" +
            "Trigger the reply rule.\r\n";

         var smtpClientSimulator = new SmtpClientSimulator();
         smtpClientSimulator.SendRaw(sender.Address, replier.Address, message);

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The reply itself is delivered; it is the hop after it that must not happen.
         ImapClientSimulator.AssertMessageCount(sender.Address, "test", "Inbox", 1);
         ImapClientSimulator.AssertMessageCount(observer.Address, "test", "Inbox", 0);

         // Polls for the entry and then clears the log. The clearing matters as much as
         // the assertion: PerformBasicSetup calls AssertNoReportedError, so an error log
         // left behind by a test that provoked one on purpose fails every test after it.
         CustomAsserts.AssertReportedError("Maximum rule loop count reached");
      }

      /// <summary>
      /// MessageData::GetRuleLoopCount read X-hMailServer-LoopCount with _ttoi, and the
      /// header comes with the message, from whoever sent it. "-5" passed
      /// IsGeneratedResponseAllowed's count >= limit, so a forwarding loop between two
      /// accounts - what RuleLoopLimit exists to bound - went round once more for each
      /// unit below zero the sender wrote, some two thousand million times for
      /// "-2147483648"; on Linux, where _ttoi is (int) wcstol, "2147483648" and
      /// "4294967295" read as INT_MIN and -1 and did the same. What is not a count up to
      /// the limit now reads as the limit (RuleGuard::ReadLoopCount), and the forward is
      /// refused as it is at the limit.
      ///
      /// Against the unfixed server the observer receives the forward of "-5" (on
      /// Linux, of "2147483648" before it), and the test fails there; "-2147483648",
      /// "+1" and the empty value would be forwarded too.
      /// </summary>
      [Test]
      [Description("A rule forward refuses a message whose X-hMailServer-LoopCount is not a count, as at the rule loop limit.")]
      public void WhenTheLoopCountIsNoCountAForwardShouldBeRefused()
      {
         _settings.RuleLoopLimit = 5;

         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopread-src@example.test", "test");
         var forwarder = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopread-fwd@example.test", "test");
         var observer = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopread-obs@example.test", "test");

         // The forwarder forwards anything it receives to the observer.
         var forwardRule = forwarder.Rules.Add();
         forwardRule.Name = "Loop read forward";
         forwardRule.Active = true;

         var forwardCriteria = forwardRule.Criterias.Add();
         forwardCriteria.UsePredefined = true;
         forwardCriteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         forwardCriteria.MatchType = eRuleMatchType.eMTGreaterThan;
         forwardCriteria.MatchValue = "0";
         forwardCriteria.Save();

         var forwardAction = forwardRule.Actions.Add();
         forwardAction.Type = eRuleActionType.eRAForwardEmail;
         forwardAction.To = observer.Address;
         forwardAction.Save();

         forwardRule.Save();

         string[] values = { "2147483647", "2147483648", "4294967295", "99999999999", "-5", "-2147483648", "12abc", "+1", "" };

         var delivered = 0;
         foreach (var value in values)
         {
            var message =
               "From: " + sender.Address + "\r\n" +
               "To: " + forwarder.Address + "\r\n" +
               "Subject: Loop count '" + value + "'\r\n" +
               "X-hMailServer-LoopCount: " + value + "\r\n" +
               "\r\n" +
               "Trigger the forward rule.\r\n";

            var smtpClientSimulator = new SmtpClientSimulator();
            smtpClientSimulator.SendRaw(sender.Address, forwarder.Address, message);

            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            // The message itself is delivered; it is the forward that must not happen.
            delivered++;
            ImapClientSimulator.AssertMessageCount(forwarder.Address, "test", "Inbox", delivered);
            Assert.AreEqual(0, new Pop3ClientSimulator().GetMessageCount(observer.Address, "test"),
               "A message carrying X-hMailServer-LoopCount: '" + value + "' was forwarded.");
         }

         // Each refusal is reported, as at the limit; this reads and clears the log.
         CustomAsserts.AssertReportedError("Maximum rule loop count reached");

         // The control: a count below the limit is forwarded, carrying + 1.
         var counted =
            "From: " + sender.Address + "\r\n" +
            "To: " + forwarder.Address + "\r\n" +
            "Subject: A count\r\n" +
            "X-hMailServer-LoopCount: 1\r\n" +
            "\r\n" +
            "Trigger the forward rule.\r\n";

         new SmtpClientSimulator().SendRaw(sender.Address, forwarder.Address, counted);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var forwarded = Pop3ClientSimulator.AssertGetFirstMessageText(observer.Address, "test");
         StringAssert.Contains("X-hMailServer-LoopCount: 2", forwarded, forwarded);
      }

      private static Rule NewRuleMatchingEverything(Account account, string name)
      {
         var rule = account.Rules.Add();
         rule.Name = name;
         rule.Active = true;

         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = "0";
         criteria.Save();

         return rule;
      }

      // The fields of this name in a delivered message's header, each unfolded - a
      // continuation line joined to the line before it - up to the first empty line.
      private static List<string> UnfoldedHeaderFields(string message, string name)
      {
         var fields = new List<string>();
         string current = null;
         foreach (var line in message.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.Length == 0)
               break;

            if (line[0] == ' ' || line[0] == '\t')
            {
               if (current != null)
                  current += " " + line.Trim();
               continue;
            }

            if (current != null)
               fields.Add(current);
            current = line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase) ? line : null;
         }

         if (current != null)
            fields.Add(current);

         return fields;
      }

      // The lines of a delivered message's header that begin a field of this name, as
      // a mail client finds them: up to the first empty line.
      private static List<string> HeaderFieldLines(string message, string name)
      {
         var lines = new List<string>();
         foreach (var line in message.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.Length == 0)
               break;

            if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
               lines.Add(line);
         }

         return lines;
      }

      /// <summary>
      /// X-hMailServer-LoopCount counts the forwards, redirects and replies a message has
      /// been through, and a forward reads it when it runs - after the actions before it.
      /// A Set header value action that set it to 0 ahead of a forward gave every copy
      /// the whole rule loop limit again, so that one such account and an ordinary
      /// forwarder looped between local accounts without bound; an account may change its
      /// own rules over COM. RuleAction.Save now refuses the header in any case, and a
      /// name that would write a field of another name - one with a line break or a
      /// colon in it - and says why; so does Rule.Save, which saves the actions as they
      /// are in memory, for an action changed after its own Save.
      ///
      /// Against the unfixed server every Save below succeeds.
      /// </summary>
      [Test]
      [Description("A Set header value action on X-hMailServer-LoopCount, or on a name that is not a header name, is refused when it is saved, and says why.")]
      public void ARuleActionMayNotSetTheLoopCount()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopset@example.test", "test");
         var rule = NewRuleMatchingEverything(account, "Loop count set");
         rule.Save();

         var refusals = new Dictionary<string, string>
         {
            { "X-hMailServer-LoopCount", "a rule may not set it" },
            { "x-hmailserver-loopcount", "a rule may not set it" },
            { " X-HMAILSERVER-LOOPCOUNT ", "a rule may not set it" },
            { "X-Note\r\nX-hMailServer-LoopCount", "is not a header name a rule can set" },
            { "X-hMailServer-LoopCount:", "is not a header name a rule can set" },
         };

         foreach (var refusal in refusals)
         {
            var action = rule.Actions.Add();
            action.Type = eRuleActionType.eRASetHeaderValue;
            action.HeaderName = refusal.Key;
            action.Value = "0";

            var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => action.Save(),
               "A Set header value action on '" + refusal.Key + "' was saved.");
            StringAssert.Contains(refusal.Value, refused.Message, refused.Message);
         }

         Assert.AreEqual(0, rule.Actions.Count, "A refused action is among the rule's actions.");

         // The control: another header is set, and saved.
         var note = rule.Actions.Add();
         note.Type = eRuleActionType.eRASetHeaderValue;
         note.HeaderName = "X-Rule-Note";
         note.Value = "set";
         note.Save();
         rule.Save();

         // Changed after its own Save, the action is saved with the rule, which refuses it.
         note.HeaderName = "X-hMailServer-LoopCount";
         var refusedWithTheRule = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
            "A rule whose action was changed to set X-hMailServer-LoopCount was saved.");
         StringAssert.Contains("a rule may not set it", refusedWithTheRule.Message, refusedWithTheRule.Message);
      }

      /// <summary>
      /// An action that sets X-hMailServer-LoopCount stored before Save refused it - or
      /// restored from a backup, or written to the table - is not applied, and the
      /// application log says so; the forward after it in the same rule still goes,
      /// counted from the count the message carries. Here two accounts forward to each
      /// other and the first resets the count before its forward: against the unfixed
      /// server the message goes round for ever and the queue never empties. With the
      /// action not applied the count goes 0, 1, 2 and so on, and the second's forward at
      /// 5 is refused - the first holds the copies at 0, 2 and 4, and the second those at
      /// 1, 3 and 5.
      /// </summary>
      [Test]
      [Description("A stored Set header value action on X-hMailServer-LoopCount is not applied, and a forwarding loop it was in ends at the rule loop limit.")]
      public void AStoredActionOnTheLoopCountIsNotApplied()
      {
         _settings.RuleLoopLimit = 5;

         var first = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopreset-a@example.test", "test");
         var second = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopreset-b@example.test", "test");

         var resetRule = NewRuleMatchingEverything(first, "Reset and forward");
         var reset = resetRule.Actions.Add();
         reset.Type = eRuleActionType.eRASetHeaderValue;
         reset.HeaderName = "X-Loop-Placeholder-Stored";
         reset.Value = "0";
         reset.Save();
         var forwardOn = resetRule.Actions.Add();
         forwardOn.Type = eRuleActionType.eRAForwardEmail;
         forwardOn.To = second.Address;
         forwardOn.Save();
         resetRule.Save();

         // As an action stored before Save refused the name. The account's rules are
         // read from the table again at the next delivery, the save having asked for it.
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionheader = 'X-hMailServer-LoopCount' where actionheader = 'X-Loop-Placeholder-Stored'");

         var backRule = NewRuleMatchingEverything(second, "Forward back");
         var forwardBack = backRule.Actions.Add();
         forwardBack.Type = eRuleActionType.eRAForwardEmail;
         forwardBack.To = first.Address;
         forwardBack.Save();
         backRule.Save();

         SmtpClientSimulator.StaticSend("loopreset-sender@example.test", first.Address, "Loop", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ImapClientSimulator.AssertMessageCount(first.Address, "test", "Inbox", 3);
         ImapClientSimulator.AssertMessageCount(second.Address, "test", "Inbox", 3);

         Assert.IsTrue(LogHandler.DefaultLogContains("Set header value action of rule"),
            "The action not applied is not in the application log.");
         Assert.IsTrue(LogHandler.DefaultLogContains("X-hMailServer-LoopCount counts the forwards"),
            "The application log does not say why the action was not applied.");

         // The second's forward at the limit is reported; this reads and clears the log.
         CustomAsserts.AssertReportedError("Maximum rule loop count reached");
      }

      /// <summary>
      /// The MIME writer wrote a line break in a value as it stood, so a Set header value
      /// action whose value held a line break and "X-hMailServer-LoopCount: 0" wrote a
      /// second field - the count, reset, under a name Save did not see. A line break that
      /// does not fold the line is now written as a space.
      ///
      /// Against the unfixed server the delivered header holds X-hMailServer-LoopCount: 0
      /// as a field of its own, above the 3 the message came with.
      /// </summary>
      [Test]
      [Description("A Set header value action's value with a line break is written as one field, and writes no loop count.")]
      public void ASetHeaderValueWithALineBreakIsOneField()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopvalue@example.test", "test");
         var rule = NewRuleMatchingEverything(account, "Note with a line break");
         var note = rule.Actions.Add();
         note.Type = eRuleActionType.eRASetHeaderValue;
         note.HeaderName = "X-Rule-Note";
         note.Value = "a\r\nX-hMailServer-LoopCount: 0";
         note.Save();
         rule.Save();

         var message =
            "From: loopvalue-sender@example.test\r\n" +
            "To: " + account.Address + "\r\n" +
            "Subject: A note\r\n" +
            "X-hMailServer-LoopCount: 3\r\n" +
            "\r\n" +
            "Body.\r\n";

         new SmtpClientSimulator().SendRaw("loopvalue-sender@example.test", account.Address, message);
         var delivered = Pop3ClientSimulator.AssertGetFirstMessageText(account.Address, "test");

         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 3" }, HeaderFieldLines(delivered, "X-hMailServer-LoopCount"), delivered);
         CollectionAssert.AreEqual(new[] { "X-Rule-Note: a  X-hMailServer-LoopCount: 0" }, HeaderFieldLines(delivered, "X-Rule-Note"), delivered);
      }

      /// <summary>
      /// A copy carries its rule's name in X-CopyRule, set where the message already has
      /// one - above the copy's count from the second copy on, and from the first when
      /// the sender wrote one at the top - and the MIME writer wrote a line break in a
      /// value as it stood. A rule named "Copy", a line break and
      /// "X-hMailServer-LoopCount: 0" wrote the count, reset, above the copy's own at
      /// every copy, so that an account copying its own mail made copies without end; an
      /// account may name its own rules over COM. The writer now keeps a value to one
      /// field (MimeField::AppendAsOneField): the copies count 1 to 5, and the one at 5
      /// is not copied again.
      ///
      /// Against the unfixed server every copy reads a count of 0, and the queue never
      /// empties.
      /// </summary>
      [Test]
      [Description("A copy rule whose name holds a line break writes no loop count into the copy, and the copies stop at the rule loop limit.")]
      public void ACopyRuleWhoseNameHoldsALineBreakStopsAtTheLimit()
      {
         _settings.RuleLoopLimit = 5;

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "loopcopy@example.test", "test");
         var rule = NewRuleMatchingEverything(account, "Copy\r\nX-hMailServer-LoopCount: 0");
         var copy = rule.Actions.Add();
         copy.Type = eRuleActionType.eRACreateCopy;
         copy.Save();
         rule.Save();

         var message =
            "X-CopyRule: written by the sender\r\n" +
            "From: loopcopy-sender@example.test\r\n" +
            "To: " + account.Address + "\r\n" +
            "Subject: Copied\r\n" +
            "\r\n" +
            "Body.\r\n";

         new SmtpClientSimulator().SendRaw("loopcopy-sender@example.test", account.Address, message);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The message, and its copies at 1, 2, 3, 4 and 5.
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 6);

         // The copy at the limit is not copied, which is reported; this reads and clears
         // the log.
         CustomAsserts.AssertReportedError("Maximum rule loop count reached");
      }

      /// <summary>
      /// An account may change its own rules over COM, signed in as itself, and a rule's
      /// reply went out from whatever address its action named - another account's here,
      /// or anyone's - signed with the DKIM key of the domain it named. A reply of an
      /// account's rule now goes only from an address the account may send as, the rule
      /// authenticated SMTP and the webmail's send apply: Rule.Save and RuleAction.Save
      /// refuse another, whoever saves the rule, and say why; the account's own address
      /// and an alias of it are saved.
      ///
      /// Against the unfixed server every Save below succeeds.
      /// </summary>
      [Test]
      [Description("A reply of an account's rule is saved only from an address the account may send as, whether the account or the administrator saves it.")]
      public void AnAccountsRuleRepliesOnlyAsTheAccount()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replyas-owner@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replyas-ceo@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "replyas-alias@example.test", owner.Address);

         // Signed in as the account itself, as a mail client or a script of its own is.
         var itself = new Application();
         var signedIn = itself.Authenticate(owner.Address, "test") as Account;
         Assert.IsNotNull(signedIn, "The account could not sign in to COM.");

         foreach (var asSaved in new[] { signedIn, owner })
         {
            foreach (var from in new[] { other.Address, "someone@example.net" })
            {
               var rule = NewRuleMatchingEverything(asSaved, "Reply as " + from);
               var reply = rule.Actions.Add();
               reply.Type = eRuleActionType.eRAReply;
               reply.FromAddress = from;
               reply.FromName = "Reply";
               reply.Subject = "Autoreply";
               reply.Save();

               var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
                  "An account's rule replying from " + from + " was saved.");
               StringAssert.Contains("may send as", refused.Message, refused.Message);
            }

            // The account's own address, and an alias of it, are saved; and an action of
            // a saved rule changed to another's address is refused by its own Save.
            foreach (var from in new[] { owner.Address, "replyas-alias@example.test" })
            {
               var rule = NewRuleMatchingEverything(asSaved, "Reply as " + from);
               var reply = rule.Actions.Add();
               reply.Type = eRuleActionType.eRAReply;
               reply.FromAddress = from;
               reply.Subject = "Autoreply";
               reply.Save();
               rule.Save();

               reply.FromAddress = other.Address;
               var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => reply.Save(),
                  "A saved rule's reply changed to " + other.Address + " was saved.");
               StringAssert.Contains("may send as", refused.Message, refused.Message);

               rule.Delete();
            }
         }

         GC.KeepAlive(itself);
      }

      /// <summary>
      /// A Set header value action of an account's rule or a domain's set any header but
      /// the loop count, so an account could set From to another account's address, or
      /// take away the DKIM-Signature a message came with, ahead of a forward - and the
      /// forwarded copy went out in that name, signed with that domain's key. From,
      /// Sender, Resent-From, Resent-Sender and DKIM-Signature may now be set by a global
      /// rule alone, which only the server administrator saves: an account's or a
      /// domain's rule is refused, by name in any case, and says why.
      ///
      /// Against the unfixed server every Save below succeeds.
      /// </summary>
      [Test]
      [Description("A Set header value action on From, Sender, Resent-From, Resent-Sender or DKIM-Signature is refused for an account's rule and a domain's, and saved for a global rule.")]
      public void OnlyAGlobalRuleMaySetWhoTheMessageIsFrom()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "setfrom-owner@example.test", "test");
         var names = new[] { "From", "sender", " Resent-From ", "RESENT-SENDER", "DKIM-Signature" };

         var itself = new Application();
         var signedIn = itself.Authenticate(owner.Address, "test") as Account;
         Assert.IsNotNull(signedIn, "The account could not sign in to COM.");

         var accountRule = NewRuleMatchingEverything(signedIn, "Set who it is from");
         accountRule.Save();

         var domainRule = _domain.Rules.Add();
         domainRule.Name = "Domain sets who it is from";
         domainRule.Active = false;
         domainRule.Save();

         foreach (var rule in new[] { accountRule, domainRule })
         {
            foreach (var name in names)
            {
               var action = rule.Actions.Add();
               action.Type = eRuleActionType.eRASetHeaderValue;
               action.HeaderName = name;
               action.Value = "ceo@example.test";

               var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => action.Save(),
                  "A Set header value action on '" + name + "' was saved for " + rule.Name + ".");
               StringAssert.Contains("says who the message is from", refused.Message, refused.Message);
            }

            Assert.AreEqual(0, rule.Actions.Count, "A refused action is among the actions of " + rule.Name + ".");

            // Reply-To names no sender, and is saved.
            var replyTo = rule.Actions.Add();
            replyTo.Type = eRuleActionType.eRASetHeaderValue;
            replyTo.HeaderName = "Reply-To";
            replyTo.Value = "somewhere@example.test";
            replyTo.Save();
         }

         // The server's own rule may.
         var globalRule = _application.Rules.Add();
         globalRule.Name = "Global sets who it is from";
         globalRule.Active = false;
         globalRule.Save();
         var setFrom = globalRule.Actions.Add();
         setFrom.Type = eRuleActionType.eRASetHeaderValue;
         setFrom.HeaderName = "From";
         setFrom.Value = "noreply@example.test";
         setFrom.Save();
         globalRule.Save();
         globalRule.Delete();
         domainRule.Delete();

         GC.KeepAlive(itself);
      }

      /// <summary>
      /// A Run function action's name was stored as it was given, and looked at only when
      /// the rule fired, where a name that is not a plain function name is refused before
      /// a script engine sees it (HM5018, API/Events.cs): a rule saved naming
      /// "MarkTheSubject()", "Mark The Subject" or nothing ran no function on any message
      /// it matched, and whoever saved it was told nothing. It is refused now when it is
      /// saved - by RuleAction.Save, for a rule saved and for one not saved yet, and by
      /// Rule.Save, for an action changed after its own Save - for a rule of every owner,
      /// whether the administrator saves an account's rule or the account itself, signed
      /// in as itself, with the reason; a plain name, the longest included, is saved.
      /// Upstream PR 671's save-time half, held to the firing path's own test.
      ///
      /// Against the unfixed server every Save below succeeds.
      /// </summary>
      [Test]
      [Description("A Run function action naming no function, or a name that is not a plain function name, is refused when it is saved over COM, for a rule of every owner, saved or not yet saved, by the administrator or by the account signed in as itself, and says why; a plain name is saved.")]
      public void ARunFunctionActionIsSavedOnlyNamingAFunctionTheServerRuns()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "runfunction@example.test", "test");

         // Signed in as the account itself, as a mail client or a script of its own is -
         // the one upstream's own test of the fix saves its rule as.
         var itself = new Application();
         var signedIn = itself.Authenticate(account.Address, "test") as Account;
         Assert.IsNotNull(signedIn, "The account could not sign in to COM.");

         // Each name with the words its refusal must hold. A name too long to be one is
         // described by its length, and a line break is shown as an escape, so that the
         // refusal is one line.
         var refusals = new Dictionary<string, string>
         {
            { "", "must name the function of the event script" },
            { "MarkTheSubject()", "names 'MarkTheSubject()', which is not a plain function name" },
            { "Mark The Subject", "which is not a plain function name" },
            { " MarkTheSubject", "which is not a plain function name" },
            { "1Mark", "which is not a plain function name" },
            { "Mark-Subject", "which is not a plain function name" },
            { "Marqu" + (char) 0xE9, "which is not a plain function name" },
            { "Line\r\nBreak", "names 'Line\\u000D\\u000ABreak', which is not a plain function name" },
            { "MarkTheSubject(HMAILSERVER_MESSAGE); EventLog.Write(\"INJ\" + \"ECTED\"); //", "which is not a plain function name" },
            { new string('a', 256), "names a function of 256 characters" },
         };

         var accountRule = NewRuleMatchingEverything(account, "Runs a function");
         accountRule.Save();

         var ownRule = NewRuleMatchingEverything(signedIn, "Runs a function of its own");
         ownRule.Save();

         var domainRule = _domain.Rules.Add();
         domainRule.Name = "Domain runs a function";
         domainRule.Active = false;
         domainRule.Save();

         var globalRule = _application.Rules.Add();
         globalRule.Name = "Global runs a function";
         globalRule.Active = false;
         globalRule.Save();

         // Not saved yet: RuleAction.Save adds the action to the rule, which stores it
         // when it is saved, and refuses a name no owner of the rule would make good.
         var unsavedRule = _application.Rules.Add();
         unsavedRule.Name = "Global not saved yet";
         unsavedRule.Active = false;

         var plainNames = new[] { "MarkTheSubject", "_x$1", new string('a', 255) };

         foreach (var rule in new[] { accountRule, ownRule, domainRule, globalRule, unsavedRule })
         {
            foreach (var refusal in refusals)
            {
               var action = rule.Actions.Add();
               action.Type = eRuleActionType.eRARunScriptFunction;
               action.ScriptFunction = refusal.Key;

               var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => action.Save(),
                  "A Run function action naming '" + refusal.Key + "' was saved for " + rule.Name + ".");
               StringAssert.Contains(refusal.Value, refused.Message, refused.Message);
               StringAssert.DoesNotContain("\n", refused.Message, "The refusal is not one line.");
            }

            Assert.AreEqual(0, rule.Actions.Count, "A refused action is among the actions of " + rule.Name + ".");

            // The control: plain names are saved.
            RuleAction first = null;
            foreach (var name in plainNames)
            {
               var action = rule.Actions.Add();
               action.Type = eRuleActionType.eRARunScriptFunction;
               action.ScriptFunction = name;
               action.Save();
               first = first ?? action;
            }

            rule.Save();

            // Changed after its own Save, the action is saved with the rule, which refuses
            // it; put back, the rule is saved.
            first.ScriptFunction = "MarkTheSubject()";
            var refusedWithTheRule = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
               rule.Name + " was saved with a Run function action changed to name MarkTheSubject().");
            StringAssert.Contains("which is not a plain function name", refusedWithTheRule.Message, refusedWithTheRule.Message);

            first.ScriptFunction = "MarkTheSubject";
            rule.Save();
         }

         // What the server holds, read through collections of their own: the plain names
         // of each rule, and nothing refused.
         var domainNow = _application.Domains.get_ItemByName(_domain.Name);
         var accountNow = domainNow.Accounts.get_ItemByAddress(account.Address);
         var stored = new[]
         {
            accountNow.Rules.get_ItemByDBID(accountRule.ID),
            accountNow.Rules.get_ItemByDBID(ownRule.ID),
            domainNow.Rules.get_ItemByDBID(domainRule.ID),
            _application.Rules.get_ItemByDBID(globalRule.ID),
            _application.Rules.get_ItemByDBID(unsavedRule.ID),
         };

         foreach (var rule in stored)
         {
            var names = new List<string>();
            for (int i = 0; i < rule.Actions.Count; i++)
               names.Add(rule.Actions[i].ScriptFunction);

            CollectionAssert.AreEqual(plainNames, names, "The Run function actions stored for " + rule.Name + ".");
         }

         GC.KeepAlive(itself);
      }

      // An account's rule matching a message whose subject holds the word given, or any
      // criterion added later: its first action marks the message with X-Rule-Note:
      // taken, and those after it are made by the steps given, in order. Its id.
      private static int NewRuleOnTheSubject(Account owner, string name, string word, params Action<RuleAction>[] actions)
      {
         var rule = owner.Rules.Add();
         rule.Name = name;
         rule.Active = true;
         rule.UseAND = false;

         var criterion = rule.Criterias.Add();
         criterion.UsePredefined = false;
         criterion.HeaderField = "Subject";
         criterion.MatchType = eRuleMatchType.eMTContains;
         criterion.MatchValue = word;
         criterion.Save();

         var note = rule.Actions.Add();
         note.Type = eRuleActionType.eRASetHeaderValue;
         note.HeaderName = "X-Rule-Note";
         note.Value = "taken";
         note.Save();

         foreach (var make in actions)
         {
            var action = rule.Actions.Add();
            make(action);
            action.Save();
         }

         rule.Save();
         return rule.ID;
      }

      // The rule as the Control Panel reads it for each change it makes: through
      // collections of its own, so as the table holds it now.
      private Rule ReadRuleAfresh(Account owner, int ruleId)
      {
         var domain = _application.Domains.get_ItemByName(_domain.Name);
         var account = domain.Accounts.get_ItemByAddress(owner.Address);
         return account.Rules.get_ItemByDBID(ruleId);
      }

      // The first action of the type given, of a rule as read.
      private static RuleAction ActionOfType(Rule rule, eRuleActionType type)
      {
         for (int i = 0; i < rule.Actions.Count; i++)
         {
            if (rule.Actions[i].Type == type)
               return rule.Actions[i];
         }

         Assert.Fail("The rule " + rule.Name + " has no action of type " + type + ".");
         return null;
      }

      // A message on the subject given sent to the owner, as delivered.
      private static string DeliveredTo(Account owner, string sender, string subject)
      {
         SmtpClientSimulator.StaticSend(sender, owner.Address, subject, "Body.");
         return Pop3ClientSimulator.AssertGetFirstMessageText(owner.Address, "test");
      }

      // The three changes the Control Panel makes to a rule's parts, each as it makes it -
      // the rule read afresh, the part saved or removed, then Rule.Save (RuleCriteriaDialog,
      // RulesView's Remove, RuleActionDialog) - with a delivery after each, which follows
      // the table only once a save has asked for the owner's rules to be read again.
      private void ChangeItsPartsAsTheControlPanelDoes(Account owner, string sender, int ruleId)
      {
         // Delivery reads the owner's rules here, and holds them until a save asks again.
         StringAssert.Contains("X-Rule-Note: taken", DeliveredTo(owner, sender, "Alpha"), "The rule did not match before it was changed.");

         var rule = ReadRuleAfresh(owner, ruleId);
         var added = rule.Criterias.Add();
         added.UsePredefined = false;
         added.HeaderField = "Subject";
         added.MatchType = eRuleMatchType.eMTContains;
         added.MatchValue = "Beta";
         added.Save();
         rule.Save();
         StringAssert.Contains("X-Rule-Note: taken", DeliveredTo(owner, sender, "Beta"), "Delivery does not hold the criterion added.");

         rule = ReadRuleAfresh(owner, ruleId);
         int alpha = 0;
         for (int i = 0; i < rule.Criterias.Count; i++)
         {
            if (rule.Criterias[i].MatchValue == "Alpha")
               alpha = rule.Criterias[i].ID;
         }
         Assert.AreNotEqual(0, alpha, "The rule's first criterion is not stored.");
         rule.Criterias.DeleteByDBID(alpha);
         rule.Save();
         StringAssert.DoesNotContain("X-Rule-Note", DeliveredTo(owner, sender, "Alpha"), "Delivery still holds the criterion removed.");

         rule = ReadRuleAfresh(owner, ruleId);
         var another = rule.Actions.Add();
         another.Type = eRuleActionType.eRASetHeaderValue;
         another.HeaderName = "X-Rule-Added";
         another.Value = "taken too";
         another.Save();
         rule.Save();
         StringAssert.Contains("X-Rule-Added: taken too", DeliveredTo(owner, sender, "Beta"), "Delivery does not hold the action added.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
      }

      /// <summary>
      /// Rule.Save held every action of the rule to the save's checks, those stored as
      /// they were with the one changed, so a rule holding a Run function action whose
      /// name the save refuses - stored before the check, restored from a backup or
      /// written to the table, and refused each time the rule fires - could not be saved
      /// again for any change at all. The Control Panel stores a criterion or an action
      /// before it saves the rule: the part was stored and told of as not saved, stored
      /// again at each retry, and delivery went on with the rule as it was, the reload of
      /// the owner's rules being asked for by the save refused. An action Rule.Save
      /// writes as its row holds it, under the owner the rule is stored under, is not
      /// held now, and is left as it is. Scripting is off here, so the firing path runs
      /// no function whatever the name.
      ///
      /// Against the unfixed server the first Rule.Save below throws, refusing the name
      /// nothing changed.
      /// </summary>
      [Test]
      [Description("A rule holding a stored Run function action whose name the save refuses is still changed over COM as the Control Panel changes it - a criterion added, a criterion removed, an action added - and delivery follows each change; the stored name is left as it was.")]
      public void ARuleHoldingAStoredFunctionNameTheSaveRefusesIsStillChanged()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedname@example.test", "test");

         var ruleId = NewRuleOnTheSubject(owner, "Stored name", "Alpha", action =>
         {
            action.Type = eRuleActionType.eRARunScriptFunction;
            action.ScriptFunction = "MarkTheSubject";
         });

         // As stored before Save refused the name. The save asked for the account's rules
         // to be read again at the next delivery, which reads the table's name.
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionscriptfunction = 'MarkTheSubject()' where actionruleid = " + ruleId);

         ChangeItsPartsAsTheControlPanelDoes(owner, "storedname-sender@example.test", ruleId);

         var rule = ReadRuleAfresh(owner, ruleId);
         Assert.AreEqual("MarkTheSubject()", ActionOfType(rule, eRuleActionType.eRARunScriptFunction).ScriptFunction,
            "The saves around the stored name changed it.");
      }

      /// <summary>
      /// The same for a rule holding a stored Reply from an address its owner may not send
      /// as and a Set header value of From, which the save has refused an account's rule
      /// since 7 October 2026 and delivery does not take (AStoredReplyOrFromOfAnotherIsNotApplied).
      ///
      /// Against the unfixed server the first Rule.Save below throws, refusing the reply
      /// nothing changed.
      /// </summary>
      [Test]
      [Description("A rule holding a stored Reply from another's address and a stored Set header value of From is still changed over COM as the Control Panel changes it, and delivery follows each change while taking neither; both are left as they were.")]
      public void ARuleHoldingAStoredReplyOrFromTheSaveRefusesIsStillChanged()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedparts-sender@example.test", "test");
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedparts-owner@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedparts-ceo@example.test", "test");

         var ruleId = NewRuleOnTheSubject(owner, "Stored reply and From", "Alpha",
            action =>
            {
               action.Type = eRuleActionType.eRAReply;
               action.FromAddress = owner.Address;
               action.Subject = "Autoreply";
            },
            action =>
            {
               action.Type = eRuleActionType.eRASetHeaderValue;
               action.HeaderName = "X-From-Placeholder-Kept";
               action.Value = "storedparts-ceo@example.test";
            });

         // As stored before Save refused them.
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionfromaddress = 'storedparts-ceo@example.test' where actionruleid = " + ruleId +
            " and actionfromaddress = '" + owner.Address + "'");
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionheader = 'From' where actionruleid = " + ruleId +
            " and actionheader = 'X-From-Placeholder-Kept'");

         ChangeItsPartsAsTheControlPanelDoes(owner, sender.Address, ruleId);

         var rule = ReadRuleAfresh(owner, ruleId);
         Assert.AreEqual("storedparts-ceo@example.test", ActionOfType(rule, eRuleActionType.eRAReply).FromAddress,
            "The saves around the stored reply changed it.");
         var headers = new List<string>();
         for (int i = 0; i < rule.Actions.Count; i++)
         {
            if (rule.Actions[i].Type == eRuleActionType.eRASetHeaderValue)
               headers.Add(rule.Actions[i].HeaderName);
         }
         CollectionAssert.AreEqual(new[] { "X-Rule-Note", "From", "X-Rule-Added" }, headers,
            "The saves around the stored Set header value of From changed it.");

         // Neither was taken at any delivery.
         ImapClientSimulator.AssertMessageCount(sender.Address, "test", "Inbox", 0);
         Assert.IsTrue(LogHandler.DefaultLogContains("Reply action of rule"),
            "The reply not sent is not in the application log.");
         Assert.IsTrue(LogHandler.DefaultLogContains("Set header value action of rule"),
            "The Set header value not made is not in the application log.");
      }

      /// <summary>
      /// What Rule.Save leaves out of its checks is only what it does not change (the two
      /// tests above): a stored action it changes is held - a Run function name changed,
      /// a Forward action that carried a name made a Run function action - and neither
      /// is stored.
      ///
      /// Against a server that held no function name when the rule was saved, both are
      /// stored; against one that left a stored action out whatever type it was made,
      /// the second is.
      /// </summary>
      [Test]
      [Description("Rule.Save still holds a stored action it changes - a Run function name changed to one that is not plain, a Forward action carrying such a name made a Run function action - and stores neither.")]
      public void RuleSaveHoldsAStoredActionItChanges()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "heldchange@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "heldchange-other@example.test", "test");

         var ruleId = NewRuleOnTheSubject(owner, "Changed parts", "Alpha",
            action =>
            {
               action.Type = eRuleActionType.eRARunScriptFunction;
               action.ScriptFunction = "MarkTheSubject";
            },
            action =>
            {
               // Every action carries every field; a Forward action's name is not one the
               // save holds, as the firing path never reads it.
               action.Type = eRuleActionType.eRAForwardEmail;
               action.To = other.Address;
               action.ScriptFunction = "Carried()";
            });

         var rule = ReadRuleAfresh(owner, ruleId);
         ActionOfType(rule, eRuleActionType.eRARunScriptFunction).ScriptFunction = "Other()";
         var renamed = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
            "The rule was saved with its Run function name changed to Other().");
         StringAssert.Contains("names 'Other()', which is not a plain function name", renamed.Message, renamed.Message);

         rule = ReadRuleAfresh(owner, ruleId);
         ActionOfType(rule, eRuleActionType.eRAForwardEmail).Type = eRuleActionType.eRARunScriptFunction;
         var retyped = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
            "The rule was saved with its Forward action, naming Carried(), made a Run function action.");
         StringAssert.Contains("names 'Carried()', which is not a plain function name", retyped.Message, retyped.Message);

         rule = ReadRuleAfresh(owner, ruleId);
         Assert.AreEqual("MarkTheSubject", ActionOfType(rule, eRuleActionType.eRARunScriptFunction).ScriptFunction,
            "A Run function name refused with the rule was stored.");
         Assert.AreEqual(other.Address, ActionOfType(rule, eRuleActionType.eRAForwardEmail).To,
            "A Forward action refused with the rule as a Run function action was stored as one.");
      }

      /// <summary>
      /// And every action of a rule moved to another account is held to the account it is
      /// moved to, stored as it is or not: here a reply from the address of the account
      /// the rule is moved from, which the account it is moved to may not send as. The
      /// move is not stored.
      ///
      /// Against a server that left a stored action out whoever the rule was moved to, the
      /// rule is moved replying as the account it left.
      /// </summary>
      [Test]
      [Description("Rule.Save holds every action of a rule moved to another account to that account, the stored ones too - a reply from the address of the account it leaves is refused - and the move is not stored.")]
      public void RuleSaveHoldsEveryActionOfARuleMovedToAnotherAccount()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "heldmove@example.test", "test");
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "heldmove-other@example.test", "test");

         var movedId = NewRuleOnTheSubject(owner, "Moved", "Alpha", action =>
         {
            action.Type = eRuleActionType.eRAReply;
            action.FromAddress = owner.Address;
            action.Subject = "Autoreply";
         });

         var moving = ReadRuleAfresh(owner, movedId);
         moving.AccountID = other.ID;
         var moved = Assert.Throws<System.Runtime.InteropServices.COMException>(() => moving.Save(),
            "A rule replying from " + owner.Address + " was moved to " + other.Address + ".");
         StringAssert.Contains("is sent only from an address the account may send as", moved.Message, moved.Message);
         Assert.AreEqual(movedId, ReadRuleAfresh(owner, movedId).ID, "The rule refused its move is not the account's it was.");
      }

      /// <summary>
      /// The Control Panel moved an action with RuleAction.MoveUp or MoveDown, which store
      /// the places they give it and the action it passes, and then asked RuleAction.Save of
      /// it before Rule.Save. RuleAction.Save holds the action it saves to the save's checks,
      /// changed or not, so that the action dialog tells of a stored one they refuse until it
      /// is corrected; and so such an action, moved itself, was moved and told of as not
      /// moved, moved again at each click, and the rule's save, which asks for the owner's
      /// rules to be read again, was never reached. The Control Panel now moves the action
      /// and saves the rule (RulesView's MoveAction, held to it by ControlPanel.Tests'
      /// RuleActionMoveTests), as here: each move is stored, delivery follows them, the
      /// stored actions are left as they were and not taken, and RuleAction.Save of each is
      /// still refused.
      ///
      /// Against a server whose Rule.Save held every action of the rule, the stored ones too,
      /// the first move's Rule.Save throws, refusing the reply nothing changed.
      /// </summary>
      [Test]
      [Description("A rule's stored Run function name and Reply that the save refuses are moved over COM as the Control Panel moves an action - the name up, the reply down, then another action - each move stored and delivery following the moves; both are left as they were and not taken, and RuleAction.Save of each, unchanged, is still refused.")]
      public void AStoredActionTheSaveRefusesIsMovedAsTheControlPanelMovesIt()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedmove-sender@example.test", "test");
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedmove-owner@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "storedmove-ceo@example.test", "test");

         // Of the two Set header value actions on X-Rule-Order, the message keeps the value of
         // the one taken last.
         var ruleId = NewRuleOnTheSubject(owner, "Stored parts moved", "Alpha",
            action =>
            {
               action.Type = eRuleActionType.eRASetHeaderValue;
               action.HeaderName = "X-Rule-Order";
               action.Value = "a";
            },
            action =>
            {
               action.Type = eRuleActionType.eRARunScriptFunction;
               action.ScriptFunction = "MarkTheSubject";
            },
            action =>
            {
               action.Type = eRuleActionType.eRAReply;
               action.FromAddress = owner.Address;
               action.Subject = "Autoreply";
            },
            action =>
            {
               action.Type = eRuleActionType.eRASetHeaderValue;
               action.HeaderName = "X-Rule-Order";
               action.Value = "b";
            });

         // As stored before Save refused them. The save asked for the account's rules to be
         // read again at the next delivery, which reads the table's.
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionscriptfunction = 'MarkTheSubject()' where actionruleid = " + ruleId +
            " and actiontype = " + (int)eRuleActionType.eRARunScriptFunction);
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionfromaddress = 'storedmove-ceo@example.test' where actionruleid = " + ruleId +
            " and actionfromaddress = '" + owner.Address + "'");

         const string name = "Run MarkTheSubject()";
         const string reply = "Reply from storedmove-ceo@example.test";
         CollectionAssert.AreEqual(new[] { "X-Rule-Note: taken", "X-Rule-Order: a", name, reply, "X-Rule-Order: b" },
            StoredActions(owner, ruleId), "The rule is not stored as it was made.");
         var before = DeliveredTo(owner, sender.Address, "Alpha");
         StringAssert.Contains("X-Rule-Order: b", before, "The rule's actions were not taken in their order before the moves.");
         StringAssert.DoesNotContain("X-Rule-Order: a", before, "The rule's actions were not taken in their order before the moves.");

         // The stored name up and the stored reply down, each the action moved itself, and
         // then the second X-Rule-Order up past the first.
         MoveAsTheControlPanelDoes(owner, ruleId, name, true);
         CollectionAssert.AreEqual(new[] { "X-Rule-Note: taken", name, "X-Rule-Order: a", reply, "X-Rule-Order: b" },
            StoredActions(owner, ruleId), "Moving the stored Run function action up did not store its place.");

         MoveAsTheControlPanelDoes(owner, ruleId, reply, false);
         CollectionAssert.AreEqual(new[] { "X-Rule-Note: taken", name, "X-Rule-Order: a", "X-Rule-Order: b", reply },
            StoredActions(owner, ruleId), "Moving the stored reply down did not store its place.");

         MoveAsTheControlPanelDoes(owner, ruleId, "X-Rule-Order: b", true);
         var moved = new[] { "X-Rule-Note: taken", name, "X-Rule-Order: b", "X-Rule-Order: a", reply };
         CollectionAssert.AreEqual(moved, StoredActions(owner, ruleId), "Moving an action past another did not store its place.");

         var delivered = DeliveredTo(owner, sender.Address, "Alpha");
         StringAssert.Contains("X-Rule-Note: taken", delivered, "The rule did not match after the moves.");
         StringAssert.Contains("X-Rule-Order: a", delivered, "Delivery does not follow the moves.");
         StringAssert.DoesNotContain("X-Rule-Order: b", delivered, "Delivery does not follow the moves.");

         // Neither stored action was taken at either delivery.
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(sender.Address, "test", "Inbox", 0);
         Assert.IsTrue(LogHandler.DefaultLogContains("Reply action of rule"),
            "The reply not sent is not in the application log.");

         // RuleAction.Save of each, unchanged, is still refused, and stores nothing.
         var rule = ReadRuleAfresh(owner, ruleId);
         var nameRefused = Assert.Throws<System.Runtime.InteropServices.COMException>(
            () => ActionOfType(rule, eRuleActionType.eRARunScriptFunction).Save(),
            "RuleAction.Save stored the Run function action naming MarkTheSubject().");
         StringAssert.Contains("names 'MarkTheSubject()', which is not a plain function name", nameRefused.Message, nameRefused.Message);
         var replyRefused = Assert.Throws<System.Runtime.InteropServices.COMException>(
            () => ActionOfType(rule, eRuleActionType.eRAReply).Save(),
            "RuleAction.Save stored the reply from storedmove-ceo@example.test.");
         StringAssert.Contains("is sent only from an address the account may send as", replyRefused.Message, replyRefused.Message);
         CollectionAssert.AreEqual(moved, StoredActions(owner, ruleId), "A RuleAction.Save refused changed the rule's actions.");
      }

      // An action moved as the Control Panel moves one (RulesView's MoveAction): the rule read
      // afresh, the action found by its id and moved up or down - RuleAction.MoveUp and
      // MoveDown store its place - and the rule saved, which asks for the owner's rules to be
      // read again.
      private void MoveAsTheControlPanelDoes(Account owner, int ruleId, string described, bool up)
      {
         var rule = ReadRuleAfresh(owner, ruleId);
         int actionId = 0;
         for (int i = 0; i < rule.Actions.Count; i++)
         {
            if (Described(rule.Actions[i]) == described)
               actionId = rule.Actions[i].ID;
         }
         Assert.AreNotEqual(0, actionId, "The rule " + rule.Name + " has no action " + described + ".");

         var action = rule.Actions.get_ItemByDBID(actionId);
         if (up)
            action.MoveUp();
         else
            action.MoveDown();
         rule.Save();
      }

      // A rule's actions in their order, as the table holds them.
      private List<string> StoredActions(Account owner, int ruleId)
      {
         var rule = ReadRuleAfresh(owner, ruleId);
         var actions = new List<string>();
         for (int i = 0; i < rule.Actions.Count; i++)
            actions.Add(Described(rule.Actions[i]));
         return actions;
      }

      private static string Described(RuleAction action)
      {
         switch (action.Type)
         {
            case eRuleActionType.eRASetHeaderValue:
               return action.HeaderName + ": " + action.Value;
            case eRuleActionType.eRARunScriptFunction:
               return "Run " + action.ScriptFunction;
            case eRuleActionType.eRAReply:
               return "Reply from " + action.FromAddress;
            default:
               return action.Type.ToString();
         }
      }

      /// <summary>
      /// A new rule's criteria and actions were bound to rule id 0 when they were first
      /// read before its first save - and Rule.Save reads the actions itself, to hold them
      /// to the rule's owner, so that every rule saved over COM had its actions bound so.
      /// An action or a criterion added through the same Rule object after that save was
      /// then stored by nothing but another Rule.Save, and RuleAction.Save held such an
      /// action to no owner's limits (OnlyAGlobalRuleMaySetWhoTheMessageIsFrom). They are
      /// bound to the id the save gives the rule, and an action or a criterion made before
      /// that save and saved after it is stored under the rule too: with the rule id 0 it
      /// was made with, the foreign key on its table would refuse it.
      ///
      /// Against the server before the fix none of the actions and criteria saved after
      /// the rule's first save is stored.
      /// </summary>
      [Test]
      [Description("An action or a criterion saved through a new rule after the rule's first save, made before that save or after it, is stored under the rule at once.")]
      public void ANewRulesPartsAreStoredUnderIt()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "ruleparts@example.test", "test");

         var rule = NewRuleMatchingEverything(account, "Parts saved after the rule");

         // Made before the rule's first save and saved after it.
         var early = rule.Actions.Add();
         early.Type = eRuleActionType.eRASetHeaderValue;
         early.HeaderName = "X-Rule-Early";
         early.Value = "stored";
         var earlyCriterion = rule.Criterias.Add();
         earlyCriterion.UsePredefined = true;
         earlyCriterion.PredefinedField = eRulePredefinedField.eFTSubject;
         earlyCriterion.MatchType = eRuleMatchType.eMTContains;
         earlyCriterion.MatchValue = "early criterion";

         rule.Save();
         early.Save();
         earlyCriterion.Save();

         // Made and saved after it.
         var note = rule.Actions.Add();
         note.Type = eRuleActionType.eRASetHeaderValue;
         note.HeaderName = "X-Rule-Note";
         note.Value = "stored";
         note.Save();

         var second = rule.Criterias.Add();
         second.UsePredefined = true;
         second.PredefinedField = eRulePredefinedField.eFTSubject;
         second.MatchType = eRuleMatchType.eMTContains;
         second.MatchValue = "second criterion";
         second.Save();

         // As stored: the rule read again from the database, through collections of their
         // own, and not the Rule object above.
         var stored = _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(account.Address).Rules
            .get_ItemByDBID(rule.ID);

         var actions = new List<string>();
         for (int i = 0; i < stored.Actions.Count; i++)
            actions.Add(stored.Actions[i].HeaderName);
         var criteria = new List<string>();
         for (int i = 0; i < stored.Criterias.Count; i++)
            criteria.Add(stored.Criterias[i].MatchValue);

         CollectionAssert.AreEqual(new[] { "X-Rule-Early", "X-Rule-Note" }, actions,
            "The actions stored under the rule are not the two saved through it after its first save: " + string.Join(", ", actions));
         CollectionAssert.AreEqual(new[] { "0", "early criterion", "second criterion" }, criteria,
            "The criteria stored under the rule are not the three saved through it: " + string.Join(", ", criteria));
      }

      /// <summary>
      /// Moving a rule or one of its actions wrote every rule or action whose place changed
      /// whole, as it was in memory, so a change made over COM and not saved was stored by
      /// the move without being held to anything: an account could turn a Set header value
      /// action of its own rule to From or Sender, which RuleAction.Save and Rule.Save refuse
      /// it, and have it stored by moving the action, or the rule, up. A move now writes the
      /// place alone, and the change not saved is still the one Rule.Save refuses.
      ///
      /// Against the server before the fix the moved action is stored as From and the moved
      /// rule's action as Sender.
      /// </summary>
      [Test]
      [Description("Moving an account's rule or one of its actions stores the new order and no change that was not saved: an action turned to From or Sender and not saved is not stored by the move.")]
      public void MovingARuleOrAnActionStoresItsPlaceAlone()
      {
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rulemove@example.test", "test");

         var itself = new Application();
         var signedIn = itself.Authenticate(owner.Address, "test") as Account;
         Assert.IsNotNull(signedIn, "The account could not sign in to COM.");

         var first = NewRuleMatchingEverything(signedIn, "First");
         first.Save();
         var second = NewRuleMatchingEverything(signedIn, "Second");
         second.Save();

         AddSavedSetHeader(second, "X-Move-Kept");
         var moved = AddSavedSetHeader(second, "X-Move-Moved");

         // Turned to From, not saved, and moved up past the other action.
         moved.HeaderName = "From";
         moved.MoveUp();

         CollectionAssert.AreEqual(new[] { "X-Move-Moved", "X-Move-Kept" }, StoredHeaderNames(owner, second.ID),
            "Moving an action stored more than its place, or not its place.");

         // A rule whose action was turned to Sender and not saved, moved up past the second.
         var third = NewRuleMatchingEverything(signedIn, "Third");
         third.Save();
         var turned = AddSavedSetHeader(third, "X-Move-Turned");
         turned.HeaderName = "Sender";
         third.MoveUp();

         CollectionAssert.AreEqual(new[] { "X-Move-Turned" }, StoredHeaderNames(owner, third.ID),
            "Moving a rule stored a change to its action that was not saved.");
         CollectionAssert.AreEqual(new[] { "X-Move-Moved", "X-Move-Kept" }, StoredHeaderNames(owner, second.ID),
            "Moving a rule stored a change to the action of the rule it passed that was not saved.");

         var storedRules = _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(owner.Address).Rules;
         var order = new List<string>();
         for (int i = 0; i < storedRules.Count; i++)
            order.Add(storedRules[i].Name);
         CollectionAssert.AreEqual(new[] { "First", "Third", "Second" }, order,
            "Moving a rule did not store its place: " + string.Join(", ", order));

         // The changes are still in memory, and saving them is still refused.
         foreach (var rule in new[] { second, third })
         {
            var refused = Assert.Throws<System.Runtime.InteropServices.COMException>(() => rule.Save(),
               rule.Name + " was saved with an action that sets who the message is from.");
            StringAssert.Contains("says who the message is from", refused.Message, refused.Message);
         }

         GC.KeepAlive(itself);
      }

      private static RuleAction AddSavedSetHeader(Rule rule, string headerName)
      {
         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRASetHeaderValue;
         action.HeaderName = headerName;
         action.Value = "ceo@example.test";
         action.Save();
         return action;
      }

      // The header names of a rule's Set header value actions, in their order, as the
      // database holds them: read through collections of their own, not the objects the
      // test changed.
      private List<string> StoredHeaderNames(Account account, int ruleId)
      {
         var stored = _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(account.Address).Rules
            .get_ItemByDBID(ruleId);

         var names = new List<string>();
         for (int i = 0; i < stored.Actions.Count; i++)
            names.Add(stored.Actions[i].HeaderName);
         return names;
      }

      /// <summary>
      /// A rule's reply wrote its name in front of its address as the name stood, so a
      /// name of "x &lt;ceo@...&gt;, ceo@..." added that address to the reply's From, and a
      /// program reading the field took the first address for the sender. The name is
      /// now one quoted string: the From names the account's address alone.
      ///
      /// Against the unfixed server the From begins "x &lt;replyname-ceo@example.test&gt;,".
      /// </summary>
      [Test]
      [Description("A rule reply's name is written as one quoted name, and adds no address to its From.")]
      public void ARuleReplysNameAddsNoAddress()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replyname-sender@example.test", "test");
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replyname-owner@example.test", "test");

         var rule = NewRuleMatchingEverything(owner, "Reply with a name");
         var reply = rule.Actions.Add();
         reply.Type = eRuleActionType.eRAReply;
         reply.FromAddress = owner.Address;
         reply.FromName = "x <replyname-ceo@example.test>, replyname-ceo@example.test";
         reply.Subject = "Autoreply";
         reply.Save();
         rule.Save();

         SmtpClientSimulator.StaticSend(sender.Address, owner.Address, "Named", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var answer = Pop3ClientSimulator.AssertGetFirstMessageText(sender.Address, "test");
         var from = UnfoldedHeaderFields(answer, "From");
         Assert.AreEqual(1, from.Count, answer);
         StringAssert.StartsWith("From: \"x <replyname-ceo@example.test>, replyname-ceo@example.test\" <" + owner.Address + ">", from[0], answer);
      }

      /// <summary>
      /// An action stored before Save refused it - or restored from a backup, or written
      /// to the table - is not taken at delivery, and the application log says so: an
      /// account's rule whose reply names another account's address sends no reply, and
      /// one whose Set header value sets From leaves the From the message came with,
      /// while the rule's other action is taken.
      ///
      /// Against the unfixed server the sender gets a reply from replystored-ceo, and the
      /// stored message says it is from replystored-ceo.
      /// </summary>
      [Test]
      [Description("A stored reply from another's address is not sent, and a stored Set header value of From is not made, while the rule's other action is.")]
      public void AStoredReplyOrFromOfAnotherIsNotApplied()
      {
         var sender = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replystored-sender@example.test", "test");
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replystored-owner@example.test", "test");
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "replystored-ceo@example.test", "test");

         var rule = NewRuleMatchingEverything(owner, "Stored reply and From");
         var reply = rule.Actions.Add();
         reply.Type = eRuleActionType.eRAReply;
         reply.FromAddress = owner.Address;
         reply.Subject = "Autoreply";
         reply.Save();
         var setFrom = rule.Actions.Add();
         setFrom.Type = eRuleActionType.eRASetHeaderValue;
         setFrom.HeaderName = "X-From-Placeholder-Stored";
         setFrom.Value = "replystored-ceo@example.test";
         setFrom.Save();
         var note = rule.Actions.Add();
         note.Type = eRuleActionType.eRASetHeaderValue;
         note.HeaderName = "X-Rule-Note";
         note.Value = "taken";
         note.Save();
         rule.Save();

         // As stored before Save refused them. The account's rules are read from the
         // table again at the next delivery, the save having asked for it.
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionfromaddress = 'replystored-ceo@example.test' where actionfromaddress = '" + owner.Address + "'");
         _application.Database.ExecuteSQL(
            "update hm_rule_actions set actionheader = 'From' where actionheader = 'X-From-Placeholder-Stored'");

         SmtpClientSimulator.StaticSend(sender.Address, owner.Address, "Stored", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         var stored = Pop3ClientSimulator.AssertGetFirstMessageText(owner.Address, "test");
         var storedFrom = UnfoldedHeaderFields(stored, "From");
         Assert.AreEqual(1, storedFrom.Count, stored);
         Assert.AreEqual("From: " + sender.Address, storedFrom[0], stored);
         StringAssert.Contains("X-Rule-Note: taken", stored, "The rule's other action was not taken.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(sender.Address, "test", "Inbox", 0);

         Assert.IsTrue(LogHandler.DefaultLogContains("Reply action of rule"),
            "The reply not sent is not in the application log.");
         Assert.IsTrue(LogHandler.DefaultLogContains("Set header value action of rule"),
            "The Set header value not made is not in the application log.");
      }

      /// <summary>
      /// RegularExpression::TestExactMatch swallows a pattern that will not compile and
      /// returns "no match", so a criterion with one unmatched bracket produces a rule
      /// that is Active in the editor, matches nothing ever, and says nothing about it.
      /// That is the worst configuration fault to diagnose, because the rule looks
      /// correct and the mail looks like it simply did not match.
      ///
      /// The message must still be delivered normally - a broken criterion may not break
      /// delivery - so the observable change is the report, not the routing.
      ///
      /// Against the unfixed server the two message-count assertions pass and the final
      /// assertion fails, because the error log is empty.
      /// </summary>
      [Test]
      [Description("A regex criterion that cannot be compiled must be reported, not silently never match.")]
      public void WhenRegexCriterionCannotBeCompiledItShouldBeReported()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "badregex@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Broken regex criteria";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTRegExMatch;

         // An unterminated character class. Perl syntax has no reading of this, so Boost
         // raises error_brack while constructing the expression - which is the same
         // exception type it raises when it abandons a match, which is exactly why the
         // two had to be told apart.
         //
         // The GUID is not decoration. A pattern that has been reported once is suspended
         // for RegexSuspensionSeconds and deliberately NOT reported again inside that
         // window, so a fixed pattern would make this test pass on a cold service and
         // fail if it were run twice within five minutes - the sort of test that gets
         // marked flaky and then ignored. A fresh pattern per run is always a fresh key.
         ruleCriteria.MatchValue = "[unterminated-" + Guid.NewGuid().ToString("N");
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.BrokenRegex";
         ruleAction.Save();

         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();
         smtpClientSimulator.Send(account.Address, account.Address, "Any subject at all",
            "The criterion cannot compile, so this must be delivered untouched.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // Unchanged behaviour: the criterion does not match, so the rule does not fire.
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 1);

         CustomAsserts.AssertReportedError("is not a valid regular expression");
      }

      /// <summary>
      /// The negative control for the pair above, and the reason it is here: the two
      /// tests would both pass if a regex criterion had simply stopped working
      /// altogether. This one pins that a valid pattern still matches and still moves
      /// the message, and that nothing is reported for it.
      /// </summary>
      [Test]
      [Description("Negative control: a valid regex criterion must still match, move the message, and report nothing.")]
      public void WhenRegexCriterionIsValidItShouldStillMatchAndReportNothing()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "goodregex@example.test", "test");

         var rule = account.Rules.Add();
         rule.Name = "Working regex criteria";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTRegExMatch;
         ruleCriteria.MatchValue = "[a-f]*";
         ruleCriteria.Save();

         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.WorkingRegex";
         ruleAction.Save();

         rule.Save();

         var smtpClientSimulator = new SmtpClientSimulator();
         smtpClientSimulator.Send(account.Address, account.Address, "abcdef", "This matches [a-f]*.");
         smtpClientSimulator.Send(account.Address, account.Address, "xyz", "This does not.");

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox.WorkingRegex", 1);
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 1);

         // Nothing at all should have been reported. Asserting the absence of the whole
         // log rather than the absence of one string is deliberate: a criterion that
         // silently stopped being evaluated would satisfy a narrower check.
         CustomAsserts.AssertNoReportedError();
      }

      private void AddExactMatchRule(Account account)
      {
         var rule = account.Rules.Add();
         rule.Name = "TestRule 3";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTEquals;
         ruleCriteria.MatchValue = "CORPORATE EXACT MATCH";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Corporate";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();
      }

      public void AddCorporateRule(Account account)
      {
         var rule = account.Rules.Add();
         rule.Name = "TestRule 2";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "**CORPORATE**";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Corporate";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();
      }

      public void AddSpamRule(Account account)
      {
         var rule = account.Rules.Add();
         rule.Name = "TestRule 1";
         rule.Active = true;

         var ruleCriteria = rule.Criterias.Add();
         ruleCriteria.UsePredefined = false;
         ruleCriteria.HeaderField = "Subject";
         ruleCriteria.MatchType = eRuleMatchType.eMTContains;
         ruleCriteria.MatchValue = "**SPAM**";
         ruleCriteria.Save();

         // Add action
         var ruleAction = rule.Actions.Add();
         ruleAction.Type = eRuleActionType.eRAMoveToImapFolder;
         ruleAction.IMAPFolder = "INBOX.Spam";
         ruleAction.Save();

         // Save the rule in the database
         rule.Save();
      }
   }
}