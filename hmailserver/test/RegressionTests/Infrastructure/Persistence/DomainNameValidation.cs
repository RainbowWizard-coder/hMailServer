// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.IO;
using System.Runtime.InteropServices;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure.Persistence
{
   [TestFixture]
   public class DomainNameValidation : TestFixtureBase
   {
      [Test]
      public void TestDomainNameContainingSpace()
      {
         AssertInvalidDomainName("example 1.com");
      }

      [Test]
      public void TestDomainNameContainigSlash()
      {
         AssertInvalidDomainName("example\\1.com");
      }

      [Test]
      public void TestDomainWithoutName()
      {
         AssertInvalidDomainName("");
      }

      [Test]
      public void TestSimpleDomain()
      {
         AssertValidDomainName("example.com");
      }

      [Test]
      public void TestDomainWithDot()
      {
         AssertValidDomainName("examp.co.uk");
      }

      [Test]
      public void TestDomainWithIPv4Literal()
      {
         AssertValidDomainName("[192.168.1.1]");
      }

      [Test]
      public void TestDomainWithIPv6Literal()
      {
         AssertValidDomainName("[IPv6:2001:0db8:85a3:0000:0000:8a2e:0370:7334]");
      }

      [Test]
      public void TestDomainWithInvalidIPv6Literal()
      {
         AssertInvalidDomainName("[IPv6:invalid]");
      }

      // The directories the server keeps its own files in directly under the data
      // directory (PreSaveLimitationsCheck.cpp), in the cases an administrator might
      // type them. A domain's mail is kept in the directory named after it in the
      // same place, so a domain of one of these names shares the store's directory.
      private static readonly string[] ReservedNames =
      {
         "protected", "Confidential", "RECOVERABLE", "discovery", "quarantine", "files", "lists", "blobs",
         "sieve", "dkim", "acme", "updates", "certificates", "temp", "events", "database"
      };

      [Test]
      [Description("A new domain named as a directory the server keeps its own files in under the data directory is refused over COM, every one of them and in any case; the old check took each for a valid single-label name.")]
      public void TestANewDomainCannotHaveTheNameOfAStoreDirectory()
      {
         foreach (string name in ReservedNames)
         {
            var domain = _application.Domains.Add();
            domain.Name = name;

            try
            {
               var exception = Assert.Throws<COMException>(() => domain.Save(), name + " was saved as a domain.");
               StringAssert.Contains("The name " + name + " is reserved", exception.Message);
            }
            finally
            {
               // A domain the old check let through, removed again: its directory
               // is the store's, which Delete leaves while files are in it.
               if (domain.ID != 0)
                  domain.Delete();
            }
         }

         // A name with a dot is none of them, whatever its first label.
         AssertValidDomainName("protected.example.test");
      }

      [Test]
      [Description("An existing domain cannot be renamed to the name of a store directory either.")]
      public void TestADomainCannotBeRenamedToTheNameOfAStoreDirectory()
      {
         var domain = _application.Domains.Add();
         domain.Name = "reserved-rename.test";
         domain.Save();

         domain.Name = "Protected";
         var exception = Assert.Throws<COMException>(() => domain.Save());
         StringAssert.Contains("is reserved", exception.Message);

         var stored = _application.Domains.get_ItemByDBID(domain.ID);
         Assert.AreEqual("reserved-rename.test", stored.Name, "The refused rename changed nothing.");
      }

      [Test]
      [Description("A domain that already has the name of a store directory - made before such names were refused - can still be saved, is reported in the error log at start and on the first-hour checklist, and is renamed without taking the store's files with it: only its mailboxes move. The old rename copied the whole directory, the store's files included, and deleted it.")]
      public void TestAnExistingDomainWithTheNameOfAStoreDirectoryIsReportedAndRenamedAway()
      {
         string dataDirectory = _settings.Directories.DataDirectory;
         string reservedDirectory = Path.Combine(dataDirectory, "protected");
         bool reservedDirectoryExisted = Directory.Exists(reservedDirectory);
         string sentinelDirectory = Path.Combine(reservedDirectory, "reserved-probe-sentinel");
         string sentinel = Path.Combine(sentinelDirectory, "keep.txt");
         string mailboxDirectory = Path.Combine(reservedDirectory, "probe");
         string renamedMessage = Path.Combine(dataDirectory, "reserved-renamed.test", "probe", "AB", "{reserved-probe}.eml");

         var domain = _application.Domains.Add();
         domain.Name = "reserved-existing.test";
         domain.Active = true;
         domain.Save();
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(domain, "probe@reserved-existing.test", "test");

         try
         {
            // The name as a server before this check could have saved it.
            _application.Database.ExecuteSQL("update hm_domains set domainname = 'protected' where domainid = " + domain.ID);
            _application.Database.ExecuteSQL("update hm_accounts set accountaddress = 'probe@protected' where accountid = " + account.ID);
            _settings.Cache.Clear();

            // Still saved under its own name, so that it can be managed and renamed away.
            var existing = _application.Domains.get_ItemByName("protected");
            existing.Active = false;
            existing.Save();
            existing.Active = true;
            existing.Save();

            // The checklist says so.
            string checklist = _application.Utilities.GetSetupChecklist(false);
            StringAssert.Contains("\"code\":\"domain_name_reserved\",\"args\":[\"protected\"", checklist);

            // And the error log, at start.
            LogHandler.DeleteErrorLog();
            _application.Reinitialize();
            string errors = LogHandler.ReadAndDeleteErrorLog();
            StringAssert.Contains("7597", errors);
            StringAssert.Contains("The domain protected has the name the server reserves for its own directory", errors);
            Assert.IsTrue(LogHandler.ClearErrorLogUntilSettled(), "Nothing else is reported.");

            // A file of the store's, and a message in the domain's mailbox, both
            // in the one directory.
            Directory.CreateDirectory(sentinelDirectory);
            File.WriteAllText(sentinel, "the store's own file");
            Directory.CreateDirectory(Path.Combine(mailboxDirectory, "AB"));
            File.WriteAllText(Path.Combine(mailboxDirectory, "AB", "{reserved-probe}.eml"), "Subject: probe\r\n\r\nprobe\r\n");

            existing = _application.Domains.get_ItemByName("protected");
            existing.Name = "reserved-renamed.test";
            existing.Save();

            Assert.IsTrue(File.Exists(renamedMessage), "The mailbox moved with the domain.");
            Assert.IsFalse(Directory.Exists(mailboxDirectory), "The mailbox is no longer under the store's directory.");
            Assert.IsTrue(File.Exists(sentinel), "The store's file stayed where the store keeps it; the old rename carried it into the renamed domain.");
            Assert.IsNotNull(_application.Domains.get_ItemByName("reserved-renamed.test").Accounts.get_ItemByAddress("probe@reserved-renamed.test"),
               "The account followed the domain.");

            string after = _application.Utilities.GetSetupChecklist(false);
            StringAssert.DoesNotContain("domain_name_reserved", after, "The renamed domain is no longer reported.");
         }
         finally
         {
            _application.Database.ExecuteSQL("update hm_accounts set accountaddress = 'probe@reserved-existing.test' where accountaddress = 'probe@protected'");
            _application.Database.ExecuteSQL("update hm_domains set domainname = 'reserved-existing.test' where domainname = 'protected'");
            _settings.Cache.Clear();

            if (Directory.Exists(sentinelDirectory))
               Directory.Delete(sentinelDirectory, true);
            if (Directory.Exists(mailboxDirectory))
               Directory.Delete(mailboxDirectory, true);
            if (!reservedDirectoryExisted && Directory.Exists(reservedDirectory) && Directory.GetFileSystemEntries(reservedDirectory).Length == 0)
               Directory.Delete(reservedDirectory);

            // The renamed domain's mailbox, which a domain delete leaves while a file is in it.
            string renamedDirectory = Path.Combine(dataDirectory, "reserved-renamed.test");
            if (Directory.Exists(renamedDirectory))
               Directory.Delete(renamedDirectory, true);

            LogHandler.ClearErrorLogUntilSettled();
         }
      }

      private void AssertInvalidDomainName(string domainName)
      {
         var domain = _application.Domains.Add();
         domain.Name = domainName;

         var exception = Assert.Throws<COMException>(() => domain.Save());
         Assert.AreEqual("Failed to save object. The domain name you have entered is not a valid domain name.",
            exception.Message);
      }

      private void AssertValidDomainName(string domainName)
      {
         var domain = _application.Domains.Add();
         domain.Name = domainName;
         domain.Save();
      }
   }
}