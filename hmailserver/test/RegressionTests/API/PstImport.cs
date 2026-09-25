// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.API
{
   /// <summary>
   ///    PST import (docs/Migration.md, "Outlook data files (PST)"): a migration of kind
   ///    pst, a job per file, read by the server's own PST reader (Common/Pst) from the
   ///    target domain's subdirectory of pst_import_directory and filed as the IMAP
   ///    migration files a message.
   ///
   ///    The files are build/fixtures/pst/*.pst, which build/generate-pst-fixtures.py
   ///    writes from Microsoft's [MS-PST] with the standard library alone; the script's
   ///    --check proves the committed bytes are its output. Each test copies what it
   ///    needs into a scratch pst_import_directory of its own, with a subdirectory
   ///    per domain, sets the setting over REST, and puts it back and removes the
   ///    directory in TearDown. Everything asserted is read back from the target
   ///    account over IMAP, or from the job's report over REST.
   ///
   ///    The message NIDs a test names (0x86a4, the draft) are the ones the fixture
   ///    script gives: a folder's NIDs follow the order it writes them in, and the
   ///    folders added after those NIDs were named carry NIDs of their own.
   ///
   ///    The owner's sample (C:\Users\chris\Downloads\sample.pst, or HMTEST_PST_SAMPLE)
   ///    is a file from outside, never committed: its test is ignored where it is not
   ///    there, and asserts its shape, not its text. The test copies it into its
   ///    scratch import directory, since a job reads only inside that directory, and
   ///    TearDown removes the copy with the directory. This fixture is in the API
   ///    namespace, the half of the split gate that runs on the machine the sample is
   ///    on, and it is not linked into LinuxRegressionTests, which has no COM.
   ///
   ///    Port: the REST listener on 10150 (or the suite's own). 10151 to 10159 are this
   ///    row's and unused. The contacts, calendar and tasks test also starts the web
   ///    services listener, for CardDAV, on 10190 - the extension's range, 10190 to
   ///    10199 - and closes it again.
   /// </summary>
   [TestFixture]
   public class PstImport : TestFixtureBase
   {
      private static int RestPort = 10150;
      private const int DavPort = 10190;
      private const string AdminPassword = "testar";
      private const string TargetPassword = "Target-Pass-1";
      private const string RecordKey = "5053545f666978747572655f72656331";
      private const string SampleSha256 = "EE997FC7DD5C40BEF49B753B782F76B17109057B18C19232CC87E0B63E0711FE";

      // The special folders of standard.pst are under names IMAP's name table does
      // not hold, so that only their EntryIDs give them their use: as the report
      // names them, and as LIST and SELECT do (modified UTF-7).
      private const string Geloeschte = "Gel\u00f6schte Elemente";
      private const string GeloeschteListed = "Gel&APY-schte Elemente";
      private const string Entwuerfe = "Entw\u00fcrfe";
      private const string EntwuerfeListed = "Entw&APw-rfe";

      private string _directory;
      private readonly List<string> _scratch = new List<string>();

      [SetUp]
      public void StartListener()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/migrations");
         Assert.AreEqual(200, probe.status, "The REST listener did not answer GET /api/v1/migrations. Body: " + probe.body);

         RemoveEveryMigration();

         _directory = Scratch();
         Directory.CreateDirectory(Path.Combine(_directory, "example.test"));
         SetImportDirectory(_directory);
      }

      [TearDown]
      public void StopListener()
      {
         Exception first = null;
         try
         {
            try
            {
               RemoveEveryMigration();
            }
            catch (Exception e)
            {
               first = e;
            }

            // Put back whatever happened above: a setting left naming a
            // directory the finally removes would change every later fixture.
            try
            {
               SetImportDirectory("");
            }
            catch (Exception e)
            {
               if (first == null)
                  first = e;
            }
         }
         finally
         {
            foreach (string directory in _scratch)
               RemoveDirectory(directory);
            _scratch.Clear();

            RestListener.Stop();
            _application.Reinitialize();
         }

         if (first != null)
            throw first;
      }

      // ------------------------------------------------------------ the tests

      [Test]
      [Description("The standard fixture arrives whole: the IPM subtree's folders at their paths, the special folders the store names by EntryID given their use, contacts and a calendar counted and not made, every message once with its flags, keywords and internal date, the headers built or kept as the file had them, the bodies and attachments intact, and the report saying what was thinner.")]
      public void TheStandardFileArrivesWithItsFoldersFlagsAndDates()
      {
         Target("pst");
         Place("standard.pst");

         long migration = CreateMigration("Standard", "");
         AddJobs(migration, "standard.pst,,pst@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));

         string d = _settings.IMAPHierarchyDelimiter;

         // The folders: the IPM subtree's children at the top, the tree below them,
         // the names made ones this server can hold; nothing from outside the
         // subtree, no search folder, and no contacts or calendar folder.
         string list = List("pst@example.test");
         foreach (string name in new[] { "INBOX", "Gesendete Elemente", "Sent Items", GeloeschteListed, EntwuerfeListed, "Junk-E-Mail",
                                         "Junk E-mail", "Projects", "Projects" + d + "2025", "Projects" + d + "2025" + d + "Q3", "_#hashtag",
                                         "&ANw-berpr&APw-fung &JxM-", Dotted(d), "Extras", "Deep" })
            Assert.IsNotNull(ListLine(list, name), "No folder " + name + " in " + list);
         foreach (string name in new[] { "Top of Personal Folders", "Search Root", "A search folder", "Outside the subtree", "Contacts", "Calendar" })
            Assert.IsNull(ListLine(list, name), "A folder " + name + " was made: " + list);

         // The special folders by EntryID, not by name: each is under a name the
         // name table does not hold, and the ordinary folders called Sent Items
         // and Junk E-mail - which it does hold - are just that.
         StringAssert.Contains("\\Sent", ListLine(list, "Gesendete Elemente"));
         StringAssert.DoesNotContain("\\Sent", ListLine(list, "Sent Items"));
         StringAssert.Contains("\\Trash", ListLine(list, GeloeschteListed));
         StringAssert.Contains("\\Drafts", ListLine(list, EntwuerfeListed));
         StringAssert.Contains("\\Junk", ListLine(list, "Junk-E-Mail"));
         StringAssert.DoesNotContain("\\Junk", ListLine(list, "Junk E-mail"));
         foreach (string use in new[] { "Sent", "Trash", "Drafts", "Junk" })
            Assert.AreEqual(1, Regex.Matches(list, "\\\\" + use).Count, use + ": " + list);

         string subscribed = Lsub("pst@example.test");
         StringAssert.Contains("Projects" + d + "2025" + d + "Q3", subscribed, "A folder the import makes is subscribed.");

         // Every message once, in its folder; the appointment and the associated
         // item nowhere.
         List<Fetched> inbox = FetchAll("pst@example.test", "INBOX");
         Assert.AreEqual(15, inbox.Count);
         foreach (string token in new[] { "PLAIN", "HTML1252", "MIXED", "RTFHTML", "TRANSPORT", "CP1251", "CATEGORIES", "OUTER", "BIGBODY",
                                          "UNICODE", "REFERENCE", "LIST", "FORWARDED", "SIGNED", "EPOCH" })
            Assert.AreEqual(1, inbox.Count(m => m.Text.Contains("PSTTOKEN-" + token + "\r\n") || m.Text.Contains("PSTTOKEN-" + token + "<") ||
                                                 m.Text.Contains("PSTTOKEN-" + token + " ")), "PSTTOKEN-" + token);
         Assert.IsFalse(inbox.Any(m => m.Text.Contains("Not mail.") || m.Text.Contains("FAI\r\n")), "An appointment or an associated item was imported.");

         Assert.AreEqual(1, OnlyToken("pst@example.test", "Gesendete Elemente", "SENT").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", "Sent Items", "ORDINARYSENT").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", GeloeschteListed, "DELETED").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", "Junk-E-Mail", "JUNK").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", "Junk E-mail", "ORDINARYJUNK").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", "Projects" + d + "2025" + d + "Q3", "Q3").Count);
         Assert.AreEqual(1, OnlyToken("pst@example.test", "_#hashtag", "HASHED").Count);

         // Flags: read is \Seen, unsent \Draft, flagged for follow-up \Flagged, a
         // reply \Answered, a forward $Forwarded - and each opposite not.
         Fetched plain = Of(inbox, "PLAIN");
         Assert.AreEqual("\\Seen", plain.Flags);
         Assert.IsFalse(Of(inbox, "HTML1252").Flags.Contains("\\Seen"), "An unread message is not \\Seen.");
         StringAssert.Contains("\\Flagged", Of(inbox, "MIXED").Flags);
         Fetched categories = Of(inbox, "CATEGORIES");
         StringAssert.Contains("\\Answered", categories.Flags);
         StringAssert.Contains("Red_category", categories.Flags);
         StringAssert.Contains("Work", categories.Flags);
         Fetched forwarded = Of(inbox, "FORWARDED");
         StringAssert.Contains("$Forwarded", forwarded.Flags);
         Assert.IsFalse(forwarded.Flags.Contains("\\Answered") || forwarded.Flags.Contains("\\Flagged"), "A forward, flag complete: " + forwarded.Flags);
         Fetched draft = OnlyToken("pst@example.test", EntwuerfeListed, "DRAFT")[0];
         StringAssert.Contains("\\Draft", draft.Flags);
         Assert.IsFalse(draft.Flags.Contains("\\Seen"), draft.Flags);

         // The internal date is the delivery time, and Date the submit time: a
         // converter that wrote only Date would give both the same. Both are the
         // instant the file holds, whatever this server's offset from UTC.
         Assert.AreEqual(new DateTime(2024, 3, 1, 9, 0, 0, DateTimeKind.Utc), plain.InternalDateUtc);
         StringAssert.Contains("Message-ID: <plain@fixture.test>\r\n", plain.Text);
         StringAssert.Contains("Date: Fri, 01 Mar 2024 08:59:00 +0000\r\n", plain.Text);
         // A delivery time in 1970 is not one: the submit time is used.
         Assert.AreEqual(new DateTime(2023, 6, 15, 12, 30, 0, DateTimeKind.Utc), Of(inbox, "EPOCH").InternalDateUtc);

         // Headers: kept where the file had them, less their MIME fields; built
         // otherwise, with an Exchange sender by its SMTP address and a line break
         // in a display name never a header of its own.
         Fetched transport = Of(inbox, "TRANSPORT");
         StringAssert.Contains("Received: from relay.example.org", transport.Text);
         StringAssert.Contains("X-Fixture: kept\r\n", transport.Text);
         StringAssert.DoesNotContain("stale-boundary-XYZ", transport.Text);
         Assert.AreEqual(1, Regex.Matches(transport.Text, "MIME-Version:").Count, transport.Text);

         Fetched sent = OnlyToken("pst@example.test", "Gesendete Elemente", "SENT")[0];
         StringAssert.Contains("From: Alice Example <alice@example.org>\r\n", sent.Text);
         StringAssert.Contains("Bcc: Dave Example <dave@example.org>\r\n", sent.Text);
         StringAssert.DoesNotContain("/O=EXAMPLE", sent.Text);

         Fetched unicode = Of(inbox, "UNICODE");
         Assert.IsFalse(Regex.IsMatch(unicode.Text, "^Bcc:", RegexOptions.Multiline), "A CR LF in a display name started a header.");
         Assert.AreEqual("\u039A\u03B1\u03BB\u03B7\u03BC\u03AD\u03C1\u03B1 \u03BA\u03CC\u03C3\u03BC\u03B5 \uD83D\uDE00", DecodedSubject(unicode.Text));

         Fetched cp1251 = Of(inbox, "CP1251");
         Assert.AreEqual("RE: \u041F\u0440\u0438\u0432\u0435\u0442", DecodedSubject(cp1251.Text), "The subject marker is removed and the rest kept.");
         StringAssert.Contains("=D0=9F=D1=80=D0=B8=D0=B2=D0=B5=D1=82 PSTTOKEN-CP1251", cp1251.Text, "PT_STRING8 in cp1251, decoded.");

         // Bodies: HTML in its own charset, its bytes kept; HTML carried in RTF.
         Fetched html = Of(inbox, "HTML1252");
         StringAssert.Contains("Content-Type: text/html; charset=\"windows-1252\"", html.Text);
         StringAssert.Contains("Caf=E9", html.Text);
         StringAssert.Contains("<b>PSTTOKEN-RTFHTML</b>", Of(inbox, "RTFHTML").Text);
         StringAssert.Contains("Content-Type: text/html; charset=\"utf-8\"", Of(inbox, "RTFHTML").Text);
         StringAssert.Contains("PSTTOKEN-BIGBODY", Of(inbox, "BIGBODY").Text, "A body past one block.");

         // Attachments: related under the HTML with its Content-ID, the rest mixed,
         // a name outside ASCII in RFC 2231, a path and a line break gone from a
         // name, an empty file kept, an embedded message as message/rfc822.
         Fetched mixed = Of(inbox, "MIXED");
         StringAssert.Contains("multipart/mixed", mixed.Text);
         StringAssert.Contains("multipart/related", mixed.Text);
         StringAssert.Contains("multipart/alternative", mixed.Text);
         StringAssert.Contains("Content-ID: <logo@fixture>", mixed.Text);
         StringAssert.Contains("filename*=UTF-8''r%C3%A9sum%C3%A9.txt", mixed.Text);
         StringAssert.Contains("filename=\"empty.txt\"", mixed.Text);
         byte[] blob = Enumerable.Range(0, 20000).Select(i => (byte) ((i * 7 + 3) & 0xFF)).ToArray();
         Assert.AreEqual(Sha256(blob), Sha256(Attachment(mixed.Text, "data.bin")), "An attachment past one block, byte for byte.");

         Fetched outer = Of(inbox, "OUTER");
         StringAssert.Contains("Content-Type: message/rfc822", outer.Text);
         StringAssert.Contains("Subject: Inner message", outer.Text);
         StringAssert.Contains("PSTTOKEN-INNER", outer.Text);
         StringAssert.Contains("PSTTOKEN-INNERATTACH", Encoding.ASCII.GetString(Attachment(outer.Text, "inner.txt")));

         Fetched signedMessage = Of(inbox, "SIGNED");
         StringAssert.Contains("MIME-Version: 1.0\r\nContent-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; micalg=sha-256; boundary=\"sig-boundary\"", signedMessage.Text,
            "A clear-signed message's signed entity is kept as it was.");

         StringAssert.Contains("person179@example.org", Of(inbox, "LIST").Text, "A table whose rows span blocks.");

         // A chain 27 folders deep: the 25 levels a folder here may have, and the
         // two below them filed into the 25th.
         string deep25 = string.Join(d, new[] { "Deep" }.Concat(Enumerable.Range(2, 24).Select(i => "L" + i.ToString("00"))));
         Assert.AreEqual(1, OnlyToken("pst@example.test", deep25, "DEEP").Count, "The message 27 folders down is not in the 25th.");

         // The report.
         JsonElement report = Report(migration, Number(job, "id"));
         JsonElement pst = report.GetProperty("pst");
         Assert.AreEqual("Fixture Personal Folders", Text(pst, "store"));
         Assert.AreEqual(RecordKey, Text(pst, "record_key"));
         Assert.AreEqual(23, Number(pst, "version"));
         Assert.AreEqual("permute", Text(pst, "encoding"));
         Assert.AreEqual(2, Number(pst.GetProperty("not_mail"), "IPF.Contact"));
         Assert.AreEqual(2, Number(pst.GetProperty("not_mail_folders"), "IPF.Contact"));
         Assert.AreEqual(1, Number(pst.GetProperty("not_mail_messages"), "IPM.Appointment"));
         Assert.AreEqual(1, Number(pst.GetProperty("not_mail"), "IPF.Appointment"));
         Assert.AreEqual(1, Number(pst.GetProperty("not_mail"), "IPM.Appointment"));
         Assert.AreEqual("", Text(pst, "refusal"));

         JsonElement inboxTally = Folder(report, "Inbox");
         Assert.AreEqual("INBOX", Text(inboxTally, "target"));
         Assert.AreEqual(15, Number(inboxTally, "copied"));
         Assert.AreEqual(1, Reason(inboxTally, "not_mail"));
         Assert.AreEqual(2, Warning(inboxTally, "attachment_not_imported"), "A link and a reference are named, not imported.");
         Assert.AreEqual(1, Warning(inboxTally, "delivery_time_out_of_range"));
         Assert.AreEqual("\\Sent", Text(Folder(report, "Gesendete Elemente"), "special_use"));
         Assert.AreEqual("\\Trash", Text(Folder(report, Geloeschte), "special_use"));
         Assert.AreEqual("\\Drafts", Text(Folder(report, Entwuerfe), "special_use"));
         Assert.AreEqual("\\Junk", Text(Folder(report, "Junk-E-Mail"), "special_use"));
         Assert.AreEqual("", Text(Folder(report, "Sent Items"), "special_use"));
         Assert.AreEqual("", Text(Folder(report, "Junk E-mail"), "special_use"));
         StringAssert.Contains("filed into its ancestor", Text(Folder(report, string.Join(" / ", new[] { "Deep" }.Concat(
            Enumerable.Range(2, 26).Select(i => "L" + i.ToString("00"))))), "note"));
         Assert.AreEqual(34, Number(job, "messages_copied"));

         // The contacts and calendars phases read the file's contacts and
         // calendar folders: the fixture's two contacts have no e-mail address,
         // which the address book keeps each contact by, and its one meeting has
         // no time - each counted with its reason, and nothing made.
         JsonElement contactsPhase = report.GetProperty("contacts");
         Assert.AreEqual("copied", Text(contactsPhase, "state"), report.ToString());
         Assert.AreEqual(0, Number(contactsPhase, "copied"));
         Assert.AreEqual(2, Reason(contactsPhase, "no_address"));
         JsonElement calendarsPhase = report.GetProperty("calendars");
         Assert.AreEqual("copied", Text(calendarsPhase, "state"), report.ToString());
         Assert.AreEqual(1, Reason(calendarsPhase, "no_start"));
         StringAssert.Contains("imported by the contacts phase", Text(Folder(report, "Contacts"), "note"));

         // The Extras: each thing a message was copied without, said, and each
         // header built from the properties.
         List<Fetched> extras = FetchAll("pst@example.test", "Extras");
         Assert.AreEqual(8, extras.Count);
         JsonElement extrasTally = Folder(report, "Extras");
         Fetched rtfOnly = Of(extras, "RTFONLY");
         StringAssert.Contains("filename=\"body.rtf\"", rtfOnly.Text, "A body in RTF alone is kept as body.rtf.");
         Assert.AreEqual(1, Warning(extrasTally, "rtf_body_kept"));
         Fetched twelve = Of(extras, "NEST00");
         StringAssert.Contains("PSTTOKEN-NEST09", twelve.Text);
         StringAssert.DoesNotContain("PSTTOKEN-NEST10", twelve.Text);
         Assert.AreEqual(1, Warning(extrasTally, "embedded_too_deep"));
         Fetched longCategories = Of(extras, "LONGCATEGORIES");
         StringAssert.Contains("Category_0_", longCategories.Flags);
         StringAssert.Contains("Category_1_", longCategories.Flags);
         StringAssert.DoesNotContain("Category_2_", longCategories.Flags);
         Assert.AreEqual(1, Warning(extrasTally, "keywords_left_out"));
         StringAssert.Contains("Content-Type: text/html\r\n", Of(extras, "CP37").Text, "HTML in a code page with no name is kept with no charset.");
         Assert.AreEqual(1, Warning(extrasTally, "html_charset_unknown"));
         Fetched opaque = extras.Single(m => m.Text.Contains("Subject: Opaque S/MIME"));
         StringAssert.Contains("Content-Type: application/pkcs7-mime;", opaque.Text);
         StringAssert.DoesNotContain("smime-type", opaque.Text);
         Fetched behalf = Of(extras, "BEHALF");
         StringAssert.Contains("From: Boss Example <boss@example.org>\r\n", behalf.Text);
         StringAssert.Contains("Sender: Assistant Example <assistant@example.org>\r\n", behalf.Text);
         Fetched ids = Of(extras, "IDS");
         StringAssert.Contains("Message-ID: <ids@fixture.test>\r\n", ids.Text);
         StringAssert.Contains("In-Reply-To: <parent@fixture.test>\r\n", ids.Text);
         StringAssert.Contains("References: <root@fixture.test> <parent@fixture.test>\r\n", ids.Text);
         Assert.IsFalse(Regex.IsMatch(Of(extras, "LONGID").Text, "^Message-ID:", RegexOptions.Multiline), "A Message-ID longer than a line was written.");
         Assert.AreEqual(1, Warning(extrasTally, "identifier_too_long"));
      }

      [Test]
      [Description("The same messages under no encoding, NDB_CRYPT_PERMUTE and NDB_CRYPT_CYCLIC are rebuilt as the same bytes: the reader decodes each, and the rebuild depends on nothing but the message.")]
      public void TheSameMessagesUnderEachEncodingAreTheSameBytes()
      {
         Target("none");
         Target("permute");
         Target("cyclic");
         Place("mini-none.pst");
         Place("mini-permute.pst");
         Place("mini-cyclic.pst");

         long migration = CreateMigration("Encodings", "");
         AddJobs(migration, "mini-none.pst,,none@example.test\r\nmini-permute.pst,,permute@example.test\r\nmini-cyclic.pst,,cyclic@example.test", 3);
         Start(migration);
         foreach (JsonElement job in WaitForJobs(migration, all => all.All(Settled), "the jobs to finish"))
            Assert.AreEqual("done", Text(job, "state"), Describe(job));

         string none = Bodies("none@example.test", "INBOX");
         Assert.AreEqual(3, FetchAll("none@example.test", "INBOX").Count);
         StringAssert.Contains("PSTTOKEN-MINI1", none);
         Assert.AreEqual(none, Bodies("permute@example.test", "INBOX"), "Permute and none differ.");
         Assert.AreEqual(none, Bodies("cyclic@example.test", "INBOX"), "Cyclic and none differ.");
         Assert.AreEqual(Bodies("none@example.test", "Archive"), Bodies("cyclic@example.test", "Archive"));

         byte[] blob = Enumerable.Range(0, 9000).Select(i => (byte) ((i * 13 + 5) & 0xFF)).ToArray();
         Fetched third = FetchAll("cyclic@example.test", "INBOX").Single(m => m.Text.Contains("PSTTOKEN-MINI3"));
         Assert.AreEqual(Sha256(blob), Sha256(Attachment(third.Text, "blob.bin")), "An attachment in cyclic-encoded blocks, byte for byte.");
      }

      [Test]
      [Description("A file the reader does not take fails its job with the sentence that says what to do - an OST and a PST in the 4K-page layout, an ANSI header with an unknown encoding, an unknown version, Windows Information Protection under version 23 and 37, a file cut short, a damaged header - and nothing is filed.")]
      public void AFileTheReaderRefusesFailsItsJobWithTheReason()
      {
         Target("refused");
         var expected = new Dictionary<string, string>
         {
            { "refuse-ost.pst", "offline copy of a mailbox" },
            { "refuse-ansi.pst", "(version 14, encoding 7)" },
            { "refuse-4k.pst", "4K-page layout" },
            { "refuse-version.pst", "(version 40, encoding 1)" },
            { "refuse-wip.pst", "Windows Information Protection" },
            { "refuse-wip37.pst", "Windows Information Protection" },
            { "refuse-eof.pst", "it was cut short" },
            { "refuse-crc.pst", "header is damaged" }
         };
         foreach (string file in expected.Keys)
            Place(file);

         long migration = CreateMigration("Refused", "");
         AddJobs(migration, string.Join("\r\n", expected.Keys.Select(f => f + ",,refused@example.test")), expected.Count);
         Start(migration);
         List<JsonElement> jobs = WaitForJobs(migration, all => all.All(Settled), "the jobs to settle");

         foreach (KeyValuePair<string, string> file in expected)
         {
            JsonElement job = jobs.Single(j => Text(j, "source_user") == file.Key);
            Assert.AreEqual("failed", Text(job, "state"), Describe(job));
            StringAssert.Contains(file.Value, Text(job, "last_error"), file.Key);
            StringAssert.Contains(file.Value, Text(Report(migration, Number(job, "id")).GetProperty("pst"), "refusal"), file.Key);
            Assert.AreEqual(0, Number(job, "messages_copied"));
         }

         Assert.AreEqual(0, FetchAll("refused@example.test", "INBOX").Count, "A refused file filed something.");
      }

      [Test]
      [Description("A PST with a password is read only with it: without one the job fails asking for it, with the wrong one it fails saying so, and retried with the right one it copies the file.")]
      public void APasswordProtectedFileIsReadOnlyWithItsPassword()
      {
         Target("locked");
         Place("protected.pst");

         long migration = CreateMigration("Protected", "");
         AddJobs(migration, "protected.pst,,locked@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to settle")[0];
         Assert.AreEqual("failed", Text(job, "state"), Describe(job));
         StringAssert.Contains("protected with a password", Text(job, "last_error"));

         Retry(migration, Number(job, "id"), "not-the-password");
         job = WaitForJobs(migration, all => all.All(Settled), "the retry to settle")[0];
         Assert.AreEqual("failed", Text(job, "state"), Describe(job));
         StringAssert.Contains("is not this PST's", Text(job, "last_error"));

         Retry(migration, Number(job, "id"), "fixture-password");
         job = WaitForJobs(migration, all => all.All(Settled), "the second retry to settle")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(3, FetchAll("locked@example.test", "INBOX").Count);
         StringAssert.DoesNotContain("fixture-password", Admin("GET", "/api/v1/migrations/" + migration + "/jobs").body, "A secret is never answered.");
      }

      [Test]
      [Description("A message whose block is damaged fails alone, named in the report, and its siblings arrive; a file whose node B-tree points back at itself ends its job with that said, rather than reading for ever.")]
      public void ADamagedMessageFailsAloneAndALoopingTreeEndsTheJob()
      {
         Target("damaged");
         Target("looping");
         Place("corrupt-block.pst");
         Place("corrupt-loop.pst");

         long migration = CreateMigration("Damaged", "");
         AddJobs(migration, "corrupt-block.pst,,damaged@example.test\r\ncorrupt-loop.pst,,looping@example.test", 2);
         Start(migration);
         List<JsonElement> jobs = WaitForJobs(migration, all => all.All(Settled), "the jobs to settle", 120);

         JsonElement damaged = jobs.Single(j => Text(j, "source_user") == "corrupt-block.pst");
         Assert.AreEqual("done", Text(damaged, "state"), Describe(damaged));
         List<Fetched> inbox = FetchAll("damaged@example.test", "INBOX");
         Assert.AreEqual(2, inbox.Count);
         Assert.AreEqual(1, inbox.Count(m => m.Text.Contains("PSTTOKEN-MINI1")));
         Assert.AreEqual(1, inbox.Count(m => m.Text.Contains("PSTTOKEN-MINI3")));
         Assert.AreEqual(1, FetchAll("damaged@example.test", "Archive").Count);
         JsonElement report = Report(migration, Number(damaged, "id"));
         Assert.AreEqual(1, Reason(Folder(report, "Inbox"), "unreadable"));
         StringAssert.Contains("damaged (its checksum does not match)", report.GetProperty("errors").ToString());

         JsonElement looping = jobs.Single(j => Text(j, "source_user") == "corrupt-loop.pst");
         string said = Text(looping, "last_error") + " " + Report(migration, Number(looping, "id")).ToString();
         StringAssert.Contains("points back at one of its own pages", said, Describe(looping));
      }

      [Test]
      [Description("A job names a file in its target's domain's directory and nowhere else: no directory set, a path, a drive, a share, a name with .., a junction that leads out, a file in another domain's directory and a domain with no directory are each refused with a sentence when the job is added, and so is a setting that is not an absolute local path.")]
      public void NamesOutsideTheDomainsDirectoryAreRefused()
      {
         Target("names");
         Place("mini-permute.pst");

         // Another domain's file, and a directory outside the import directory that
         // a junction inside it leads to.
         Directory.CreateDirectory(Path.Combine(_directory, "other.test"));
         File.Copy(Fixture("mini-permute.pst"), Path.Combine(_directory, "other.test", "theirs.pst"));
         string outside = Scratch();
         File.Copy(Fixture("mini-permute.pst"), Path.Combine(outside, "outside.pst"));
         string junction = Path.Combine(_directory, "example.test", "escape");
         Run("cmd.exe", "/c mklink /J \"" + junction + "\" \"" + outside + "\"");
         Assert.IsTrue(Directory.Exists(junction), "The junction was not made.");

         long migration = CreateMigration("Names", "");
         AssertRefused(migration, "..\\other.test\\theirs.pst", "not a path");
         AssertRefused(migration, "../other.test/theirs.pst", "not a path");
         AssertRefused(migration, Path.Combine(_directory, "example.test", "mini-permute.pst"), "not a path");
         AssertRefused(migration, "C:\\mini-permute.pst", "not a path");
         AssertRefused(migration, "\\\\server\\share\\mini-permute.pst", "not a path");
         AssertRefused(migration, "theirs.pst", "There is no file theirs.pst");
         AssertRefused(migration, "escape\\outside.pst", "leads outside this domain's PST import directory");

         // A domain with no directory of its own.
         Domain elsewhere = SingletonProvider<TestSetup>.Instance.AddDomain("nodirectory.test");
         SingletonProvider<TestSetup>.Instance.AddAccount(elsewhere, "someone@nodirectory.test", TargetPassword);
         (int status, string body) noDirectory = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", "mini-permute.pst,,someone@nodirectory.test");
         Assert.AreEqual(400, noDirectory.status, noDirectory.body);
         StringAssert.Contains("There is no directory", noDirectory.body);

         // No import directory at all.
         SetImportDirectory("");
         (int status, string body) unset = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", "mini-permute.pst,,names@example.test");
         Assert.AreEqual(400, unset.status, unset.body);
         StringAssert.Contains("No PST import directory is set", unset.body);

         // The setting itself: an absolute local path or nothing.
         (int status, string body) relative = Admin("PUT", "/api/v1/settings", "{\"pst_import_directory\":\"relative\\\\dir\"}");
         Assert.AreEqual(400, relative.status, relative.body);
         StringAssert.Contains("pst_import_directory must be empty, or an absolute path", relative.body);

         // The control: the plain name is taken.
         SetImportDirectory(_directory);
         AddJobs(migration, "mini-permute.pst,,names@example.test", 1);
         Assert.AreEqual(0, Jobs(migration).Count(j => Text(j, "source_user") != "mini-permute.pst"));

         // One file however it is spelled: 2019\a.pst is kept as 2019/a.pst, and
         // 2019/a.pst, 2019\a.pst. and 2019/A.PST are that job again. A first line
         // file,password,target is a heading here.
         Directory.CreateDirectory(Path.Combine(_directory, "example.test", "2019"));
         File.Copy(Fixture("mini-permute.pst"), Path.Combine(_directory, "example.test", "2019", "a.pst"));
         long spellings = CreateMigration("Spellings", "");
         AddJobs(spellings, "file,password,target\r\n2019\\a.pst,,names@example.test", 1);
         Assert.AreEqual("2019/a.pst", Text(Jobs(spellings)[0], "source_user"));
         foreach (string again in new[] { "2019/a.pst", "2019\\a.pst.", "2019/A.PST" })
         {
            (int status, string body) twice = Admin("POST", "/api/v1/migrations/" + spellings + "/jobs", again + ",,names@example.test");
            Assert.AreEqual(400, twice.status, again + " -> " + twice.body);
            StringAssert.Contains("already a job of this migration", twice.body, again);
         }

         // For a migration from a server, a first line that begins "file" is a
         // mailbox called file, not a heading.
         (int status, string body) server = Admin("POST", "/api/v1/migrations", "{\"name\":\"Server\",\"kind\":\"imap\",\"host\":\"127.0.0.1\"}");
         Assert.AreEqual(201, server.status, server.body);
         AddJobs(Number(Json(server.body), "id"), "file,secret,names@example.test", 1);
      }

      [Test]
      [Description("A file another program holds without sharing - as Outlook holds its data files - pauses its job with that said; closed, and the job retried, it is read.")]
      public void AFileHeldOpenPausesTheJobUntilItIsClosed()
      {
         Target("held");
         string file = Place("mini-permute.pst");

         long migration = CreateMigration("Held", "");
         AddJobs(migration, "mini-permute.pst,,held@example.test", 1);

         using (new FileStream(file, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
         {
            Start(migration);
            JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused", "the job to pause")[0];
            StringAssert.Contains("open in Outlook or another program", Text(paused, "last_error"));
            Assert.AreEqual(0, Number(paused, "messages_copied"));
         }

         JsonElement job = Jobs(migration)[0];
         Retry(migration, Number(job, "id"), null);
         job = WaitForJobs(migration, all => all.All(Settled), "the retry to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(3, FetchAll("held@example.test", "INBOX").Count);
      }

      [Test]
      [Description("A job interrupted by a restart in the middle of a folder goes on from the message after its mark and files every message once: nothing past the mark is read again, so nothing is found a duplicate.")]
      public void AnInterruptedJobGoesOnFromTheMessageAfterItsMark()
      {
         Target("resumed");
         Place("many.pst");

         // Five messages a second into an Inbox of ninety, the job's row written
         // every fifty: the restart lands after the first mark and before the
         // folder's end.
         long migration = CreateMigration("Interrupted", ",\"concurrency\":1,\"messages_per_minute\":300");
         AddJobs(migration, "many.pst,,resumed@example.test", 1);
         Start(migration);
         JsonElement before = WaitForJobs(migration, all => Number(all[0], "messages_copied") >= 50, "the first fifty messages")[0];

         _application.Reinitialize();

         // What the stop wrote: the Inbox begun and not finished.
         JsonElement atRestart = Folder(Report(migration, Number(before, "id")), "Inbox");
         Assert.GreaterOrEqual(Number(atRestart, "copied"), 50);
         Assert.Less(Number(atRestart, "copied"), 90, "The Inbox was finished before the restart; the test proves nothing.");

         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the job to finish", 240)[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         List<Fetched> inbox = FetchAll("resumed@example.test", "INBOX");
         Assert.AreEqual(90, inbox.Count, "Every message of the INBOX, once.");
         Assert.AreEqual(90, inbox.Select(m => Regex.Match(m.Text, "PSTTOKEN-MANY[0-9]+").Value).Distinct().Count());
         Assert.AreEqual(1, FetchAll("resumed@example.test", "Archive").Count);

         JsonElement inboxTally = Folder(Report(migration, Number(done, "id")), "Inbox");
         Assert.AreEqual(90, Number(inboxTally, "copied"));
         Assert.AreEqual(0, Reason(inboxTally, "duplicate"), "The second run read the folder again from before its mark.");
      }

      [Test]
      [Description("The reader's guard: a message or a folder's list the service ended in the middle of twice is left out and named, and one it ended in once is read again; and a job that reads the file's folders again after a pause counts what is not mail once, not once a pass.")]
      public void TheGuardLeavesOutWhatStoppedTheServiceTwice()
      {
         Target("guarded");
         Place("standard.pst");

         // One message a second, and the Inbox - read first - holds sixteen: the
         // pause lands while the job is in it, before the folders marked below.
         long migration = CreateMigration("Guarded", ",\"concurrency\":1,\"messages_per_minute\":60");
         AddJobs(migration, "standard.pst,,guarded@example.test", 1);
         Start(migration);
         WaitForJobs(migration, all => Number(all[0], "folders_total") > 0 || all.All(Settled), "the job's plan");

         // The markers a reader that ended the service would leave: the Drafts
         // folder's one message twice begun, the Junk folder's once, and the
         // Deleted Items folder's list of messages twice.
         Pause(migration);
         JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused" || all.All(Settled), "the job to pause")[0];
         Assert.AreEqual("paused", Text(paused, "state"), "The job finished before it could be paused; the test proves nothing. " + Describe(paused));
         Assert.AreEqual(0, Number(paused, "folders_done"), "The job had finished the Inbox before the pause; the test proves nothing.");
         long jobId = Number(paused, "id");
         Mark(jobId, "80c2", "86a4.2");
         Mark(jobId, "80e2", "86c4.1");
         Mark(jobId, "80a2", "l.2");
         Resume(migration);

         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the job to finish", 240)[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));
         Assert.AreEqual(15, FetchAll("guarded@example.test", "INBOX").Count, "Every message of the INBOX, once.");

         JsonElement report = Report(migration, jobId);
         Assert.AreEqual(1, Reason(Folder(report, Entwuerfe), "reader_stopped"), "The message the reader stopped in twice is left out.");
         Assert.AreEqual(0, FetchAll("guarded@example.test", EntwuerfeListed).Count);
         Assert.AreEqual(1, OnlyToken("guarded@example.test", "Junk-E-Mail", "JUNK").Count, "The message the reader stopped in once is read again.");
         StringAssert.Contains("stopped twice", Text(Folder(report, Geloeschte), "note"));
         Assert.AreEqual(0, FetchAll("guarded@example.test", GeloeschteListed).Count);
         StringAssert.Contains("the service stopped twice while this message was being read or filed", report.GetProperty("errors").ToString());

         // The pause ended a pass, and the resume read the file's folders again:
         // what is not mail is counted once.
         JsonElement notMail = report.GetProperty("pst").GetProperty("not_mail");
         Assert.AreEqual(2, Number(notMail, "IPF.Contact"), notMail.ToString());
         Assert.AreEqual(1, Number(notMail, "IPF.Appointment"), notMail.ToString());
         Assert.AreEqual(1, Number(notMail, "IPM.Appointment"), notMail.ToString());
      }

      [Test]
      [Description("Into an account that already has a top-level Sent Items with no use stored - \\Sent by its name alone - the file's sent folder is filed into it, that use is stored so that no folder made later takes it by name, and the file's ordinary Sent Items arrives beside it as Sent Items (PST).")]
      public void AnOrdinaryFolderNeverPoursIntoTheAccountsSpecialFolder()
      {
         Target("presets");
         CreateFolder("presets@example.test", "Sent Items");
         StringAssert.Contains("\\Sent", ListLine(List("presets@example.test"), "Sent Items"),
            "The name table no longer gives Sent Items \\Sent; the test proves nothing.");
         Place("standard.pst");

         long migration = CreateMigration("Presets", "");
         AddJobs(migration, "standard.pst,,presets@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));

         string list = List("presets@example.test");
         StringAssert.Contains("\\Sent", ListLine(list, "Sent Items"));
         Assert.IsNotNull(ListLine(list, "Sent Items (PST)"), list);
         StringAssert.DoesNotContain("\\Sent", ListLine(list, "Sent Items (PST)"));
         Assert.IsNull(ListLine(list, "Gesendete Elemente"), "The file's sent folder was made rather than filed into the account's.");
         Assert.AreEqual(1, OnlyToken("presets@example.test", "Sent Items", "SENT").Count);
         Assert.AreEqual(1, OnlyToken("presets@example.test", "Sent Items (PST)", "ORDINARYSENT").Count);
         StringAssert.Contains("Sent Items (PST)", Text(Folder(Report(migration, Number(job, "id")), "Sent Items"), "note"));

         // The use is stored: a folder called Sent - which the name table puts
         // before Sent Items - made now does not take it.
         CreateFolder("presets@example.test", "Sent");
         list = List("presets@example.test");
         StringAssert.Contains("\\Sent", ListLine(list, "Sent Items"));
         StringAssert.DoesNotContain("\\Sent", ListLine(list, "Sent"));
      }

      [Test]
      [Description("A file whose root folder names no Drafts or Junk E-mail folder, as an older file keeps them on the Inbox: they are found there, and given their use although their names say nothing to IMAP.")]
      public void DraftsAndJunkAreFoundOnTheInboxWhenTheRootNamesNone()
      {
         Target("older");
         Place("inbox-roles.pst");

         long migration = CreateMigration("Older", "");
         AddJobs(migration, "inbox-roles.pst,,older@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));

         string list = List("older@example.test");
         StringAssert.Contains("\\Drafts", ListLine(list, "Brouillons"));
         StringAssert.Contains("\\Junk", ListLine(list, "Courrier ind&AOk-sirable"));
         Assert.AreEqual(1, OnlyToken("older@example.test", "Brouillons", "BROUILLON").Count);
         Assert.AreEqual(1, OnlyToken("older@example.test", "Courrier ind&AOk-sirable", "INDESIRABLE").Count);

         JsonElement report = Report(migration, Number(job, "id"));
         Assert.AreEqual("\\Drafts", Text(Folder(report, "Brouillons"), "special_use"));
         Assert.AreEqual("\\Junk", Text(Folder(report, "Courrier ind\u00e9sirable"), "special_use"));
      }

      [Test]
      [Description("An account the file would take past its quota pauses the job with that said, filed up to the quota and not past it; with the quota raised and the job retried, the rest arrives, each message once.")]
      public void AFullQuotaPausesTheJobAndARetryGoesOn()
      {
         Account account = Target("quota");
         account.MaxSize = 1;
         account.Save();

         // All but full: 1 MB less some 6,000 bytes - room for the file's two
         // small messages and not for its third, which carries 9,000 bytes.
         var filler = new StringBuilder("From: filler@example.test\r\nTo: quota@example.test\r\nSubject: Filler\r\n\r\n");
         while (filler.Length < 1048576 - 6000 - 80)
            filler.Append(new string('f', 76)).Append("\r\n");
         string literal = filler.ToString();
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon("quota@example.test", TargetPassword));
         StringAssert.Contains("A1 OK", imap.SendSingleCommandWithLiteral("A1 APPEND INBOX {" + literal.Length + "}", literal));
         imap.Disconnect();

         Place("mini-permute.pst");
         long migration = CreateMigration("Quota", ",\"concurrency\":1");
         AddJobs(migration, "mini-permute.pst,,quota@example.test", 1);
         Start(migration);
         JsonElement paused = WaitForJobs(migration, all => Text(all[0], "state") == "paused" || all.All(Settled), "the job to pause")[0];
         Assert.AreEqual("paused", Text(paused, "state"), Describe(paused));
         StringAssert.Contains("no room left under its quota", Text(paused, "last_error"));

         List<Fetched> atPause = FetchAll("quota@example.test", "INBOX");
         Assert.AreEqual(3, atPause.Count, "The filler and the two messages that fit.");
         Assert.IsFalse(atPause.Any(m => m.Text.Contains("PSTTOKEN-MINI3")), "A message past the quota was filed.");

         account.MaxSize = 10;
         account.Save();
         Retry(migration, Number(paused, "id"), null);
         JsonElement done = WaitForJobs(migration, all => all.All(Settled), "the retry to finish")[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         List<Fetched> inbox = FetchAll("quota@example.test", "INBOX");
         Assert.AreEqual(4, inbox.Count);
         foreach (string token in new[] { "MINI1", "MINI2", "MINI3" })
            Assert.AreEqual(1, inbox.Count(m => m.Text.Contains("PSTTOKEN-" + token + "\r\n")), token);
         Assert.AreEqual(1, FetchAll("quota@example.test", "Archive").Count);
      }

      [Test]
      [Description("A file whose store a finished job already gave an account is refused for that account, naming the job, rather than filed a second time.")]
      public void TheSameFileIsNotImportedTwiceIntoOneAccount()
      {
         Target("twice");
         Place("mini-permute.pst");

         long first = CreateMigration("First", "");
         AddJobs(first, "mini-permute.pst,,twice@example.test", 1);
         Start(first);
         JsonElement done = WaitForJobs(first, all => all.All(Settled), "the first job to finish")[0];
         Assert.AreEqual("done", Text(done, "state"), Describe(done));

         long second = CreateMigration("Second", "");
         AddJobs(second, "mini-permute.pst,,twice@example.test", 1);
         Start(second);
         JsonElement refused = WaitForJobs(second, all => all.All(Settled), "the second job to settle")[0];
         Assert.AreEqual("failed", Text(refused, "state"), Describe(refused));
         StringAssert.Contains("imported into this account already, by job " + Number(done, "id"), Text(refused, "last_error"));
         Assert.AreEqual(3, FetchAll("twice@example.test", "INBOX").Count, "The second job filed something.");
      }

      [Test]
      [Description("The owner's sample - a file Aspose's evaluation wrote, from outside this project - is read whole: its one message imported with a plain and an HTML body, nothing failed and nothing warned of, and its wastebasket given \\Trash.")]
      public void TheOwnersSampleIsImported()
      {
         // The sample is not this project's and is never committed: it is read
         // from where its owner keeps it, and only its shape is asserted here.
         string sample = Environment.GetEnvironmentVariable("HMTEST_PST_SAMPLE");
         if (string.IsNullOrEmpty(sample))
            sample = "C:\\Users\\chris\\Downloads\\sample.pst";
         if (!File.Exists(sample) || !Sha256(File.ReadAllBytes(sample)).Equals(SampleSha256, StringComparison.OrdinalIgnoreCase))
            Assert.Ignore("The owner's sample PST is not on this machine.");

         Target("sample");
         File.Copy(sample, Path.Combine(_directory, "example.test", "sample.pst"));

         long migration = CreateMigration("Sample", "");
         AddJobs(migration, "sample.pst,,sample@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(1, Number(job, "messages_copied"));
         Assert.AreEqual(0, Number(job, "skipped"));

         JsonElement report = Report(migration, Number(job, "id"));
         Assert.AreEqual(0, report.GetProperty("pst").GetProperty("not_read").GetArrayLength());
         foreach (JsonElement folder in report.GetProperty("folders").EnumerateArray())
            Assert.AreEqual(0, folder.GetProperty("warnings").EnumerateObject().Count(), folder.ToString());

         string list = List("sample@example.test");
         Assert.AreEqual(1, Regex.Matches(list, "\\\\Trash").Count, list);

         JsonElement copied = report.GetProperty("folders").EnumerateArray().Single(f => Number(f, "copied") == 1);
         List<Fetched> messages = FetchAll("sample@example.test", Text(copied, "target"));
         Assert.AreEqual(1, messages.Count);
         StringAssert.Contains("multipart/alternative", messages[0].Text);
         StringAssert.Contains("Content-Type: text/plain", messages[0].Text);
         StringAssert.Contains("Content-Type: text/html", messages[0].Text);
         StringAssert.Contains("\\Seen", messages[0].Flags);
         StringAssert.Contains("\\Draft", messages[0].Flags);
      }

      [Test]
      [Description("An ANSI PST - Outlook 97 to 2002, [MS-PST]'s 32-bit NDB layer - arrives whole: its folders, a folder name in Windows-1252, each message's eight-bit text in the code page it names (Windows-1252 and Windows-1251), an attachment past one block through an ANSI XBLOCK byte for byte, its drafts folder given \\Drafts, and the report naming version 14.")]
      public void AnAnsiFileArrivesWithItsCodePages()
      {
         Target("ansi");
         Place("ansi.pst");

         long migration = CreateMigration("ANSI", "");
         AddJobs(migration, "ansi.pst,,ansi@example.test", 1);
         Start(migration);
         JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
         Assert.AreEqual("done", Text(job, "state"), Describe(job));

         List<Fetched> inbox = FetchAll("ansi@example.test", "INBOX");
         Assert.AreEqual(3, inbox.Count);

         Fetched western = Of(inbox, "ANSI1");
         Assert.AreEqual("Café in Windows-1252", DecodedSubject(western.Text));
         StringAssert.Contains("Caf=C3=A9 cr=C3=A8me PSTTOKEN-ANSI1", western.Text, "PT_STRING8 in Windows-1252, as UTF-8.");
         StringAssert.Contains("Message-ID: <ansi1@fixture.test>", western.Text);
         StringAssert.Contains("\\Seen", western.Flags);

         Fetched cyrillic = Of(inbox, "ANSI2");
         Assert.AreEqual("Привет", DecodedSubject(cyrillic.Text));
         StringAssert.Contains("=D0=9F=D1=80=D0=B8=D0=B2=D0=B5=D1=82 PSTTOKEN-ANSI2", cyrillic.Text, "PT_STRING8 in Windows-1251, as UTF-8.");
         Assert.IsFalse(cyrillic.Flags.Contains("\\Seen"), cyrillic.Flags);

         byte[] blob = Enumerable.Range(0, 12000).Select(i => (byte) ((i * 17 + 11) & 0xFF)).ToArray();
         Assert.AreEqual(Sha256(blob), Sha256(Attachment(Of(inbox, "ANSI3").Text, "blob.bin")), "An attachment in two ANSI blocks, byte for byte.");

         // "Entwürfe", PT_STRING8 in Windows-1252: the drafts folder by the root's EntryID.
         string list = List("ansi@example.test");
         string drafts = ListLine(list, "Entw&APw-rfe");
         Assert.IsNotNull(drafts, list);
         StringAssert.Contains("\\Drafts", drafts);
         StringAssert.Contains("\\Draft", OnlyToken("ansi@example.test", "Entw&APw-rfe", "ANSI4")[0].Flags);

         JsonElement pst = Report(migration, Number(job, "id")).GetProperty("pst");
         Assert.AreEqual(14, Number(pst, "version"));
         Assert.AreEqual("permute", Text(pst, "encoding"));
         Assert.AreEqual("", Text(pst, "refusal"));
      }

      [Test]
      [Description("An OST whose pages, blocks and store are a PST's - [MS-PST] names only the magic apart - is read as one; an OST whose store names its folders by an Exchange server's EntryIDs, which [MS-PST] does not map to the file's nodes, fails its job with the sentence that says so and what to do instead.")]
      public void AnOfflineFileIsReadWhereItNamesItsFoldersAsAPstDoes()
      {
         Target("offline");
         Target("exchange");
         Place("offline.ost");
         Place("offline-exchange.ost");

         long migration = CreateMigration("Offline", "");
         AddJobs(migration, "offline.ost,,offline@example.test\r\noffline-exchange.ost,,exchange@example.test", 2);
         Start(migration);
         List<JsonElement> jobs = WaitForJobs(migration, all => all.All(Settled), "the jobs to settle");

         JsonElement read = jobs.Single(j => Text(j, "source_user") == "offline.ost");
         Assert.AreEqual("done", Text(read, "state"), Describe(read));
         Assert.AreEqual(3, FetchAll("offline@example.test", "INBOX").Count);
         Assert.AreEqual(1, OnlyToken("offline@example.test", "Archive", "MINI4").Count);

         JsonElement refused = jobs.Single(j => Text(j, "source_user") == "offline-exchange.ost");
         Assert.AreEqual("failed", Text(refused, "state"), Describe(refused));
         StringAssert.Contains("names its folders by the server's own identifiers", Text(refused, "last_error"));
         StringAssert.Contains("migrate the mailbox from its server", Text(Report(migration, Number(refused, "id")).GetProperty("pst"), "refusal"));
         Assert.AreEqual(0, FetchAll("exchange@example.test", "INBOX").Count);
      }

      [Test]
      [Description("What a PST holds besides mail arrives where a phone or the webmail would find it: contacts in the account's address books as vCard 4.0 cards - names, three e-mail addresses (an Exchange one by its SMTP address), telephones, home and work addresses, company, title, birthday, notes and photo - read back over REST and a CardDAV GET, a second contacts folder as a book of its own, a contact with no address and a distribution list counted, notes of 270,000 bytes cut to 256 KB with a warning, an address the account collected from mail it sent adopted with the PST's card; the recipient cache and a hidden folder left out, and a contacts folder under Deleted Items imported last, so that its copy of a live contact's address loses; appointments in its calendar - a single one, an all-day one, a weekly series across the clock change with an occurrence removed and one moved, a series whose zone is a TZSTRUCT alone, a monthly and a yearly one, a meeting with its organiser and attendees - read back over the calendar REST route, expanded; a calendar folder of a 300-character name as a calendar of 255, with an appointment whose description Outlook kept only as RTF; a damaged recurrence and an appointment with no time counted; tasks as VTODOs over the tasks route; and a note and a journal entry as messages in folders of their own.")]
      public void ContactsCalendarTasksNotesAndJournalArrive()
      {
         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("CardDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", DavPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");
         _application.Reinitialize();

         try
         {
            const string address = "items@example.test";
            Target("items");
            Place("items.pst");

            // An address the account collects by writing to it, before the
            // import: the PST's contact for it is to take the row over.
            Target("colin");
            CreateFolder(address, "Sent");
            (int status, string body) sent = AsAccount(address, "POST", "/api/v1/me/messages",
               "{\"to\":\"colin@example.test\",\"subject\":\"Hello\",\"text\":\"Body.\"}");
            Assert.AreEqual(201, sent.status, sent.body);
            StringAssert.Contains("\"address\":\"colin@example.test\",\"source\":\"collected\"",
               AsAccount(address, "GET", "/api/v1/me/contacts?source=collected").body, "The address was collected.");

            long migration = CreateMigration("Items", "");
            AddJobs(migration, "items.pst,," + address, 1);
            Start(migration);
            JsonElement job = WaitForJobs(migration, all => all.All(Settled), "the job to finish")[0];
            Assert.AreEqual("done", Text(job, "state"), Describe(job));

            // ---- the mail: a note and a journal entry, each in its own folder
            Assert.AreEqual(3, Number(job, "messages_copied"));
            Assert.AreEqual(1, OnlyToken(address, "INBOX", "ITEMSMAIL").Count);
            Fetched note = OnlyToken(address, "Notes", "NOTE")[0];
            StringAssert.Contains("Subject: Buy milk\r\n", note.Text);
            StringAssert.Contains("Buy milk\r\nand bread PSTTOKEN-NOTE", note.Text);
            Fetched entry = OnlyToken(address, "Journal", "JOURNAL")[0];
            StringAssert.Contains("Journal entry: Phone call\r\nStart: 2024-03-05 10:00 UTC\r\nEnd: 2024-03-05 10:15 UTC\r\nDuration: 15 minutes", entry.Text);
            StringAssert.Contains("Subject: Call with Ann\r\n", entry.Text);
            string list = List(address);
            foreach (string name in new[] { "Contacts", "Suppliers", "Calendar", "Tasks" })
               Assert.IsNull(ListLine(list, name), "A folder " + name + " was made: " + list);

            // ---- the contacts, over REST
            (int status, string body) contacts = AsAccount(address, "GET", "/api/v1/me/contacts?fields=card");
            Assert.AreEqual(200, contacts.status, contacts.body);
            List<JsonElement> cards = Json(contacts.body).GetProperty("contacts").EnumerateArray().ToList();
            Assert.AreEqual(5, cards.Count, contacts.body);

            JsonElement ann = cards.Single(c => Text(c, "address") == "ann@example.org");
            Assert.AreEqual("Ann Example", Text(ann, "name"));
            Assert.AreEqual("1980-04-12", Text(ann, "birthday"), "The birthday is the day PidLidBirthdayLocal keeps.");
            StringAssert.Contains("Met at the fair", Text(ann, "note"));
            List<string> phones = ann.GetProperty("phones").EnumerateArray().Select(p => p.GetString()).ToList();
            CollectionAssert.IsSupersetOf(phones, new[] { "+44 20 7946 0000", "+44 7700 900000", "+44 20 7946 0001" });
            Assert.IsTrue(ann.GetProperty("has_photo").GetBoolean(), "The contact's picture is on its card.");
            string card = Text(ann, "card");
            foreach (string line in new[] { "VERSION:4.0\r\n", "N:Example;Ann;Q;Dr.;\r\n", "NICKNAME:Annie\r\n", "ORG:Example Ltd;Sales\r\n", "TITLE:Director\r\n",
                                            "EMAIL;PREF=1:Ann@Example.org\r\n", "EMAIL:ann.private@example.net\r\n", "EMAIL:ann@exchange.example.org\r\n",
                                            "TEL;TYPE=work,voice:+44 20 7946 0000\r\n", "TEL;TYPE=cell:+44 7700 900000\r\n", "TEL;TYPE=home,fax:+44 20 7946 0001\r\n",
                                            "ADR;TYPE=home:;;1 High Street;London;;SW1A 1AA;United Kingdom\r\n",
                                            "ADR;TYPE=work:;;2 Low Road;Leeds;;LS1 1AA;United Kingdom\r\n", "BDAY:19800412\r\n",
                                            "URL:https://example.org/ann\r\n", "CATEGORIES:Customers\r\n", "PHOTO:data:image/jpeg;base64,/9j/4" })
               StringAssert.Contains(line, Unfold(card), "The card: " + card);
            Assert.AreEqual(200, AsAccount(address, "GET", "/api/v1/me/contacts/" + Number(ann, "id") + "/photo").status, "The photo is served.");

            JsonElement sam = cards.Single(c => Text(c, "address") == "sam@supplier.example");
            Assert.AreEqual("Suppliers", Text(sam, "book_name"), "A second contacts folder is a book of its own, the recipient cache's copy of the address never made.");
            Assert.AreEqual("Sam Supplier", Text(sam, "name"));
            Assert.AreNotEqual("Suppliers", Text(ann, "book_name"));
            Assert.IsFalse(contacts.body.Contains("Nora") || contacts.body.Contains("Team list"), "A contact with no address, or a list, was kept: " + contacts.body);
            Assert.IsFalse(contacts.body.Contains("Harriet") || contacts.body.Contains("Sam Cached") || contacts.body.Contains("Ann Old") ||
                           contacts.body.Contains("Dora"),
               "A hidden folder's contact, the recipient cache's, a deleted copy of a live address, or a contact of a deleted folder with no book of its own was kept: " + contacts.body);

            // Deleted Items: imported last, into a book of its own.
            JsonElement olga = cards.Single(c => Text(c, "address") == "olga@example.org");
            Assert.AreEqual("Old contacts", Text(olga, "book_name"));
            Assert.AreNotEqual("Old contacts", Text(ann, "book_name"));

            // Notes longer than a contact keeps: cut, not the contact lost.
            JsonElement lena = cards.Single(c => Text(c, "address") == "lena@example.org");
            string longNote = Text(lena, "note");
            StringAssert.StartsWith("PSTTOKEN-LONGNOTE", longNote);
            Assert.IsTrue(longNote.Length > 250000 && Encoding.UTF8.GetByteCount(longNote) <= 256 * 1024,
               "The notes are cut to 256 KB: " + longNote.Length + " characters.");

            // The collected address, adopted: the PST's card, kept.
            JsonElement colin = cards.Single(c => Text(c, "address") == "colin@example.test");
            Assert.AreEqual("Colin Collected", Text(colin, "name"));
            Assert.AreEqual("manual", Text(colin, "source"));
            StringAssert.Contains("TEL;TYPE=cell:+44 7700 900999\r\n", Unfold(Text(colin, "card")));

            // ---- the same card over CardDAV, byte for byte
            (int status, string body) dav = Dav("GET", "/dav/addressbooks/" + address + "/contacts/" + Number(ann, "id") + ".vcf", address);
            Assert.AreEqual(200, dav.status, dav.body);
            Assert.AreEqual(card, dav.body, "CardDAV serves the card the import kept.");

            // ---- the calendar, over REST, expanded
            List<JsonElement> march = Events(address, "20240301T000000Z", "20240415T000000Z");
            JsonElement dentist = march.Single(e => Text(e, "summary") == "Dentist");
            Assert.AreEqual("20240307T150000Z", Text(dentist, "start"));
            Assert.AreEqual("Main Street 1", Text(dentist, "location"));
            StringAssert.Contains("Bring the card.", Text(dentist, "description"));
            Assert.AreEqual(30, Number(dentist, "alarm_minutes"));

            JsonElement holiday = march.Single(e => Text(e, "summary") == "Holiday");
            Assert.IsTrue(holiday.GetProperty("all_day").GetBoolean());
            Assert.AreEqual("20240329T000000Z", Text(holiday, "start"), "An all-day appointment is the day it was in Berlin.");

            List<string> weekly = march.Where(e => Text(e, "summary").StartsWith("Weekly sync")).Select(e => Text(e, "start")).OrderBy(s => s).ToList();
            CollectionAssert.AreEqual(new[] { "20240304T080000Z", "20240311T080000Z", "20240325T090000Z", "20240401T070000Z", "20240408T070000Z" }, weekly,
               "09:00 in Berlin every Monday across the clock change, 18 March removed and 25 March moved to 10:00.");
            JsonElement moved = march.Single(e => Text(e, "summary") == "Weekly sync (moved)");
            Assert.AreEqual("Room 2", Text(moved, "location"));
            Assert.IsTrue(moved.GetProperty("overridden").GetBoolean());

            CollectionAssert.AreEqual(new[] { "20240312T130000Z", "20240409T120000Z" },
               march.Where(e => Text(e, "summary") == "Board meeting").Select(e => Text(e, "start")).OrderBy(s => s).ToList(),
               "The second Tuesday of each month at 14:00 in Berlin.");
            Assert.AreEqual("20240320T000000Z", Text(march.Single(e => Text(e, "summary") == "Anniversary"), "start"));

            JsonElement kickoff = march.Single(e => Text(e, "summary") == "Project kickoff");
            Assert.AreEqual("kickoff-uid@example.org", Text(kickoff, "uid"), "The UID the global object id carries.");
            Assert.AreEqual("alice@example.org", Text(kickoff, "organizer"));
            string attendees = kickoff.GetProperty("attendees").ToString();
            StringAssert.Contains("bob@example.org", attendees);
            StringAssert.Contains("ACCEPTED", attendees);
            StringAssert.Contains("carol@example.org", attendees);
            StringAssert.Contains("TENTATIVE", attendees);
            StringAssert.Contains("rooma@example.org", attendees);

            Assert.IsFalse(march.Any(e => Text(e, "summary") == "Broken series" || Text(e, "summary") == "No time given"), "A damaged or timeless appointment was stored.");

            CollectionAssert.AreEqual(new[] { "20240320T080000Z", "20240327T080000Z", "20240403T070000Z" },
               march.Where(e => Text(e, "summary") == "Standup").Select(e => Text(e, "start")).OrderBy(s => s).ToList(),
               "09:00 in Berlin each Wednesday across the clock change, by the zone PidLidTimeZoneStruct alone gives.");

            // The calendar of a long name, and the description kept only as RTF.
            JsonElement offsite = march.Single(e => Text(e, "summary") == "Offsite");
            StringAssert.Contains("Bring boots. PSTTOKEN-RTFBODY", Text(offsite, "description"), "The RTF-only description comes in as its text.");
            (int status, string body) calendarList = AsAccount(address, "GET", "/api/v1/me/calendars");
            Assert.AreEqual(200, calendarList.status, calendarList.body);
            string teamName = Json(calendarList.body).GetProperty("calendars").EnumerateArray().Select(c => Text(c, "display_name"))
               .Single(n => n.StartsWith("Team x"));
            Assert.AreEqual(255, teamName.Length, "A name of 300 characters is cut to the 255 a calendar's may have.");
            Assert.IsFalse(Json(calendarList.body).GetProperty("calendars").EnumerateArray().Any(c => Text(c, "display_name") == "Old calendar"),
               "A deleted folder whose only event was already there made no calendar.");

            CollectionAssert.AreEqual(new[] { "20240514T120000Z", "20240611T120000Z" },
               Events(address, "20240501T000000Z", "20241201T000000Z").Where(e => Text(e, "summary") == "Board meeting").Select(e => Text(e, "start")).OrderBy(s => s).ToList(),
               "The series ends with its last meeting on 11 June.");
            Assert.AreEqual("20250320T000000Z", Text(Events(address, "20250301T000000Z", "20250401T000000Z").Single(e => Text(e, "summary") == "Anniversary"), "start"),
               "The yearly series goes on.");

            // ---- the tasks
            (int status, string body) tasks = AsAccount(address, "GET", "/api/v1/me/tasks");
            Assert.AreEqual(200, tasks.status, tasks.body);
            List<JsonElement> todo = Json(tasks.body).GetProperty("tasks").EnumerateArray().ToList();
            Assert.AreEqual(3, todo.Count, tasks.body);
            JsonElement report = todo.Single(t => Text(t, "summary") == "File the report");
            Assert.AreEqual("20240315T000000Z", Text(report, "due"));
            Assert.AreEqual(1, Number(report, "priority"));
            Assert.AreEqual("IN-PROCESS", Text(report, "status"));
            Assert.IsFalse(report.GetProperty("completed").GetBoolean());
            JsonElement renew = todo.Single(t => Text(t, "summary") == "Renew the domain");
            Assert.IsTrue(renew.GetProperty("completed").GetBoolean());
            Assert.AreEqual("", Text(todo.Single(t => Text(t, "summary") == "Someday"), "due"), "Outlook's 4501-01-01 is no due date.");

            // ---- the report
            JsonElement full = Report(migration, Number(job, "id"));
            JsonElement contactsPhase = full.GetProperty("contacts");
            Assert.AreEqual("copied", Text(contactsPhase, "state"), full.ToString());
            Assert.AreEqual(5, Number(contactsPhase, "copied"), "Ann, Lena and Colin, Sam, and Olga from Deleted Items.");
            Assert.AreEqual(1, Reason(contactsPhase, "no_address"));
            Assert.AreEqual(1, Reason(contactsPhase, "distribution_list"));
            Assert.AreEqual(1, Reason(contactsPhase, "already_there"), "The deleted copy of Ann's address.");
            Assert.AreEqual(1, Reason(contactsPhase, "folder_left_out"), "Dora, in a deleted folder named as the default book.");
            StringAssert.Contains("under Deleted Items", Text(Folder(full, "Deleted Items / Contacts"), "note"));
            StringAssert.Contains("the contact's notes ran to 264 KB; the first 256 KB were kept", full.GetProperty("errors").ToString());
            StringAssert.Contains("keeps for itself (IPF.Contact.RecipientCache)", Text(Folder(full, "Recipient Cache"), "note"));
            StringAssert.Contains("hidden", Text(Folder(full, "Hidden contacts"), "note"));
            Assert.AreEqual(3, Number(Folder(full, "Contacts"), "copied"));
            Assert.AreEqual(1, Number(Folder(full, "Deleted Items / Old contacts"), "copied"));
            Assert.AreEqual("address book: Old contacts", Text(Folder(full, "Deleted Items / Old contacts"), "target"));
            JsonElement calendarsPhase = full.GetProperty("calendars");
            Assert.AreEqual("copied", Text(calendarsPhase, "state"), full.ToString());
            Assert.AreEqual(11, Number(calendarsPhase, "copied"), "Seven appointments in the calendar, one in the long-named one, and three tasks.");
            Assert.AreEqual(1, Reason(calendarsPhase, "recurrence_unreadable"));
            Assert.AreEqual(1, Reason(calendarsPhase, "no_start"));
            Assert.AreEqual(1, Reason(calendarsPhase, "already_there"), "The deleted copy of the kickoff, found in the live calendar.");
            StringAssert.Contains("recurrence pattern's version (0x1234)", full.GetProperty("errors").ToString());
            Assert.AreEqual("address book: Suppliers", Text(Folder(full, "Suppliers"), "target"));
            Assert.AreEqual(1, Number(Folder(full, "Suppliers"), "copied"));
            Assert.AreEqual(7, Number(Folder(full, "Calendar"), "copied"));
            Assert.AreEqual(3, Number(Folder(full, "Tasks"), "copied"));
            Assert.AreEqual(3, Number(full.GetProperty("pst").GetProperty("not_mail"), "IPF.Task"));
         }
         finally
         {
            IniFileSetting.Write("WebServicesHttpPort", "0");
         }
      }

      [Test]
      [Description("Two adjacent items the account's store refuses every time - the second and third of three events whose UIDs make one resource name - are held while the items after them are read; the fourth is stored, which shows both refusals are their own, so each is tried once more, counted not_saved and named, with no pause. A twin that is the last item of its calendar has nothing after it to show that, so it is held across exactly three pauses and counted not_saved in its fourth run; the job finishes rather than pausing until it fails.")]
      public void AnItemTheStoreRefusesIsCountedAfterItsAttempts()
      {
         const string address = "clash@example.test";
         Target("clash");
         Place("clash.pst");

         long migration = CreateMigration("Clash", ",\"concurrency\":1");
         AddJobs(migration, "clash.pst,," + address, 1);
         Start(migration);

         // A pause, were there one, retried at once rather than after its minutes.
         int pauses = 0;
         JsonElement job;
         while (true)
         {
            job = WaitForJobs(migration, all => Text(all[0], "state") == "paused" || all.All(Settled), "the job to pause or finish")[0];
            if (Text(job, "state") != "paused")
               break;
            StringAssert.Contains("could not be saved into " + address + "'s calendar", Text(job, "last_error"));
            pauses++;
            Assert.LessOrEqual(pauses, 6, "The items were tried again past their attempts: " + Describe(job));
            Retry(migration, Number(job, "id"), null);
         }
         Assert.AreEqual("done", Text(job, "state"), Describe(job));
         Assert.AreEqual(3, pauses, "The Calendar's twins cost no pause; the Tail's last item, with nothing after it, costs three.");

         JsonElement report = Report(migration, Number(job, "id"));
         JsonElement calendarsPhase = report.GetProperty("calendars");
         Assert.AreEqual("copied", Text(calendarsPhase, "state"), report.ToString());
         Assert.AreEqual(3, Number(calendarsPhase, "copied"));
         Assert.AreEqual(3, Reason(calendarsPhase, "not_saved"));
         Assert.AreEqual(2, Reason(Folder(report, "Calendar"), "not_saved"));
         Assert.AreEqual(1, Reason(Folder(report, "Tail"), "not_saved"));
         StringAssert.Contains("the account's calendar refused it twice, and stored an item after it", report.GetProperty("errors").ToString());
         StringAssert.Contains("the account's calendar refused it in 4 runs", report.GetProperty("errors").ToString());

         CollectionAssert.AreEqual(new[] { "First", "Fourth", "Tail first" },
            Events(address, "20240301T000000Z", "20240401T000000Z").Select(e => Text(e, "summary")).OrderBy(n => n).ToList(),
            "The first event, the one after the refused ones, and the Tail's first arrived.");

         // HM7411, each pause and each run's first give-up: read, and the
         // error log emptied for the next test.
         CustomAsserts.AssertReportedError("HM7411", "counted not_saved");
      }

      // ------------------------------------------------------------ the steps

      private Account Target(string local)
      {
         return SingletonProvider<TestSetup>.Instance.AddAccount(_domain, local + "@example.test", TargetPassword);
      }

      private string Scratch()
      {
         string directory = Path.Combine(Path.GetTempPath(), "hm-pst-import-" + Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(directory);
         _scratch.Add(directory);
         return directory;
      }

      private static void RemoveDirectory(string directory)
      {
         try
         {
            // A junction first, so that removing the tree does not follow it.
            foreach (string sub in Directory.GetDirectories(directory, "*", SearchOption.AllDirectories)
                        .Where(d => (File.GetAttributes(d) & FileAttributes.ReparsePoint) != 0).ToList())
               Directory.Delete(sub);
            Directory.Delete(directory, true);
         }
         catch (IOException)
         {
            // The next run's directory has a name of its own.
         }
         catch (UnauthorizedAccessException)
         {
            // As above.
         }
      }

      // A file of build/fixtures/pst, found by walking up from the test binaries.
      private static string Fixture(string name)
      {
         string relative = Path.Combine("build", "fixtures", "pst", name);
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relative);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relative + " cannot be found.");
         return null;
      }

      private string Place(string name)
      {
         string target = Path.Combine(_directory, "example.test", name);
         File.Copy(Fixture(name), target);
         return target;
      }

      private static string Dotted(string delimiter)
      {
         // "Notes.Archive/Old": the hierarchy delimiter inside the name made an
         // underscore, whichever it is.
         return "Notes.Archive/Old".Replace(delimiter, "_");
      }

      private static void CreateFolder(string address, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, TargetPassword), "Could not log on as " + address);
         try
         {
            Assert.IsTrue(imap.CreateFolder(folder), "Could not create " + folder);
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private static void SetImportDirectory(string directory)
      {
         (int status, string body) set = Admin("PUT", "/api/v1/settings", "{\"pst_import_directory\":\"" + directory.Replace("\\", "\\\\") + "\"}");
         Assert.AreEqual(200, set.status, set.body);
      }

      private void Mark(long jobId, string folderNid, string marker)
      {
         string key = RecordKey + ":" + folderNid;
         _application.Database.ExecuteSQL("update hm_migrationfolders set mfuidvalidity = '" + key + "/" + marker + "' where mfjobid = " + jobId +
                                           " and mfuidvalidity = '" + key + "'");
      }

      private static void Run(string program, string arguments)
      {
         var start = new ProcessStartInfo(program, arguments) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
         using (Process process = Process.Start(start))
         {
            process.StandardOutput.ReadToEnd();
            Assert.IsTrue(process.WaitForExit(30000), program + " " + arguments + " did not end.");
         }
      }

      private static void AssertRefused(long migration, string name, string reason)
      {
         (int status, string body) refused = Admin("POST", "/api/v1/migrations/" + migration + "/jobs",
            "\"" + name.Replace("\"", "\"\"") + "\",,names@example.test");
         Assert.AreEqual(400, refused.status, name + " -> " + refused.body);
         StringAssert.Contains(reason, Json(refused.body).GetProperty("error").GetString(), name);
         Assert.AreEqual(0, Jobs(migration).Count, "A refused name made a job: " + name);
      }

      private static long CreateMigration(string name, string extra)
      {
         string body = "{\"name\":\"" + name + "\",\"kind\":\"pst\"" + extra + "}";
         (int status, string body) created = Admin("POST", "/api/v1/migrations", body);
         Assert.AreEqual(201, created.status, body + " -> " + created.body);
         JsonElement migration = Json(created.body);
         Assert.AreEqual("pst", Text(migration, "kind"));
         return Number(migration, "id");
      }

      private static void AddJobs(long migration, string rows, int expected)
      {
         (int status, string body) added = Admin("POST", "/api/v1/migrations/" + migration + "/jobs", rows);
         Assert.AreEqual(201, added.status, rows + " -> " + added.body);
         Assert.AreEqual(expected, Number(Json(added.body), "created"), added.body);
      }

      private static void Start(long migration)
      {
         (int status, string body) started = Admin("POST", "/api/v1/migrations/" + migration + "/start", "{}");
         Assert.AreEqual(200, started.status, started.body);
      }

      private static void Pause(long migration)
      {
         (int status, string body) paused = Admin("POST", "/api/v1/migrations/" + migration + "/pause", "{}");
         Assert.AreEqual(200, paused.status, paused.body);
      }

      private static void Resume(long migration)
      {
         DateTime until = DateTime.Now.AddSeconds(30);
         while (true)
         {
            (int status, string body) resumed = Admin("POST", "/api/v1/migrations/" + migration + "/resume", "{}");
            if (resumed.status == 200)
               return;
            if (resumed.status != 409 || DateTime.Now > until)
               Assert.Fail("Resume was refused: " + resumed.status + " " + resumed.body);
            Thread.Sleep(250);
         }
      }

      // A job that has just paused may still have its thread ending, and the
      // retry is then answered 409 "the job is running": asked again, as
      // Resume does.
      private static void Retry(long migration, long job, string secret)
      {
         string body = secret == null ? "{}" : "{\"secret\":\"" + secret + "\"}";
         DateTime until = DateTime.Now.AddSeconds(30);
         while (true)
         {
            (int status, string body) retried = Admin("POST", "/api/v1/migrations/" + migration + "/jobs/" + job + "/retry", body);
            if (retried.status == 200)
               return;
            if (retried.status != 409 || DateTime.Now > until)
               Assert.Fail("Retry was refused: " + retried.status + " " + retried.body);
            Thread.Sleep(250);
         }
      }

      private static List<JsonElement> Jobs(long migration)
      {
         return Get("/api/v1/migrations/" + migration + "/jobs").GetProperty("jobs").EnumerateArray().ToList();
      }

      private static bool Settled(JsonElement job)
      {
         string state = Text(job, "state");
         return state == "done" || state == "failed" || state == "cancelled";
      }

      private static string Describe(JsonElement job)
      {
         return Text(job, "source_user") + ": " + Text(job, "state") + ", " + Number(job, "messages_copied") + " of " +
                Number(job, "messages_total") + ", last error '" + Text(job, "last_error") + "'";
      }

      private static List<JsonElement> WaitForJobs(long migration, Func<List<JsonElement>, bool> until, string what, int seconds = 180)
      {
         DateTime deadline = DateTime.Now.AddSeconds(seconds);
         List<JsonElement> jobs = Jobs(migration);
         while (!until(jobs))
         {
            if (DateTime.Now > deadline)
               Assert.Fail("Waited " + seconds + " seconds for " + what + ". The jobs: " + string.Join("; ", jobs.Select(Describe)));
            Thread.Sleep(200);
            jobs = Jobs(migration);
         }
         return jobs;
      }

      private static JsonElement Report(long migration, long job)
      {
         return Get("/api/v1/migrations/" + migration + "/jobs/" + job + "/report");
      }

      private static JsonElement Folder(JsonElement report, string source)
      {
         foreach (JsonElement folder in report.GetProperty("folders").EnumerateArray())
         {
            if (Text(folder, "source") == source)
               return folder;
         }

         Assert.Fail("The report has no folder " + source + ": " + report);
         return default(JsonElement);
      }

      private static long Reason(JsonElement tally, string reason)
      {
         JsonElement count;
         return tally.GetProperty("skipped_reasons").TryGetProperty(reason, out count) ? count.GetInt64() : 0;
      }

      private static long Warning(JsonElement tally, string word)
      {
         JsonElement count;
         return tally.GetProperty("warnings").TryGetProperty(word, out count) ? count.GetInt64() : 0;
      }

      private void RemoveEveryMigration()
      {
         (int status, string body) listed = Admin("GET", "/api/v1/migrations");
         if (listed.status != 200)
            return;

         var stuck = new List<string>();
         foreach (JsonElement migration in Json(listed.body).GetProperty("migrations").EnumerateArray())
         {
            long id = Number(migration, "id");
            if (Text(migration, "state") == "running")
               Admin("POST", "/api/v1/migrations/" + id + "/pause", "{}");

            DateTime until = DateTime.Now.AddSeconds(60);
            while (true)
            {
               (int status, string body) deleted = Admin("DELETE", "/api/v1/migrations/" + id);
               if (deleted.status == 200 || deleted.status == 404)
                  break;
               if (DateTime.Now > until)
               {
                  stuck.Add(id + ": " + deleted.status + " " + deleted.body);
                  break;
               }
               Thread.Sleep(250);
            }
         }

         if (stuck.Count > 0)
            Assert.Fail("Migrations could not be deleted: " + string.Join("; ", stuck));
      }

      // ------------------------------------------------------------ IMAP

      private sealed class Fetched
      {
         public string Flags = "";
         public DateTime InternalDateUtc;
         public string Text = "";
      }

      private static string List(string address)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, TargetPassword), "Could not log on as " + address);
         try
         {
            return imap.List();
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private static string Lsub(string address)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, TargetPassword), "Could not log on as " + address);
         try
         {
            return imap.LSUB();
         }
         finally
         {
            imap.Disconnect();
         }
      }

      // The LIST line of a folder, by its name as the server lists it.
      private static string ListLine(string list, string name)
      {
         foreach (string line in list.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries))
         {
            if (line.StartsWith("* LIST") && (line.EndsWith(" " + name) || line.EndsWith(" \"" + name + "\"")))
               return line;
         }
         return null;
      }

      // Every message of a folder, one FETCH each: its flags (sorted, \Recent left
      // out), its internal date in UTC and its whole text. None when there is no
      // such folder.
      private static List<Fetched> FetchAll(string address, string folder)
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(address, TargetPassword), "Could not log on as " + address);
         try
         {
            string selected;
            var messages = new List<Fetched>();
            if (!imap.SelectFolder(folder, out selected))
               return messages;

            Match exists = Regex.Match(selected, @"\* (\d+) EXISTS");
            int count = exists.Success ? int.Parse(exists.Groups[1].Value) : 0;
            for (int i = 1; i <= count; i++)
            {
               string answer = imap.Fetch(i + " (FLAGS INTERNALDATE BODY.PEEK[])");
               Match flags = Regex.Match(answer, @"FLAGS \(([^)]*)\)");
               Match date = Regex.Match(answer, "INTERNALDATE \"([^\"]*)\"");
               Assert.IsTrue(flags.Success && date.Success, "A FETCH without FLAGS or INTERNALDATE: " + answer);
               messages.Add(new Fetched
               {
                  Flags = string.Join(" ", flags.Groups[1].Value.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries)
                     .Where(f => !f.Equals("\\Recent", StringComparison.OrdinalIgnoreCase)).OrderBy(f => f, StringComparer.Ordinal)),
                  InternalDateUtc = InternalDate(date.Groups[1].Value),
                  Text = answer
               });
            }
            return messages;
         }
         finally
         {
            imap.Disconnect();
         }
      }

      private static List<Fetched> OnlyToken(string address, string folder, string token)
      {
         List<Fetched> messages = FetchAll(address, folder);
         Assert.IsTrue(messages.All(m => m.Text.Contains("PSTTOKEN-" + token)), folder + " holds something other than PSTTOKEN-" + token);
         return messages;
      }

      private static Fetched Of(List<Fetched> messages, string token)
      {
         List<Fetched> found = messages.Where(m => Regex.IsMatch(m.Text, "PSTTOKEN-" + token + "(?![A-Z0-9])")).ToList();
         Assert.AreEqual(1, found.Count, "Messages with PSTTOKEN-" + token);
         return found[0];
      }

      private static string Bodies(string address, string folder)
      {
         return string.Join("\n----\n", FetchAll(address, folder).Select(m => Regex.Replace(m.Text, "^\\* \\d+ FETCH \\(FLAGS \\([^)]*\\) ", "")));
      }

      // "01-Mar-2024 10:00:00 +0100" as the instant it names.
      private static DateTime InternalDate(string text)
      {
         Match match = Regex.Match(text.Trim(), @"^(\d{1,2})-(\w{3})-(\d{4}) (\d{2}):(\d{2}):(\d{2}) ([+-])(\d{2})(\d{2})$");
         Assert.IsTrue(match.Success, "Not an internal date: " + text);
         int month = Array.IndexOf(new[] { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }, match.Groups[2].Value) + 1;
         var local = new DateTime(int.Parse(match.Groups[3].Value), month, int.Parse(match.Groups[1].Value),
                                  int.Parse(match.Groups[4].Value), int.Parse(match.Groups[5].Value), int.Parse(match.Groups[6].Value), DateTimeKind.Unspecified);
         var offset = new TimeSpan(int.Parse(match.Groups[8].Value), int.Parse(match.Groups[9].Value), 0);
         DateTime utc = match.Groups[7].Value == "+" ? local - offset : local + offset;
         return DateTime.SpecifyKind(utc, DateTimeKind.Utc);
      }

      // The Subject header, its RFC 2047 words decoded.
      private static string DecodedSubject(string message)
      {
         // [^\r\n], not ".": in .NET "." takes the CR of a CRLF, and a folded
         // subject's next line would never be reached.
         Match subject = Regex.Match(message, "^Subject: ([^\r\n]*(?:\r\n[ \t][^\r\n]*)*)", RegexOptions.Multiline);
         Assert.IsTrue(subject.Success, "No Subject: " + message);
         string value = Regex.Replace(subject.Groups[1].Value, "\r\n[ \t]", " ");
         value = Regex.Replace(value, @"\?=\s+=\?", "?==?");
         return Regex.Replace(value, @"=\?UTF-8\?B\?([^?]*)\?=", m => Encoding.UTF8.GetString(Convert.FromBase64String(m.Groups[1].Value)), RegexOptions.IgnoreCase);
      }

      // An attachment's decoded bytes, by its file name.
      private static byte[] Attachment(string message, string name)
      {
         int at = message.IndexOf("filename=\"" + name + "\"", StringComparison.Ordinal);
         Assert.GreaterOrEqual(at, 0, "No attachment " + name);
         int body = message.IndexOf("\r\n\r\n", at, StringComparison.Ordinal) + 4;
         int end = message.IndexOf("\r\n--", body, StringComparison.Ordinal);
         string base64 = message.Substring(body, end - body).Replace("\r\n", "");
         return Convert.FromBase64String(base64);
      }

      private static string Sha256(byte[] bytes)
      {
         using (SHA256 hash = SHA256.Create())
            return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "");
      }

      // ------------------------------------------------------------ HTTP

      private static JsonElement Get(string path)
      {
         (int status, string body) answer = Admin("GET", path);
         Assert.AreEqual(200, answer.status, "GET " + path + ": " + answer.body);
         return Json(answer.body);
      }

      private static JsonElement Json(string body)
      {
         using (JsonDocument document = JsonDocument.Parse(body))
            return document.RootElement.Clone();
      }

      private static string Text(JsonElement element, string name)
      {
         return element.GetProperty(name).GetString();
      }

      private static long Number(JsonElement element, string name)
      {
         return element.GetProperty(name).GetInt64();
      }

      private static (int status, string body) Admin(string method, string path, string body = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path,
            "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes("Administrator:" + AdminPassword)), body);
      }

      // The target account's own routes, as its owner signs in to them.
      private static (int status, string body) AsAccount(string address, string method, string path, string body = null)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path,
            "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + TargetPassword)), body);
      }

      // Every occurrence of a window, as the calendar page asks for them.
      private static List<JsonElement> Events(string address, string start, string end)
      {
         (int status, string body) events = AsAccount(address, "GET", "/api/v1/me/calendar/events?start=" + start + "&end=" + end);
         Assert.AreEqual(200, events.status, events.body);
         return Json(events.body).GetProperty("events").EnumerateArray().ToList();
      }

      // One request to the CardDAV listener, as a phone makes it through a proxy
      // that ends TLS in front of it (X-Forwarded-Proto: https).
      private static (int status, string body) Dav(string method, string path, string address)
      {
         using (var client = new TcpClient())
         {
            client.Connect("127.0.0.1", DavPort);
            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               string request = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1:" + DavPort + "\r\n" +
                                "Authorization: Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(address + ":" + TargetPassword)) + "\r\n" +
                                "X-Forwarded-Proto: https\r\nConnection: close\r\n\r\n";
               byte[] head = Encoding.ASCII.GetBytes(request);
               stream.Write(head, 0, head.Length);
               stream.ReadTimeout = 30000;
               stream.CopyTo(memory);

               string text = Encoding.UTF8.GetString(memory.ToArray());
               int split = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               Match status = Regex.Match(text, "^HTTP/1\\.[01] (\\d{3})");
               Assert.IsTrue(status.Success, "Not an HTTP answer: " + text);
               return (int.Parse(status.Groups[1].Value), split >= 0 ? text.Substring(split + 4) : "");
            }
         }
      }

      // A card or an event with its folded lines joined again.
      private static string Unfold(string text)
      {
         return Regex.Replace(text, "\r\n[ \t]", "");
      }
   }
}
