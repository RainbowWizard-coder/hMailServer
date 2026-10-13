// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    Quarantine.Search over COM (review finding F261): the Control Panel's search
   ///    looked only through the page Refresh loads - the newest five hundred - so a
   ///    held message older than that could not be found from the page at all.
   ///
   ///    The rows are written straight into hm_quarantine, as the REST fixture of the
   ///    quarantine's routes writes its own: more of them than one page holds, and
   ///    values no SMTP conversation would give - a recipient list with an
   ///    underscore in it, a reason with a bracket - which is what the LIKE has to
   ///    get right. No file stands behind any of them; nothing here releases one.
   ///    The time each was held is counted back from now in seconds, through
   ///    DATEADD and GETDATE, which SQL Server and Compact both take: the two
   ///    databases a Windows bench runs.
   /// </summary>
   [TestFixture]
   public class QuarantineSearch : TestFixtureBase
   {
      private const string FilePrefix = "qsearch-com-test-";

      [SetUp]
      public void EmptyTheQuarantineFirst()
      {
         RemoveSeededRows();
         EmptyTheQuarantine();
      }

      [TearDown]
      public void RemoveTheRowsWritten()
      {
         RemoveSeededRows();
      }

      private void RemoveSeededRows()
      {
         _application.Database.ExecuteSQL("delete from hm_quarantine where quarantinefilename like '" + FilePrefix + "%'");
      }

      private void EmptyTheQuarantine()
      {
         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();

         while (quarantine.Count > 0)
         {
            quarantine.DeleteByDBID(quarantine[0].ID);
            quarantine.Refresh();
         }
      }

      // One row, held secondsAgo seconds ago. The values are written into the
      // statement with their quotes doubled; none of the tests' values holds
      // anything else a literal would need.
      private int Seed(int secondsAgo, string sender, string recipients, string subject, string reason)
      {
         string Literal(string value) => "'" + value.Replace("'", "''") + "'";

         return _application.Database.ExecuteSQLWithReturn(
            "insert into hm_quarantine (quarantinefilename, quarantinesender, quarantinerecipients, quarantinesubject, quarantinereason, quarantinescore, quarantinesize, quarantinecreated) values (" +
            Literal(FilePrefix + Guid.NewGuid().ToString("N") + ".eml") + ", " + Literal(sender) + ", " + Literal(recipients) + ", " +
            Literal(subject) + ", " + Literal(reason) + ", 12, 345, DATEADD(second, -" + secondsAgo + ", GETDATE()))");
      }

      // hMailServer's Quarantine, named whole: in this namespace the bare name is the
      // Quarantine fixture.
      private static List<string> Subjects(hMailServer.Quarantine quarantine)
      {
         var subjects = new List<string>();
         for (int i = 0; ; i++)
         {
            hMailServer.QuarantinedMessage message;
            try
            {
               message = quarantine[i];
            }
            catch (COMException)
            {
               // The collection holds a page, and indexing past it is how the page ends.
               break;
            }

            subjects.Add(message.Subject);
         }

         return subjects;
      }

      [Test]
      [Description("Search finds a held message older than the page Refresh loads, which the Control Panel's search could not reach")]
      public void SearchFindsWhatRefreshDoesNotLoad()
      {
         // Three old messages, then a page and a few more of newer ones above them.
         Seed(4000, "old@spam-sender.test", "ada@example.test", "Old invoice 1", "Score 21");
         Seed(3990, "old@spam-sender.test", "ada@example.test", "Old invoice 2", "Score 21");
         Seed(3980, "old@spam-sender.test", "ada@example.test", "Old invoice 3", "Score 21");

         for (int i = 0; i < 505; i++)
            Seed(3000 - i, "filler@spam-sender.test", "ada@example.test", "Filler " + i, "Score 21");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         quarantine.Refresh();
         Assert.AreEqual(508, quarantine.Count, "Count is every message held.");

         List<string> page = Subjects(quarantine);
         Assert.AreEqual(500, page.Count, "Refresh loads the newest five hundred.");
         Assert.IsFalse(page.Exists(s => s.StartsWith("Old invoice", StringComparison.Ordinal)), "The old messages are past the page Refresh loads.");

         int matches = quarantine.Search("old invoice", 0, 500);
         Assert.AreEqual(3, matches);
         Assert.AreEqual(new[] { "Old invoice 3", "Old invoice 2", "Old invoice 1" }, Subjects(quarantine).ToArray(), "Newest first.");

         // And the item a page of a search holds is the message: its id finds it.
         hMailServer.QuarantinedMessage found = quarantine[0];
         Assert.AreEqual("Old invoice 3", quarantine.get_ItemByDBID(found.ID).Subject);

         // An empty text is every message: the page after the one Refresh loads.
         Assert.AreEqual(508, quarantine.Search("", 500, 500));
         Assert.AreEqual(new[] { "Filler 4", "Filler 3", "Filler 2", "Filler 1", "Filler 0", "Old invoice 3", "Old invoice 2", "Old invoice 1" },
            Subjects(quarantine).ToArray());

         // Refresh gives the newest page back.
         quarantine.Refresh();
         Assert.AreEqual("Filler 504", quarantine[0].Subject);
      }

      [Test]
      [Description("Search looks in the sender, the recipients, the subject and the reason, in any case")]
      public void SearchLooksInTheSenderTheRecipientsTheSubjectAndTheReason()
      {
         Seed(50, "Billing@Supplier.example", "ada@example.test", "Monthly statement", "Score 25: SPF fail");
         Seed(40, "news@letters.example", "ops_team@example.test,ada@example.test", "Weekly digest", "Score 30: DNSBL zen.spamhaus.org");
         Seed(30, "news@letters.example", "ada@example.test", "Your parcel", "Rule: Parcels [held]");

         var quarantine = _application.Settings.AntiSpam.Quarantine;

         Assert.AreEqual(1, quarantine.Search("billing@SUPPLIER", 0, 10));
         Assert.AreEqual(new[] { "Monthly statement" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(1, quarantine.Search("OPS_TEAM@", 0, 10));
         Assert.AreEqual(new[] { "Weekly digest" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(1, quarantine.Search("PARCEL", 0, 10));
         Assert.AreEqual(new[] { "Your parcel" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(1, quarantine.Search("spamhaus", 0, 10));
         Assert.AreEqual(new[] { "Weekly digest" }, Subjects(quarantine).ToArray());

         // Text from more than one column is no match: each column is searched on its own.
         Assert.AreEqual(0, quarantine.Search("statement Score", 0, 10));
         Assert.AreEqual(0, Subjects(quarantine).Count);

         // Spaces round the text are not part of it.
         Assert.AreEqual(2, quarantine.Search("  news@letters  ", 0, 10));
      }

      [Test]
      [Description("Search matches its text as it is written: %, _, [ and the escape character are the characters they are")]
      public void SearchMatchesTheTextAsItIsWritten()
      {
         Seed(60, "promo@spam-sender.test", "ada@example.test", "100% off", "Score 21");
         Seed(50, "promo@spam-sender.test", "ada@example.test", "100 percent off", "Score 21");
         Seed(40, "a_b@spam-sender.test", "ada@example.test", "From a_b", "Score 21");
         Seed(30, "axb@spam-sender.test", "ada@example.test", "From axb", "Score 21");
         Seed(20, "promo@spam-sender.test", "ada@example.test", "p", "Rule: Promotions [promo]");
         Seed(10, "promo@spam-sender.test", "ada@example.test", "Slash / and percent /% together", "Score 21");

         var quarantine = _application.Settings.AntiSpam.Quarantine;

         Assert.AreEqual(1, quarantine.Search("100%", 0, 10), "% found every subject with 100 in it.");
         Assert.AreEqual(1, quarantine.Search("a_b@", 0, 10), "_ found axb@ too.");
         Assert.AreEqual(new[] { "From a_b" }, Subjects(quarantine).ToArray());

         // A bracket opens a character class on SQL Server and Compact: [promo] would
         // find every message with a p, r, o or m in its sender.
         Assert.AreEqual(1, quarantine.Search("[promo]", 0, 10));
         Assert.AreEqual(new[] { "p" }, Subjects(quarantine).ToArray());

         // The escape character itself, beside a wildcard.
         Assert.AreEqual(1, quarantine.Search("/%", 0, 10));
         Assert.AreEqual(new[] { "Slash / and percent /% together" }, Subjects(quarantine).ToArray());
      }

      [Test]
      [Description("Search pages newest first, and its answer is every match however the page is asked for")]
      public void SearchPagesNewestFirst()
      {
         for (int i = 1; i <= 5; i++)
            Seed(100 - i, "pages@spam-sender.test", "ada@example.test", "Page " + i, "Score 21");

         // Two held in the same second come in the order of their ids, newest first.
         Seed(10, "pages@spam-sender.test", "ada@example.test", "Page 6", "Score 21");
         Seed(10, "pages@spam-sender.test", "ada@example.test", "Page 7", "Score 21");

         var quarantine = _application.Settings.AntiSpam.Quarantine;

         Assert.AreEqual(7, quarantine.Search("page", 0, 3));
         Assert.AreEqual(new[] { "Page 7", "Page 6", "Page 5" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(7, quarantine.Search("page", 3, 3));
         Assert.AreEqual(new[] { "Page 4", "Page 3", "Page 2" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(7, quarantine.Search("page", 6, 3));
         Assert.AreEqual(new[] { "Page 1" }, Subjects(quarantine).ToArray());

         Assert.AreEqual(7, quarantine.Search("page", 7, 3));
         Assert.AreEqual(0, Subjects(quarantine).Count);
      }

      [Test]
      [Description("Search refuses a text over 255 characters, a Count outside 1 to 500 and an Offset outside 0 to 999999999, and loads nothing")]
      public void SearchRefusesWhatIsOutOfBounds()
      {
         Seed(10, "bounds@spam-sender.test", "ada@example.test", "Bounds", "Score 21");

         var quarantine = _application.Settings.AntiSpam.Quarantine;
         Assert.AreEqual(1, quarantine.Search("bounds", 0, 500));
         Assert.AreEqual(0, quarantine.Search(new string('x', 255), 0, 1), "255 characters is a search.");

         var refused = new (string text, int offset, int count)[]
         {
            (new string('x', 256), 0, 1),
            ("bounds", 0, 0),
            ("bounds", 0, 501),
            ("bounds", -1, 1),
            ("bounds", 1000000000, 1)
         };

         foreach ((string text, int offset, int count) in refused)
         {
            Assert.AreEqual(1, quarantine.Search("bounds", 0, 1), "The page before the refusal.");
            var error = Assert.Throws<COMException>(() => quarantine.Search(text, offset, count),
               "Search(" + text.Length + " characters, " + offset + ", " + count + ") was not refused.");
            StringAssert.Contains(text.Length > 255 ? "255 characters" : offset != 0 ? "Offset" : "Count", error.Message);
         }

         // Nothing of the quarantine, its search with it, without the server
         // administrator's credential.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "qsearch-user@example.test", "test");
         var user = new hMailServer.Application();
         Assert.IsNotNull(user.Authenticate(account.Address, "test"));
         Assert.Throws<COMException>(() => user.Settings.AntiSpam.Quarantine.Search("bounds", 0, 1));
      }
   }
}
