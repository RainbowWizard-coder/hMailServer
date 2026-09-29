// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using DBUpdater;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    eDiscovery (docs/Discovery.md): a case, a search across the custodians' mailboxes
   ///    and the archive, a hold, an export - each claim beside its negative control. The
   ///    messages are made for each test with a word no other test or run uses, so a count
   ///    is this test's own; the archive index outlives its accounts, so what it held
   ///    before is read and never counted.
   ///
   ///    Every case this fixture opens is closed in its teardown, which releases every hold
   ///    the case placed: a hold left standing would keep the next test's setup from
   ///    deleting the test domain. The teardown first waits for every search and export
   ///    the test queued, so that none is cut off by the reinitialise; then it removes
   ///    what the test's cases left - their rows and their files under the data
   ///    directory - and the archive index rows of the fixture's sender, whose files it
   ///    deleted. Keys and roles are made per test and removed with it.
   /// </summary>
   [TestFixture]
   public class Discovery : TestFixtureBase
   {
      // The ports this fixture may use are 10060 to 10069; the REST listener is the first.
      private static int RestPort = 10060;
      private const string AdminPassword = "testar";
      private const string Password = "Discovery-Passw0rd!";
      private const string Sender = "partner@discovery-outside.test";
      private const string ZeroAccessSalt = "AAECAwQFBgcICQoLDA0ODw==";

      private string archiveRoot_;
      private long firstCase_ = -1;
      private readonly List<string> _keys = new List<string>();
      private readonly List<string> _roles = new List<string>();

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         ClassicAssert.AreEqual(200, Admin("GET", "/api/v1/status", null).Status, "REST API did not answer /api/v1/status.");

         // A run that died left nothing standing for this one.
         CloseEveryOpenCase();

         // Every case with a higher id is this test's, and goes in the teardown.
         firstCase_ = NewestCaseId();
      }

      [TearDown]
      public void CloseEveryCaseAndStopRestApi()
      {
         try
         {
            // Nothing queued by the test is cut off by the reinitialise below.
            Tidy(WaitForTheRunner);
            Tidy(CloseEveryOpenCase);

            foreach (string id in _keys)
               Tidy(() => Admin("DELETE", "/api/v1/apikeys/" + id, null));
            _keys.Clear();

            foreach (string role in _roles)
               Tidy(() => Admin("DELETE", "/api/v1/roles/" + role, null));
            _roles.Clear();

            Tidy(RemoveThisTestsCases);
         }
         finally
         {
            if (archiveRoot_ != null)
            {
               IniFileSetting.Write("ArchiveDir", "");
               Tidy(() => Directory.Delete(archiveRoot_, true));

               // The index rows of the copies whose files went with the directory:
               // every later search of the archive would list them as unsearched.
               Tidy(() => _application.Database.ExecuteSQL("delete from hm_archiveindex where archivesender = '" + Sender + "'"));
               archiveRoot_ = null;
            }

            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      // ---------------------------------------------------------------- the search

      [Test]
      [Description("A search by custodian, date and words over two accounts finds exactly the messages made to match - one whose word is only in the body of a message over 1 MB among them - and not one sent before the date range, one without the word, nor one of an account that is not a custodian. With scope both the archived copy is a hit as well; with mailboxes it is not. IMAP SEARCH TEXT finds the same messages of the same mailbox by the same word - two of its three, by their Message-IDs. The audit trail carries the query. A hold on the archived copies the search matched holds that copy by a hold of the case, not by the archive's own flag: the custodian's address erased with the archive leaves the held copy's row and file, and takes the unheld copy's, and a search run again says the copy is held.")]
      public void ASearchFindsExactlyTheMatchingMessagesAndTheAuditTrailCarriesItsQuery()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);
         Account carol = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "carol@" + _domain.Name, Password);

         string now = DateTime.UtcNow.ToString("r");

         // Matching: alice, today, the word in the body.
         string m1 = Id("m1");
         Deliver(alice.Address, Raw(alice.Address, "Budget", m1, now, "The figures for " + word + " are attached below."));

         // Not matching: before the range; without the word; not a custodian.
         Deliver(bob.Address, Raw(bob.Address, "Old news", Id("m2"), "Tue, 01 Jan 2019 10:00:00 +0000", "An old mention of " + word + "."));
         Deliver(bob.Address, Raw(bob.Address, "Lunch", Id("m3"), now, "Nothing to see here."));
         Deliver(carol.Address, Raw(carol.Address, "Budget too", Id("m4"), now, "Carol also writes " + word + "."));

         // Matching: bob, a message over 1 MB whose only mention of the word is at the end of its body.
         var big = new StringBuilder();
         for (int line = 0; line < 16000; line++)
            big.Append("Filler line ").Append(line.ToString("00000")).Append(" of a large message that says nothing in particular.\r\n");
         big.Append("The last line names " + word + ".");
         string m5 = Id("m5");
         Deliver(bob.Address, Raw(bob.Address, "Large", m5, now, big.ToString()));

         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // Two more to alice with the archive on, so that both are archived as they
         // are received: one with the word, and one without it - IMAP SEARCH's
         // control, and the erasure's.
         EnableArchive();
         string m6 = Id("m6");
         Deliver(alice.Address, Raw(alice.Address, "Budget, archived", m6, now, "Archived mention of " + word + "."));
         string m7 = Id("m7");
         Deliver(alice.Address, Raw(alice.Address, "Lunch, archived", m7, now, "Nothing about the figures here."));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 3);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 3);
         ImapClientSimulator.AssertMessageCount(carol.Address, Password, "INBOX", 1);

         long caseId = OpenCase("Budget inquiry " + word);
         string from = DateTime.UtcNow.AddDays(-1).ToString("yyyy-MM-dd");
         string to = DateTime.UtcNow.AddDays(1).ToString("yyyy-MM-dd");
         string custodians = "[\"" + alice.Address + "\",\"" + bob.Address + "\"]";

         JsonElement mailboxes = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"from\":\"" + from + "\",\"to\":\"" + to + "\",\"words\":\"" + word + "\",\"scope\":\"mailboxes\"}");
         CollectionAssert.AreEquivalent(new[] { "mailbox:" + m1, "mailbox:" + m5, "mailbox:" + m6 }, Hits(mailboxes),
            "The hits are exactly the three messages made to match, in the custodians' mailboxes.");

         JsonElement both = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"from\":\"" + from + "\",\"to\":\"" + to + "\",\"words\":\"" + word + "\",\"scope\":\"both\"}");
         CollectionAssert.AreEquivalent(new[] { "mailbox:" + m1, "mailbox:" + m5, "mailbox:" + m6, "archive:" + m6 }, Hits(both),
            "With scope both the archived copy is a hit as well.");

         JsonElement archived = HitsOf(both).First(h => h.GetProperty("kind").GetString() == "archive");
         ClassicAssert.AreEqual(alice.Address, archived.GetProperty("custodian").GetString());
         JsonElement large = HitsOf(both).First(h => h.GetProperty("message_id").GetString() == "<" + m5 + ">");
         ClassicAssert.Greater(large.GetProperty("size").GetInt64(), 1024L * 1024L, "The message whose word was found is over 1 MB.");

         // The same word through IMAP SEARCH TEXT, in the same mailbox of three:
         // the same two messages, by their Message-IDs, and not the third.
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(alice.Address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         string[] found = imap.Search("TEXT " + word).Trim().Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
         var imapIds = new List<string>();
         foreach (string number in found)
         {
            string fetched = imap.Fetch(number + " BODY.PEEK[HEADER.FIELDS (MESSAGE-ID)]");
            Match messageId = Regex.Match(fetched, "<([^>]+)>");
            ClassicAssert.IsTrue(messageId.Success, "FETCH answered no Message-ID: " + fetched);
            imapIds.Add(messageId.Groups[1].Value);
         }
         imap.Disconnect();

         ClassicAssert.AreEqual(2, found.Length, "IMAP SEARCH TEXT finds two of alice's three messages: " + string.Join(" ", found));
         CollectionAssert.AreEquivalent(
            HitsOf(mailboxes).Where(h => h.GetProperty("custodian").GetString() == alice.Address).Select(h => h.GetProperty("message_id").GetString().Trim('<', '>')).ToList(),
            imapIds, "IMAP SEARCH TEXT and the discovery search find the same messages by the same word.");
         CollectionAssert.DoesNotContain(imapIds, m7, "The message without the word is found by neither.");

         // Every search logged: the audit trail's row for the search carries the query.
         Reply trail = Admin("GET", "/api/v1/audit?object_type=" + Uri.EscapeDataString("discovery search") + "&limit=50", null);
         ClassicAssert.AreEqual(200, trail.Status, trail.Body);
         var details = trail.Json.GetProperty("entries").EnumerateArray().Select(e => e.GetProperty("detail").GetString()).ToList();
         ClassicAssert.IsTrue(details.Any(d => d.Contains(word) && d.Contains("\"scope\":\"mailboxes\"") && d.Contains(bob.Address)),
            "The audit trail's row for the search carries the whole query: " + string.Join(" | ", details));

         // The archived copy the search did not match, found by a search of the
         // archive alone: the erasure's negative control below.
         JsonElement archiveOnly = RunSearch(caseId, "{\"custodians\":[\"" + alice.Address + "\"],\"scope\":\"archive\"}");
         JsonElement unheldCopy = HitsOf(archiveOnly).Single(h => h.GetProperty("message_id").GetString() == "<" + m7 + ">");
         long heldId = archived.GetProperty("id").GetInt64();
         long unheldId = unheldCopy.GetProperty("id").GetInt64();

         // The archived copy the search matched, put on hold by the case - and not by
         // the archive's own flag, which somebody else could clear.
         long searchId = both.GetProperty("id").GetInt64();
         Reply held = Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"archive\",\"search\":" + searchId + "}");
         ClassicAssert.AreEqual(201, held.Status, held.Body);
         ClassicAssert.AreEqual(1, held.Json.GetProperty("placed").GetInt32());
         Reply entry = Admin("GET", "/api/v1/archive/" + heldId, null);
         ClassicAssert.AreEqual(200, entry.Status, entry.Body);
         ClassicAssert.IsFalse(entry.Json.GetProperty("hold").GetBoolean(), "The archive's own flag is not what holds the copy.");
         string heldPath = entry.Json.GetProperty("path").GetString();

         Reply unheldEntry = Admin("GET", "/api/v1/archive/" + unheldId, null);
         ClassicAssert.AreEqual(200, unheldEntry.Status, unheldEntry.Body);
         string unheldPath = unheldEntry.Json.GetProperty("path").GetString();
         ClassicAssert.IsTrue(File.Exists(heldPath), heldPath);
         ClassicAssert.IsTrue(File.Exists(unheldPath), unheldPath);

         // A second hold of the same copies places none.
         Reply again = Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"archive\",\"search\":" + searchId + "}");
         ClassicAssert.AreEqual(200, again.Status, again.Body);
         ClassicAssert.AreEqual(0, again.Json.GetProperty("placed").GetInt32());

         // The hold holds: alice's address erased with the archive, as a person's
         // erasure asks, leaves the held copy's row and file and takes the other's.
         Reply erased = Admin("POST", "/api/v1/addresses/erase", "{\"address\":\"" + alice.Address + "\",\"include_archive\":true}");
         ClassicAssert.AreEqual(200, erased.Status, erased.Body);
         ClassicAssert.AreEqual(200, Admin("GET", "/api/v1/archive/" + heldId, null).Status, "The held copy's index row is kept.");
         ClassicAssert.IsTrue(File.Exists(heldPath), "The held copy's file is kept: " + heldPath);
         ClassicAssert.AreEqual(404, Admin("GET", "/api/v1/archive/" + unheldId, null).Status, "The control: the unheld copy's index row is erased.");
         ClassicAssert.IsFalse(File.Exists(unheldPath), "The control: the unheld copy's file is erased: " + unheldPath);

         // And a search run again says the copy is held.
         JsonElement rerun = RunSearch(caseId, "{\"custodians\":[\"" + alice.Address + "\"],\"words\":\"" + word + "\",\"scope\":\"archive\"}");
         ClassicAssert.IsTrue(HitsOf(rerun).Single(h => h.GetProperty("id").GetInt64() == heldId).GetProperty("held").GetBoolean(),
            "The held copy's hit says it is held.");
      }

      [Test]
      [Description("An attachment criterion matches only the message that carries such an attachment: a PDF named for a report and not a text file, nor a message with none; asked for the text file, the text file's message alone; asked for an image, none.")]
      public void AnAttachmentCriterionMatchesOnlyTheMessageThatCarriesIt()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         string now = DateTime.UtcNow.ToString("r");

         string pdf = Id("pdf");
         string text = Id("txt");
         string none = Id("none");
         Deliver(alice.Address, WithAttachment(alice.Address, "Report " + word, pdf, now, "quarterly-report.pdf", "application/pdf", Encoding.ASCII.GetBytes("%PDF-1.4 not really a PDF " + word)));
         Deliver(alice.Address, WithAttachment(alice.Address, "Notes " + word, text, now, "notes.txt", "text/plain", Encoding.ASCII.GetBytes("Meeting notes " + word)));
         Deliver(alice.Address, Raw(alice.Address, "Plain " + word, none, now, "No attachment at all: " + word));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 3);

         long caseId = OpenCase("Attachments " + word);
         string custodians = "[\"" + alice.Address + "\"]";

         JsonElement report = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"words\":\"" + word + "\",\"attachment\":{\"name\":\"report\",\"type\":\"application/pdf\"}}");
         CollectionAssert.AreEquivalent(new[] { "mailbox:" + pdf }, Hits(report), "Only the message carrying the PDF named for a report.");
         JsonElement pdfHit = HitsOf(report).Single();
         CollectionAssert.Contains(pdfHit.GetProperty("attachments").EnumerateArray().Select(a => a.GetString()).ToList(), "quarterly-report.pdf");

         JsonElement notes = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"attachment\":{\"name\":\"notes\",\"type\":\"text/plain\"}}");
         CollectionAssert.AreEquivalent(new[] { "mailbox:" + text }, Hits(notes), "Asked for the text file, the text file's message alone.");

         JsonElement images = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"words\":\"" + word + "\",\"attachment\":{\"type\":\"image/\"}}");
         CollectionAssert.IsEmpty(Hits(images), "No message carries an image.");

         JsonElement any = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"words\":\"" + word + "\"}");
         ClassicAssert.AreEqual(3, Hits(any).Count, "The control: without the criterion the word finds all three.");
      }

      // ---------------------------------------------------------------- the hold

      [Test]
      [Description("A hold on an account: the person's IMAP delete and EXPUNGE leaves the message among its deleted items with RecoverDeletedDays at 0 and none of them can be deleted for good; retention does not end them, and the account's own message retention policy leaves its old mail where the unheld control account's goes; an account rule's Delete keeps its copy there too, where the control account under no hold keeps nothing; a mailbox restore's replace, the account's deletion over REST and over COM are refused with the sentence; a search finds what the hold kept. The hold's placing and its release are on the audit trail. Released, the deletion proceeds at the next sweep.")]
      public void AHoldKeepsWhatThePersonARuleRetentionAndARestoreWouldDelete()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);
         string now = DateTime.UtcNow.ToString("r");
         Deliver(alice.Address, Raw(alice.Address, "Held " + word, Id("held"), now, "Kept by the hold: " + word));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);

         long caseId = OpenCase("Hold " + word);
         Reply placed = Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}");
         ClassicAssert.AreEqual(201, placed.Status, placed.Body);
         long holdId = placed.Json.GetProperty("holds")[0].GetProperty("id").GetInt64();
         ClassicAssert.AreEqual(409, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}").Status,
            "The case holds the account once.");

         // The person deletes it over IMAP.
         ExpungeFirst(alice.Address);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 0);

         JsonElement kept = Recoverable(alice.Address);
         ClassicAssert.AreEqual(0, kept.GetProperty("days").GetInt32(), "RecoverDeletedDays is 0: the hold is what keeps the message, not the recovery window.");
         ClassicAssert.IsTrue(kept.GetProperty("held").GetBoolean());
         ClassicAssert.AreEqual(1, kept.GetProperty("items").GetArrayLength(), "The deleted message is kept by the hold.");
         long itemId = kept.GetProperty("items")[0].GetProperty("id").GetInt64();

         Reply purge = Send("DELETE", "/api/v1/me/recoverable/" + itemId, Basic(alice.Address, Password), null);
         ClassicAssert.AreEqual(409, purge.Status, purge.Body);
         StringAssert.Contains("legal hold", purge.Body);

         // Retention does not end what the hold keeps - the recover-deleted sweep
         // passes the held copy by - nor does the account's own message retention
         // policy delete its old mail, where the unheld control account's goes.
         alice.MessageRetentionDays = 1;
         alice.Save();
         bob.MessageRetentionDays = 1;
         bob.Save();
         Deliver(alice.Address, Raw(alice.Address, "Aged", Id("aged"), now, "Old enough for the policy."));
         Deliver(bob.Address, Raw(bob.Address, "Aged", Id("aged-control"), now, "Old enough for the policy too."));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 1);
         AgeEveryMessage(alice, 10);
         AgeEveryMessage(bob, 10);

         _application.Utilities.RunMessageRetention();
         ClassicAssert.AreEqual(1, Recoverable(alice.Address).GetProperty("items").GetArrayLength(), "The sweep left the held copy.");
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 0);

         // A rule's Delete: the copy is kept, the mailbox gets nothing - and for the
         // account under no hold the same rule keeps nothing.
         AddDeleteRule(alice, "DropMe");
         AddDeleteRule(bob, "DropMe");
         Deliver(alice.Address, Raw(alice.Address, "DropMe " + word, Id("dropped"), now, "A rule deletes this: " + word));
         Deliver(bob.Address, Raw(bob.Address, "DropMe " + word, Id("dropped-control"), now, "A rule deletes this too: " + word));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 0);
         ClassicAssert.AreEqual(2, Recoverable(alice.Address).GetProperty("items").GetArrayLength(), "The rule's Delete kept the held account's copy.");
         ClassicAssert.AreEqual(0, Recoverable(bob.Address).GetProperty("items").GetArrayLength(), "The control: under no hold, the rule's Delete keeps nothing.");

         // What would delete the mailbox whole is refused, with the sentence.
         Reply replace = Admin("POST", "/api/v1/backup/no-such-archive.7z/mailboxes/" + alice.Address + "/restore", "{\"mode\":\"replace\"}");
         ClassicAssert.AreEqual(409, replace.Status, replace.Body);
         StringAssert.Contains("legal hold", replace.Body);
         Reply replaceControl = Admin("POST", "/api/v1/backup/no-such-archive.7z/mailboxes/" + bob.Address + "/restore", "{\"mode\":\"replace\"}");
         StringAssert.DoesNotContain("legal hold", replaceControl.Body, "The control: the account under no hold is refused for its archive, not for a hold.");

         Reply deleted = Admin("DELETE", "/api/v1/accounts/" + alice.Address, null);
         ClassicAssert.AreEqual(409, deleted.Status, deleted.Body);
         StringAssert.Contains("legal hold", deleted.Body);

         var comRefusal = Assert.Throws<COMException>(() => _domain.Accounts.get_ItemByAddress(alice.Address).Delete());
         StringAssert.Contains("legal hold", comRefusal.Message);
         ClassicAssert.IsNotNull(_domain.Accounts.get_ItemByAddress(alice.Address), "The account is still there.");

         // A search finds what the hold kept.
         JsonElement search = RunSearch(caseId, "{\"custodians\":[\"" + alice.Address + "\"],\"words\":\"" + word + "\",\"scope\":\"mailboxes\"}");
         ClassicAssert.AreEqual(2, HitsOf(search).Count(h => h.GetProperty("kind").GetString() == "recoverable"),
            "The search reads the deleted items the hold kept.");
         ClassicAssert.IsTrue(HitsOf(search).All(h => h.GetProperty("held").GetBoolean()));

         // Released: the next sweep ends them, as RecoverDeletedDays 0 says, and the
         // policy deletes the old message it had left.
         Reply released = Admin("DELETE", CasePath(caseId) + "/holds/" + holdId, null);
         ClassicAssert.AreEqual(200, released.Status, released.Body);
         ClassicAssert.IsFalse(released.Json.GetProperty("live").GetBoolean());
         ClassicAssert.AreEqual(409, Admin("DELETE", CasePath(caseId) + "/holds/" + holdId, null).Status, "Released once.");

         // The hold's placing and its release, each on the audit trail under the case.
         Reply holdTrail = Admin("GET", "/api/v1/audit?object_type=" + Uri.EscapeDataString("discovery hold") + "&limit=50", null);
         ClassicAssert.AreEqual(200, holdTrail.Status, holdTrail.Body);
         var holdRows = holdTrail.Json.GetProperty("entries").EnumerateArray()
            .Where(e => e.GetProperty("object").GetString().StartsWith("case " + caseId + ":")).Select(e => e.GetProperty("action").GetString()).ToList();
         CollectionAssert.Contains(holdRows, "created", "The hold placed is on the audit trail: " + holdTrail.Body);
         CollectionAssert.Contains(holdRows, "updated", "The hold released is on the audit trail: " + holdTrail.Body);

         _application.Utilities.RunMessageRetention();
         JsonElement after = Recoverable(alice.Address);
         ClassicAssert.IsFalse(after.GetProperty("held").GetBoolean());
         ClassicAssert.AreEqual(0, after.GetProperty("items").GetArrayLength(), "Released, the deletion proceeds.");
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 0);
      }

      [Test]
      [Description("IMAP MOVE: a held account's message moved into a public folder has left the mailbox, so its original is kept among the deleted items with RecoverDeletedDays at 0; a move within the account keeps nothing, since the message is still in the mailbox; and the same move out of an account under no hold keeps nothing.")]
      public void AMoveOutOfAHeldAccountKeepsTheOriginalAndAMoveWithinItDoesNot()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);
         alice.IMAPFolders.Add("Moved");

         IMAPFolder share = _settings.PublicFolders.Add("DiscoveryShare");
         share.Save();
         foreach (Account account in new[] { alice, bob })
         {
            var permission = share.Permissions.Add();
            permission.PermissionAccountID = account.ID;
            permission.PermissionType = eACLPermissionType.ePermissionTypeUser;
            permission.set_Permission(eACLPermission.ePermissionLookup, true);
            permission.set_Permission(eACLPermission.ePermissionRead, true);
            permission.set_Permission(eACLPermission.ePermissionInsert, true);
            permission.Save();
         }

         // "Within" is in Alice's inbox before "Out" is sent: the two MOVE 1s below
         // take "Within" first and "Out" second, and two messages accepted one after
         // the other can be delivered in either order by two delivery threads - in
         // the other order the kept original would be "Within".
         string now = DateTime.UtcNow.ToString("r");
         Deliver(alice.Address, Raw(alice.Address, "Within " + word, Id("within"), now, "Moved inside the mailbox."));
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);
         Deliver(alice.Address, Raw(alice.Address, "Out " + word, Id("out"), now, "Moved out of the mailbox."));
         Deliver(bob.Address, Raw(bob.Address, "Out " + word, Id("out-control"), now, "Moved out of a mailbox under no hold."));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 2);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 1);

         long caseId = OpenCase("Moves " + word);
         ClassicAssert.AreEqual(201, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}").Status);

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(alice.Address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));

         // Within the account: the message is still in the mailbox, and nothing is kept.
         string within = imap.SendSingleCommand("A20 MOVE 1 \"Moved\"");
         StringAssert.Contains("A20 OK", within);
         ClassicAssert.AreEqual(0, Recoverable(alice.Address).GetProperty("items").GetArrayLength(),
            "A move within the held account keeps no second copy.");

         // Out of the account, into a public folder anybody with the right may expunge.
         string outOfIt = imap.SendSingleCommand("A21 MOVE 1 \"#Public.DiscoveryShare\"");
         StringAssert.Contains("A21 OK", outOfIt);
         imap.Disconnect();

         JsonElement kept = Recoverable(alice.Address);
         ClassicAssert.AreEqual(0, kept.GetProperty("days").GetInt32(), "RecoverDeletedDays is 0: the hold is what keeps it.");
         ClassicAssert.AreEqual(1, kept.GetProperty("items").GetArrayLength(), "The original of a move out of the held account is kept.");
         StringAssert.StartsWith("Out ", kept.GetProperty("items")[0].GetProperty("subject").GetString());

         // The control: the same move out of an account under no hold keeps nothing.
         var control = new ImapClientSimulator();
         ClassicAssert.IsTrue(control.ConnectAndLogon(bob.Address, Password));
         ClassicAssert.IsTrue(control.SelectFolder("INBOX"));
         StringAssert.Contains("A22 OK", control.SendSingleCommand("A22 MOVE 1 \"#Public.DiscoveryShare\""));
         control.Disconnect();
         ClassicAssert.AreEqual(0, Recoverable(bob.Address).GetProperty("items").GetArrayLength(), "The control keeps nothing.");

         CustomAsserts.AssertFolderMessageCount(share, 2);
      }

      [Test]
      [Description("A hold on a domain: an account in it keeps what its person expunges, where an account of another domain under no hold keeps nothing; the domain's deletion is refused over REST and over COM with the sentence; the held account is not moved to another domain - 409 with the sentence, and it is still there and still held - while an account under no hold moves into the held domain and is held there.")]
      public void ADomainHoldKeepsItsAccountsAndRefusesTheDomainsDeletionAndAMoveOutOfIt()
      {
         string word = Word();
         const string secondDomain = "discovery-second.test";
         Domain second = SingletonProvider<TestSetup>.Instance.AddDomain(secondDomain);
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account carol = SingletonProvider<TestSetup>.Instance.AddAccount(second, "carol@" + secondDomain, Password);

         string now = DateTime.UtcNow.ToString("r");
         Deliver(alice.Address, Raw(alice.Address, "Domain held " + word, Id("dheld"), now, "Held by the domain's hold."));
         Deliver(carol.Address, Raw(carol.Address, "Not held " + word, Id("dcontrol"), now, "In a domain under no hold."));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         long caseId = OpenCase("Domain hold " + word);
         Reply placed = Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"domain\",\"target\":\"" + _domain.Name + "\"}");
         ClassicAssert.AreEqual(201, placed.Status, placed.Body);

         ExpungeFirst(alice.Address);
         ExpungeFirst(carol.Address);
         JsonElement heldItems = Recoverable(alice.Address);
         ClassicAssert.IsTrue(heldItems.GetProperty("held").GetBoolean(), "The domain's hold covers the account in it.");
         ClassicAssert.AreEqual(1, heldItems.GetProperty("items").GetArrayLength(), "The expunged message is kept by the domain's hold.");
         JsonElement controlItems = Recoverable(carol.Address);
         ClassicAssert.IsFalse(controlItems.GetProperty("held").GetBoolean());
         ClassicAssert.AreEqual(0, controlItems.GetProperty("items").GetArrayLength(), "The control: another domain's account keeps nothing.");

         // The domain is not deleted, over REST or COM.
         Reply refused = Admin("DELETE", "/api/v1/domains/" + _domain.Name, null);
         ClassicAssert.AreEqual(409, refused.Status, refused.Body);
         StringAssert.Contains("legal hold", refused.Body);
         var comRefusal = Assert.Throws<COMException>(() => _application.Domains.get_ItemByName(_domain.Name).Delete());
         StringAssert.Contains("legal hold", comRefusal.Message);

         // The held account is not moved out of the domain the hold is on.
         Reply move = Admin("POST", "/api/v1/accounts/" + alice.Address + "/move", "{\"new_address\":\"alice@" + secondDomain + "\",\"keep_alias\":false}");
         ClassicAssert.AreEqual(409, move.Status, move.Body);
         StringAssert.Contains("legal hold", move.Body);
         ClassicAssert.IsNotNull(_application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(alice.Address), "The account is where it was.");
         ClassicAssert.IsTrue(Recoverable(alice.Address).GetProperty("held").GetBoolean(), "And still held.");

         // The control: an account under no hold moves into the held domain, and the
         // domain's hold covers it there.
         string moved = "carol@" + _domain.Name;
         Reply moveIn = Admin("POST", "/api/v1/accounts/" + carol.Address + "/move", "{\"new_address\":\"" + moved + "\",\"keep_alias\":false}");
         ClassicAssert.AreEqual(200, moveIn.Status, moveIn.Body);
         ClassicAssert.IsTrue(Recoverable(moved).GetProperty("held").GetBoolean(), "Moved into the held domain, the account is held.");
      }

      [Test]
      [Description("A Sieve discard on a held account keeps the copy among its deleted items, where the same script on an account under no hold keeps nothing.")]
      public void ASieveDiscardUnderAHoldIsKept()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);

         const string script = "if header :contains \"Subject\" \"DiscardMe\" {\r\n  discard;\r\n}";
         SetSieveScript(alice, script);
         SetSieveScript(bob, script);

         long caseId = OpenCase("Sieve " + word);
         ClassicAssert.AreEqual(201, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}").Status);

         string now = DateTime.UtcNow.ToString("r");
         Deliver(alice.Address, Raw(alice.Address, "DiscardMe " + word, Id("sieve"), now, "A script discards this: " + word));
         Deliver(bob.Address, Raw(bob.Address, "DiscardMe " + word, Id("sieve-control"), now, "A script discards this too: " + word));
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 0);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 0);
         JsonElement kept = Recoverable(alice.Address);
         ClassicAssert.AreEqual(1, kept.GetProperty("items").GetArrayLength(), "The discarded copy is kept by the hold.");
         StringAssert.StartsWith("DiscardMe ", kept.GetProperty("items")[0].GetProperty("subject").GetString());
         ClassicAssert.AreEqual(0, Recoverable(bob.Address).GetProperty("items").GetArrayLength(), "The control: under no hold, the discard keeps nothing.");
      }

      [Test]
      [Description("A zero-access mailbox under a hold: an account rule's Delete keeps the copy among its deleted items sealed to the mailbox's key - PGP/MIME, the body nowhere in the file - where the same rule on a held ordinary mailbox keeps the message as it came. A search matches the zero-access mailbox on its headers alone and says so, and does not find a word only in its bodies, which it finds in the ordinary mailbox; an export withholds the zero-access mailbox's messages with the reason in the manifest and writes the other's.")]
      public void AZeroAccessMailboxUnderAHoldKeepsItsCopiesSealedAndIsSearchedOnItsHeaders()
      {
         string word = Word();
         string bodyWord = Word();
         Account sealedAccount = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "sealed@" + _domain.Name, Password);
         Account plain = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "plain@" + _domain.Name, Password);

         Reply allowed = Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":true}");
         ClassicAssert.AreEqual(200, allowed.Status, allowed.Body);

         try
         {
            SwitchZeroAccessOn(sealedAccount.Address);

            long caseId = OpenCase("Zero access " + word);
            ClassicAssert.AreEqual(201, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + sealedAccount.Address + "\"}").Status);
            ClassicAssert.AreEqual(201, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + plain.Address + "\"}").Status);

            AddDeleteRule(sealedAccount, "DropMe");
            AddDeleteRule(plain, "DropMe");

            string now = DateTime.UtcNow.ToString("r");
            string secret = "The body of this message must never be on the disk in the clear: " + bodyWord;
            Deliver(sealedAccount.Address, Raw(sealedAccount.Address, "DropMe " + word, Id("zdrop"), now, secret));
            Deliver(plain.Address, Raw(plain.Address, "DropMe " + word, Id("pdrop"), now, secret));
            Deliver(sealedAccount.Address, Raw(sealedAccount.Address, "Kept " + word, Id("zkept"), now, "Delivered and sealed."));
            Deliver(sealedAccount.Address, Raw(sealedAccount.Address, "Quiet", Id("zquiet"), now, "Only in the body: " + bodyWord));
            Deliver(plain.Address, Raw(plain.Address, "Quiet", Id("pquiet"), now, "Only in the body: " + bodyWord));
            CustomAsserts.AssertRecipientsInDeliveryQueue(0);

            ImapClientSimulator.AssertMessageCount(sealedAccount.Address, Password, "INBOX", 2);
            ImapClientSimulator.AssertMessageCount(plain.Address, Password, "INBOX", 1);
            ClassicAssert.AreEqual(1, Recoverable(sealedAccount.Address).GetProperty("items").GetArrayLength(), "The rule's Delete kept the held zero-access mailbox's copy.");
            ClassicAssert.AreEqual(1, Recoverable(plain.Address).GetProperty("items").GetArrayLength(), "The rule's Delete kept the held ordinary mailbox's copy.");

            // Sealed as the mailbox would have held it: never in the clear.
            string sealedKept = File.ReadAllText(KeptFiles(sealedAccount).Single());
            StringAssert.Contains("-----BEGIN PGP MESSAGE-----", sealedKept);
            StringAssert.Contains("multipart/encrypted", sealedKept);
            StringAssert.DoesNotContain(bodyWord, sealedKept, "The kept copy of a zero-access mailbox's message holds no plaintext body.");
            StringAssert.Contains("DropMe " + word, sealedKept, "Its headers, which zero access keeps, are there.");

            // The control: the ordinary mailbox's copy is the message as it came.
            string plainKept = File.ReadAllText(KeptFiles(plain).Single());
            StringAssert.Contains(bodyWord, plainKept);
            StringAssert.DoesNotContain("-----BEGIN PGP MESSAGE-----", plainKept);

            // Searched on its headers: the word in the subjects is found, and said to be.
            string custodians = "[\"" + sealedAccount.Address + "\",\"" + plain.Address + "\"]";
            JsonElement byHeader = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"words\":\"" + word + "\",\"scope\":\"mailboxes\"}");
            List<JsonElement> sealedHits = HitsOf(byHeader).Where(h => h.GetProperty("custodian").GetString() == sealedAccount.Address).ToList();
            ClassicAssert.AreEqual(2, sealedHits.Count, "The delivered message and the kept one, by the word in their subjects.");
            ClassicAssert.IsTrue(sealedHits.All(h => h.GetProperty("headers_only").GetBoolean() && h.GetProperty("zero_access").GetBoolean()));
            JsonElement plainHit = HitsOf(byHeader).Single(h => h.GetProperty("custodian").GetString() == plain.Address);
            ClassicAssert.IsFalse(plainHit.GetProperty("headers_only").GetBoolean());

            // A word only in the bodies: found in the ordinary mailbox, not in the sealed one.
            JsonElement byBody = RunSearch(caseId, "{\"custodians\":" + custodians + ",\"words\":\"" + bodyWord + "\",\"scope\":\"mailboxes\"}");
            ClassicAssert.IsTrue(HitsOf(byBody).All(h => h.GetProperty("custodian").GetString() == plain.Address), "Nothing of the zero-access mailbox is found by its bodies.");
            ClassicAssert.AreEqual(2, HitsOf(byBody).Count, "The ordinary mailbox's two messages with the word in their bodies.");

            // The export withholds the zero-access mailbox's messages, with the reason, and writes the other's.
            long searchId = byHeader.GetProperty("id").GetInt64();
            Reply queued = Admin("POST", CasePath(caseId) + "/searches/" + searchId + "/export", "{}");
            ClassicAssert.AreEqual(202, queued.Status, queued.Body);
            long exportId = queued.Json.GetProperty("id").GetInt64();
            JsonElement export = WaitForExport(caseId, exportId);
            ClassicAssert.AreEqual(1, export.GetProperty("items").GetInt32());
            ClassicAssert.AreEqual(2, export.GetProperty("withheld").GetInt32());

            Reply download = Admin("GET", CasePath(caseId) + "/exports/" + exportId, null);
            ClassicAssert.AreEqual(200, download.Status, download.Body);
            using (var zip = new ZipArchive(new MemoryStream(download.Bytes), ZipArchiveMode.Read))
            {
               ZipArchiveEntry manifestEntry = zip.GetEntry("manifest.json");
               ClassicAssert.IsNotNull(manifestEntry);
               using (JsonDocument manifest = JsonDocument.Parse(ReadEntry(manifestEntry)))
               {
                  var withheld = manifest.RootElement.GetProperty("items").EnumerateArray().Where(i => i.GetProperty("withheld").GetBoolean()).ToList();
                  ClassicAssert.AreEqual(2, withheld.Count);
                  ClassicAssert.IsTrue(withheld.All(i => i.GetProperty("custodian").GetString() == sealedAccount.Address &&
                                                        i.GetProperty("reason").GetString().Contains("zero-access")),
                     "Each withheld item is the zero-access mailbox's, with the reason.");
               }

               ClassicAssert.IsFalse(zip.Entries.Any(e => e.FullName.StartsWith(sealedAccount.Address + "/")), "Nothing of the zero-access mailbox is written.");
            }
         }
         finally
         {
            Admin("PUT", "/api/v1/settings/openpgp", "{\"zero_access_allowed\":false}");
         }
      }

      [Test]
      [Description("A restore with the domains option, under a hold on an account, is refused before its first deletion with the hold's sentence: the domain whose name sorts before the held account's - which the restore used to delete before it reached the held one - is still there with its account, and so is the held account.")]
      public void AFullRestoreUnderAHoldIsRefusedBeforeAnyDomainIsDeleted()
      {
         const string firstDomain = "discovery-a.test";
         Domain first = SingletonProvider<TestSetup>.Instance.AddDomain(firstDomain);
         Account early = SingletonProvider<TestSetup>.Instance.AddAccount(first, "early@" + firstDomain, Password);
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         ClassicAssert.Less(string.CompareOrdinal(firstDomain, _domain.Name), 0, "The unheld domain must sort before the held one for this to prove anything.");

         var backupSettings = _application.Settings.Backup;
         string previousDestination = backupSettings.Destination;
         bool previousDomains = backupSettings.BackupDomains;
         bool previousMessages = backupSettings.BackupMessages;
         bool previousSettings = backupSettings.BackupSettings;
         bool previousCompress = backupSettings.CompressDestinationFiles;
         string backupDirectory = Paths.Combine(Path.GetTempPath(), "hMailDiscoveryRestore-" + TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);

         try
         {
            backupSettings.Destination = backupDirectory;
            backupSettings.BackupDomains = true;
            backupSettings.BackupMessages = false;
            backupSettings.BackupSettings = false;
            backupSettings.CompressDestinationFiles = true;

            CustomAsserts.AssertDeleteFile(backupSettings.LogFile);
            _application.BackupManager.StartBackup();
            ClassicAssert.IsTrue(WaitForBackupLog("Backup completed successfully", 300), "The backup did not complete: " + ReadBackupLog());

            string archive = Directory.GetFiles(backupDirectory, "*.7z").Single();
            CustomAsserts.AssertDeleteFile(backupSettings.LogFile);

            long caseId = OpenCase("Restore " + Word());
            ClassicAssert.AreEqual(201, Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}").Status);

            var backup = _application.BackupManager.LoadBackup(archive);
            ClassicAssert.IsNotNull(backup);
            ClassicAssert.IsTrue(backup.ContainsDomains);
            backup.RestoreDomains = true;
            backup.RestoreSettings = false;
            backup.RestoreMessages = false;
            backup.StartRestore();

            ClassicAssert.IsTrue(WaitForBackupLog("legal hold", 60), "The restore was not refused for the hold: " + ReadBackupLog());
            StringAssert.Contains("Nothing has been changed", ReadBackupLog());

            List<string> domains = DomainNames();
            CollectionAssert.Contains(domains, firstDomain, "The domain that sorts first was not deleted.");
            CollectionAssert.Contains(domains, _domain.Name.ToLowerInvariant(), "The held account's domain was not deleted.");
            ClassicAssert.IsNotNull(_application.Domains.get_ItemByName(firstDomain).Accounts.get_ItemByAddress(early.Address));
            ClassicAssert.IsNotNull(_application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(alice.Address));

            // A refused restore is a failed one, HM5014, consumed here.
            CustomAsserts.AssertReportedError("HM5014", "legal hold");
         }
         finally
         {
            backupSettings.Destination = previousDestination;
            backupSettings.BackupDomains = previousDomains;
            backupSettings.BackupMessages = previousMessages;
            backupSettings.BackupSettings = previousSettings;
            backupSettings.CompressDestinationFiles = previousCompress;
            Tidy(() => Directory.Delete(backupDirectory, true));
         }
      }

      // ---------------------------------------------------------------- the export

      [Test]
      [Description("The export of a search downloads as a zip whose EML files are the stored messages byte for byte - each ending in what the test sent - whose manifest names the case, the search and its query and whose every SHA-256, and SHA256SUMS's own, verifies; whose mbox for each custodian folder parses as one message per From_ line; and whose own SHA-256 is the one the case answers. The export's making and its download are on the audit trail.")]
      public void AnExportHoldsTheMessagesByteForByteWithAManifestThatVerifies()
      {
         string word = Word();
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         Account bob = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "bob@" + _domain.Name, Password);
         string now = DateTime.UtcNow.ToString("r");

         string e1 = Id("e1");
         string e2 = Id("e2");
         string sent1 = Raw(alice.Address, "Export one " + word, e1, now, "First exported message: " + word);
         string sent2 = WithAttachment(bob.Address, "Export two " + word, e2, now, "minutes.txt", "text/plain", Encoding.ASCII.GetBytes("Minutes of the meeting: " + word));
         Deliver(alice.Address, sent1);
         Deliver(bob.Address, sent2);
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);
         ImapClientSimulator.AssertMessageCount(alice.Address, Password, "INBOX", 1);
         ImapClientSimulator.AssertMessageCount(bob.Address, Password, "INBOX", 1);

         long caseId = OpenCase("Export " + word);
         JsonElement search = RunSearch(caseId, "{\"custodians\":[\"" + alice.Address + "\",\"" + bob.Address + "\"],\"words\":\"" + word + "\",\"scope\":\"mailboxes\"}");
         ClassicAssert.AreEqual(2, HitsOf(search).Count);
         long searchId = search.GetProperty("id").GetInt64();

         Reply queued = Admin("POST", CasePath(caseId) + "/searches/" + searchId + "/export", "{}");
         ClassicAssert.AreEqual(202, queued.Status, queued.Body);
         long exportId = queued.Json.GetProperty("id").GetInt64();

         JsonElement export = WaitForExport(caseId, exportId);
         ClassicAssert.AreEqual(2, export.GetProperty("items").GetInt32());
         ClassicAssert.AreEqual(0, export.GetProperty("withheld").GetInt32());
         string caseSha = export.GetProperty("sha256").GetString();

         Reply download = Admin("GET", CasePath(caseId) + "/exports/" + exportId, null);
         ClassicAssert.AreEqual(200, download.Status, download.Body);
         StringAssert.StartsWith("application/zip", download.Header("Content-Type"));
         StringAssert.Contains("attachment", download.Header("Content-Disposition"));
         ClassicAssert.AreEqual(caseSha, Sha256(download.Bytes), "The zip's SHA-256 is the one the case answers.");
         ClassicAssert.AreEqual(export.GetProperty("size").GetInt64(), download.Bytes.LongLength);

         using (var zip = new ZipArchive(new MemoryStream(download.Bytes), ZipArchiveMode.Read))
         {
            var entries = zip.Entries.ToDictionary(e => e.FullName, e => ReadEntry(e));

            // The EML files are the stored messages, byte for byte, and each ends in what was sent.
            string aliceFile = Stored(alice).Single();
            string bobFile = Stored(bob).Single();
            string aliceEml = entries.Keys.Single(k => k.StartsWith(alice.Address + "/INBOX/") && k.EndsWith(".eml"));
            string bobEml = entries.Keys.Single(k => k.StartsWith(bob.Address + "/INBOX/") && k.EndsWith(".eml"));
            CollectionAssert.AreEqual(File.ReadAllBytes(aliceFile), entries[aliceEml], "alice's message, byte for byte.");
            CollectionAssert.AreEqual(File.ReadAllBytes(bobFile), entries[bobEml], "bob's message, byte for byte.");
            string aliceText = Encoding.ASCII.GetString(entries[aliceEml]);
            StringAssert.Contains("Message-ID: <" + e1 + ">", aliceText, "The EML is the message that was sent.");
            StringAssert.Contains("First exported message: " + word, aliceText);
            StringAssert.Contains(e1, aliceEml, "The file is named for its Message-ID.");

            // The manifest: the case, the search and its query, and each file's SHA-256.
            using (JsonDocument manifest = JsonDocument.Parse(entries["manifest.json"]))
            {
               JsonElement root = manifest.RootElement;
               ClassicAssert.AreEqual(caseId, root.GetProperty("case").GetProperty("id").GetInt64());
               ClassicAssert.AreEqual(searchId, root.GetProperty("search").GetProperty("id").GetInt64());
               ClassicAssert.AreEqual(word, root.GetProperty("search").GetProperty("query").GetProperty("words").GetString());
               ClassicAssert.AreEqual(2, root.GetProperty("files").GetInt32());
               ClassicAssert.AreEqual(0, root.GetProperty("unsearched_list").GetArrayLength(), "Nothing went unsearched, and the manifest says so.");

               foreach (JsonElement item in root.GetProperty("items").EnumerateArray())
               {
                  ClassicAssert.IsFalse(item.GetProperty("withheld").GetBoolean());
                  string path = item.GetProperty("path").GetString();
                  ClassicAssert.IsTrue(entries.ContainsKey(path), "The manifest names " + path + ", which the zip holds.");
                  ClassicAssert.AreEqual(item.GetProperty("sha256").GetString(), Sha256(entries[path]), "The SHA-256 of " + path + " verifies.");
                  ClassicAssert.AreEqual(entries[path].LongLength, item.GetProperty("size").GetInt64());
               }

               string sums = Encoding.UTF8.GetString(entries["SHA256SUMS"]);
               ClassicAssert.AreEqual(root.GetProperty("sums_sha256").GetString(), Sha256(entries["SHA256SUMS"]), "SHA256SUMS verifies against the manifest.");
               foreach (string line in sums.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries))
               {
                  string digest = line.Substring(0, 64);
                  string path = line.Substring(66);
                  ClassicAssert.AreEqual(digest, Sha256(entries[path]), "SHA256SUMS's line for " + path + " verifies.");
               }
            }

            // The mbox of each custodian folder: one message, after one From_ line.
            byte[] mbox = entries[alice.Address + "/INBOX.mbox"];
            string mboxText = Encoding.ASCII.GetString(mbox);
            ClassicAssert.IsTrue(mboxText.StartsWith("From "), "An mbox begins with a From_ line.");
            ClassicAssert.AreEqual(1, mboxText.Split(new[] { "\r\n" }, StringSplitOptions.None).Count(l => l.StartsWith("From ")),
               "One message, one From_ line.");
            StringAssert.Contains("First exported message: " + word, mboxText);
            ClassicAssert.IsTrue(entries.ContainsKey(bob.Address + "/INBOX.mbox"));
         }

         // Asked for, and taken off the server: both on the audit trail.
         Reply made = Admin("GET", "/api/v1/audit?object_type=" + Uri.EscapeDataString("discovery export") + "&action=created&limit=20", null);
         ClassicAssert.AreEqual(200, made.Status, made.Body);
         ClassicAssert.IsTrue(made.Json.GetProperty("entries").EnumerateArray().Any(e => e.GetProperty("object").GetString().StartsWith("case " + caseId + ":")),
            "The export asked for is recorded under its case: " + made.Body);

         Reply trail = Admin("GET", "/api/v1/audit?object_type=" + Uri.EscapeDataString("discovery export") + "&action=downloaded&limit=20", null);
         ClassicAssert.AreEqual(200, trail.Status, trail.Body);
         ClassicAssert.IsTrue(trail.Json.GetProperty("entries").EnumerateArray().Any(e => e.GetProperty("detail").GetString().Contains(caseSha)),
            "The download is recorded with the zip's SHA-256: " + trail.Body);

         // Neither a closed case nor an unfinished search exports.
         ClassicAssert.AreEqual(200, Admin("POST", CasePath(caseId) + "/close", "{}").Status);
         ClassicAssert.AreEqual(409, Admin("POST", CasePath(caseId) + "/searches/" + searchId + "/export", "{}").Status, "A closed case makes no export.");
      }

      // ---------------------------------------------------------------- who

      [Test]
      [Description("A key whose role holds neither discovery permission is refused the cases; an auditor reads a case and is refused searching, holding, releasing, exporting and closing; a role that reads the cases and not the mailboxes is refused the download; a role holding manage_discovery and not view_archive searches the mailboxes and is refused the archive, and a search that read the archive. A key confined to example.test names no custodian outside it, nor *, nor holds an account there - and names one inside it; it is listed the empty case it opened and not one somebody else opened, nor a case that searched outside its domain.")]
      public void TheRolesDecideWhoReadsWhoActsAndWhereAConfinedKeyReaches()
      {
         Account alice = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "alice@" + _domain.Name, Password);
         long caseId = OpenCase("Roles " + Word());
         Reply held = Admin("POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}");
         ClassicAssert.AreEqual(201, held.Status, held.Body);
         long holdId = held.Json.GetProperty("holds")[0].GetProperty("id").GetInt64();

         // Neither permission.
         MakeRole("discoverytest-none", "view_accounts");
         var none = MakeKey("discovery none", null, "discoverytest-none");
         ClassicAssert.AreEqual(403, AsKey(none, "GET", "/api/v1/discovery/cases", null).Status);
         ClassicAssert.AreEqual(403, AsKey(none, "POST", "/api/v1/discovery/cases", "{\"name\":\"refused\"}").Status);

         // The auditor: reads, and changes nothing.
         var auditor = MakeKey("discovery auditor", "readonly", null);
         ClassicAssert.AreEqual(200, AsKey(auditor, "GET", "/api/v1/discovery/cases", null).Status);
         ClassicAssert.AreEqual(200, AsKey(auditor, "GET", CasePath(caseId), null).Status);
         ClassicAssert.AreEqual(403, AsKey(auditor, "POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"" + alice.Address + "\"]}").Status);
         ClassicAssert.AreEqual(403, AsKey(auditor, "POST", CasePath(caseId) + "/holds", "{\"scope\":\"account\",\"target\":\"" + alice.Address + "\"}").Status);
         ClassicAssert.AreEqual(403, AsKey(auditor, "DELETE", CasePath(caseId) + "/holds/" + holdId, null).Status, "An auditor does not release a hold.");
         ClassicAssert.AreEqual(403, AsKey(auditor, "POST", CasePath(caseId) + "/searches/1/export", "{}").Status);
         ClassicAssert.AreEqual(403, AsKey(auditor, "POST", CasePath(caseId) + "/close", "{}").Status);
         ClassicAssert.AreEqual(403, AsKey(auditor, "POST", "/api/v1/discovery/cases", "{\"name\":\"refused\"}").Status);
         ClassicAssert.AreEqual(404, AsKey(auditor, "GET", CasePath(caseId) + "/exports/999999", null).Status,
            "An auditor holds view_mailbox too, so the download is the handler's to answer - here, no such export.");
         ClassicAssert.IsTrue(Admin("GET", CasePath(caseId), null).Json.GetProperty("holds")[0].GetProperty("live").GetBoolean(), "The hold the auditor was refused to release stands.");

         // Reading the cases is not taking the mail away.
         MakeRole("discoverytest-reader", "view_discovery");
         var reader = MakeKey("discovery reader", null, "discoverytest-reader");
         ClassicAssert.AreEqual(200, AsKey(reader, "GET", CasePath(caseId), null).Status);
         Reply download = AsKey(reader, "GET", CasePath(caseId) + "/exports/999999", null);
         ClassicAssert.AreEqual(403, download.Status, download.Body);
         StringAssert.Contains("view_mailbox", download.Body);

         // manage_discovery searches the mailboxes; the archive also needs view_archive.
         MakeRole("discoverytest-manager", "view_discovery,manage_discovery");
         var manager = MakeKey("discovery manager", null, "discoverytest-manager");
         Reply archiveRefused = AsKey(manager, "POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"" + alice.Address + "\"]}");
         ClassicAssert.AreEqual(403, archiveRefused.Status, "The default scope reads the archive: " + archiveRefused.Body);
         StringAssert.Contains("view_archive", archiveRefused.Body);
         ClassicAssert.AreEqual(202, AsKey(manager, "POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"" + alice.Address + "\"],\"scope\":\"mailboxes\"}").Status,
            "The control: the mailboxes alone.");

         // A key confined to example.test.
         var confined = MakeKey("discovery confined", "full", null, _domain.Name);
         long confinedCase = AsKey(confined, "POST", "/api/v1/discovery/cases", "{\"name\":\"Confined\"}").Json.GetProperty("id").GetInt64();

         // An empty case is listed to the confined key that opened it, and not to it when somebody else did.
         long othersEmpty = OpenCase("Somebody else's " + Word());
         List<long> listed = AsKey(confined, "GET", "/api/v1/discovery/cases", null).Json.GetProperty("cases").EnumerateArray().Select(c => c.GetProperty("id").GetInt64()).ToList();
         CollectionAssert.Contains(listed, confinedCase, "The empty case the confined key opened is listed to it.");
         CollectionAssert.DoesNotContain(listed, othersEmpty, "An empty case somebody else opened is not.");
         ClassicAssert.AreEqual(404, AsKey(confined, "GET", CasePath(othersEmpty), null).Status);
         ClassicAssert.AreEqual(404, AsKey(confined, "POST", CasePath(othersEmpty) + "/close", "{}").Status, "Nor may it close one.");

         ClassicAssert.AreEqual(403, AsKey(confined, "POST", CasePath(confinedCase) + "/searches", "{\"custodians\":[\"someone@discovery-other.test\"]}").Status,
            "A custodian outside the key's domains is refused.");
         ClassicAssert.AreEqual(403, AsKey(confined, "POST", CasePath(confinedCase) + "/searches", "{\"custodians\":[\"discovery-other.test\"]}").Status);
         ClassicAssert.AreEqual(403, AsKey(confined, "POST", CasePath(confinedCase) + "/searches", "{\"custodians\":[\"*\"]}").Status,
            "Every mailbox is outside a confined key's reach.");
         ClassicAssert.AreEqual(403, AsKey(confined, "POST", CasePath(confinedCase) + "/holds", "{\"scope\":\"account\",\"target\":\"someone@discovery-other.test\"}").Status);
         ClassicAssert.AreEqual(202, AsKey(confined, "POST", CasePath(confinedCase) + "/searches", "{\"custodians\":[\"" + alice.Address + "\"]}").Status,
            "The control: a custodian inside the key's domains.");

         // A case the confined key may not see - one that searched a domain outside it - is not there for it.
         long elsewhere = OpenCase("Elsewhere " + Word());
         Reply outside = Admin("POST", CasePath(elsewhere) + "/searches", "{\"custodians\":[\"discovery-other.test\"],\"scope\":\"archive\"}");
         ClassicAssert.AreEqual(202, outside.Status, outside.Body);
         ClassicAssert.AreEqual(404, AsKey(confined, "GET", CasePath(elsewhere), null).Status);
         ClassicAssert.AreEqual(200, Admin("GET", CasePath(elsewhere), null).Status);

         // And a search that read the archive is not read by a role without view_archive.
         long outsideSearch = outside.Json.GetProperty("id").GetInt64();
         WaitForSearch(elsewhere, outsideSearch);
         Reply notRead = AsKey(manager, "GET", CasePath(elsewhere) + "/searches/" + outsideSearch, null);
         ClassicAssert.AreEqual(403, notRead.Status, notRead.Body);
         StringAssert.Contains("view_archive", notRead.Body);
         ClassicAssert.AreEqual(200, AsKey(auditor, "GET", CasePath(elsewhere) + "/searches/" + outsideSearch, null).Status,
            "The control: the auditor holds view_archive.");
      }

      [Test]
      [Description("A search the query of which is not one is refused with the reason, and a closed case runs none; the case answers 404 for an id that is none.")]
      public void AQueryThatIsNotOneIsRefusedWithTheReason()
      {
         long caseId = OpenCase("Refusals " + Word());

         Reply noCustodian = Admin("POST", CasePath(caseId) + "/searches", "{\"words\":\"anything\"}");
         ClassicAssert.AreEqual(400, noCustodian.Status, noCustodian.Body);
         StringAssert.Contains("custodian", noCustodian.Body);

         ClassicAssert.AreEqual(400, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"not an address\"]}").Status);
         ClassicAssert.AreEqual(400, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"*\"],\"from\":\"yesterday\"}").Status);
         ClassicAssert.AreEqual(400, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"*\"],\"from\":\"2026-02-01\",\"to\":\"2026-01-01\"}").Status);
         ClassicAssert.AreEqual(400, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"*\"],\"scope\":\"everywhere\"}").Status);
         ClassicAssert.AreEqual(400, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"*\"],\"sender\":\"x\"}").Status);

         ClassicAssert.AreEqual(404, Admin("GET", CasePath(9999999), null).Status);

         ClassicAssert.AreEqual(200, Admin("POST", CasePath(caseId) + "/close", "{}").Status);
         ClassicAssert.AreEqual(409, Admin("POST", CasePath(caseId) + "/close", "{}").Status, "Closed once.");
         ClassicAssert.AreEqual(409, Admin("POST", CasePath(caseId) + "/searches", "{\"custodians\":[\"*\"]}").Status, "A closed case runs no search.");
      }

      [Test]
      [Description("DBUpdater's probes for schema 6097 - every column of the four discovery tables and their foreign keys - succeed on the bench's database, and a probe naming a column that is not there fails as a missing column rather than as a statement the provider could not read, its error-log line asserted and cleared.")]
      public void TheSchemaProbesForTheDiscoveryTablesSucceed()
      {
         Database database = _application.Database;
         var probes = SchemaVerification.GetProbesFor(6097).ToList();
         ClassicAssert.Greater(probes.Count, 40, "Every column of the four tables has its probe.");

         foreach (SchemaProbe probe in probes)
            database.ExecuteSQL(probe.Statement);

         SchemaProbe searchQuery = probes.First(p => p.Describes == "hm_discoverysearches.searchquery");
         string absent = searchQuery.Statement.Replace("searchquery", "searchnosuchcolumn");
         ClassicAssert.AreNotEqual(searchQuery.Statement, absent);

         try
         {
            var refused = Assert.Throws<COMException>(() => database.ExecuteSQL(absent), "The probe must fail for a column that is not there.");
            StringAssert.Contains("HM10044", refused.Message);
            StringAssert.DoesNotContain("HM10045", refused.Message, "A missing column, not a statement the provider could not read.");
         }
         finally
         {
            CustomAsserts.AssertReportedError("searchnosuchcolumn");
         }
      }

      // ---------------------------------------------------------------- helpers

      private void EnableArchive()
      {
         archiveRoot_ = Paths.Combine(Path.GetTempPath(), "hmail-discovery-archive-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(archiveRoot_);
         IniFileSetting.Write("ArchiveDir", archiveRoot_);
         _application.Reinitialize();
      }

      // A word nothing else on the server holds.
      private static string Word()
      {
         return "zq" + Guid.NewGuid().ToString("N").Substring(0, 10);
      }

      private static string Id(string label)
      {
         return label + "." + Guid.NewGuid().ToString("N").Substring(0, 12) + "@discovery.test";
      }

      private static string Raw(string to, string subject, string messageId, string date, string body)
      {
         return "From: Partner <" + Sender + ">\r\n" +
                "To: " + to + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "Date: " + date + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                body + "\r\n";
      }

      private static string WithAttachment(string to, string subject, string messageId, string date, string fileName, string type, byte[] content)
      {
         const string boundary = "discovery-boundary-0001";
         string encoded = Convert.ToBase64String(content, Base64FormattingOptions.InsertLineBreaks);

         return "From: Partner <" + Sender + ">\r\n" +
                "To: " + to + "\r\n" +
                "Subject: " + subject + "\r\n" +
                "Date: " + date + "\r\n" +
                "Message-ID: <" + messageId + ">\r\n" +
                "MIME-Version: 1.0\r\n" +
                "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n" +
                "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: text/plain; charset=us-ascii\r\n" +
                "\r\n" +
                "See the attachment.\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Type: " + type + "; name=\"" + fileName + "\"\r\n" +
                "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "\r\n" +
                encoded + "\r\n" +
                "--" + boundary + "--\r\n";
      }

      private static void Deliver(string to, string text)
      {
         new SmtpClientSimulator().SendRaw(Sender, to, text);
      }

      private static void ExpungeFirst(string address)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.IsTrue(imap.SetDeletedFlag(1));
         ClassicAssert.IsTrue(imap.Expunge());
         imap.Disconnect();
      }

      private static void AddDeleteRule(Account account, string subjectContains)
      {
         var rule = account.Rules.Add();
         rule.Name = "Discovery delete";
         rule.Active = true;

         var criterion = rule.Criterias.Add();
         criterion.UsePredefined = false;
         criterion.HeaderField = "Subject";
         criterion.MatchType = eRuleMatchType.eMTContains;
         criterion.MatchValue = subjectContains;
         criterion.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRADeleteEmail;
         action.Save();

         rule.Save();
      }

      // Account.SieveScript over COM, late-bound, as the Sieve fixtures set it.
      private static void SetSieveScript(object account, string script)
      {
         account.GetType().InvokeMember("SieveScript", BindingFlags.SetProperty, null, account, new object[] { script });
      }

      // Moves the stored creation time of every message the account holds back by
      // the given number of days, as the message retention fixture does.
      private void AgeEveryMessage(Account account, int days)
      {
         var created = DateTime.Now.AddDays(-days).ToString("yyyy-MM-dd HH:mm:ss");
         _application.Database.ExecuteSQL("update hm_messages set messagecreatetime = '" + created + "' where messageaccountid = " + account.ID);
      }

      // The files the account's inbox is stored in, from COM.
      private static List<string> Stored(Account account)
      {
         var files = new List<string>();
         Messages messages = account.IMAPFolders.get_ItemByName("INBOX").Messages;
         for (int index = 0; index < messages.Count; index++)
            files.Add(messages[index].Filename);
         return files;
      }

      // The copies the recoverable store keeps for the account (RecoverableStore.h:
      // <data directory>/Recoverable/<account id>/).
      private List<string> KeptFiles(Account account)
      {
         string directory = Paths.Combine(_settings.Directories.DataDirectory, "Recoverable", account.ID.ToString());
         return Directory.Exists(directory) ? Directory.GetFiles(directory).ToList() : new List<string>();
      }

      // Zero access switched on as the webmail switches it on, less the
      // derivations: the fixture key stored as the mailbox's own, then the switch
      // (SMTP/ZeroAccess.cs does the same).
      private static void SwitchZeroAccessOn(string address)
      {
         string me = Basic(address, Password);

         string ownKey = "{\"public_key\":\"" + OpenPgpTestSupport.RingOwnPublicBase64 + "\"," +
                         "\"secret_key\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"," +
                         "\"address\":\"" + address + "\",\"name\":\"Sealed\",\"source\":\"generated\",\"revoked\":false}";
         Reply stored = Send("PUT", "/api/v1/me/openpgp/keys/own", me, ownKey);
         ClassicAssert.AreEqual(201, stored.Status, stored.Body);

         string switchBody = "{\"fingerprint\":\"" + OpenPgpTestSupport.RingOwnFingerprint + "\",\"salt\":\"" + ZeroAccessSalt + "\",\"passes\":3,\"memory_kib\":65536,\"lanes\":4," +
                             "\"password_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\",\"recovery_wrapped\":\"" + Escaped(OpenPgpTestSupport.RingOwnSecretArmoured) + "\"}";
         Reply on = Send("PUT", "/api/v1/me/zero-access", me, switchBody);
         ClassicAssert.AreEqual(201, on.Status, on.Body);
      }

      private static string Escaped(string armoured)
      {
         return armoured.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n");
      }

      private List<string> DomainNames()
      {
         var names = new List<string>();
         hMailServer.Domains domains = _application.Domains;
         domains.Refresh();
         for (int index = 0; index < domains.Count; index++)
            names.Add(domains[index].Name.ToLowerInvariant());
         return names;
      }

      private string ReadBackupLog()
      {
         string logFile = _application.Settings.Backup.LogFile;

         try
         {
            if (!File.Exists(logFile))
               return "(no backup log)";

            // FileShare.ReadWrite: the server holds this log open.
            using (var stream = new FileStream(logFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(stream))
               return reader.ReadToEnd();
         }
         catch (IOException exception)
         {
            return "(backup log could not be read: " + exception.Message + ")";
         }
      }

      private bool WaitForBackupLog(string expected, int timeoutSeconds)
      {
         for (int attempt = 0; attempt < timeoutSeconds * 4; attempt++)
         {
            if (ReadBackupLog().Contains(expected))
               return true;

            Thread.Sleep(250);
         }

         return false;
      }

      private JsonElement Recoverable(string address)
      {
         Reply reply = Send("GET", "/api/v1/me/recoverable", Basic(address, Password), null);
         ClassicAssert.AreEqual(200, reply.Status, reply.Body);
         return reply.Json;
      }

      private static string CasePath(long caseId)
      {
         return "/api/v1/discovery/cases/" + caseId;
      }

      private static long OpenCase(string name)
      {
         Reply opened = Admin("POST", "/api/v1/discovery/cases", "{\"name\":\"" + name + "\",\"description\":\"Opened by the discovery fixture.\"}");
         ClassicAssert.AreEqual(201, opened.Status, opened.Body);
         ClassicAssert.AreEqual("open", opened.Json.GetProperty("state").GetString());
         return opened.Json.GetProperty("id").GetInt64();
      }

      private static List<long> CaseIds()
      {
         Reply cases = Admin("GET", "/api/v1/discovery/cases", null);
         if (cases.Status != 200)
            return new List<long>();

         return cases.Json.GetProperty("cases").EnumerateArray().Select(c => c.GetProperty("id").GetInt64()).ToList();
      }

      // The newest case's id, 0 when there is none, -1 when the listing failed.
      private static long NewestCaseId()
      {
         Reply cases = Admin("GET", "/api/v1/discovery/cases", null);
         if (cases.Status != 200)
            return -1;

         List<long> ids = cases.Json.GetProperty("cases").EnumerateArray().Select(c => c.GetProperty("id").GetInt64()).ToList();
         return ids.Count == 0 ? 0 : ids.Max();
      }

      private static void CloseEveryOpenCase()
      {
         Reply cases = Admin("GET", "/api/v1/discovery/cases", null);
         if (cases.Status != 200)
            return;

         foreach (JsonElement theCase in cases.Json.GetProperty("cases").EnumerateArray())
         {
            if (theCase.GetProperty("state").GetString() == "open")
               Admin("POST", CasePath(theCase.GetProperty("id").GetInt64()) + "/close", "{}");
         }
      }

      // Until no search or export of this test's cases is queued or running, for at
      // most two minutes: the reinitialise in the teardown must not cut one off.
      private void WaitForTheRunner()
      {
         if (firstCase_ < 0)
            return;

         for (int attempt = 0; attempt < 600; attempt++)
         {
            bool busy = false;

            foreach (long caseId in CaseIds().Where(candidate => candidate > firstCase_))
            {
               Reply reply = Admin("GET", CasePath(caseId), null);
               if (reply.Status != 200)
                  continue;

               JsonElement theCase = reply.Json;
               busy = theCase.GetProperty("searches").EnumerateArray().Concat(theCase.GetProperty("exports").EnumerateArray())
                  .Any(job => job.GetProperty("state").GetString() == "queued" || job.GetProperty("state").GetString() == "running");
               if (busy)
                  break;
            }

            if (!busy)
               return;

            Thread.Sleep(200);
         }
      }

      // The rows and the files of every case this test opened: a case is never
      // deleted by the server, and a fixture that ran every day would otherwise add
      // to what every later listing reads and every later backup would have carried.
      private void RemoveThisTestsCases()
      {
         if (firstCase_ < 0)
            return;

         string after = firstCase_.ToString();
         _application.Database.ExecuteSQL("delete from hm_discoveryexports where exportcaseid > " + after);
         _application.Database.ExecuteSQL("delete from hm_discoveryholds where holdcaseid > " + after);
         _application.Database.ExecuteSQL("delete from hm_discoverysearches where searchcaseid > " + after);
         _application.Database.ExecuteSQL("delete from hm_discoverycases where caseid > " + after);

         string root = Paths.Combine(_settings.Directories.DataDirectory, "Discovery");
         if (!Directory.Exists(root))
            return;

         foreach (string directory in Directory.GetDirectories(root, "case-*"))
         {
            if (long.TryParse(Path.GetFileName(directory).Substring("case-".Length), out long id) && id > firstCase_)
               Tidy(() => Directory.Delete(directory, true));
         }
      }

      // Runs a search and waits for it: the search's document with its hits.
      private static JsonElement RunSearch(long caseId, string query)
      {
         Reply queued = Admin("POST", CasePath(caseId) + "/searches", query);
         ClassicAssert.AreEqual(202, queued.Status, queued.Body);
         return WaitForSearch(caseId, queued.Json.GetProperty("id").GetInt64());
      }

      private static JsonElement WaitForSearch(long caseId, long searchId)
      {
         for (int attempt = 0; attempt < 300; attempt++)
         {
            Reply reply = Admin("GET", CasePath(caseId) + "/searches/" + searchId + "?limit=1000", null);
            ClassicAssert.AreEqual(200, reply.Status, reply.Body);

            string state = reply.Json.GetProperty("state").GetString();
            if (state == "done")
               return reply.Json;

            if (state == "failed")
               Assert.Fail("The search failed: " + reply.Body);

            Thread.Sleep(200);
         }

         Assert.Fail("The search did not finish within a minute.");
         return default(JsonElement);
      }

      private static JsonElement WaitForExport(long caseId, long exportId)
      {
         for (int attempt = 0; attempt < 300; attempt++)
         {
            Reply reply = Admin("GET", CasePath(caseId), null);
            ClassicAssert.AreEqual(200, reply.Status, reply.Body);

            JsonElement export = reply.Json.GetProperty("exports").EnumerateArray().Single(e => e.GetProperty("id").GetInt64() == exportId);
            string state = export.GetProperty("state").GetString();
            if (state == "done")
               return export;

            if (state == "failed")
               Assert.Fail("The export failed: " + export.GetProperty("error").GetString());

            Thread.Sleep(200);
         }

         Assert.Fail("The export did not finish within a minute.");
         return default(JsonElement);
      }

      private static List<JsonElement> HitsOf(JsonElement search)
      {
         return search.GetProperty("hits").EnumerateArray().ToList();
      }

      // Each hit as kind:message-id, the message id without its angle brackets.
      private static List<string> Hits(JsonElement search)
      {
         return HitsOf(search).Select(h => h.GetProperty("kind").GetString() + ":" + h.GetProperty("message_id").GetString().Trim('<', '>')).ToList();
      }

      private static byte[] ReadEntry(ZipArchiveEntry entry)
      {
         using (Stream stream = entry.Open())
         using (var memory = new MemoryStream())
         {
            stream.CopyTo(memory);
            return memory.ToArray();
         }
      }

      private static string Sha256(byte[] bytes)
      {
         using (SHA256 hasher = SHA256.Create())
            return string.Concat(hasher.ComputeHash(bytes).Select(b => b.ToString("x2")));
      }

      private string MakeKey(string label, string scope, string role, string domains = null)
      {
         var body = new StringBuilder("{\"label\":\"" + label + "\"");
         if (scope != null)
            body.Append(",\"scope\":\"" + scope + "\"");
         if (role != null)
            body.Append(",\"role\":\"" + role + "\"");
         if (domains != null)
            body.Append(",\"domains\":\"" + domains + "\"");
         body.Append("}");

         Reply created = Admin("POST", "/api/v1/apikeys", body.ToString());
         ClassicAssert.AreEqual(201, created.Status, created.Body);

         _keys.Add(created.Json.GetProperty("id").GetString());
         return created.Json.GetProperty("key").GetString();
      }

      private void MakeRole(string name, string permissions)
      {
         Reply role = Admin("POST", "/api/v1/roles",
            "{\"name\":\"" + name + "\",\"description\":\"discovery fixture\",\"permissions\":\"" + permissions + "\"}");
         ClassicAssert.AreEqual(201, role.Status, role.Body);
         _roles.Add(name);
      }

      private static void Tidy(Action step)
      {
         try
         {
            step();
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            Console.WriteLine("Discovery teardown: " + tidying.Message);
         }
      }

      private sealed class Reply
      {
         public int Status;
         public string Headers = "";
         public byte[] Bytes = new byte[0];

         public string Body
         {
            get { return Encoding.UTF8.GetString(Bytes); }
         }

         public string Header(string name)
         {
            foreach (string line in Headers.Split(new[] { "\r\n" }, StringSplitOptions.None))
            {
               int colon = line.IndexOf(':');
               if (colon > 0 && string.Equals(line.Substring(0, colon).Trim(), name, StringComparison.OrdinalIgnoreCase))
                  return line.Substring(colon + 1).Trim();
            }

            return "";
         }

         public JsonElement Json
         {
            get
            {
               using (JsonDocument document = JsonDocument.Parse(Body))
                  return document.RootElement.Clone();
            }
         }
      }

      private static string Basic(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private static Reply Admin(string method, string path, string requestBody)
      {
         return Send(method, path, Basic("Administrator", AdminPassword), requestBody);
      }

      private static Reply AsKey(string token, string method, string path, string requestBody)
      {
         return Send(method, path, "Bearer " + token, requestBody);
      }

      // One request, the answer's header block and its body's bytes kept apart: an
      // export's body is a zip, which is not text.
      private static Reply Send(string method, string path, string authorization, string requestBody)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", RestPort);

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);

               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1:" + RestPort + "\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (requestBody != null)
               {
                  head.Append("Content-Type: application/json\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }
               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[65536];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               byte[] raw = memory.ToArray();
               int separator = -1;
               for (int i = 0; i + 3 < raw.Length; i++)
               {
                  if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n')
                  {
                     separator = i;
                     break;
                  }
               }

               var reply = new Reply
               {
                  Headers = Encoding.ASCII.GetString(raw, 0, separator >= 0 ? separator : raw.Length),
                  Bytes = separator >= 0 ? raw.Skip(separator + 4).ToArray() : new byte[0]
               };

               string[] statusLine = reply.Headers.Split(new[] { "\r\n" }, StringSplitOptions.None)[0].Split(' ');
               if (statusLine.Length >= 2)
                  int.TryParse(statusLine[1], out reply.Status);

               return reply;
            }
         }
      }
   }
}
