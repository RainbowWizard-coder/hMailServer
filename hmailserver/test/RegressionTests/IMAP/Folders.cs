// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   [TestFixture]
   public class Folders : TestFixtureBase
   {
      [Test]
      public void TestCreateDeepFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder4@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         var result =
            simulator.Send("A01 CREATE 1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17.18.19.20.21.22.23.24.25.26");
         Assert.IsTrue(result.Contains("A01 NO"));

         simulator.Disconnect();
      }


      [Test]
      public void TestCreateFolderWithHash()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder6@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsFalse(simulator.CreateFolder("#Test"));
         Assert.IsTrue(simulator.CreateFolder("Test.#Testar"));
         simulator.Disconnect();
      }


      [Test]
      public void TestCreateFolderWithQuote()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");


         const string folderName = "ABC\"123";

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsTrue(simulator.List().Contains(folderName));
         Assert.IsTrue(simulator.SelectFolder(folderName));
         simulator.Disconnect();
      }

      [Test]
      public void TestCreateFolderWithSlash()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         const string folderName = "ABC\\123";

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsTrue(simulator.List().Contains(folderName));
         Assert.IsTrue(simulator.SelectFolder(folderName));
         simulator.Disconnect();
      }

      [Test]
      public void TestCreateLongFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder9@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");

         var folderName = new string('A', 255);

         var result = simulator.Send("A01 CREATE " + folderName);
         Assert.IsTrue(result.Contains("A01 OK"));
         Assert.IsTrue(simulator.SelectFolder(folderName));

         folderName = new string('A', 256);

         result = simulator.Send("A01 CREATE " + folderName);
         Assert.IsTrue(result.Contains("A01 NO"));
         Assert.IsFalse(simulator.SelectFolder(folderName));

         simulator.Disconnect();
      }

      [Test]
      public void TestCreateParallelFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder5@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         var result = simulator.Send("A01 CREATE 1.2.3");
         Assert.IsTrue(result.Contains("A01 OK"));
         result = simulator.Send("A01 CREATE 1.2.4");
         Assert.IsTrue(result.Contains("A01 OK"));

         simulator.Disconnect();
      }

      [Test]
      public void TestCreateUnnamedSubFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder7@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         var result = simulator.Send("A01 CREATE 1..3");
         Assert.IsTrue(result.Contains("A01 NO"));
         result = simulator.Send("A01 CREATE 1....3");
         Assert.IsTrue(result.Contains("A01 NO"));
         result = simulator.Send("A01 CREATE 1.......3");
         Assert.IsTrue(result.Contains("A01 NO"));


         simulator.Disconnect();
      }

      [Test]
      public void TestFolderCaseInLIST()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var folderName = "ABC.def.GHI";

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsFalse(simulator.List("ABC.DEF.*").Contains("ABC.def.GHI"));
         Assert.IsTrue(simulator.List("ABC.DEF.*").Contains("ABC.DEF.GHI"));
         Assert.IsFalse(simulator.List("ABC.def.*").Contains("ABC.DEF"));
         Assert.IsTrue(simulator.List("ABC.def.*").Contains("ABC.def.GHI"));
         Assert.IsTrue(simulator.SelectFolder(folderName));
         simulator.Disconnect();
      }

      [Test]
      public void TestFolderCaseInLSUB()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var folderName = "ABC.def.GHI";

         var simulator = new ImapClientSimulator();
         var sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsTrue(simulator.Subscribe(folderName));
         Assert.IsFalse(simulator.LSUB("ABC.DEF.*").Contains("ABC.def.GHI"));
         Assert.IsTrue(simulator.LSUB("ABC.DEF.*").Contains("ABC.DEF.GHI"));
         Assert.IsFalse(simulator.LSUB("ABC.def.*").Contains("ABC.DEF"));
         Assert.IsTrue(simulator.LSUB("ABC.def.*").Contains("ABC.def.GHI"));
         Assert.IsTrue(simulator.SelectFolder(folderName));
         simulator.Disconnect();
      }

      /// <summary>
      ///    Tests that the LSUB resposne doesn't contain an empty line if a folder is not subscribed to.
      /// </summary>
      [Test]
      public void TestFolderLSUBUnsubscribedFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         const string folderName = "ABC.def.GHI";

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsFalse(simulator.LSUB().Contains("\r\n\r\n"));
         simulator.Disconnect();
      }


      /// <summary>
      ///    Test that folder names in the LSUB response are put in quotes.
      ///    This is how gmail acts, and Thunderbird crashes if a folder name
      ///    is * without quotes.
      /// </summary>
      [Test]
      public void TestFolderQuote()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         const string folderName = "Test*";

         var simulator = new ImapClientSimulator();
         var sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsTrue(simulator.Subscribe(folderName));
         Assert.IsTrue(simulator.LSUB().Contains("\"" + folderName + "\""));
         simulator.Disconnect();
      }

      [Test]
      public void TestStatusFolderWithBackslashDelimiter()
      {
         _settings.IMAPHierarchyDelimiter = "\\";

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "status-delimiter@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         simulator.CreateFolder("Test\\HelloWorld");

         string result = simulator.Status("Test\\\\HelloWorld", "MESSAGES");
         Assert.IsTrue(result.Contains("\"Test\\\\HelloWorld\""), "STATUS response should escape backslashes. Got: " + result);
         Assert.IsTrue(result.Contains("A08 OK"));
         simulator.Disconnect();
      }

      [Test]
      public void TestStatusFolderWithBackslashDelimiter_Literal()
      {
         _settings.IMAPHierarchyDelimiter = "\\";

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "status-delimiter@example.test", "test");

         string folderName = "Test\\HelloWorld";

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         simulator.CreateFolder(folderName);

         simulator.SendRaw($"A08 STATUS {{{folderName.Length}}}\r\n");
         simulator.Receive();
         // CRLF, spelled out. Environment.NewLine is a bare LF on Linux, and the
         // literal the line above announced is terminated by CRLF in IMAP on every
         // platform there is - so this test hung for two minutes there, waiting for
         // an end of line the server was right not to have seen.
         simulator.SendRaw(folderName + "\r\n");
         var statusResponse = simulator.Receive();

         Assert.IsTrue(statusResponse.Contains("\"Test\\\\HelloWorld\""), "STATUS response should escape backslashes. Got: " + statusResponse);
         Assert.IsTrue(statusResponse.Contains("A08 OK"));
         simulator.Disconnect();
      }

      [Test]
      public void TestListCommandChangedHierarchyDelimiter()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");
         _settings.IMAPHierarchyDelimiter = "\\";

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         simulator.CreateFolder("Test\\HelloWorld\\Test2");

         var response = simulator.List("%");
         Assert.IsTrue(response.Contains("\"Test\""), response);

         response = simulator.List("%\\%");
         Assert.IsTrue(response.Contains("\"Test\\HelloWorld\""), response);

         response = simulator.List("%\\%\\%%");
         Assert.IsTrue(response.Contains("\"Test\\HelloWorld\\Test2\""), response);
      }


      [Test]
      [Description("Test that the namespace is included in an empty list response.")]
      public void TestListSpecial()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var simulator = new ImapClientSimulator();
         var sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         var response = simulator.List("");
         Assert.IsTrue(response.StartsWith("* LIST (\\Noselect) \".\" \"\""));
         simulator.Disconnect();

         _settings.IMAPHierarchyDelimiter = "/";

         simulator = new ImapClientSimulator();
         sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         response = simulator.List("");
         Assert.IsTrue(response.StartsWith("* LIST (\\Noselect) \"/\" \"\""));
         simulator.Disconnect();

         _settings.IMAPHierarchyDelimiter = "\\";

         simulator = new ImapClientSimulator();
         sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         response = simulator.List("", false);
         var expectedResponse = "* LIST (\\Noselect) \"\\\\\" \"\"";
         Assert.IsTrue(response.StartsWith(expectedResponse));
         simulator.Disconnect();
      }

      [Test]
      [Description("Test to include reference in LIST command (issue 163).")]
      public void TestListWithReference()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("Main.Sub1.Sub2.Sub3");
         simulator.CreateFolder("SomeOtherFolder");

         simulator.Subscribe("Main");
         simulator.Subscribe("Main.Sub1");
         simulator.Subscribe("Main.Sub1.Sub2");
         simulator.Subscribe("Main.Sub1.Sub2.Sub3");
         simulator.Subscribe("SomeOtherFolder");

         var response = simulator.List("Main", "*", true);
         Assert.IsFalse(response.Contains("INBOX"));
         Assert.IsFalse(response.Contains("SomeOtherFolder"));
         Assert.IsTrue(response.Contains("* LIST (\\HasChildren) \".\" \"Main.Sub1\""));
         Assert.IsTrue(response.Contains("* LIST (\\HasChildren) \".\" \"Main.Sub1.Sub2\""));
         Assert.IsTrue(response.Contains("* LIST (\\HasNoChildren) \".\" \"Main.Sub1.Sub2.Sub3\""));

         response = simulator.List("Main.Sub1", "*", true);
         Assert.IsFalse(response.Contains("INBOX"));
         Assert.IsFalse(response.Contains("SomeOtherFolder"));
         Assert.IsTrue(response.Contains("* LIST (\\HasChildren) \".\" \"Main.Sub1.Sub2\""));
         Assert.IsTrue(response.Contains("* LIST (\\HasNoChildren) \".\" \"Main.Sub1.Sub2.Sub3\""));

         response = simulator.LSUB("Main", "*");
         Assert.IsFalse(response.Contains("INBOX"));
         Assert.IsFalse(response.Contains("SomeOtherFolder"));
         Assert.IsTrue(response.Contains("* LSUB (\\HasChildren) \".\" \"Main.Sub1\""));
         Assert.IsTrue(response.Contains("* LSUB (\\HasChildren) \".\" \"Main.Sub1.Sub2\""));
         Assert.IsTrue(response.Contains("* LSUB (\\HasNoChildren) \".\" \"Main.Sub1.Sub2.Sub3\""));

         response = simulator.LSUB("Main.Sub1", "*");
         Assert.IsFalse(response.Contains("INBOX"));
         Assert.IsFalse(response.Contains("SomeOtherFolder"));
         Assert.IsTrue(response.Contains("* LSUB (\\HasChildren) \".\" \"Main.Sub1.Sub2\""));
         Assert.IsTrue(response.Contains("* LSUB (\\HasNoChildren) \".\" \"Main.Sub1.Sub2.Sub3\""));

         simulator.Disconnect();
      }

      [Test]
      [Description("RFC 5258 (LIST-EXTENDED): RETURN (SUBSCRIBED) annotates subscribed mailboxes " +
                   "with the \\Subscribed attribute and leaves unsubscribed ones unannotated.")]
      public void TestListExtendedReturnSubscribed()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "listext1@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("SubFolder");
         simulator.CreateFolder("UnsubFolder");
         simulator.Subscribe("SubFolder");

         string response = simulator.SendSingleCommand("A10 LIST \"\" \"*\" RETURN (SUBSCRIBED)");
         Assert.IsTrue(response.Contains("\"SubFolder\""), response);
         Assert.IsTrue(response.Contains("\"UnsubFolder\""), response);

         foreach (var line in response.Split(new[] {"\r\n"}, StringSplitOptions.None))
         {
            if (line.Contains("\"SubFolder\""))
               Assert.IsTrue(line.Contains("\\Subscribed"), "Subscribed folder should be annotated: " + line);
            if (line.Contains("\"UnsubFolder\""))
               Assert.IsFalse(line.Contains("\\Subscribed"), "Unsubscribed folder must not be annotated: " + line);
         }

         simulator.Disconnect();
      }

      [Test]
      [Description("RFC 5258 (LIST-EXTENDED): the SUBSCRIBED selection option returns only " +
                   "subscribed mailboxes, emitted as * LIST with the \\Subscribed attribute.")]
      public void TestListExtendedSelectSubscribed()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "listext2@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("SubOnly");
         simulator.CreateFolder("NotSubscribed");
         simulator.Subscribe("SubOnly");

         string response = simulator.SendSingleCommand("A10 LIST (SUBSCRIBED) \"\" \"*\"");
         Assert.IsTrue(response.Contains("* LIST"), response);
         Assert.IsTrue(response.Contains("\"SubOnly\""), response);
         Assert.IsTrue(response.Contains("\\Subscribed"), response);
         Assert.IsFalse(response.Contains("\"NotSubscribed\""), response);

         simulator.Disconnect();
      }

      [Test]
      [Description("RFC 5258 (LIST-EXTENDED): a parenthesised list of mailbox patterns matches " +
                   "each pattern and lists every matching mailbox exactly once.")]
      public void TestListExtendedMultiplePatterns()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "listext3@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("Alpha");
         simulator.CreateFolder("Beta");

         string response = simulator.SendSingleCommand("A10 LIST \"\" (\"Alpha\" \"Beta\")");
         Assert.IsTrue(response.Contains("\"Alpha\""), response);
         Assert.IsTrue(response.Contains("\"Beta\""), response);
         Assert.IsTrue(response.Contains("A10 OK"), response);

         simulator.Disconnect();
      }

      [Test]
      [Description("Test to include reference in LIST command (test case from issue 165).")]
      public void TestListWithReferenceTestCase2()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("INBOX.MyApp.SubFolder1");
         simulator.CreateFolder("INBOX.MyApp.SubFolder2");
         simulator.CreateFolder("INBOX.SomeOtherFolder");


         var response = simulator.List("INBOX.MyApp", "%.%", true);
         Assert.IsFalse(response.Contains("\"INBOX.MyApp\""));
         Assert.IsFalse(response.Contains("\"INBOX.SomeOtherFolder\""));
         Assert.IsTrue(response.Contains("* LIST (\\HasNoChildren) \".\" \"INBOX.MyApp.SubFolder1\""));
         Assert.IsTrue(response.Contains("* LIST (\\HasNoChildren) \".\" \"INBOX.MyApp.SubFolder2\""));

         simulator.Disconnect();
      }

      /// <summary>
      ///    Tests that a folder which is not subscribed to isn't included and vice versa.
      /// </summary>
      [Test]
      public void TestLsubInclusion()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "quote@example.test", "test");

         var folderName = "Folder1";

         var simulator = new ImapClientSimulator();
         var sWelcomeMessage = simulator.Connect();
         simulator.Logon(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder(folderName));
         Assert.IsFalse(simulator.LSUB().Contains(folderName));
         Assert.IsTrue(simulator.Subscribe(folderName));
         Assert.IsTrue(simulator.LSUB().Contains(folderName));

         simulator.Disconnect();
      }

      [Test]
      public void TestRenameIncorrectParameters()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder8@example.test", "test");

         var simulator = new ImapClientSimulator();

         var sWelcomeMessage = simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         var result = simulator.Send("A01 CREATE A");

         result = simulator.Send("A02 RENAME A B C");
         Assert.IsTrue(result.Contains("A02 BAD"));

         simulator.Disconnect();
      }

      [Test]
      public void TestRenameLongFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder2@example.test", "test");

         var simulator = new ImapClientSimulator();

         var sWelcomeMessage = simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder("1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17.18.19.20.21.22.23.24.25"));
         Assert.IsTrue(simulator.CreateFolder("A"));

         Assert.IsFalse(simulator.RenameFolder("1", "A.1"));
         Assert.IsTrue(simulator.RenameFolder("1.2.3", "A.1"));
         Assert.IsTrue(simulator.SelectFolder("A.1.4"));


         simulator.Disconnect();
      }

      [Test]
      public void TestRenameRootPublicFolder()
      {
         var folders = _application.Settings.PublicFolders;
         var folder = folders.Add("ShareA");
         folder.Save();

         var permission = folder.Permissions.Add();
         permission.PermissionType = eACLPermissionType.ePermissionTypeAnyone;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionDeleteMailbox, true);
         permission.Save();

         var folder2 = folders.Add("ShareB");
         folder2.Save();

         var permission2 = folder2.Permissions.Add();
         permission2.PermissionType = eACLPermissionType.ePermissionTypeAnyone;
         permission2.set_Permission(eACLPermission.ePermissionLookup, true);
         permission2.set_Permission(eACLPermission.ePermissionRead, true);
         permission2.set_Permission(eACLPermission.ePermissionDeleteMailbox, true);
         permission2.Save();

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "shared@example.test", "test");

         var simulator1 = new ImapClientSimulator();
         simulator1.ConnectAndLogon(account.Address, "test");
         Assert.IsTrue(simulator1.SelectFolder("#Public.ShareA"));
         Assert.IsTrue(simulator1.SelectFolder("#Public.ShareB"));
         Assert.IsTrue(simulator1.RenameFolder("#Public.ShareA", "#Public.ShareB.ShareA"));
         Assert.IsFalse(simulator1.SelectFolder("#Public.ShareA"));
         Assert.IsTrue(simulator1.SelectFolder("#Public.ShareB"));
         Assert.IsTrue(simulator1.SelectFolder("#Public.ShareB.ShareA"));
      }

      [Test]
      [Description("Issue 274, IMAP: Moving folder to sub-folder")]
      public void TestRenameSubFolderToChildSub()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");

         var simulator = new ImapClientSimulator();
         Assert.IsTrue(simulator.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(simulator.CreateFolder("A.B"));

         string result;
         Assert.IsFalse(simulator.RenameFolder("A.B", "A.B.C", out result));
         Assert.IsTrue(result.Contains("A folder cannot be moved into one of its subfolders."));

         Assert.IsTrue(simulator.SelectFolder("A.B"));

         simulator.Disconnect();
      }

      [Test]
      [Description("Issue 274, Test side effect of fix for 274")]
      public void TestRenameSubFolderToMatchingName()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");

         var simulator = new ImapClientSimulator();
         Assert.IsTrue(simulator.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(simulator.CreateFolder("Folder1"));
         Assert.IsTrue(simulator.SelectFolder("Folder1"));

         var result = string.Empty;
         Assert.IsFalse(simulator.RenameFolder("Folder1", "Folder1.Sub1", out result));
         Assert.IsTrue(result.Contains("A folder cannot be moved into one of its subfolders."));
         Assert.IsTrue(simulator.SelectFolder("Folder1"));

         result = string.Empty;
         Assert.IsTrue(simulator.RenameFolder("Folder1", "Folder1Test", out result));
         Assert.IsTrue(simulator.SelectFolder("Folder1Test"));

         simulator.Disconnect();
      }

      [Test]
      public void TestRenameToAndFromINBOX()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder3@example.test", "test");

         var simulator = new ImapClientSimulator();

         var sWelcomeMessage = simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         simulator.Send("A01 CREATE A\r\n");
         Assert.IsFalse(simulator.RenameFolder("A", "INBOX"));
         Assert.IsFalse(simulator.RenameFolder("INBOX", "B"));

         simulator.Disconnect();
      }

      [Test]
      [Description("Test renaming sub folder to a new sub folder where the new parent does not exist.")]
      public void TestRenameToParallelFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");

         var simulator = new ImapClientSimulator();

         var sWelcomeMessage = simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder("Main.Sub"));
         Assert.IsTrue(simulator.RenameFolder("Main.Sub", "Second.Sub"));

         var listResponse = simulator.List();
         Assert.IsFalse(listResponse.Contains("Main.Sub"));
         Assert.IsTrue(listResponse.Contains("Second.Sub"));


         simulator.Disconnect();
      }

      [Test]
      public void TestRenameToSubFolder()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder("1.2.3.4.5"));
         Assert.IsTrue(simulator.CreateFolder("A"));
         Assert.IsTrue(simulator.RenameFolder("1", "A.1"));
         Assert.IsTrue(simulator.SelectFolder("A.1"));
         Assert.IsTrue(simulator.SelectFolder("A.1.2.3.4.5"));
         Assert.IsTrue(simulator.RenameFolder("A.1", "1"));
         Assert.IsFalse(simulator.SelectFolder("A.1"));
         Assert.IsFalse(simulator.SelectFolder("A.1.2.3.4.5"));
         Assert.IsTrue(simulator.SelectFolder("1"));
         Assert.IsTrue(simulator.SelectFolder("1.2.3.4.5"));

         simulator.Disconnect();
      }

      [Test]
      public void TestTryCreateInvalidStructure()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "folder1@example.test", "test");

         var simulator = new ImapClientSimulator();

         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");
         Assert.IsTrue(simulator.CreateFolder("1.2.3"));
         Assert.IsTrue(simulator.CreateFolder("1.2.3.4"));

         // Should fail because name taken.
         Assert.IsFalse(simulator.RenameFolder("1.2.3.4", "1.2.3"));
         Assert.IsFalse(simulator.RenameFolder("1.2.3.4", "1.2"));
         Assert.IsFalse(simulator.RenameFolder("1.2.3.4", "1"));

         // Should fail because invalid destination name.
         Assert.IsFalse(simulator.RenameFolder("1.2.3.4", ""));

         // Should fail because destination name taken.
         Assert.IsFalse(simulator.RenameFolder("1.2.3", "1.2.3.4"));
         Assert.IsFalse(simulator.RenameFolder("1.2", "1.2.3.4"));
         Assert.IsFalse(simulator.RenameFolder("1", "1.2.3.4"));

         Assert.IsFalse(simulator.RenameFolder("1.2.3", "1.2"));
         Assert.IsFalse(simulator.RenameFolder("1.2.3", "1"));

         Assert.IsTrue(simulator.RenameFolder("1.2.3", "A"));

         simulator.Disconnect();
      }

      [Test]
      [Description("A LIST or LSUB pattern longer than any mailbox name the server can hold - quoted, a literal, or quoted and continued over the lines a literal joins - or more patterns than one LIST may carry, is refused BAD rather than matched against every folder on the shared threads; a literal reference or pattern is its octets; a run of wildcards still matches as one wildcard, and a '%' in a run with '*' crosses the delimiter as '*' does.")]
      public void TestListPatternBounds()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "listbounds@example.test", "test");

         var simulator = new ImapClientSimulator();
         simulator.ConnectAndLogon(account.Address, "test");
         simulator.CreateFolder("Alpha.Beta");

         string runs = simulator.SendSingleCommand("A01 LIST \"\" \"Al**a.%%%\"");
         Assert.IsTrue(runs.Contains("\"Alpha.Beta\""), runs);
         Assert.IsTrue(runs.Contains("A01 OK"), runs);

         string mixed = simulator.SendSingleCommand("A02 LIST \"\" \"A%*%a\"");
         Assert.IsTrue(mixed.Contains("\"Alpha.Beta\""), "\"%*%\" is \"*\" and crosses the delimiter: " + mixed);

         string percent = simulator.SendSingleCommand("A03 LIST \"\" \"A%%a\"");
         Assert.IsTrue(percent.Contains("\"Alpha\""), percent);
         Assert.IsFalse(percent.Contains("\"Alpha.Beta\""), "\"%%\" is \"%\" and stops at the delimiter: " + percent);

         // A pattern sent as a literal is the literal's octets (RFC 3501 section 9),
         // not the digits between its braces, which is what LIST used to match.
         simulator.SendRaw("A04 LIST \"\" {5}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw("Alpha\r\n");
         string literal = simulator.ReceiveUntil("A04 ");
         Assert.IsTrue(literal.Contains("\"Alpha\""), "A literal pattern is its octets: " + literal);
         Assert.IsTrue(literal.Contains("A04 OK"), literal);

         // A literal reference, then a pattern list holding a literal: the list's
         // literal is the command's second.
         simulator.SendRaw("A05 LIST {6}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw("Alpha. ({4}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw("Beta \"Zulu\")\r\n");
         literal = simulator.ReceiveUntil("A05 ");
         Assert.IsTrue(literal.Contains("\"Alpha.Beta\""), "A literal reference and a literal in a pattern list are their octets: " + literal);
         Assert.IsTrue(literal.Contains("A05 OK"), literal);

         // One pattern of 9,000 wildcards - as a literal, as a quoted string and as a
         // literal in a pattern list: longer than any name.
         string tooLong = new string('*', 9000);
         simulator.SendRaw("A06 LIST \"\" {" + tooLong.Length + "}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw(tooLong + "\r\n");
         string refused = simulator.ReceiveUntil("A06 ");
         Assert.IsTrue(refused.Contains("A06 BAD"), "A 9,000-character pattern must be refused: " + refused);

         refused = simulator.SendSingleCommand("A07 LIST \"\" \"" + tooLong + "\"");
         Assert.IsTrue(refused.Contains("A07 BAD"), "A 9,000-character quoted pattern must be refused: " + refused);

         simulator.SendRaw("A08 LIST \"\" ({" + tooLong.Length + "}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw(tooLong + " \"Alpha\")\r\n");
         refused = simulator.ReceiveUntil("A08 ");
         Assert.IsTrue(refused.Contains("A08 BAD"), "A 9,000-character literal in a pattern list must be refused: " + refused);

         simulator.SendRaw("A09 LSUB \"\" {" + tooLong.Length + "}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw(tooLong + "\r\n");
         refused = simulator.ReceiveUntil("A09 ");
         Assert.IsTrue(refused.Contains("A09 BAD"), "A 9,000-character LSUB pattern must be refused: " + refused);

         // A quoted pattern goes on over the lines a literal joins - the command is
         // the text of all of them - so no one line has to hold it: two lines of
         // 5,000 wildcards are one pattern of 10,003 characters.
         string half = new string('*', 5000);
         simulator.SendRaw("A10 LIST \"\" \"" + half + "{1}\r\n");
         StringAssert.Contains("+ ", simulator.ReceiveUntil("+ "));
         simulator.SendRaw("X" + half + "\"\r\n");
         refused = simulator.ReceiveUntil("A10 ");
         Assert.IsTrue(refused.Contains("A10 BAD"), "A quoted pattern continued past a literal is one pattern, and 10,003 characters must be refused: " + refused);

         // Thirty-three patterns in one LIST-EXTENDED command.
         var many = new System.Text.StringBuilder();
         for (int i = 0; i < 33; i++)
            many.Append(i == 0 ? "" : " ").Append("\"P").Append(i).Append("\"");
         string tooMany = simulator.SendSingleCommand("A11 LIST \"\" (" + many + ")");
         Assert.IsTrue(tooMany.Contains("A11 BAD"), "Thirty-three patterns must be refused: " + tooMany);

         // The connection is still in step.
         string after = simulator.SendSingleCommand("A12 LIST \"\" \"Alpha\"");
         Assert.IsTrue(after.Contains("A12 OK"), after);

         simulator.Disconnect();
      }
   }
}
