// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    Somebody's mail read through an administrator's route is on the audit
   ///    trail: the folder tree, a folder's permissions, every message, a folder's
   ///    messages, a message's header fields and its source under
   ///    /api/v1/accounts/{address}, the mailbox exported to the server's disk or
   ///    downloaded whole, a page of an eDiscovery search's hits, and a support
   ///    session opened as the account. The person reading their own mail through
   ///    /api/v1/me is not.
   ///
   ///    Until 6.3.6 none of them was. The trail recorded changes, and a read
   ///    changes nothing, so the server administrator - or a domain administrator,
   ///    in its own domain - could list anybody's mailbox and download every
   ///    message in it, and the one record built to answer "whose mail did this
   ///    administrator open" had nothing to say. Every test here but the
   ///    refusal's fails against that server, because the rows it looks for are
   ///    not there; the refusal's fails against one that records a read whatever
   ///    it was answered.
   ///
   ///    A read is recorded once a minute for the same credential, from the same
   ///    address, of the same thing, so that a page read again at every click and
   ///    a listing paged do not bury the trail; a different message, a different
   ///    folder or a different credential is a row at once. The tests hold both
   ///    halves of that, and that a refusal - a message or a folder that is not the
   ///    account's - read nothing and is not recorded.
   ///
   ///    Each test reads a mailbox of its own, under an address no other test and
   ///    no earlier run uses, and finds its rows by that address: the trail is not
   ///    emptied here, because against the Linux server it cannot be, and because
   ///    the window the server keeps is per account and would otherwise hold a row
   ///    back for a read another test made.
   /// </summary>
   [TestFixture]
   public class RestApiAuditMailboxReads : TestFixtureBase
   {
      // A port of this fixture's own, as every REST fixture has one; the suite's
      // own against the Linux server.
      private static int RestPort = 9132;
      private const string AdminPassword = "testar";
      private const string UserPassword = "test";

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);

         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status");
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("Each of the six reads of another account's mailbox - its folders, a folder's permissions, every message, a folder's messages, a message's header fields and its source - is one row naming the account, the credential, the interface and the address it came from, and what was read, by id and never by subject.")]
      public void EachReadOfAnotherAccountsMailboxIsOnTheTrail()
      {
         Account victim = AddAccount("read");
         long inbox = InboxOf(victim);
         long message = Added(victim, inbox, "Quarterly figures");

         string mailbox = "/api/v1/accounts/" + victim.Address;
         string[] paths =
         {
            mailbox + "/folders",
            mailbox + "/folders/" + inbox + "/permissions",
            mailbox + "/messages",
            mailbox + "/folders/" + inbox + "/messages",
            mailbox + "/messages/" + message,
            mailbox + "/messages/" + message + "/source"
         };

         foreach (string path in paths)
         {
            (int status, string body) read = Admin("GET", path);
            Assert.AreEqual(200, read.status, "GET " + path + ": " + read.body);
         }

         List<Dictionary<string, object>> rows = Reads(victim.Address);

         string[] details =
         {
            "its folders listed",
            "the permissions of folder " + inbox + " listed",
            "every message listed",
            "the messages of folder " + inbox + " listed",
            "message " + message + ": its header fields read",
            "message " + message + ": its source read"
         };

         foreach (string detail in details)
         {
            List<Dictionary<string, object>> named = rows.Where(row => MiniJson.Text(row, "detail").StartsWith(detail, StringComparison.Ordinal)).ToList();
            Assert.AreEqual(1, named.Count, "One row for '" + detail + "' among: " + Describe(rows));

            Dictionary<string, object> row = named[0];
            Assert.AreEqual("account", MiniJson.Text(row, "object_type"));
            Assert.AreEqual("mailbox-read", MiniJson.Text(row, "action"));
            Assert.AreEqual("administrator", MiniJson.Text(row, "actor"), "The REST layer names the credential the way its own log line does.");
            Assert.AreEqual("administrator", MiniJson.Text(row, "actor_type"));
            Assert.AreEqual("REST", MiniJson.Text(row, "interface"));
            Assert.AreEqual("127.0.0.1", MiniJson.Text(row, "address"), "A read made over the network records where it came from.");
         }

         Assert.AreEqual(details.Length, rows.Count, "Six reads, six rows: " + Describe(rows));

         foreach (Dictionary<string, object> row in rows)
         {
            StringAssert.DoesNotContain("Quarterly", MiniJson.Text(row, "detail"),
               "The trail says which message was read, by id; a subject in it would make the trail a copy of the mail.");
         }
      }

      [Test]
      [Description("The same read by the same credential from the same address within a minute is the row already written - a listing read three times is one row - while another message, or the other half of the same message, is a row of its own at once.")]
      public void TheSameReadAgainWithinAMinuteIsTheSameRowAndAnotherIsARowAtOnce()
      {
         Account victim = AddAccount("window");
         long inbox = InboxOf(victim);
         long first = Added(victim, inbox, "First");
         long second = Added(victim, inbox, "Second");

         string mailbox = "/api/v1/accounts/" + victim.Address;

         for (int i = 0; i < 3; i++)
            Assert.AreEqual(200, Admin("GET", mailbox + "/messages").status);

         Assert.AreEqual(1, Count(victim.Address, "every message listed"),
            "A listing read again and again - a page redrawn, a client paging - is one row a minute: " + Describe(Reads(victim.Address)));

         Assert.AreEqual(200, Admin("GET", mailbox + "/messages/" + first).status);
         Assert.AreEqual(200, Admin("GET", mailbox + "/messages/" + second).status);
         Assert.AreEqual(200, Admin("GET", mailbox + "/messages/" + first).status);

         Assert.AreEqual(1, Count(victim.Address, "message " + first + ": its header fields read"), Describe(Reads(victim.Address)));
         Assert.AreEqual(1, Count(victim.Address, "message " + second + ": its header fields read"),
            "Another message is another row, at once: the trail has to say which messages were read, and not only that one was.");

         Assert.AreEqual(200, Admin("GET", mailbox + "/messages/" + first + "/source").status);
         Assert.AreEqual(1, Count(victim.Address, "message " + first + ": its source read"),
            "The source of a message whose header fields were read a moment ago is a row of its own: the body is more than the headers.");

         Assert.AreEqual(4, Reads(victim.Address).Count, Describe(Reads(victim.Address)));
      }

      [Test]
      [Description("A read that found nothing - a message, a folder or a source that is not the account's - is refused and leaves no row.")]
      public void ARefusedReadIsNotRecorded()
      {
         Account victim = AddAccount("refused");
         Account other = AddAccount("other");
         long othersMessage = Added(other, InboxOf(other), "Not the victim's");

         string mailbox = "/api/v1/accounts/" + victim.Address;
         string[] paths =
         {
            mailbox + "/messages/999999999",
            mailbox + "/messages/" + othersMessage,
            mailbox + "/messages/" + othersMessage + "/source",
            mailbox + "/folders/999999999/messages",
            mailbox + "/folders/999999999/permissions"
         };

         foreach (string path in paths)
         {
            (int status, string body) refused = Admin("GET", path);
            Assert.AreEqual(404, refused.status, "GET " + path + ": " + refused.body);
         }

         Assert.AreEqual(0, Reads(victim.Address).Count, "Nothing was read, so nothing is recorded: " + Describe(Reads(victim.Address)));
         Assert.AreEqual(0, Reads(other.Address).Count, "A message reached through another account's address is not found, and not a read of its own account either: " + Describe(Reads(other.Address)));
      }

      [Test]
      [Description("A domain administrator's read of a mailbox in its domain is recorded under its own address, with the role it read under; the server administrator reading the same listing a moment later is a row of its own.")]
      public void ADomainAdministratorsReadIsRecordedUnderItsOwnNameAndRole()
      {
         Account victim = AddAccount("da-victim");
         Added(victim, InboxOf(victim), "For the domain administrator");

         Account domainAdmin = AddAccount("da");
         domainAdmin.AdminLevel = eAdminLevel.hAdminLevelDomainAdmin;
         domainAdmin.Save();

         string listing = "/api/v1/accounts/" + victim.Address + "/messages";

         (int status, string body) asDomainAdmin = As(domainAdmin.Address, "GET", listing);
         Assert.AreEqual(200, asDomainAdmin.status, asDomainAdmin.body);

         List<Dictionary<string, object>> rows = Reads(victim.Address);
         Assert.AreEqual(1, rows.Count, Describe(rows));
         Assert.AreEqual("account:" + domainAdmin.Address, MiniJson.Text(rows[0], "actor"));
         Assert.AreEqual("administrator", MiniJson.Text(rows[0], "actor_type"), "A domain administrator's role is an administrator's.");
         StringAssert.StartsWith("every message listed", MiniJson.Text(rows[0], "detail"));
         StringAssert.EndsWith("[role: domainadmin]", MiniJson.Text(rows[0], "detail"), "Who read it, and with what authority.");

         Assert.AreEqual(200, Admin("GET", listing).status);
         rows = Reads(victim.Address);
         Assert.AreEqual(2, rows.Count, "Another credential's read of the same listing inside the minute is its own row: " + Describe(rows));
         Assert.AreEqual("administrator", MiniJson.Text(rows[0], "actor"), "Newest first.");
      }

      [Test]
      [Description("The mailbox exported to the server's disk, and downloaded whole as an archive, is recorded every time it is done, with where the export went.")]
      public void AnExportIsRecordedEveryTime()
      {
         Account victim = AddAccount("export");
         long inbox = InboxOf(victim);
         Added(victim, inbox, "Exported one");
         Added(victim, inbox, "Exported two");

         // Two exports one after the other, inside the minute: each is the whole
         // mailbox read and a copy left behind, and each is a row.
         string[] directories =
         {
            Path.Combine(Path.GetTempPath(), "hm-audit-export-" + TestSetup.UniqueString()),
            Path.Combine(Path.GetTempPath(), "hm-audit-export-" + TestSetup.UniqueString())
         };

         try
         {
            foreach (string directory in directories)
            {
               string body = "{\"directory\":\"" + directory.Replace("\\", "\\\\") + "\"}";
               (int status, string answer) exported = Admin("POST", "/api/v1/accounts/" + victim.Address + "/export", body);
               Assert.AreEqual(200, exported.status, exported.answer);
            }
         }
         finally
         {
            foreach (string directory in directories)
            {
               if (Directory.Exists(directory))
                  Directory.Delete(directory, true);
            }
         }

         foreach (string directory in directories)
         {
            Assert.AreEqual(1, Count(victim.Address, "2 message(s) exported as .eml files to " + directory),
               "An export is recorded with where the copy went: " + Describe(Reads(victim.Address)));
         }

         // The same download twice inside the minute is two rows: the window that
         // keeps a listing paged to one row a minute is not for a whole mailbox
         // taken away.
         for (int i = 0; i < 2; i++)
         {
            (int status, string headers, string body) downloaded = RestRequest.Exchange("127.0.0.1", RestPort, "GET",
               "/api/v1/accounts/" + Uri.EscapeDataString(victim.Address) + "/export.zip", AdminAuthorization(), null, null);
            Assert.AreEqual(200, downloaded.status, downloaded.headers);
         }

         Assert.AreEqual(2, Count(victim.Address, "everything held on it downloaded as one archive"), Describe(Reads(victim.Address)));
      }

      [Test]
      [Description("An export that stops part of the way - a message whose file is gone from the store - has read and written out the mail before it, which stays on the server's disk: it is a row too, in both forms, saying where it went and how far it got.")]
      public void AnExportThatStopsPartOfTheWayIsRecorded()
      {
         Account victim = AddAccount("stopped");
         long inbox = InboxOf(victim);
         Added(victim, inbox, "Written out before the stop");
         long second = Added(victim, inbox, "Whose file is gone");

         // The second message's file taken away beneath its row, as a store that
         // has lost a file is: the export copies the first message and stops at
         // the second, and the first stays where it was written.
         (int status, string body) read = Admin("GET", "/api/v1/accounts/" + victim.Address + "/messages/" + second);
         Assert.AreEqual(200, read.status, read.body);
         string file = MiniJson.Text(MiniJson.Object(read.body), "file");
         Assert.IsTrue(File.Exists(file), "The message file is where the row says before it is removed: " + file);
         File.Delete(file);

         string directory = Path.Combine(Path.GetTempPath(), "hm-audit-stopped-" + TestSetup.UniqueString());
         string archive = Path.Combine(Path.GetTempPath(), "hm-audit-stopped-" + TestSetup.UniqueString() + ".zip");

         try
         {
            (int status, string answer) toDirectory = Admin("POST", "/api/v1/accounts/" + victim.Address + "/export",
               "{\"directory\":\"" + directory.Replace("\\", "\\\\") + "\"}");
            Assert.AreEqual(500, toDirectory.status, "The export stops at the missing file: " + toDirectory.answer);
            Assert.AreEqual(1, Directory.GetFiles(directory, "*.eml", SearchOption.AllDirectories).Length,
               "The message before the missing file was written out and stays: what the row has to account for.");

            // The copy that failed is reported, as every failed copy of a file is;
            // the archive's read of the same file fails without a report.
            CustomAsserts.AssertReportedError("Could not copy the file");

            (int status, string answer) toArchive = Admin("POST", "/api/v1/accounts/" + victim.Address + "/export",
               "{\"archive\":\"" + archive.Replace("\\", "\\\\") + "\"}");
            Assert.AreEqual(500, toArchive.status, "The archive's export stops at the missing file too: " + toArchive.answer);
         }
         finally
         {
            if (Directory.Exists(directory))
               Directory.Delete(directory, true);

            if (File.Exists(archive))
               File.Delete(archive);
         }

         Assert.AreEqual(1, Count(victim.Address, "an export as .eml files to " + directory + " stopped with an error after 1 message(s)"),
            "An export that stopped is recorded with where it went and how far it got: " + Describe(Reads(victim.Address)));
         Assert.AreEqual(1, Count(victim.Address, "an export of everything held on it to the archive " + archive + " stopped with an error"),
            "So is the archive's: " + Describe(Reads(victim.Address)));
      }

      [Test]
      [Description("A support session opened as the account - the webmail, as the person, from the administrator's browser - is recorded under the credential that opened it.")]
      public void OpeningASupportSessionIsRecorded()
      {
         Account victim = AddAccount("support");

         (int status, string body) allowed = As(victim.Address, "PUT", "/api/v1/me/preferences", "{\"support_allowed\":\"1\"}");
         Assert.AreEqual(200, allowed.status, allowed.body);

         (int status, string body) opened = Admin("POST", "/api/v1/accounts/" + victim.Address + "/support-session");
         Assert.AreEqual(201, opened.status, opened.body);

         (int status, string body) trail = Admin("GET", "/api/v1/audit?object_type=account&action=support-session-opened&limit=1000");
         Assert.AreEqual(200, trail.status, trail.body);

         List<Dictionary<string, object>> rows = MiniJson.List(MiniJson.Object(trail.body), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "object") == victim.Address)
            .ToList();

         Assert.AreEqual(1, rows.Count, "The opening of a support session is on the trail: " + trail.body);
         Assert.AreEqual("administrator", MiniJson.Text(rows[0], "actor"));
         Assert.AreEqual("127.0.0.1", MiniJson.Text(rows[0], "address"));
         StringAssert.StartsWith("a support session opened in the webmail as the account", MiniJson.Text(rows[0], "detail"));
      }

      [Test]
      [Description("A page of an eDiscovery search's hits read - the custodians' mail as the search found it - is a row naming the case and the search, under the credential that read it; reading the same search again inside the minute, on another page, is the row already written, and a search whose result holds nothing to read leaves no row.")]
      public void AnEDiscoverySearchsHitsReadAreOnTheTrail()
      {
         Account custodian = AddAccount("custodian");
         long inbox = InboxOf(custodian);
         string word = "zq" + TestSetup.UniqueString().Substring(0, 10).ToLowerInvariant();
         Added(custodian, inbox, "Minutes " + word);
         Added(custodian, inbox, "Agenda " + word);

         string name = "Audited reads " + word;
         (int status, string body) opened = Admin("POST", "/api/v1/discovery/cases", "{\"name\":\"" + name + "\",\"description\":\"RestApiAuditMailboxReads\"}");
         Assert.AreEqual(201, opened.status, opened.body);
         long caseId = MiniJson.Number(MiniJson.Object(opened.body), "id");
         string casePath = "/api/v1/discovery/cases/" + caseId;

         try
         {
            long found = QueuedSearch(casePath, "{\"custodians\":[\"" + custodian.Address + "\"],\"words\":\"" + word + "\",\"scope\":\"mailboxes\"}");
            long none = QueuedSearch(casePath, "{\"custodians\":[\"" + custodian.Address + "\"],\"words\":\"" + word + "nothing\",\"scope\":\"mailboxes\"}");

            // Polled until done, as the Deck and hmctl do: a search still running
            // answers no hit, and the poll that finds it done is the read.
            Assert.AreEqual(2, MiniJson.List(SearchWhenDone(casePath, found, ""), "hits").Count, "Both messages are hits.");
            Assert.AreEqual(0, MiniJson.List(SearchWhenDone(casePath, none, ""), "hits").Count, "The other search finds nothing.");

            // The same search paged through inside the minute: the row already written.
            for (int offset = 0; offset < 2; offset++)
            {
               (int status, string body) page = Admin("GET", casePath + "/searches/" + found + "?offset=" + offset + "&limit=1");
               Assert.AreEqual(200, page.status, page.body);
               Assert.AreEqual(1, MiniJson.List(MiniJson.Object(page.body), "hits").Count, page.body);
            }

            List<Dictionary<string, object>> rows = DiscoveryReads("case " + caseId + ": " + name);
            Assert.AreEqual(1, rows.Count, "One row for the search read, however it was paged, and none for the search with nothing in it: " + Describe(rows));
            Assert.AreEqual("search " + found + ": what it found read - 2 hit(s), and 0 message(s) it could not search [role: serveradmin]",
               MiniJson.Text(rows[0], "detail"), "What was read, and the role the credential read it under.");
            Assert.AreEqual("administrator", MiniJson.Text(rows[0], "actor"));
            Assert.AreEqual("REST", MiniJson.Text(rows[0], "interface"));
            Assert.AreEqual("127.0.0.1", MiniJson.Text(rows[0], "address"));
            StringAssert.DoesNotContain("Minutes", MiniJson.Text(rows[0], "detail"), "The row names the search, not what it found.");
         }
         finally
         {
            Admin("POST", casePath + "/close", "{}");
         }
      }

      [Test]
      [Description("An account reading its own mail through /api/v1/me - its folders, a folder's messages, a message and its source - is the person's own and leaves no row; the same message read through the administrator's route a moment later is one.")]
      public void AnAccountReadingItsOwnMailIsNotRecorded()
      {
         Account owner = AddAccount("own");
         long inbox = InboxOf(owner);
         long message = Added(owner, inbox, "My own");

         string[] paths =
         {
            "/api/v1/me/folders",
            "/api/v1/me/folders/" + inbox + "/messages",
            "/api/v1/me/messages/" + message,
            "/api/v1/me/messages/" + message + "/source"
         };

         foreach (string path in paths)
         {
            (int status, string body) read = As(owner.Address, "GET", path);
            Assert.AreEqual(200, read.status, "GET " + path + ": " + read.body);
         }

         Assert.AreEqual(0, Reads(owner.Address).Count, "The person reading their own mail is not an administrator reading it: " + Describe(Reads(owner.Address)));

         // Nor is a read recorded under the person's own name, of any kind.
         (int status, string body) trail = Admin("GET", "/api/v1/audit?actor=" + Uri.EscapeDataString(owner.Address) + "&limit=1000");
         Assert.AreEqual(200, trail.status, trail.body);
         List<string> readsByOwner = MiniJson.List(MiniJson.Object(trail.body), "entries")
            .Cast<Dictionary<string, object>>()
            .Select(row => MiniJson.Text(row, "action"))
            .Where(action => action == "mailbox-read" || action == "read" || action == "searched")
            .ToList();
         Assert.AreEqual(0, readsByOwner.Count, "The person reading their own mail records no read under their name: " + trail.body);

         // The rows are looked for where they would be: the same message read
         // through the administrator's route is one.
         Assert.AreEqual(200, Admin("GET", "/api/v1/accounts/" + owner.Address + "/messages/" + message + "/source").status);
         Assert.AreEqual(1, Count(owner.Address, "message " + message + ": its source read"), Describe(Reads(owner.Address)));
      }

      [Test]
      [Description("The minute is a minute: the same read made again once it is over is a row again.")]
      public void AReadRecordedAMinuteAgoIsRecordedAgain()
      {
         Account victim = AddAccount("minute");
         string listing = "/api/v1/accounts/" + victim.Address + "/messages";

         Assert.AreEqual(200, Admin("GET", listing).status);
         Assert.AreEqual(1, Count(victim.Address, "every message listed"), Describe(Reads(victim.Address)));

         // The window is sixty seconds, measured on the server's monotonic clock.
         Thread.Sleep(TimeSpan.FromSeconds(62));

         Assert.AreEqual(200, Admin("GET", listing).status);
         Assert.AreEqual(2, Count(victim.Address, "every message listed"),
            "A read held back for a minute and then never again would let anybody who reads slowly enough read unrecorded: " + Describe(Reads(victim.Address)));
      }

      // ---- helpers -----------------------------------------------------------

      // An address of the test's own, which no other test and no earlier run of
      // this one has read: see the summary.
      private Account AddAccount(string name)
      {
         string address = "audit-" + name + "-" + TestSetup.UniqueString().Substring(0, 12) + "@" + _domain.Name;
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, UserPassword);
      }

      // The account's INBOX, by the account's own view of its folders - over COM
      // on Windows, and over the account's own /api/v1/me/folders against the
      // Linux server - so that finding it is not itself a read on the trail.
      private static long InboxOf(Account account)
      {
         return account.IMAPFolders.get_ItemByName("INBOX").ID;
      }

      // A message written into the folder through the administrator's route,
      // answered with its id: the add is a write, and is not a read.
      private static long Added(Account account, long folderId, string subject)
      {
         string raw = "From: sender@example.com\r\n" +
                      "To: " + account.Address + "\r\n" +
                      "Subject: " + subject + "\r\n" +
                      "Date: Mon, 14 Sep 2026 10:00:00 +0000\r\n" +
                      "\r\n" +
                      "The body of " + subject + ".\r\n";

         (int status, string body) created = Admin("POST", "/api/v1/accounts/" + account.Address + "/folders/" + folderId + "/messages", raw);
         Assert.AreEqual(201, created.status, created.body);
         return MiniJson.Number(MiniJson.Object(created.body), "id");
      }

      // The mailbox-read rows naming the address, newest first.
      private static List<Dictionary<string, object>> Reads(string address)
      {
         (int status, string body) trail = Admin("GET", "/api/v1/audit?object_type=account&action=mailbox-read&limit=1000");
         Assert.AreEqual(200, trail.status, trail.body);

         return MiniJson.List(MiniJson.Object(trail.body), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "object") == address)
            .ToList();
      }

      // An eDiscovery search queued in the case, answered with its id.
      private static long QueuedSearch(string casePath, string query)
      {
         (int status, string body) queued = Admin("POST", casePath + "/searches", query);
         Assert.AreEqual(202, queued.status, queued.body);
         return MiniJson.Number(MiniJson.Object(queued.body), "id");
      }

      // The search once it is done, read as the Deck and hmctl poll it.
      private static Dictionary<string, object> SearchWhenDone(string casePath, long searchId, string query)
      {
         for (int attempt = 0; attempt < 300; attempt++)
         {
            (int status, string body) reply = Admin("GET", casePath + "/searches/" + searchId + query);
            Assert.AreEqual(200, reply.status, reply.body);

            Dictionary<string, object> search = MiniJson.Object(reply.body);
            string state = MiniJson.Text(search, "state");
            if (state == "done")
               return search;

            Assert.AreNotEqual("failed", state, reply.body);
            Thread.Sleep(200);
         }

         Assert.Fail("The search did not finish within a minute.");
         return null;
      }

      // The rows of eDiscovery searches read in the case, newest first.
      private static List<Dictionary<string, object>> DiscoveryReads(string caseLabel)
      {
         (int status, string body) trail = Admin("GET", "/api/v1/audit?object_type=" + Uri.EscapeDataString("discovery search") + "&action=read&limit=1000");
         Assert.AreEqual(200, trail.status, trail.body);

         return MiniJson.List(MiniJson.Object(trail.body), "entries")
            .Cast<Dictionary<string, object>>()
            .Where(row => MiniJson.Text(row, "object") == caseLabel)
            .ToList();
      }

      // How many of them say what was read in these words.
      private static int Count(string address, string detail)
      {
         return Reads(address).Count(row => MiniJson.Text(row, "detail").StartsWith(detail, StringComparison.Ordinal));
      }

      private static string Describe(List<Dictionary<string, object>> rows)
      {
         return rows.Count == 0
            ? "(no rows)"
            : string.Join(" | ", rows.Select(row => MiniJson.Text(row, "actor") + ": " + MiniJson.Text(row, "detail")));
      }

      private static string AdminAuthorization()
      {
         return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
      }

      private static (int status, string body) Admin(string method, string path, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, AdminAuthorization(), requestBody);
      }

      private static (int status, string body) As(string address, string method, string path, string requestBody = null)
      {
         string credentials = Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + UserPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, "Basic " + credentials, requestBody,
                                 method == "GET" ? null : "X-Requested-With: hMailServer");
      }
   }
}
