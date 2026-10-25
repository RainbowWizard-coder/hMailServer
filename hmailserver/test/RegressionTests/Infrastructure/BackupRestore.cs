// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;
using hMailServer;
using Microsoft.Win32;
using NUnit.Framework;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   [TestFixture]
   public class BackupRestore : TestFixtureBase
   {
      [SetUp]
      public new void SetUp()
      {
         InitializeBackupSettings();
      }

      private string _backupDir;
      private string _folderCreationTime;
      private bool _backupMessages = true;
      private readonly int _fetchAccountPort = TestSetup.GetNextFreePort();

      public void InitializeBackupSettings()
      {
         _application = SingletonProvider<TestSetup>.Instance.GetApp();
         SetBackupDir(Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString()));

         var dirInfo = new DirectoryInfo(_backupDir);
         dirInfo.Create();

         if (File.Exists(_application.Settings.Backup.LogFile))
            File.Delete(_application.Settings.Backup.LogFile);
      }

      public void SetBackupDir(string backupDir)
      {
         _backupDir = backupDir;
      }

      internal bool Execute()
      {
         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         SetupEnvironment();
         if (!BackupEnvironment())
            return false;

         DeleteEnvironment();
         RestoreEnvironment();
         ConfirmRestore();

         Directory.Delete(_backupDir, true);

         return true;
      }

      private bool WaitForBackupCompletion()
      {
         Assert.IsNotNull(_application);

         for (var i = 0; i < 40; i++)
         {
            try
            {
               var logFile = _application.Settings.Backup.LogFile;

               var contents = TestSetup.ReadExistingTextFile(logFile);

               if (contents.IndexOf("Backup completed successfully") > 0)
                  return true;

               if (contents.IndexOf("BACKUP ERROR:") > 0) return false;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // probably a share access violation.
            }

            Thread.Sleep(250);
         }

         return false;
      }

      private void WaitForRestoreCompletion(string lastServerStartTime)
      {
         Assert.IsNotNull(_application);

         for (var i = 0; i < 600; i++)
         {
            try
            {
               var startTime = _application.Status.StartTime;

               if (startTime.Length > 0 && startTime != lastServerStartTime)
                  return;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               // probably a share access violation.
            }

            Thread.Sleep(100);
         }

         throw new Exception("Timeout while waiting for backup or restore completion");
      }

      public void SetupEnvironment()
      {
         // Set up the domain. It's already set-up...
         var domain = _application.Domains[0];
         Assert.IsNotNull(domain);
         Assert.AreEqual("example.test", domain.Name);

         SetupDomainObject(domain);
         SetupDomainObjects(domain);
         SetupSettings();

         // Last, because it shares an account's own folder with a group as well as
         // with an account, and SetupSettings is what creates the group.
         SetupAccountFolderPermissions(domain);
      }

      /// <summary>
      ///    A share on an ACCOUNT's own folder, which until 16 September 2026 no
      ///    backup carried: IMAPFolder::XMLStore wrote the grants of a public folder
      ///    only, while a restore deletes every domain first and deleting a folder
      ///    deletes its hm_acl rows. So restoring a backup destroyed every share on
      ///    every mailbox - and with it every Send-As grant, which is the post right
      ///    on an owner's INBOX - and had nothing in the archive to put back.
      ///
      ///    Both holders are chosen to land after the folder in the restore, which is
      ///    where it would be easiest to get this wrong: accounts are restored in
      ///    address order, so zz-delegate@ is put back after test@ whose INBOX this
      ///    is, and groups are not restored until the settings phase after every
      ///    domain. Neither grant can be written where it is read.
      /// </summary>
      private void SetupAccountFolderPermissions(Domain domain)
      {
         var delegated = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "zz-delegate@example.test", "test");

         var inbox = domain.Accounts.get_ItemByAddress("test@example.test").IMAPFolders.get_ItemByName("INBOX");

         var permission = inbox.Permissions.Add();
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.PermissionAccountID = delegated.ID;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionRead, true);
         permission.set_Permission(eACLPermission.ePermissionPost, true);
         permission.Save();

         var group = _application.Settings.Groups.get_ItemByName("ACLTestGroup");
         Assert.IsNotNull(group, "SetupGroupObject must have run before this.");

         permission = inbox.Permissions.Add();
         permission.PermissionType = eACLPermissionType.ePermissionTypeGroup;
         permission.PermissionGroupID = group.ID;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.Save();
      }

      private void ConfirmAccountFolderPermissions()
      {
         // A folder rides the archive only when messages do, so its grants do too;
         // and a restore without them puts no folder back to hold one.
         if (!_backupMessages)
            return;

         var domain = _application.Domains[0];
         var inbox = domain.Accounts.get_ItemByAddress("test@example.test").IMAPFolders.get_ItemByName("INBOX");
         var permissions = inbox.Permissions;

         Assert.AreEqual(2, permissions.Count,
            "The two grants on this account's own INBOX must survive the backup and the restore.");

         IMAPFolderPermission byAccount = null;
         IMAPFolderPermission byGroup = null;

         for (int i = 0; i < permissions.Count; i++)
         {
            var permission = permissions[i];

            if (permission.PermissionType == eACLPermissionType.ePermissionTypeUser)
               byAccount = permission;
            else if (permission.PermissionType == eACLPermissionType.ePermissionTypeGroup)
               byGroup = permission;
         }

         Assert.IsNotNull(byAccount, "The grant held by an account is not there.");
         Assert.IsNotNull(byGroup, "The grant held by a group is not there.");

         // The IDs are the restored server's, not the archive's: a grant is carried
         // as its holder's NAME, and the whole of what makes that necessary is that
         // every one of these numbers is reassigned by the restore.
         var delegated = domain.Accounts.get_ItemByAddress("zz-delegate@example.test");
         var group = _application.Settings.Groups.get_ItemByName("ACLTestGroup");

         Assert.AreEqual(delegated.ID, byAccount.PermissionAccountID);
         Assert.IsTrue(byAccount.get_Permission(eACLPermission.ePermissionLookup));
         Assert.IsTrue(byAccount.get_Permission(eACLPermission.ePermissionRead));
         Assert.IsTrue(byAccount.get_Permission(eACLPermission.ePermissionPost),
            "The post right is what a Send-As grant is, and is the one this most needed to carry.");
         Assert.IsFalse(byAccount.get_Permission(eACLPermission.ePermissionAdminister));

         Assert.AreEqual(group.ID, byGroup.PermissionGroupID);
         Assert.IsTrue(byGroup.get_Permission(eACLPermission.ePermissionLookup));
         Assert.IsTrue(byGroup.get_Permission(eACLPermission.ePermissionInsert));
         Assert.IsFalse(byGroup.get_Permission(eACLPermission.ePermissionRead));
      }

      private void SetupSettings()
      {
         SetupBlockedAttachment();
         SetupGreyListingWhiteList();
         SetupWhiteList();
         SetupRoutes();
         SetupDNSBlackLists();
         SetupSURBLServers();
         SetupSSLCertificates();
         SetupGroupObject();
         SetupPublicFolders();
         SetupIncomingRelay();
      }

      private void SetupSSLCertificates()
      {
         var cert = _application.Settings.SSLCertificates.Add();
         cert.CertificateFile = "file1.txt";
         cert.Name = "name1";
         cert.PrivateKeyFile = "pk1";
         cert.Save();

         cert = _application.Settings.SSLCertificates.Add();
         cert.CertificateFile = "file2.txt";
         cert.Name = "name2";
         cert.PrivateKeyFile = "pk2";
         cert.Save();
      }

      private void SetupBlockedAttachment()
      {
         var ba = _application.Settings.AntiVirus.BlockedAttachments.Add();
         ba.Description = "My description";
         ba.Wildcard = "*.my";
         ba.Save();
      }

      private void SetupDNSBlackLists()
      {
         var black = _application.Settings.AntiSpam.DNSBlackLists.Add();
         black.Active = true;
         black.DNSHost = "127.0.0.1";
         black.ExpectedResult = "127.5.2.1";
         black.RejectMessage = "Test";
         black.Score = 4;
         black.Save();

         black = _application.Settings.AntiSpam.DNSBlackLists.Add();
         black.Active = true;
         black.DNSHost = "127.0.0.2";
         black.ExpectedResult = "127.5.2.2";
         black.RejectMessage = "Test2";
         black.Score = 5;
         black.Save();
      }

      private void SetupIncomingRelay()
      {
         var relay = _application.Settings.IncomingRelays.Add();
         relay.Name = "Test";
         relay.LowerIP = "1.2.3.4";
         relay.UpperIP = "4.3.2.1";
         relay.Save();
      }


      private void SetupSURBLServers()
      {
         var black = _application.Settings.AntiSpam.SURBLServers.Add();
         black.Active = true;
         black.DNSHost = "127.0.0.1";
         black.RejectMessage = "Test";
         black.Score = 4;
         black.Save();

         black = _application.Settings.AntiSpam.SURBLServers.Add();
         black.Active = true;
         black.DNSHost = "127.0.0.2";
         black.RejectMessage = "Test2";
         black.Score = 5;
         black.Save();
      }

      private void SetupGreyListingWhiteList()
      {
         var addresses = _application.Settings.AntiSpam.GreyListingWhiteAddresses;

         var address = addresses.Add();
         address.Description = "helo1";
         address.IPAddress = "1.1.1.1";
         address.Save();

         address = addresses.Add();
         address.Description = "helo2";
         address.IPAddress = "2.2.2.2";
         address.Save();
      }


      private void SetupWhiteList()
      {
         var addresses = _application.Settings.AntiSpam.WhiteListAddresses;

         var address = addresses.Add();
         address.Description = "Desc1";
         address.EmailAddress = "Email1";
         address.LowerIPAddress = "1.1.1.1";
         address.UpperIPAddress = "2.2.2.2";
         address.Save();

         address = addresses.Add();
         address.Description = "Desc2";
         address.EmailAddress = "Email2";
         address.LowerIPAddress = "2.2.2.2";
         address.UpperIPAddress = "21.21.21.21";
         address.Save();
      }


      private void SetupRoutes()
      {
         var routes = _application.Settings.Routes;

         var route = routes.Add();
         route.DomainName = "test1.com";
         route.Description = "description";
         route.UseSSL = true;
         route.TreatSenderAsLocalDomain = true;
         route.TreatRecipientAsLocalDomain = false;
         route.Save();

         route = routes.Add();
         route.DomainName = "test2.com";
         route.Description = "description2";
         route.UseSSL = false;
         route.TreatSenderAsLocalDomain = false;
         route.TreatRecipientAsLocalDomain = true;
         route.Save();

         var addresses = route.Addresses;

         var address1 = addresses.Add();
         address1.Address = "address1@test2.com";
         address1.Save();

         var address2 = addresses.Add();
         address2.Address = "address2@test2.com";
         address2.Save();
      }

      private void SetupDomainObject(Domain domain)
      {
         domain.Postmaster = "someone@example.test";
         domain.SignatureEnabled = true;
         domain.AddSignaturesToLocalMail = true;
         domain.AddSignaturesToReplies = true;

         domain.SignaturePlainText = "PLS";
         domain.SignatureHTML = "HTML";

         // The DKIM key rotation's policy rides the domain through the backup. The
         // schedule is left off: a keyless domain with it on would be given a key by
         // the first scheduled pass, which is not what this test is about.
         ComSetDkimRotationIntervalDays(domain, 90);
         ComSetDkimRotationPropagationHours(domain, 48);
         ComSetDkimRotationRetentionDays(domain, 14);
         ComSetDkimRotationKeyBits(domain, 3072);
         domain.Save();

         var da = domain.DomainAliases.Add();
         da.AliasName = "test1.com";
         da.Save();

         var da2 = domain.DomainAliases.Add();
         da2.AliasName = "test2.com";
         da2.Save();
      }

      private void SetupDomainObjects(Domain domain)
      {
         SetupAccountObject(domain);
         SetupAliasObject(domain);
         SetupDistributionListObject(domain);
      }

      private void SetupPublicFolders()
      {
         var folders = _application.Settings.PublicFolders;
         var folder = folders.Add("Test1");
         folder.Save();

         folder = folders.Add("Test2");
         folder.Save();

         folder = folders.Add("Test3");
         folder.Save();

         folder = folders.Add("ACL");
         folder.Save();

         var domain = _application.Domains[0];
         var account1 = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "acltest-1@example.test", "test");

         var permission = folder.Permissions.Add();
         permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
         permission.PermissionAccountID = account1.ID;
         permission.set_Permission(eACLPermission.ePermissionAdminister, true);
         permission.set_Permission(eACLPermission.ePermissionInsert, true);
         permission.Save();

         folder = folder.SubFolders.Add("MySubFolder");
         folder.Save();

         var group1 = _application.Settings.Groups.Add();
         group1.Name = "ACLTestGroup";
         group1.Save();

         permission = folder.Permissions.Add();
         permission.PermissionType = eACLPermissionType.ePermissionTypeGroup;
         permission.PermissionGroupID = group1.ID;
         permission.set_Permission(eACLPermission.ePermissionLookup, true);
         permission.set_Permission(eACLPermission.ePermissionDeleteMailbox, true);
         permission.Save();
      }

      private void ConfirmPublicFolders()
      {
         if (_backupMessages)
         {
            var domain = _application.Domains[0];

            var publicFolders = _application.Settings.PublicFolders;

            Assert.IsNotNull(publicFolders.get_ItemByName("Test1"));
            Assert.IsNotNull(publicFolders.get_ItemByName("Test2"));
            Assert.IsNotNull(publicFolders.get_ItemByName("Test3"));
            Assert.IsNotNull(publicFolders.get_ItemByName("ACL"));

            var permissions = publicFolders.get_ItemByName("ACL").Permissions;
            Assert.AreEqual(1, permissions.Count);

            var account1 = domain.Accounts.get_ItemByAddress("acltest-1@example.test");

            var permission = permissions[0];

            Assert.AreEqual(eACLPermissionType.ePermissionTypeUser, permission.PermissionType);
            Assert.AreEqual(account1.ID, permission.PermissionAccountID);

            Assert.IsTrue(permission.get_Permission(eACLPermission.ePermissionAdminister));
            Assert.IsTrue(permission.get_Permission(eACLPermission.ePermissionInsert));
            Assert.IsFalse(permission.get_Permission(eACLPermission.ePermissionRead));
            Assert.IsFalse(permission.get_Permission(eACLPermission.ePermissionWriteSeen));

            var group1 = _application.Settings.Groups.get_ItemByName("ACLTestGroup");

            permissions = publicFolders.get_ItemByName("ACL").SubFolders.get_ItemByName("MySubFolder").Permissions;
            permission = permissions[0];

            Assert.AreEqual(eACLPermissionType.ePermissionTypeGroup, permission.PermissionType);
            Assert.AreEqual(group1.ID, permission.PermissionGroupID);

            Assert.IsTrue(permission.get_Permission(eACLPermission.ePermissionLookup));
            Assert.IsTrue(permission.get_Permission(eACLPermission.ePermissionDeleteMailbox));
            Assert.IsFalse(permission.get_Permission(eACLPermission.ePermissionRead));
            Assert.IsFalse(permission.get_Permission(eACLPermission.ePermissionWriteSeen));
         }
      }

      private void SetupDistributionListObject(Domain domain)
      {
         var listRecipients = new List<string>();
         listRecipients.Add("member1@example.test");
         listRecipients.Add("member2@example.test");
         listRecipients.Add("member3@example.test");

         var list = SingletonProvider<TestSetup>.Instance.AddDistributionList(domain, "list@example.test",
            listRecipients);

         // The list manager's settings and a member's own delivery
         // (docs/MailingLists.md), which the backup carries as attributes of the
         // list and of the recipient.
         list.Managed = true;
         list.SubscribePolicy = 2;
         list.Archive = 1;
         list.ArchiveDays = 30;
         list.Digest = true;
         list.DigestHour = 9;
         list.ReplyToList = true;
         list.SubjectPrefix = "[list]";
         list.Footer = "The list's footer.";
         list.Verp = true;
         list.Owner = "owner@example.test";
         list.Save();

         var member2 = list.Recipients[1];
         member2.Delivery = 1;
         member2.Save();

         // The manager's own columns of a row, which nothing but the manager
         // writes and COM does not: a request waiting for the owner, with
         // bounce counts, a reason and a token of its own. Written here straight
         // into the row, and read back after the restore.
         _application.Database.ExecuteSQL(
            "update hm_distributionlistsrecipients set distributionlistrecipientconfirmed = 2, " +
            "distributionlistrecipienthardbounces = 3, distributionlistrecipientsoftbounces = 2, " +
            "distributionlistrecipienttoken = '" + BackupListToken + "', distributionlistrecipientreason = 'Three hard bounces.' " +
            "where distributionlistrecipientaddress = 'member3@example.test'");
      }

      // The token member3 is given before the backup, and must have after it.
      private const string BackupListToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

      private void SetupGroupObject()
      {
         var group = _application.Settings.Groups.Add();
         group.Name = "TestGroup";
         group.Save();

         var gm1 = SingletonProvider<TestSetup>.Instance.AddAccount(_application.Domains[0], "gm1@example.test",
            "test");
         var gm2 = SingletonProvider<TestSetup>.Instance.AddAccount(_application.Domains[0], "gm2@example.test",
            "test");
         var gm3 = SingletonProvider<TestSetup>.Instance.AddAccount(_application.Domains[0], "gm3@example.test",
            "test");

         var gm = group.Members.Add();
         gm.AccountID = gm1.ID;
         gm.Save();

         gm = group.Members.Add();
         gm.AccountID = gm2.ID;
         gm.Save();

         gm = group.Members.Add();
         gm.AccountID = gm3.ID;
         gm.Save();
      }

      private void SetupAliasObject(Domain domain)
      {
         var alias = SingletonProvider<TestSetup>.Instance.AddAlias(domain, "alias@example.test",
            "someone@example.test");
      }

      private void SetupAccountObject(Domain domain)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "test@example.test", "test");

         // Make sure the inbox contains two messages which should be backed up.
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Message 1 Subject",
            "Message 1 Body");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 1);

         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Message 2 Subject",
            "Message 2 Body");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Message 3 Subject",
            "Message 3 Body");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);

         var sim = new ImapClientSimulator();
         Assert.IsTrue(sim.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(sim.SelectFolder("Inbox"));
         Assert.IsTrue(sim.SetDeletedFlag(2));
         Assert.IsTrue(sim.Expunge());
         sim.Disconnect();

         _folderCreationTime = account.IMAPFolders.get_ItemByName("INBOX").CreationTime;

         account.Active = true;
         account.ADDomain = "AD";
         account.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         account.ADUsername = "AU";
         account.ForwardAddress = "FA";
         account.ForwardEnabled = false;
         account.ForwardKeepOriginal = true;
         account.IsAD = false;
         account.MaxSize = 1250;
         account.PersonFirstName = "First";
         account.PersonLastName = "Last";
         account.SignatureEnabled = true;
         account.SignatureHTML = "HTML";
         account.SignaturePlainText = "PLAIN";
         account.VacationMessage = "VAC";
         account.VacationMessageExpires = true;
         account.VacationMessageExpiresDate = "2040-01-01";
         account.VacationMessageIsOn = true;
         account.VacationSubject = "SUBJ";
         account.Password = "test";
         account.Save();

         // Set up fetch account
         var fa = account.FetchAccounts.Add();
         fa.DaysToKeepMessages = 5;
         fa.Enabled = true;
         fa.MinutesBetweenFetch = 10;
         fa.Name = "test";
         fa.Port = _fetchAccountPort;
         fa.ProcessMIMEDate = true;
         fa.ProcessMIMERecipients = true;
         fa.ServerAddress = "127.0.0.1";
         fa.Username = "test";
         fa.UseSSL = false;
         fa.UseAntiSpam = true;
         fa.UseAntiVirus = true;
         fa.Save();

         // Two more, never collected from: an IMAP account that leaves every message on
         // the remote server (0) and a POP3 one that deletes each as it is collected (-1).
         // An archive written since 6.3.6 says its DaysToKeep is in the one meaning both
         // fetchers read (FetchAccount.cpp, DaysToKeepSchema) and is restored as it is; one
         // written before goes through the translation of schema 6106, which would make
         // the IMAP account's 0 a -1.
         var imapKeeps = account.FetchAccounts.Add();
         imapKeeps.Enabled = false;
         imapKeeps.Name = "imap keeps";
         imapKeeps.ServerType = 1;
         imapKeeps.ServerAddress = "127.0.0.1";
         imapKeeps.Port = _fetchAccountPort;
         imapKeeps.Username = "test";
         imapKeeps.DaysToKeepMessages = 0;
         imapKeeps.Save();

         var pop3Deletes = account.FetchAccounts.Add();
         pop3Deletes.Enabled = false;
         pop3Deletes.Name = "pop3 deletes";
         pop3Deletes.ServerAddress = "127.0.0.1";
         pop3Deletes.Port = _fetchAccountPort;
         pop3Deletes.Username = "test";
         pop3Deletes.DaysToKeepMessages = -1;
         pop3Deletes.Save();

         DownloadFromExternalAccount(account, fa);

         var rule = account.Rules.Add();
         rule.Name = "MyRule";
         var criteria = rule.Criterias.Add();
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchValue = "0";
         criteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRAForwardEmail;
         action.To = "someone@example.test";
         action.Body = "Test";
         action.Filename = "File";
         action.FromAddress = "T";
         action.FromName = "N";
         action.HeaderName = "H";
         action.IMAPFolder = "Folder";
         action.ScriptFunction = "Script";
         action.Subject = "Subj";
         action.Value = "Value";
         action.Save();

         rule.Save();
      }

      private static FetchAccount FetchAccountNamed(Account account, string name)
      {
         var fetchAccounts = account.FetchAccounts;
         for (var i = 0; i < fetchAccounts.Count; i++)
         {
            var fetchAccount = fetchAccounts.get_Item(i);
            if (fetchAccount.Name == name)
               return fetchAccount;
         }

         Assert.Fail("The restored account has no external account named " + name + ".");
         return null;
      }

      private void DownloadFromExternalAccount(Account account, FetchAccount fa)
      {
         if (!_backupMessages)
            return;

         // Download messages from the external account.
         var messages = new List<string>();

         messages.Add("Subject: Message 1\r\n");
         messages.Add("Subject: Message 2\r\n");
         messages.Add("Subject: Message 3\r\n");

         using (var pop3Server = new Pop3ServerSimulator(1, fa.Port, messages))
         {
            pop3Server.StartListen();
            fa.DownloadNow();
            pop3Server.WaitForCompletion();
         }

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 5);
      }

      private bool BackupEnvironment()
      {
         var oBackupSettings = _application.Settings.Backup;
         oBackupSettings.BackupDomains = true;
         oBackupSettings.BackupMessages = _backupMessages;
         oBackupSettings.BackupSettings = true;
         oBackupSettings.CompressDestinationFiles = true;

         oBackupSettings.Destination = _backupDir;

         _application.BackupManager.StartBackup();

         return WaitForBackupCompletion();
      }

      private void DeleteEnvironment()
      {
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         var addresses = _application.Settings.AntiSpam.WhiteListAddresses;
         while (addresses.Count > 0)
            addresses[0].Delete();

         var routes = _application.Settings.Routes;
         while (routes.Count > 0)
            routes[0].Delete();

         var attachments = _application.Settings.AntiVirus.BlockedAttachments;
         while (attachments.Count > 0)
            attachments[0].Delete();

         var blackLists = _application.Settings.AntiSpam.DNSBlackLists;
         while (blackLists.Count > 0)
            blackLists[0].Delete();

         var surblServers = _application.Settings.AntiSpam.SURBLServers;
         while (surblServers.Count > 0)
            surblServers[0].Delete();

         var sslCertificates = _application.Settings.SSLCertificates;
         while (sslCertificates.Count > 0)
            sslCertificates[0].Delete();

         var incomingRelays = _application.Settings.IncomingRelays;
         while (incomingRelays.Count > 0)
            incomingRelays[0].Delete();
      }

      private void RestoreEnvironment()
      {
         var startTime = _application.Status.StartTime;

         // locate backup file
         var dirInfo = new DirectoryInfo(_backupDir);
         var files = dirInfo.GetFiles();

         var backupFile = files[0].FullName;

         var backup = _application.BackupManager.LoadBackup(backupFile);

         backup.RestoreDomains = true;
         backup.RestoreMessages = _backupMessages;
         backup.RestoreSettings = true;
         backup.StartRestore();

         WaitForRestoreCompletion(startTime);
      }

      private void ConfirmRestore()
      {
         ConfirmSettings();
         ConfirmDomainObject(_application.Domains[0]);
         ConfirmDomainObjects();
         ConfirmAccountFolderPermissions();
      }

      private void ConfirmSettings()
      {
         ConfirmGroupObject();
         ConfirmBlockedAttachments();
         ConfirmRoutes();
         ConfirmGreyListingWhiteList();
         ConfirmWhiteList();
         ConfirmDNSBlackLists();
         ConfirmSURBLServers();
         ConfirmSSLCertificates();
         ConfirmPublicFolders();
         ConfirmIncomingRelay();
      }

      private void ConfirmSSLCertificates()
      {
         var cert = _application.Settings.SSLCertificates[0];
         Assert.AreEqual("file1.txt", cert.CertificateFile);
         Assert.AreEqual("name1", cert.Name);
         Assert.AreEqual("pk1", cert.PrivateKeyFile);

         cert = _application.Settings.SSLCertificates[1];
         Assert.AreEqual("file2.txt", cert.CertificateFile);
         Assert.AreEqual("name2", cert.Name);
         Assert.AreEqual("pk2", cert.PrivateKeyFile);
      }

      private void ConfirmDNSBlackLists()
      {
         var black = _application.Settings.AntiSpam.DNSBlackLists.get_ItemByDNSHost("127.0.0.1");
         Assert.IsTrue(black.Active);
         Assert.AreEqual("127.5.2.1", black.ExpectedResult);
         Assert.AreEqual("Test", black.RejectMessage);
         Assert.AreEqual(4, black.Score);

         black = _application.Settings.AntiSpam.DNSBlackLists.get_ItemByDNSHost("127.0.0.2");
         Assert.IsTrue(black.Active);
         Assert.AreEqual("127.5.2.2", black.ExpectedResult);
         Assert.AreEqual("Test2", black.RejectMessage);
         Assert.AreEqual(5, black.Score);
      }

      private void ConfirmIncomingRelay()
      {
         var relays = _application.Settings.IncomingRelays;
         Assert.AreEqual(1, relays.Count);

         var relay = relays[0];
         Assert.AreEqual("Test", relay.Name);
         Assert.AreEqual("1.2.3.4", relay.LowerIP);
         Assert.AreEqual("4.3.2.1", relay.UpperIP);
      }

      private void ConfirmSURBLServers()
      {
         var black = _application.Settings.AntiSpam.SURBLServers.get_ItemByDNSHost("127.0.0.1");
         Assert.IsTrue(black.Active);
         Assert.AreEqual("Test", black.RejectMessage);
         Assert.AreEqual(4, black.Score);

         black = _application.Settings.AntiSpam.SURBLServers.get_ItemByDNSHost("127.0.0.2");
         Assert.IsTrue(black.Active);
         Assert.AreEqual("Test2", black.RejectMessage);
         Assert.AreEqual(5, black.Score);
      }

      private void ConfirmBlockedAttachments()
      {
         var attachments = _application.Settings.AntiVirus.BlockedAttachments;
         Assert.Greater(attachments.Count, 0);

         for (var i = 0; i < attachments.Count; i++)
         {
            var ba = attachments[i];

            if (ba.Description == "My description" && ba.Wildcard == "*.my")
               return;
         }

         Assert.Fail("Blocked attachment not found");
      }

      private void ConfirmRoutes()
      {
         var routes = _application.Settings.Routes;

         Assert.AreEqual(2, routes.Count);

         var route = routes[0];
         Assert.AreEqual("test1.com", route.DomainName);
         Assert.AreEqual("description", route.Description);
         Assert.AreEqual(true, route.UseSSL);
         Assert.AreEqual(true, route.TreatSenderAsLocalDomain);
         Assert.AreEqual(false, route.TreatRecipientAsLocalDomain);
         Assert.AreEqual(false, route.TreatSecurityAsLocalDomain);

         route = routes[1];
         Assert.AreEqual("test2.com", route.DomainName);
         Assert.AreEqual("description2", route.Description);
         Assert.AreEqual(false, route.UseSSL);
         Assert.AreEqual(false, route.TreatSenderAsLocalDomain);
         Assert.AreEqual(true, route.TreatRecipientAsLocalDomain);
         Assert.AreEqual(true, route.TreatSecurityAsLocalDomain);

         Assert.AreEqual(2, route.Addresses.Count);

         var addresses = new List<string>();
         addresses.Add(route.Addresses[0].Address);
         addresses.Add(route.Addresses[1].Address);

         Assert.IsTrue(addresses.Contains("address1@test2.com"));
         Assert.IsTrue(addresses.Contains("address2@test2.com"));
      }

      private void ConfirmGreyListingWhiteList()
      {
         var addresses = _application.Settings.AntiSpam.GreyListingWhiteAddresses;

         Assert.AreEqual(2, addresses.Count);

         var address = addresses[0];
         Assert.AreEqual("helo1", address.Description);
         Assert.AreEqual("1.1.1.1", address.IPAddress);

         address = addresses[1];
         Assert.AreEqual("helo2", address.Description);
         Assert.AreEqual("2.2.2.2", address.IPAddress);
      }

      private void ConfirmWhiteList()
      {
         var addresses = _application.Settings.AntiSpam.WhiteListAddresses;

         Assert.AreEqual(2, addresses.Count);

         var address = addresses[0];
         Assert.AreEqual("Desc1", address.Description);
         Assert.AreEqual("Email1", address.EmailAddress);
         Assert.AreEqual("1.1.1.1", address.LowerIPAddress);
         Assert.AreEqual("2.2.2.2", address.UpperIPAddress);

         address = addresses[1];
         Assert.AreEqual("Desc2", address.Description);
         Assert.AreEqual("Email2", address.EmailAddress);
         Assert.AreEqual("2.2.2.2", address.LowerIPAddress);
         Assert.AreEqual("21.21.21.21", address.UpperIPAddress);
      }

      private void ConfirmDomainObject(Domain domain)
      {
         Assert.AreEqual("someone@example.test", domain.Postmaster);
         Assert.IsTrue(domain.SignatureEnabled);
         Assert.IsTrue(domain.AddSignaturesToLocalMail);
         Assert.IsTrue(domain.AddSignaturesToReplies);
         Assert.AreEqual("PLS", domain.SignaturePlainText);
         Assert.AreEqual("HTML", domain.SignatureHTML);

         Assert.IsFalse(ComDkimRotationEnabled(domain));
         Assert.AreEqual(90, ComDkimRotationIntervalDays(domain), "The rotation interval did not survive the backup.");
         Assert.AreEqual(48, ComDkimRotationPropagationHours(domain), "The propagation period did not survive the backup.");
         Assert.AreEqual(14, ComDkimRotationRetentionDays(domain), "The retention period did not survive the backup.");
         Assert.AreEqual(3072, ComDkimRotationKeyBits(domain), "The key size did not survive the backup.");

         Assert.AreEqual("example.test", _application.Domains[0].Name);
         Assert.AreEqual(2, _application.Domains[0].DomainAliases.Count);
         Assert.AreEqual("test1.com", _application.Domains[0].DomainAliases[0].AliasName);
         Assert.AreEqual("test2.com", _application.Domains[0].DomainAliases[1].AliasName);
      }

      private void ConfirmDomainObjects()
      {
         ConfirmAccountObject();
         ConfirmAliasObject();
         ConfirmDistributionListObject();
      }

      private void ConfirmDistributionListObject()
      {
         var list = _application.Domains[0].DistributionLists[0];
         Assert.AreEqual("list@example.test", list.Address);
         Assert.AreEqual(3, list.Recipients.Count);
         Assert.AreEqual("member1@example.test", list.Recipients[0].RecipientAddress);
         Assert.AreEqual("member2@example.test", list.Recipients[1].RecipientAddress);
         Assert.AreEqual("member3@example.test", list.Recipients[2].RecipientAddress);

         Assert.IsTrue(list.Managed);
         Assert.AreEqual(2, list.SubscribePolicy);
         Assert.AreEqual(1, list.Archive);
         Assert.AreEqual(30, list.ArchiveDays);
         Assert.IsTrue(list.Digest);
         Assert.AreEqual(9, list.DigestHour);
         Assert.IsTrue(list.ReplyToList);
         Assert.AreEqual("[list]", list.SubjectPrefix);
         Assert.AreEqual("The list's footer.", list.Footer);
         Assert.IsTrue(list.Verp);
         Assert.AreEqual("owner@example.test", list.Owner);
         Assert.AreEqual(0, list.Recipients[0].Delivery);
         Assert.AreEqual(1, list.Recipients[1].Delivery);
         Assert.AreEqual(1, list.Recipients[1].Confirmed);

         // The row's state and counts came back as they were written - a state
         // of 2 cannot be the default an older backup is read with, which is 1.
         Assert.AreEqual(2, list.Recipients[2].Confirmed, "The request's state was not restored.");
         Assert.AreEqual(3, list.Recipients[2].HardBounces);
         Assert.AreEqual(2, list.Recipients[2].SoftBounces);

         // And its token: the row the token names is the restored member3's.
         _application.Database.ExecuteSQL(
            "update hm_distributionlistsrecipients set distributionlistrecipienthardbounces = 99 " +
            "where distributionlistrecipienttoken = '" + BackupListToken + "'");
         Assert.AreEqual(99, _application.Domains[0].DistributionLists[0].Recipients[2].HardBounces, "The member's token was not restored.");
      }

      private void ConfirmAliasObject()
      {
         var alias = _application.Domains[0].Aliases[0];
         Assert.AreEqual("alias@example.test", alias.Name);
         Assert.AreEqual("someone@example.test", alias.Value);
      }

      private void ConfirmAccountObject()
      {
         var account = _application.Domains[0].Accounts.get_ItemByAddress("test@example.test");


         Assert.IsTrue(account.Active);
         Assert.AreEqual("AD", account.ADDomain);
         Assert.AreEqual(eAdminLevel.hAdminLevelDomainAdmin, account.AdminLevel);
         Assert.AreEqual("AU", account.ADUsername);
         Assert.AreEqual("FA", account.ForwardAddress);
         Assert.IsFalse(account.ForwardEnabled);
         Assert.IsTrue(account.ForwardKeepOriginal);
         Assert.IsFalse(account.IsAD);
         Assert.AreEqual(1250, account.MaxSize);
         Assert.AreEqual("First", account.PersonFirstName);
         Assert.AreEqual("Last", account.PersonLastName);
         Assert.IsTrue(account.SignatureEnabled);
         Assert.AreEqual("HTML", account.SignatureHTML);
         Assert.AreEqual("PLAIN", account.SignaturePlainText);
         Assert.AreEqual("VAC", account.VacationMessage);
         Assert.IsTrue(account.VacationMessageExpires);
         Assert.AreEqual("2040-01-01", account.VacationMessageExpiresDate.Substring(0, 10));
         Assert.IsTrue(account.VacationMessageIsOn);
         Assert.AreEqual("SUBJ", account.VacationSubject);

         // Confirm fetch account
         var fa = FetchAccountNamed(account, "test");
         Assert.AreEqual(5, fa.DaysToKeepMessages);
         Assert.IsTrue(fa.Enabled);
         Assert.AreEqual(10, fa.MinutesBetweenFetch);
         Assert.AreEqual("test", fa.Name);
         Assert.AreEqual(_fetchAccountPort, fa.Port);
         Assert.AreEqual(true, fa.ProcessMIMEDate);
         Assert.IsTrue(fa.ProcessMIMERecipients);
         Assert.AreEqual("test", fa.Username);
         Assert.IsFalse(fa.UseSSL);
         Assert.AreEqual("127.0.0.1", fa.ServerAddress);
         Assert.IsTrue(fa.UseAntiSpam);
         Assert.IsTrue(fa.UseAntiVirus);

         var imapKeeps = FetchAccountNamed(account, "imap keeps");
         Assert.AreEqual(1, imapKeeps.ServerType);
         Assert.AreEqual(0, imapKeeps.DaysToKeepMessages, "An IMAP account archived at 0 - leave every message - is restored at 0.");
         var pop3Deletes = FetchAccountNamed(account, "pop3 deletes");
         Assert.AreEqual(0, pop3Deletes.ServerType);
         Assert.AreEqual(-1, pop3Deletes.DaysToKeepMessages, "A POP3 account archived at -1 is restored at -1.");

         // Make sur no additional mail is downloaded. We have already downloaded it.
         DownloadFromExternalAccount(account, fa);

         var rule = account.Rules[0];
         Assert.AreEqual("MyRule", rule.Name);

         var criteria = rule.Criterias[0];
         Assert.AreEqual(eRuleMatchType.eMTGreaterThan, criteria.MatchType);
         Assert.AreEqual(eRulePredefinedField.eFTMessageSize, criteria.PredefinedField);
         Assert.AreEqual("0", criteria.MatchValue);

         var action = rule.Actions[0];
         Assert.AreEqual(eRuleActionType.eRAForwardEmail, action.Type);
         Assert.AreEqual("someone@example.test", action.To);
         Assert.AreEqual("Test", action.Body);
         Assert.AreEqual("File", action.Filename);
         Assert.AreEqual("T", action.FromAddress);
         Assert.AreEqual("N", action.FromName);
         Assert.AreEqual("H", action.HeaderName);
         Assert.AreEqual("Folder", action.IMAPFolder);
         Assert.AreEqual("Script", action.ScriptFunction);
         Assert.AreEqual("Subj", action.Subject);
         Assert.AreEqual("Value", action.Value);

         var inbox = account.IMAPFolders.get_ItemByName("INBOX");

         var messages = inbox.Messages;

         if (_backupMessages)
         {
            Assert.AreEqual(6, inbox.CurrentUID);
            Assert.AreEqual(_folderCreationTime, inbox.CreationTime);

            Assert.AreEqual(5, messages.Count);
            Assert.AreEqual("Message 1 Subject", messages[0].Subject);
            Assert.IsTrue(messages[1].Body.Contains("Message 3 Body"));

            Assert.AreEqual(1, messages[0].UID);
            Assert.AreEqual(3, messages[1].UID);
         }
         else
         {
            Assert.AreEqual(0, messages.Count);
         }
      }

      private void ConfirmGroupObject()
      {
         var group = _application.Settings.Groups.get_ItemByName("TestGroup");

         Assert.AreEqual("TestGroup", group.Name);
         Assert.AreEqual(3, group.Members.Count);

         Assert.AreEqual("gm1@example.test", group.Members[0].Account.Address);
         Assert.AreEqual("gm2@example.test", group.Members[1].Account.Address);
         Assert.AreEqual("gm3@example.test", group.Members[2].Account.Address);
      }

      [Test]
      public void TestWithMessages()
      {
         _backupMessages = true;
         Assert.IsTrue(Execute());
      }

      [Test]
      public void TestWithoutMessages()
      {
         _backupMessages = false;
         Assert.IsTrue(Execute());
      }

      [Test]
      [Description("A backup started the moment the backup log says the last one failed is accepted. The " +
                   "failure was reported - the log line, the error, the alert and the OnBackupFailed script - " +
                   "before the run slot was released, so a backup started at that point was refused as " +
                   "already started for as long as the script ran (upstream #668).")]
      public void ABackupCanBeStartedOnceTheFailureIsLogged()
      {
         _backupMessages = false;

         // OnBackupFailed takes three seconds, so the failed run is still reporting when
         // the log says it failed.
         var scripting = _settings.Scripting;
         File.WriteAllText(scripting.CurrentScriptFile,
            "Sub OnBackupFailed(reason)\r\n" +
            "   Dim started : started = Timer\r\n" +
            "   Do While Timer < started + 3 : Loop\r\n" +
            "End Sub\r\n");
         scripting.Enabled = true;
         scripting.Reload();

         try
         {
            var backupSettings = _application.Settings.Backup;
            backupSettings.Destination = Path.Combine(_backupDir, "Missing");
            CustomAsserts.AssertDeleteFile(backupSettings.LogFile);

            _application.BackupManager.StartBackup();
            Assert.IsFalse(WaitForBackupCompletion());

            CustomAsserts.AssertReportedError("HM5014", "The specified backup directory is not accessible");
            CustomAsserts.AssertDeleteFile(backupSettings.LogFile);

            Assert.IsTrue(BackupEnvironment(), TestSetup.ReadExistingTextFile(backupSettings.LogFile));
         }
         finally
         {
            scripting.Enabled = false;
            File.WriteAllText(scripting.CurrentScriptFile, string.Empty);
            scripting.Reload();
         }
      }

      [Test]
      [Description("A destination inside the data directory is left out of the copy, so the backup completes and verifies; and a restore from it puts the mail back and keeps the destination and the archive")]
      public void ADestinationInsideTheDataDirectoryIsBackedUpToAndRestoredFrom()
      {
         // The first real Linux live update's configuration (29 September 2026):
         // /var/lib/hmailserver/backup under the data directory /var/lib/hmailserver.
         // Every backup copied its own staging folder into itself until the path
         // was too long, HM5014, and so every unattended apply was refused. The
         // same code runs here. The directory is made in capitals and given as the
         // destination in small letters, so that the copy has to know it by what
         // it is and not by how it is spelt. On Linux the same copy and restore are
         // held to this by backup-tests (BackupExecuterTester).
         _backupMessages = true;

         if (Directory.Exists(_backupDir))
            Directory.Delete(_backupDir, true);

         string inside = Paths.Combine(_application.Settings.Directories.DataDirectory, "BACKUPINSIDE-" + TestSetup.UniqueString());
         Directory.CreateDirectory(inside);
         SetBackupDir(inside.Replace("BACKUPINSIDE-", "backupinside-"));

         try
         {
            var domain = _application.Domains.get_ItemByName("example.test");
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "inside@example.test", "test");
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "First", "The first message.");
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "Second", "The second message.");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

            bool completed = BackupEnvironment();
            string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            Assert.IsTrue(completed, "The backup did not complete. Backup log: " + log);
            Assert.IsTrue(log.Contains("Left out of the copy of the data directory"), "The log does not name what the copy left out. Backup log: " + log);
            Assert.IsTrue(log.Contains("the backup destination"), "The destination was not left out. Backup log: " + log);
            Assert.IsTrue(log.Contains("exactly what this run wrote"), "The verified restore did not pass. Backup log: " + log);

            string[] archives = Directory.GetFiles(inside, "HMBackup *.7z");
            Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + inside);
            Assert.IsFalse(Directory.Exists(Paths.Combine(inside, "DataBackup")), "The staging folder was left in the destination.");

            // The restore deletes every domain and empties the data directory before
            // it copies the archive's store back: it must do that around the
            // destination, which holds the archive it is restoring from. Status.StartTime,
            // which says the restore is over, has one-second resolution.
            Thread.Sleep(1100);
            RestoreEnvironment();

            Assert.IsTrue(File.Exists(archives[0]), "The restore deleted the archive it restored from, which is inside the data directory.");
            Pop3ClientSimulator.AssertMessageCount("inside@example.test", "test", 2);

            string restoreLog = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            Assert.IsTrue(restoreLog.Contains("Left in place by the restore of the data directory"), "The restore does not name what it left in place. Backup log: " + restoreLog);
            Assert.IsTrue(restoreLog.Contains("Restore completed successfully"), "The restore did not complete. Backup log: " + restoreLog);
         }
         finally
         {
            try
            {
               if (Directory.Exists(inside))
                  Directory.Delete(inside, true);
            }
            catch (IOException)
            {
               // Left behind, it is a folder in the data directory no domain is named
               // after; not worth failing the test over.
            }
         }
      }

      [Test]
      [Description("A restore deletes every domain before it loads the backup's, and deletes no key file a DKIM key rotation generated doing it: the restored domain names the same file and the file is still on disk. The restored domain's new ID is the control that the restore really did delete and recreate it.")]
      public void ARestoreKeepsTheKeyFilesTheRotationGenerated()
      {
         _backupMessages = false;

         Domain domain = _application.Domains.get_ItemByName("example.test");
         string keyFile = MiniJson.Text(MiniJson.Object(ComDkimRotationStart(domain, "")), "next.key_file");
         Assert.IsTrue(File.Exists(keyFile), "The rotation did not write its key: " + keyFile);
         int idBefore = domain.ID;

         try
         {
            Assert.IsTrue(BackupEnvironment(), "The backup did not complete.");
            RestoreEnvironment();

            Domain restored = _application.Domains.get_ItemByName("example.test");
            Assert.AreNotEqual(idBefore, restored.ID, "The restore did not replace the domain, so this proves nothing about what a replaced domain keeps.");
            Assert.AreEqual(keyFile, restored.DKIMSecondaryPrivateKeyFile, "The restored domain does not name the key file the backup named.");
            Assert.IsTrue(File.Exists(keyFile), "The restore deleted the key file the rotation had generated for a domain it put back.");
         }
         finally
         {
            if (Directory.Exists(_backupDir))
               Directory.Delete(_backupDir, true);

            // Named by no rotation now - the restore does not carry the rotation's
            // state - so no domain delete will remove it. As far as the key's
            // permissions let the test.
            try
            {
               if (File.Exists(keyFile))
                  File.Delete(keyFile);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
         }
      }

      [Test]
      [Description("A full restore of domains and messages completes when the data directory holds files the service's account may not delete or write - as a new installation's self-signed key is to NT SERVICE\\hMailServer - and leaves them as they are and says so; it keeps this installation's own self-signed pair when the archive's key is another, and the server's own files at the data directory's root; everything else is the archive's. Up to 6.3.5 it stopped at the first such file (HM5700), failed the copy after 30 s (HM5014), and left the mail in the Temp folder's staging copy and no domain on the server.")]
      public void AFullRestoreLeavesInPlaceTheFilesTheServiceMayNotChange()
      {
         _backupMessages = true;

         string data = _application.Settings.Directories.DataDirectory;
         string certificates = Paths.Combine(data, "Certificates");
         string key = Paths.Combine(certificates, "self-signed-key.pem");
         string certificate = Paths.Combine(certificates, "self-signed-certificate.pem");
         string keys = Paths.Combine(data, "restore-keeps-" + TestSetup.UniqueString());
         string imported = Paths.Combine(keys, "imported.key");
         string same = Paths.Combine(keys, "same.key");
         string orphan = Paths.Combine(keys, "orphan.key");
         string note = Paths.Combine(keys, "note.txt");
         string extra = Paths.Combine(keys, "extra.txt");
         string rootFile = Paths.Combine(data, "restore-keeps-" + TestSetup.UniqueString() + ".dat");
         var protectedPaths = new List<string>();

         // The bench's Certificates folder is the NewInstallDefaults fixture's,
         // which removes it again; build/preflight-tests.ps1 names one left behind.
         Assert.IsFalse(Directory.Exists(certificates), certificates + " is there already: another fixture left it, and this test would remove it.");

         try
         {
            var domain = _application.Domains.get_ItemByName("example.test");
            var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "keeps@example.test", "test");
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "Kept one", "The first message.");
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "Kept two", "The second message.");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

            Directory.CreateDirectory(certificates);
            Directory.CreateDirectory(keys);
            File.WriteAllText(key, "this installation's key, as the archive will hold it");
            File.WriteAllText(certificate, "this installation's certificate, as the archive will hold it");
            File.WriteAllText(imported, "an imported key, as the archive will hold it");
            File.WriteAllText(same, "a key the archive holds byte for byte");
            File.WriteAllText(note, "a file the archive will hold");
            File.WriteAllText(rootFile, "a file of the server's own at the data directory's root");

            // What the service's account may do to them: read, and neither delete
            // nor write; and in their folders, not delete a child. As
            // --apply-new-install-defaults leaves a new installation's key for NT
            // SERVICE\hMailServer, here for whatever account the bench's service
            // runs as.
            var service = ServiceAccountSids();
            foreach (string path in new[] { key, imported, same })
            {
               ProtectFromTheService(path, service, false);
               protectedPaths.Add(path);
            }

            foreach (string folder in new[] { certificates, keys })
            {
               ProtectFromTheService(folder, service, true);
               protectedPaths.Add(folder);
            }

            Assert.IsTrue(BackupEnvironment(), "The backup did not complete. Backup log: " + TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile));

            // After the backup: this installation's pair is not the archive's -
            // the archive was taken somewhere else - the imported key changed, a
            // key and a file added that the archive does not hold, a file it holds
            // changed, and a message delivered that it does not hold.
            WriteAsTheTest(key, "this installation's key, not the archive's", service);
            File.WriteAllText(certificate, "this installation's certificate, not the archive's");
            WriteAsTheTest(imported, "an imported key, changed since the backup", service);
            File.WriteAllText(note, "a file the archive holds, changed since the backup");
            File.WriteAllText(extra, "a file the archive does not hold");
            File.WriteAllText(orphan, "a key the archive does not hold");
            ProtectFromTheService(orphan, service, false);
            protectedPaths.Add(orphan);
            SmtpClientSimulator.StaticSend("sender@example.test", account.Address, "After the backup", "Not in the archive.");
            Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 3);

            // Status.StartTime, which says the restore is over, has one-second resolution.
            Thread.Sleep(1100);
            RestoreEnvironment();

            string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            Assert.IsTrue(log.Contains("Restore completed successfully"), "The restore did not complete. Backup log: " + log);

            Pop3ClientSimulator.AssertMessageCount("keeps@example.test", "test", 2);
            Domain restored = _application.Domains.get_ItemByName("example.test");
            var messages = restored.Accounts.get_ItemByAddress("keeps@example.test").IMAPFolders.get_ItemByName("Inbox").Messages;
            var subjects = new List<string>();
            for (int i = 0; i < messages.Count; i++)
               subjects.Add(messages[i].Subject);
            CollectionAssert.AreEquivalent(new[] { "Kept one", "Kept two" }, subjects, "The restored mailbox is not the archive's.");

            Assert.AreEqual("this installation's key, not the archive's", File.ReadAllText(key), "This installation's own key was replaced.");
            Assert.AreEqual("this installation's certificate, not the archive's", File.ReadAllText(certificate),
               "This installation's own certificate was replaced, although its key was kept: the two would not be a pair.");
            StringAssert.Contains("This installation's own self-signed certificate and key", log, "The backup log does not say the pair was left.");

            Assert.AreEqual("an imported key, changed since the backup", File.ReadAllText(imported), "A file the service may not write was changed.");
            Assert.AreEqual("a key the archive holds byte for byte", File.ReadAllText(same), "A file the archive holds byte for byte was changed.");
            Assert.IsTrue(File.Exists(orphan), "A file the service may not delete is gone.");
            Assert.AreEqual("a file the archive will hold", File.ReadAllText(note), "A file the archive holds was not put back.");
            Assert.IsFalse(File.Exists(extra), "A file the archive does not hold was kept.");
            Assert.IsTrue(File.Exists(rootFile), "A file of the server's own at the data directory's root was deleted.");
            StringAssert.Contains("imported.key", log, "The backup log does not name the file left as it is.");
            StringAssert.Contains("orphan.key", log, "The backup log does not name the file it could not remove.");
            foreach (string folder in new[] { certificates, keys, Paths.Combine(data, "example.test") })
               Assert.IsEmpty(Directory.GetFileSystemEntries(folder, "*hmailserver-previous*", SearchOption.AllDirectories), "Something the restore moved aside was left behind in " + folder + ".");

            CustomAsserts.AssertReportedError("HM5705", "imported.key");
         }
         finally
         {
            foreach (string path in protectedPaths)
               GiveBack(path);

            foreach (string folder in new[] { certificates, keys })
            {
               try
               {
                  if (Directory.Exists(folder))
                     Directory.Delete(folder, true);
               }
               catch (IOException)
               {
               }
               catch (UnauthorizedAccessException)
               {
               }
            }

            if (File.Exists(rootFile))
               File.Delete(rootFile);

            if (Directory.Exists(_backupDir))
               Directory.Delete(_backupDir, true);
         }
      }

      // The SIDs the bench's service has in its token that a file's permissions can
      // name: the account it logs on as (HKLM\...\Services\hMailServer ObjectName,
      // LocalSystem by default on a bench) and its own service SID, NT SERVICE\hMailServer.
      private static List<SecurityIdentifier> ServiceAccountSids()
      {
         var sids = new List<SecurityIdentifier>();

         string account = null;
         using (var serviceKey = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\hMailServer"))
            account = serviceKey?.GetValue("ObjectName") as string;

         if (string.IsNullOrEmpty(account) || account.Equals("LocalSystem", StringComparison.OrdinalIgnoreCase))
            sids.Add(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
         else
         {
            if (account.StartsWith(@".\"))
               account = Environment.MachineName + account.Substring(1);
            sids.Add((SecurityIdentifier) new NTAccount(account).Translate(typeof(SecurityIdentifier)));
         }

         try
         {
            var serviceSid = (SecurityIdentifier) new NTAccount(@"NT SERVICE\hMailServer").Translate(typeof(SecurityIdentifier));
            if (!sids.Contains(serviceSid))
               sids.Add(serviceSid);
         }
         catch (IdentityNotMappedException)
         {
            // No service of that name registered: the account alone.
         }

         return sids;
      }

      // A file the service may read and neither delete nor write, or a folder in
      // which it may not delete a child: deny entries for its SIDs first, then
      // full control for SYSTEM, the Administrators and the account this test runs
      // as, inheriting nothing.
      private static void ProtectFromTheService(string path, List<SecurityIdentifier> service, bool folder)
      {
         var self = WindowsIdentity.GetCurrent().User;
         var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
         var administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);

         if (folder)
         {
            var security = new DirectorySecurity();
            security.SetAccessRuleProtection(true, false);
            foreach (var sid in service)
               security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.DeleteSubdirectoriesAndFiles, AccessControlType.Deny));
            foreach (var sid in new[] { system, administrators, self })
               security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            Directory.SetAccessControl(path, security);
         }
         else
         {
            var security = new FileSecurity();
            security.SetAccessRuleProtection(true, false);
            foreach (var sid in service)
               security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.Delete | FileSystemRights.WriteData | FileSystemRights.AppendData, AccessControlType.Deny));
            foreach (var sid in new[] { system, administrators, self })
               security.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, AccessControlType.Allow));
            File.SetAccessControl(path, security);
         }
      }

      // A file protected from the service, written by this test: its deny entries
      // lifted for the write and put back after. Where the tests run as the
      // service's own account - the CI runner and the service both LocalSystem on
      // the Windows runner - those entries deny this test too.
      private static void WriteAsTheTest(string path, string text, List<SecurityIdentifier> service)
      {
         GiveBack(path);
         File.WriteAllText(path, text);
         ProtectFromTheService(path, service, false);
      }

      // The permissions given back to inheritance, so that the folders can be removed.
      private static void GiveBack(string path)
      {
         try
         {
            if (Directory.Exists(path))
            {
               var security = new DirectorySecurity();
               security.SetAccessRuleProtection(false, false);
               Directory.SetAccessControl(path, security);
            }
            else if (File.Exists(path))
            {
               var security = new FileSecurity();
               security.SetAccessRuleProtection(false, false);
               File.SetAccessControl(path, security);
            }
         }
         catch (Exception giveBack) when (!ExceptionPolicy.IsFatal(giveBack))
         {
            Console.WriteLine("The permissions of " + path + " could not be given back: " + giveBack.Message);
         }
      }

      // The DKIM key rotation's members of Domain and Utilities are newer than the
      // type library build-tests.ps1 compiles against until an elevated
      // build/register-com.ps1 has run, so they bind late, as Roles.cs's do; a
      // refusal still arrives as a COMException.
      private static string ComDkimRotationStatus(Domain domain) => (string) ((dynamic) domain).DKIMRotationStatus;

      private static string ComDkimRotationStart(Domain domain, string selector) => (string) ((dynamic) domain).DKIMRotationStart(selector);

      private static string ComDkimRotationCheck(Domain domain) => (string) ((dynamic) domain).DKIMRotationCheck();

      private static string ComDkimRotationSwitch(Domain domain, bool force) => (string) ((dynamic) domain).DKIMRotationSwitch(force);

      private static string ComDkimRotationRetire(Domain domain) => (string) ((dynamic) domain).DKIMRotationRetire();

      private static string ComDkimRotationCancel(Domain domain) => (string) ((dynamic) domain).DKIMRotationCancel();

      private static bool ComDkimRotationEnabled(Domain domain) => (bool) ((dynamic) domain).DKIMRotationEnabled;

      private static void ComSetDkimRotationEnabled(Domain domain, bool value) => ((dynamic) domain).DKIMRotationEnabled = value;

      private static int ComDkimRotationIntervalDays(Domain domain) => (int) ((dynamic) domain).DKIMRotationIntervalDays;

      private static void ComSetDkimRotationIntervalDays(Domain domain, int value) => ((dynamic) domain).DKIMRotationIntervalDays = value;

      private static int ComDkimRotationPropagationHours(Domain domain) => (int) ((dynamic) domain).DKIMRotationPropagationHours;

      private static void ComSetDkimRotationPropagationHours(Domain domain, int value) => ((dynamic) domain).DKIMRotationPropagationHours = value;

      private static int ComDkimRotationRetentionDays(Domain domain) => (int) ((dynamic) domain).DKIMRotationRetentionDays;

      private static void ComSetDkimRotationRetentionDays(Domain domain, int value) => ((dynamic) domain).DKIMRotationRetentionDays = value;

      private static int ComDkimRotationKeyBits(Domain domain) => (int) ((dynamic) domain).DKIMRotationKeyBits;

      private static void ComSetDkimRotationKeyBits(Domain domain, int value) => ((dynamic) domain).DKIMRotationKeyBits = value;
   }
}