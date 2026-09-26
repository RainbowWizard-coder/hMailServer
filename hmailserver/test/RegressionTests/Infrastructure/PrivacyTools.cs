// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    The two operator-facing privacy operations: per-account export
   ///    (Account.ExportMessages) and address erasure
   ///    (Utilities.EraseAddressTraces).
   ///
   ///    Erasure is deliberately a separate operation from deleting the account,
   ///    because the two answer different requests: "close this mailbox" must
   ///    not silently destroy an archive that may be under a legal hold, and
   ///    "remove this person" usually arrives after the mailbox is already gone.
   ///    So the eraser works on an ADDRESS, and these tests never create an
   ///    account for the address they erase.
   /// </summary>
   [TestFixture]
   public class PrivacyTools : TestFixtureBase
   {
      /// <summary>
      ///    APPEND with the literal size derived from the payload rather than
      ///    typed. A wrong count does not fail the test, it HANGS it: the
      ///    server waits for octets that will never arrive while the client
      ///    waits for a response, and the whole fixture wedges. Two of these
      ///    were miscounted by hand while this file was being written, so the
      ///    arithmetic now lives in one place that cannot be wrong.
      /// </summary>
      private static void AppendTo(ImapClientSimulator imap, string tag, string folder, string message)
      {
         imap.SendSingleCommandWithLiteral(
            tag + " APPEND \"" + folder + "\" {" + message.Length + "}", message);
      }

      [Test]
      [Description("Export mirrors the folder tree as .eml files and reports how many - a partial export raises an error instead of a smaller number")]
      public void ExportMirrorsTheFolderTreeAsEmlFiles()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "exportsubject@example.test", "test");

         // Two messages into the inbox the ordinary way.
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "First", "The first exported body.");
         SmtpClientSimulator.StaticSend("sender@dummy-example.com", account.Address, "Second", "The second exported body.");
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 2);

         // And one in a folder the person made themselves, because an export
         // that only handles the inbox is an export of the easy half.
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, "test"));
         Assert.IsTrue(imap.CreateFolder("Projects"));
         AppendTo(imap, "A10", "Projects", "Subject: Filed\r\n\r\nA body filed under Projects for export.\r\n");
         imap.Disconnect();

         string exportDirectory = Paths.Combine(Path.GetTempPath(), "hm-export-" + Guid.NewGuid().ToString("N"));

         try
         {
            int exported = account.ExportMessages(exportDirectory);

            ClassicAssert.AreEqual(3, exported,
               "Two delivered messages plus one appended one should export as exactly three.");

            var files = Directory.GetFiles(exportDirectory, "*.eml", SearchOption.AllDirectories);
            ClassicAssert.AreEqual(3, files.Length,
               "The reported count must be the number of files actually written.");

            // The structure is the point: the filed message sits under a
            // directory named for its folder, not in one flat heap.
            string filed = files.LastOrDefault(file =>
               Directory.GetParent(file).Name.Equals("Projects", StringComparison.OrdinalIgnoreCase));

            Assert.IsNotNull(filed, "The appended message should be under a 'Projects' directory. Files: " +
               string.Join(", ", files));

            StringAssert.Contains("A body filed under Projects for export.", File.ReadAllText(filed),
               "The exported file must be the message itself, byte content included.");
         }
         finally
         {
            if (Directory.Exists(exportDirectory))
               Directory.Delete(exportDirectory, true);
         }
      }


      [Test]
      [Description("Two folders that sanitise to the same directory name each export their own messages - the second must not overwrite the first, and the count must match the files")]
      public void ExportDoesNotLoseMessagesToAFolderNameCollision()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "collision@example.test", "test");

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, "test"));

         // Two sibling folders whose names differ only in a character Windows
         // forbids in a path - both sanitise to "Team_A". IsValidFolderName
         // imposes no character restrictions, so a mailbox owner can genuinely
         // create these; the export must not let one clobber the other. ":" and
         // "|" are not the hierarchy delimiter ("."), so each is a single
         // folder rather than a path.
         Assert.IsTrue(imap.CreateFolder("Team:A"));
         Assert.IsTrue(imap.CreateFolder("Team|A"));

         // A message in each, and their per-folder UIDs will both be 1 - which
         // is exactly what made the naive export overwrite one with the other.
         //
         // The literal octet count is computed, never typed. A hand-counted
         // {n} that is too large makes the server wait for octets the client
         // has already stopped sending, and the fixture hangs rather than
         // failing - which costs far more than the arithmetic saves.
         AppendTo(imap, "A10", "Team:A", "Subject: FromColon\r\n\r\nColon folder body.\r\n");
         AppendTo(imap, "A11", "Team|A", "Subject: FromPipe\r\n\r\nPipe folder body.\r\n");
         imap.Disconnect();

         string exportDirectory = Paths.Combine(Path.GetTempPath(), "hm-collide-" + Guid.NewGuid().ToString("N"));

         try
         {
            int exported = account.ExportMessages(exportDirectory);

            var files = Directory.GetFiles(exportDirectory, "*.eml", SearchOption.AllDirectories);

            // The count the API returns must equal the files actually written -
            // a count of 2 with one file is the silent-loss bug this guards.
            ClassicAssert.AreEqual(exported, files.Length,
               "ExportMessages counted more messages than it wrote - a folder-name collision dropped one. Files: " +
               string.Join(", ", files));

            // And both bodies must survive, in two distinct directories.
            bool colon = false, pipe = false;
            foreach (string text in files.Select(File.ReadAllText))
            {
               if (text.Contains("Colon folder body.")) colon = true;
               if (text.Contains("Pipe folder body.")) pipe = true;
            }

            Assert.IsTrue(colon && pipe,
               "Both colliding folders' messages must survive the export; one was lost to the other's directory.");
         }
         finally
         {
            if (Directory.Exists(exportDirectory))
               Directory.Delete(exportDirectory, true);
         }
      }

      [Test]
      [Description("Erasure removes the traces account deletion leaves: aliases, list memberships, greylisting triplets and quarantine rows - and a second pass finds nothing")]
      public void ErasureRemovesWhatAccountDeletionLeaves()
      {
         const string erased = "erased-person@example.test";

         var target = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "erasetarget@example.test", "test");

         // An alias forwarding the person's mail, and a list they are on.
         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "erased-alias@example.test", erased);
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "erased-list@example.test",
            new List<string> {erased});

         // A greylisting triplet made the real way: greylisting defers the
         // first attempt and records (ip, sender, recipient).
         var antiSpam = _settings.AntiSpam;
         antiSpam.GreyListingEnabled = true;

         try
         {
            CustomAsserts.Throws<DeliveryFailedException>(
               () => SmtpClientSimulator.StaticSend(erased, target.Address, "Greylisted", "Body"));
         }
         finally
         {
            antiSpam.GreyListingEnabled = false;
         }

         // A quarantine row, seeded directly. Its file deliberately does not
         // exist: QuarantineStore.Delete removes the row first and treats a
         // missing file as already gone, so this pins that the eraser survives
         // the store's own broken-entry case.
         SingletonProvider<TestSetup>.Instance.GetApp().Database.ExecuteSQL(
            "insert into hm_quarantine (quarantinefilename, quarantinesender, quarantinerecipients, quarantinesubject, quarantinereason, quarantinescore, quarantinesize, quarantinecreated) " +
            "values ('privacy-test-no-such-file.eml', '" + erased + "', 'somebody@dummy-example.com', 'subject', 'reason', 10, 100, GETDATE())");

         int removed = SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(erased, true);

         // One alias + one list membership + at least one triplet + one
         // quarantine row. ">= 4" rather than "== 4" because greylisting is
         // free to have recorded more than one row for the deferred attempt.
         Assert.GreaterOrEqual(removed, 4,
            "The alias, the list membership, the greylisting triplet and the quarantine row should all be counted.");

         // The functional half: the list genuinely no longer carries the member.
         var list = _domain.DistributionLists.get_ItemByAddress("erased-list@example.test");
         ClassicAssert.AreEqual(0, list.Recipients.Count,
            "The erased address must be off the list, not merely uncounted.");

         // And a second pass finds nothing, which is the cheapest complete
         // proof that the first pass reached everything it counts.
         int second = SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(erased, true);
         ClassicAssert.AreEqual(0, second,
            "A second erasure of the same address must find nothing left to remove.");
      }

      [Test]
      [Description("Erasure finds an address whatever case it was written in: an alias and a list member typed in capitals go when the address is erased in lower case - on PostgreSQL too, which compares text by its bytes")]
      public void ErasureFindsTheAddressInAnyCase()
      {
         const string written = "Erased-Case@Example.Test";
         const string erased = "erased-case@example.test";

         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "case-alias@example.test", written);
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "case-list@example.test",
            new List<string> {written});

         int removed = SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(erased, false);
         Assert.GreaterOrEqual(removed, 2, "The alias and the list membership, written in capitals, are both counted.");

         _domain.Aliases.Refresh();
         for (int i = 0; i < _domain.Aliases.Count; i++)
            ClassicAssert.AreNotEqual("case-alias@example.test", _domain.Aliases[i].Name.ToLowerInvariant(), "The alias forwarding to the erased address is gone.");

         var list = _domain.DistributionLists.get_ItemByAddress("case-list@example.test");
         ClassicAssert.AreEqual(0, list.Recipients.Count, "The member written in capitals is off the list.");

         ClassicAssert.AreEqual(0, SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(erased, false),
            "A second erasure finds nothing: the count and the delete asked the same question.");
      }

      [Test]
      [Description("Erasure finds the address in whatever case each store kept it: an alias, a list member and a greylisting sender written in mixed case go with an erasure written in lower case")]
      public void ErasureMatchesTheAddressInAnyCase()
      {
         // Until 26 September 2026 the message trace, the greylisting triplets,
         // the list members and the aliases were matched with "=" on the operator's
         // spelling. PostgreSQL compares varchar case by case, so there these rows
         // were left behind while the erasure reported itself done; SQL Server,
         // Compact and MySQL matched them under their collations. On PostgreSQL
         // this test failed against that code (the list kept its member).
         const string written = "Erased-Case@Example.Test";
         const string erased = "erased-case@example.test";

         var target = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "erasecasetarget@example.test", "test");

         SingletonProvider<TestSetup>.Instance.AddAlias(_domain, "erased-case-alias@example.test", written);
         SingletonProvider<TestSetup>.Instance.AddDistributionList(_domain, "erased-case-list@example.test",
            new List<string> {written});

         // The sender as the client wrote it in MAIL FROM, which is how the
         // triplet keeps it.
         var antiSpam = _settings.AntiSpam;
         antiSpam.GreyListingEnabled = true;

         try
         {
            CustomAsserts.Throws<DeliveryFailedException>(
               () => SmtpClientSimulator.StaticSend(written, target.Address, "Greylisted", "Body"));
         }
         finally
         {
            antiSpam.GreyListingEnabled = false;
         }

         int removed = SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(erased, true);

         Assert.GreaterOrEqual(removed, 3,
            "The alias, the list membership and the greylisting triplet, each kept in mixed case, should all be counted.");

         var list = _domain.DistributionLists.get_ItemByAddress("erased-case-list@example.test");
         ClassicAssert.AreEqual(0, list.Recipients.Count,
            "The member written in mixed case must be off the list.");

         // Nothing is left in either spelling.
         ClassicAssert.AreEqual(0, SingletonProvider<TestSetup>.Instance.GetApp().Utilities.EraseAddressTraces(written, true),
            "An erasure of the spelling the stores kept must find nothing left.");
      }
   }
}
