// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Everything an account has, through a backup and a restore.
   ///
   ///    An account is not only its row, its folders and its mail. It is also an
   ///    address book (hm_contacts), the webmail's remembered choices
   ///    (hm_accountprefs), the messages it has put off (hm_scheduled), the large
   ///    files it has sent as links (hm_files), its S/MIME certificates and keys
   ///    (hm_smimekeys), the history that stops it reusing a password
   ///    (hm_passwordhistory), its calendar (hm_calendars, hm_calendarobjects), the
   ///    memory of who has written to it before (hm_knownsenders), whether it asked
   ///    for the quarantine digest to stop (hm_digeststate), its own safe and
   ///    blocked senders (hm_senderlist) and the administrative role it has been
   ///    given (hm_roleassignments) - and, on each message, the keywords the
   ///    webmail calls labels.
   ///
   ///    Until 15 September 2026 not one of those was in the archive, and because
   ///    every one of those tables cascades from hm_accounts and a restore deletes
   ///    every domain before it begins, RESTORING A BACKUP DELETED THEM ALL. That is
   ///    what these tests are: one per store, all in the same shape - make the data
   ///    through the surface that really writes it, take a backup, delete the whole
   ///    domain the way a disaster would, restore, and find the data exactly as it
   ///    was. Each of them fails on a build without AccountStores, which is the only
   ///    reason to trust them.
   ///
   ///    The data is made through the surfaces that really write it - the REST and
   ///    DAV listeners, and for the first-contact memory a delivery - rather than by
   ///    writing rows, so what is proved is what an account actually has rather than
   ///    what a test put in a table.
   ///
   ///    The role tests go further than "it comes back", because a role is authority:
   ///    an assignment names a role the server lacks, the archive is edited so that
   ///    the restore fails between the account's row and its assignment, and the
   ///    server holds a role narrower than the archive's - and in each case the
   ///    account must come back holding no more than its role gives it.
   /// </summary>
   [TestFixture]
   public class BackupAccountStores : TestFixtureBase
   {
      private const int WebServicesPort = 9111;
      private static int RestPort = 9542;

      private const string UserPassword = "Original-Passw0rd!";
      private const string AdminPassword = "testar";

      private const string FirstContactHeader = "X-hMailServer-First-Contact";

      private string _backupDirectory;
      private string _address;

      [SetUp]
      public void StartListeners()
      {
         _address = "stores@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, _address, UserPassword);
         _settings.SetAdministratorPassword(AdminPassword);

         _backupDirectory = Paths.Combine(Path.GetTempPath(), TestSetup.UniqueString());
         Directory.CreateDirectory(_backupDirectory);

         IniFileSetting.Write("CalDavRedirectUrl", "");
         IniFileSetting.Write("CardDavRedirectUrl", "");
         IniFileSetting.Write("WebServicesBindAddress", "127.0.0.1");
         IniFileSetting.Write("WebServicesHttpPort", WebServicesPort.ToString());
         IniFileSetting.Write("WebServicesHttpsPort", "0");

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         Response probe = Rest("GET", "/api/v1/me", User());
         Assert.AreEqual(200, probe.Status, "The REST listener did not answer. Body: " + probe.Body);
      }

      [TearDown]
      public void StopListeners()
      {
         TurnTheFirstContactNoteOff();
         TurnTheDigestAddressOff();

         IniFileSetting.Write("WebServicesHttpPort", "0");
         // Put back whatever a test turned on, whether it reached its own last line
         // or not: a setting left behind here runs every fixture after this one.
         IniFileSetting.Write("PasswordPolicyHistoryCount", "0");
         RestListener.Stop();
         _application.Reinitialize();

         try
         {
            if (Directory.Exists(_backupDirectory))
               Directory.Delete(_backupDirectory, true);
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            // The destination is a temp directory of this test's own. Failing the
            // test because it could not be removed would report the wrong thing.
         }
      }

      // ------------------------------------------------------------ hm_contacts

      [Test]
      [Description("hm_contacts: the account's address book survives a backup and a restore that deleted the domain, "
                   + "including the vCard a CardDAV client stored, byte for byte")]
      public void TheAddressBookComesBack()
      {
         Response added = Rest("POST", "/api/v1/me/contacts", User(),
            "{\"name\":\"Ada Lovelace\",\"address\":\"Ada@Example.com\"}");
         Assert.AreEqual(201, added.Status, "Body: " + added.Body);

         // A card as a client sends one: line breaks throughout, which is the value
         // that cannot cross an XML attribute as it stands and takes the base64 path.
         string card = Card("Grace Hopper", "Hopper;Grace", "grace@example.com");
         Response stored = Dav("PUT", AddressBook + "grace.vcf", card, "text/vcard; charset=utf-8");
         Assert.AreEqual(201, stored.Status, "Body: " + stored.Body);

         BackupDeleteAndRestore();

         Response listed = Rest("GET", "/api/v1/me/contacts", User());
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("\"name\":\"Ada Lovelace\"", listed.Body);
         StringAssert.Contains("\"address\":\"ada@example.com\"", listed.Body);
         StringAssert.Contains("\"name\":\"Grace Hopper\"", listed.Body);
         StringAssert.Contains("\"count\":2", listed.Body);

         // The card itself, and under the resource name the client chose - which is a
         // column of its own and is what a client's next sync asks for.
         Response fetched = Dav("GET", AddressBook + "grace.vcf", null, null);
         Assert.AreEqual(200, fetched.Status, "Body: " + fetched.Body);
         Assert.AreEqual(card, fetched.Body, "The stored vCard did not come back byte for byte.");
      }

      // ------------------------------------------------------- hm_accountprefs

      [Test]
      [Description("hm_accountprefs: the webmail's remembered choices survive a backup and a restore, "
                   + "including a value the server attaches no meaning to")]
      public void ThePreferencesComeBack()
      {
         // The server treats a value as opaque, so the test uses one that is: JSON
         // with quotes and a newline in it, which is what a webmail keeps here.
         Response saved = Rest("PUT", "/api/v1/me/preferences", User(),
            "{\"theme\":\"dark\",\"density\":\"cosy\",\"signature\":\"Regards,\\r\\nAda\"}");
         Assert.AreEqual(200, saved.Status, "Body: " + saved.Body);

         BackupDeleteAndRestore();

         Response read = Rest("GET", "/api/v1/me/preferences", User());
         Assert.AreEqual(200, read.Status, "Body: " + read.Body);
         StringAssert.Contains("\"theme\":\"dark\"", read.Body);
         StringAssert.Contains("\"density\":\"cosy\"", read.Body);
         StringAssert.Contains("\"signature\":\"Regards,\\r\\nAda\"", read.Body);
      }

      // ---------------------------------------------------------- hm_scheduled

      [Test]
      [Description("hm_scheduled: a draft scheduled for later survives a backup and a restore, and comes back "
                   + "pointing at the restored draft rather than at whatever was given its old id")]
      public void TheScheduledMessageComesBack()
      {
         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_address, UserPassword));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();

         Response draft = Rest("POST", "/api/v1/me/drafts", User(),
            "{\"to\":\"" + _address + "\",\"subject\":\"Not until Friday\",\"text\":\"Later.\"}");
         Assert.AreEqual(201, draft.Status, "Body: " + draft.Body);
         long draftId = long.Parse(Between(draft.Body, "\"id\":", ","));

         // Far enough away that the scheduler cannot run it during the test.
         string at = DateTime.Now.AddDays(30).ToString("yyyy-MM-dd HH:mm");
         Response scheduled = Rest("POST", "/api/v1/me/drafts/" + draftId + "/schedule", User(),
            "{\"send_at\":\"" + at + "\"}");
         Assert.AreEqual(201, scheduled.Status, "Body: " + scheduled.Body);

         BackupDeleteAndRestore();

         Response listed = Rest("GET", "/api/v1/me/scheduled", User());
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("\"action\":\"send\"", listed.Body);
         StringAssert.Contains("\"subject\":\"Not until Friday\"", listed.Body);
         StringAssert.Contains("\"at\":\"" + at + "\"", listed.Body);

         // The point of carrying the reference as a folder path and a UID rather than
         // as the id: the restored row must name the restored draft. The subject in
         // the listing above is read from the message the row points at, so a row
         // pointing at nothing - or at something else - could not produce it. This
         // pins the id as well, since a row left holding the old number would name a
         // message that no longer exists.
         long messageId = long.Parse(Between(listed.Body, "\"message_id\":", ","));
         Response message = Rest("GET", "/api/v1/me/messages/" + messageId, User());
         Assert.AreEqual(200, message.Status, "The scheduled row names a message that is not there. Body: " + message.Body);
         StringAssert.Contains("Not until Friday", message.Body);
      }

      // -------------------------------------------------------------- hm_files

      [Test]
      [Description("hm_files: a file sent as a link survives a backup and a restore, and its link still serves the bytes")]
      public void TheFileSentAsALinkComesBack()
      {
         Response made = Rest("POST", "/api/v1/me/files", User(),
            "{\"name\":\"notes.txt\",\"type\":\"text/plain\",\"size\":5}");
         Assert.AreEqual(201, made.Status, "Body: " + made.Body);
         long fileId = long.Parse(Between(made.Body, "\"id\":", ","));
         string token = Between(made.Body, "\"token\":\"", "\"");

         Response bytes = Rest("PUT", "/api/v1/me/files/" + fileId + "/content?offset=0", User(), "hello",
            "application/octet-stream");
         Assert.AreEqual(200, bytes.Status, "Body: " + bytes.Body);
         StringAssert.Contains("\"complete\":true", bytes.Body);

         BackupDeleteAndRestore();

         Response listed = Rest("GET", "/api/v1/me/files", User());
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("\"name\":\"notes.txt\"", listed.Body);
         StringAssert.Contains("\"token\":\"" + token + "\"", listed.Body);
         StringAssert.Contains("\"complete\":true", listed.Body);

         // The bytes were in the message store all along; the row that gives them a
         // name, an owner and an expiry was what the archive did not have. Both
         // halves have to be back for this to answer.
         Response fetched = Rest("GET", "/files/" + token, null);
         Assert.AreEqual(200, fetched.Status, "Body: " + fetched.Body);
         Assert.AreEqual("hello", fetched.Body);
         StringAssert.Contains("notes.txt", fetched.Header("Content-Disposition"));
      }

      // ---------------------------------------------------------- hm_smimekeys

      [Test]
      [Description("hm_smimekeys: the account's own certificate with its wrapped private key, and a correspondent's "
                   + "certificate, survive a backup and a restore")]
      public void TheSmimeKeyStoreComesBack()
      {
         string certificate = Convert.ToBase64String(new byte[] { 0x30, 0x03, 0x02, 0x01, 0x01 });
         string own = new string('a', 64);
         string bob = new string('b', 64);
         string key = "{\"kdf\":\"PBKDF2-SHA256\",\"iterations\":600000,\"salt\":\"AAAAAAAAAAAAAAAAAAAAAA==\","
                      + "\"iv\":\"AAAAAAAAAAAAAAAA\",\"data\":\"AQIDBA==\"}";

         Response mine = Rest("PUT", "/api/v1/me/smime/own", User(),
            "{\"address\":\"" + _address + "\",\"name\":\"Me, Myself\",\"fingerprint\":\"" + own + "\","
            + "\"certificate\":\"" + certificate + "\",\"chain\":[\"" + certificate + "\"],\"key\":" + key
            + ",\"not_after\":2000000000}");
         Assert.AreEqual(201, mine.Status, "Body: " + mine.Body);

         Response theirs = Rest("PUT", "/api/v1/me/smime/recipients", User(),
            "{\"address\":\"bob@example.test\",\"name\":\"Bob\",\"fingerprint\":\"" + bob + "\","
            + "\"certificate\":\"" + certificate + "\",\"not_after\":1900000000}");
         Assert.AreEqual(201, theirs.Status, "Body: " + theirs.Body);

         BackupDeleteAndRestore();

         Response listed = Rest("GET", "/api/v1/me/smime", User());
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("\"fingerprint\":\"" + own + "\"", listed.Body);
         StringAssert.Contains("\"fingerprint\":\"" + bob + "\"", listed.Body);
         StringAssert.Contains("\"chain\":[\"" + certificate + "\"]", listed.Body);

         // The wrapped key above all. This server cannot open it, so if a restore
         // dropped it nothing here would fail - the account would simply never be
         // able to read its own encrypted mail again.
         StringAssert.Contains("\"key\":" + key, listed.Body);
         StringAssert.Contains("\"not_after\":2000000000", listed.Body);
      }

      // ---------------------------------------------- hm_calendars/-objects

      [Test]
      [Description("hm_calendars and hm_calendarobjects: the account's calendar and the event a CalDAV client "
                   + "stored survive a backup and a restore, byte for byte and under the same resource name")]
      public void TheCalendarComesBack()
      {
         string ics = Event("backup-0001", "20261101T120000Z", "20261101T130000Z", "A meeting");
         Response stored = Dav("PUT", Calendar + "meeting.ics", ics, "text/calendar; charset=utf-8");
         Assert.AreEqual(201, stored.Status, "Body: " + stored.Body);

         BackupDeleteAndRestore();

         Response fetched = Dav("GET", Calendar + "meeting.ics", null, null);
         Assert.AreEqual(200, fetched.Status, "Body: " + fetched.Body);
         Assert.AreEqual(ics, fetched.Body, "The stored calendar object did not come back byte for byte.");

         // And the collection it hangs under, which is a row of its own: the object
         // is a child of the calendar, so a calendar that came back under a new
         // identity with its objects still pointing at the old one would list empty.
         Response report = Dav("PROPFIND", Calendar,
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/></D:prop></D:propfind>",
            "application/xml; charset=utf-8", "Depth: 1\r\n");
         Assert.AreEqual(207, report.Status, "Body: " + report.Body);
         StringAssert.Contains("meeting.ics", report.Body);
      }

      // --------------------------------------------------- hm_passwordhistory

      [Test]
      [Description("hm_passwordhistory: the passwords an account has been made to change away from survive a "
                   + "backup and a restore, so a restore does not silently let every account go back to one")]
      public void ThePasswordHistoryComesBack()
      {
         IniFileSetting.Write("PasswordPolicyHistoryCount", "3");
         _application.Reinitialize();

         string second = "Second-Passw0rd!";
         Response changed = Rest("POST", "/api/v1/me/password", User(),
            "{\"current\":\"" + UserPassword + "\",\"new\":\"" + second + "\"}");
         Assert.AreEqual(200, changed.Status, "Body: " + changed.Body);

         // The old password is now history, and going back to it is refused.
         Response back = Rest("POST", "/api/v1/me/password", BasicAuth(_address, second),
            "{\"current\":\"" + second + "\",\"new\":\"" + UserPassword + "\"}");
         Assert.AreEqual(409, back.Status, "The old password should be refused before the backup. Body: " + back.Body);

         BackupDeleteAndRestore();

         // And is still refused after it. On a build whose archive has no
         // hm_passwordhistory this answers 200: the history was deleted with the
         // domain and never came back, and nothing anywhere says so.
         Response again = Rest("POST", "/api/v1/me/password", BasicAuth(_address, second),
            "{\"current\":\"" + second + "\",\"new\":\"" + UserPassword + "\"}");
         Assert.AreEqual(409, again.Status,
            "The restored account accepted a password it had been made to change away from, so its password "
            + "history did not come back. Body: " + again.Body);

      }

      // ------------------------------------------------------ hm_knownsenders

      [Test]
      [Description("hm_knownsenders: the memory the first-contact note is decided against survives a backup and a "
                   + "restore, so a sender the account knew is not announced as a first contact again - while a "
                   + "sender first seen after the backup still is")]
      public void TheFirstContactMemoryComesBack()
      {
         // Nothing reads this table through an API, so it is proved by what it is
         // for: whether a delivery carries the note.
         const string known = "known@outside-stores.test";
         const string late = "late@outside-stores.test";

         string domainName = _domain.Name;

         // Turned off again in TearDown, on whatever domain the restore left.
         _domain.FirstContactTip = true;
         _domain.Save();

         // The account's first message ever. Remembered, not announced: a mailbox
         // that remembers nobody has nothing for a sender to be unusual against.
         // That rule is why the two deliveries after the restore are in the order
         // they are.
         SmtpClientSimulator.StaticSend(known, _address, "Before the backup", "Body");
         StringAssert.DoesNotContain(FirstContactHeader,
            Pop3ClientSimulator.AssertGetFirstMessageText(_address, UserPassword),
            "The first message an account ever receives carries no note.");

         RunBackup();

         // A sender first seen AFTER the backup was taken, so absent from the archive.
         // Announced, because the account already remembers the first sender - which
         // is also what shows that the memory held that sender when the backup ran.
         SmtpClientSimulator.StaticSend(late, _address, "After the backup", "Body");
         StringAssert.Contains(FirstContactHeader,
            Pop3ClientSimulator.AssertGetFirstMessageText(_address, UserPassword),
            "A sender the account had never had mail from must be announced.");

         DeleteAndRestore();

         Assert.IsTrue(_application.Domains.ItemByName[domainName].FirstContactTip,
            "The domain came back with the first-contact note off, so no delivery below could show anything.");

         // THE ORDER OF THE NEXT TWO DELIVERIES IS WHAT MAKES THIS A TEST.
         //
         // The negative control first: the sender recorded after the backup is not in
         // the archive, so the restored memory does not know them, and because that
         // memory is not empty they are announced. On a build whose archive does not
         // carry hm_knownsenders the restored account remembers nobody, this is the
         // first message of a mailbox with no memory, it carries no note - and the
         // test fails here.
         SmtpClientSimulator.StaticSend(late, _address, "Late sender, after the restore", "Body");
         StringAssert.Contains(FirstContactHeader,
            Pop3ClientSimulator.AssertGetFirstMessageText(_address, UserPassword),
            "A sender first seen after the backup is not in the archive and must be announced again. It was not, "
            + "which means the restored account remembered nobody at all: hm_knownsenders did not come back.");

         // And the sender the account knew when the backup was taken is not a first
         // contact. On a build without the table in the archive this one is announced
         // too, because by now that account remembers the late sender and nobody else.
         // Asked the other way round, both would pass on that build: an empty memory
         // announces nobody, and one delivery later it knows exactly one sender.
         SmtpClientSimulator.StaticSend(known, _address, "Known sender, after the restore", "Body");
         StringAssert.DoesNotContain(FirstContactHeader,
            Pop3ClientSimulator.AssertGetFirstMessageText(_address, UserPassword),
            "The restored account announced a sender it had had mail from before the backup, so the memory of "
            + "who has written to it did not come back.");
      }

      // ------------------------------------------------------- hm_digeststate

      [Test]
      [Description("hm_digeststate: an account that asked for the quarantine digest to stop is still opted out after a "
                   + "backup and a restore that deleted the domain, and the domain comes back with its digest on")]
      public void TheDigestOptOutComesBack()
      {
         string domainName = _domain.Name;

         // A server setting in hm_settings, which this restore does not touch; put
         // back to empty in TearDown. dynamic because the test project's interop
         // predates the property.
         dynamic settings = _settings;
         settings.QuarantineDigestBaseUrl = "http://127.0.0.1:" + RestPort;

         dynamic domain = _domain;
         domain.QuarantineDigest = true;
         domain.QuarantineDigestHour = 5;
         _domain.Save();

         Response offered = Rest("GET", "/api/v1/me/quarantine-digest", User());
         Assert.AreEqual(200, offered.Status, "Body: " + offered.Body);
         StringAssert.Contains("\"enabled\":true", offered.Body,
            "Before anything is stopped the digest is on for the account, or the test below proves nothing. Body: " + offered.Body);

         Response stopped = Rest("PUT", "/api/v1/me/quarantine-digest", User(), "{\"enabled\":false}");
         Assert.AreEqual(200, stopped.Status, "Body: " + stopped.Body);
         StringAssert.Contains("\"enabled\":false", stopped.Body);

         BackupDeleteAndRestore();

         // The domain first, because the account's answer below is "off" whenever the
         // domain's digest is off, whatever the archive held for the account - and a
         // test that passed that way would be passing on the wrong row.
         dynamic restored = _application.Domains.ItemByName[domainName];
         Assert.IsTrue((bool) restored.QuarantineDigest,
            "The domain came back with the quarantine digest off, so Domain::XMLStore did not carry it.");
         Assert.AreEqual(5, (int) restored.QuarantineDigestHour,
            "The domain came back with a different digest hour, so Domain::XMLStore did not carry it.");

         Response read = Rest("GET", "/api/v1/me/quarantine-digest", User());
         Assert.AreEqual(200, read.Status, "Body: " + read.Body);
         StringAssert.Contains("\"available\":true", read.Body);

         // On a build whose archive has no hm_digeststate the restored account has no
         // state row, which reads as an account that never asked - and the next digest
         // would go to somebody who had said to stop.
         StringAssert.Contains("\"enabled\":false", read.Body,
            "The restored account is being sent the digest again, so its opt-out did not come back. Body: " + read.Body);
      }

      // -------------------------------------------------------- hm_senderlist

      [Test]
      [Description("hm_senderlist: an account's own safe and blocked senders, and its domain's allow and block list, survive "
                   + "a backup and a restore that deleted the domain - each under the account or domain the restore made")]
      public void TheSafeSendersComeBack()
      {
         string domainName = _domain.Name;

         Response safe = Rest("POST", "/api/v1/me/sender-list", User(),
            "{\"value\":\"Friend.Example\",\"reason\":\"my accountant\"}");
         Assert.AreEqual(201, safe.Status, "Body: " + safe.Body);

         Response blocked = Rest("POST", "/api/v1/me/sender-list", User(),
            "{\"value\":\"pest@example.net\",\"blocked\":true}");
         Assert.AreEqual(201, blocked.Status, "Body: " + blocked.Body);

         // The domain's list is edited by an administrator, over COM as the Control
         // Panel does.
         dynamic domainList = ((dynamic) _domain).SenderList;
         dynamic entry = domainList.Add();
         entry.Value = "supplier.example";
         entry.Blocked = false;
         entry.Reason = "invoices";
         entry.Save();

         BackupDeleteAndRestore();

         // The account's own list, read the way its owner reads it. On a build whose
         // archive does not carry hm_senderlist this is empty: the restore deleted the
         // account, PersistentAccount::DeleteObject took the entries with it, and the
         // archive had nothing to put back.
         Response listed = Rest("GET", "/api/v1/me/sender-list", User());
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("\"value\":\"friend.example\"", listed.Body,
            "The account's safe sender did not come back. Body: " + listed.Body);
         StringAssert.Contains("\"reason\":\"my accountant\"", listed.Body);
         StringAssert.Contains("\"value\":\"pest@example.net\",\"blocked\":true", listed.Body,
            "The account's blocked sender did not come back blocked. Body: " + listed.Body);
         StringAssert.Contains("\"scope\":\"account\"", listed.Body);

         // The domain's list, on the domain the restore made - which has a new id, so
         // an entry restored under the old one would be invisible here - read the way
         // the Deck reads it.
         Response domainListed = Rest("GET", "/api/v1/domains/" + domainName + "/sender-list",
            BasicAuth("Administrator", AdminPassword));
         Assert.AreEqual(200, domainListed.Status, "Body: " + domainListed.Body);
         StringAssert.Contains("\"scope\":\"domain\",\"value\":\"supplier.example\",\"blocked\":false,\"reason\":\"invoices\"",
            domainListed.Body, "The domain's allow and block list did not come back. Body: " + domainListed.Body);
      }

      // --------------------------------------------------- hm_roleassignments

      [Test]
      [Description("hm_roleassignments: the role an account was given, and the domains it reaches, survive a backup "
                   + "and a restore - so a restore neither demotes a help desk nor promotes an account whose "
                   + "AdminLevel is wider than the role it was narrowed to")]
      public void TheRoleAssignmentComesBack()
      {
         // The account's AdminLevel says server administrator, which is what it
         // resolves to without an assignment - and the archive carries AdminLevel.
         // Narrowed to the help desk, it is the case where losing the assignment in
         // a restore is not a demotion but a promotion to the whole server.
         hMailServer.Account account = _domain.Accounts.get_ItemByAddress(_address);
         account.AdminLevel = hMailServer.eAdminLevel.hAdminLevelServerAdmin;
         account.Save();

         string domain = _domain.Name.ToLowerInvariant();
         Response assigned = Rest("PUT", "/api/v1/role-assignments/" + _address,
            BasicAuth("Administrator", AdminPassword), "{\"role\":\"helpdesk\",\"domains\":\"" + domain + "\"}");
         Assert.AreEqual(200, assigned.Status, "Body: " + assigned.Body);

         Response before = Rest("GET", "/api/v1/session", User());
         Assert.AreEqual(200, before.Status, "Body: " + before.Body);
         StringAssert.Contains("\"role\":\"helpdesk\"", before.Body, "The assignment did not take before the backup.");

         BackupDeleteAndRestore();

         // Through the credential itself: what the restored account may do now is
         // what the server resolves for it, which is the whole point.
         Response after = Rest("GET", "/api/v1/session", User());
         Assert.AreEqual(200, after.Status, "Body: " + after.Body);
         StringAssert.Contains("\"role\":\"helpdesk\"", after.Body,
            "The restored account does not hold the help desk role. On a build whose archive has no "
            + "hm_roleassignments it answers serveradmin: its AdminLevel came back and its assignment did not. Body: " + after.Body);
         StringAssert.DoesNotContain("administer_server", after.Body,
            "The restored account holds the server administrator's authority it had been narrowed away from. Body: " + after.Body);

         // And the row itself, with the domains it was given.
         Response listed = Rest("GET", "/api/v1/role-assignments", BasicAuth("Administrator", AdminPassword));
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.Contains("{\"address\":\"" + _address + "\",\"role\":\"helpdesk\",\"domains\":\"" + domain + "\"",
            listed.Body, "The restored assignment is not listed with the role and the domains it was given.");
      }

      [Test]
      [Description("hm_roles: a custom role, and the account given it, survive a backup and a restore onto a server "
                   + "that no longer has the role - the role comes back first, with its permissions and its "
                   + "description, so the assignment that names it is restored rather than dropped")]
      public void ACustomRoleComesBackBeforeTheAssignmentThatNamesIt()
      {
         const string role = "storestest-mailops";
         // A line break, so that the description takes the base64 path across the
         // archive and has to come back byte for byte.
         const string description = "Runs the delivery queue\r\nat night";

         try
         {
            Response created = Rest("POST", "/api/v1/roles", BasicAuth("Administrator", AdminPassword),
               "{\"name\":\"" + role + "\",\"description\":\"Runs the delivery queue\\r\\nat night\",\"permissions\":\"view_queue,manage_queue\"}");
            Assert.AreEqual(201, created.Status, "Body: " + created.Body);

            Response assigned = Rest("PUT", "/api/v1/role-assignments/" + _address,
               BasicAuth("Administrator", AdminPassword), "{\"role\":\"" + role + "\",\"domains\":\"*\"}");
            Assert.AreEqual(200, assigned.Status, "Body: " + assigned.Body);

            RunBackup();

            // A fresh server, as far as the role is concerned: it is gone, and its
            // assignment with it, before the restore begins.
            Response deleted = Rest("DELETE", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword));
            Assert.AreEqual(200, deleted.Status, "Body: " + deleted.Body);
            Assert.AreEqual(404, Rest("GET", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword)).Status,
               "The role is still on the server, so the restore would not be proving anything.");

            DeleteAndRestore();

            Response restored = Rest("GET", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword));
            Assert.AreEqual(200, restored.Status,
               "The custom role did not come back: the archive carries no <Roles>, or the restore did not read it. Body: " + restored.Body);

            using (JsonDocument document = JsonDocument.Parse(restored.Body))
            {
               Assert.AreEqual("view_queue,manage_queue", document.RootElement.GetProperty("permissions").GetString());
               Assert.AreEqual(description, document.RootElement.GetProperty("description").GetString(),
                  "The description did not come back byte for byte.");
               Assert.IsFalse(document.RootElement.GetProperty("builtin").GetBoolean());
            }

            Response session = Rest("GET", "/api/v1/session", User());
            Assert.AreEqual(200, session.Status, "Body: " + session.Body);
            StringAssert.Contains("\"role\":\"" + role + "\"", session.Body,
               "The restored account does not hold the custom role, so its assignment was dropped - which is what a "
               + "restore did when the role had to be on the server already. Body: " + session.Body);

            Response listed = Rest("GET", "/api/v1/role-assignments", BasicAuth("Administrator", AdminPassword));
            Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
            StringAssert.Contains("{\"address\":\"" + _address + "\",\"role\":\"" + role + "\",\"domains\":\"*\"", listed.Body);

            string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            Assert.IsTrue(Regex.IsMatch(log, @"Restored [1-9][0-9]* custom role\(s\)\."), "The restore did not say it restored the custom roles. Log: " + log);
         }
         finally
         {
            Rest("DELETE", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword));
         }
      }

      [Test]
      [Description("hm_roleassignments: an assignment naming a role that neither the archive nor the server has is "
                   + "dropped, and the account's admin level goes with it - an account whose level says server "
                   + "administrator but which had been narrowed to a help desk comes back an ordinary account, "
                   + "and the backup log names the account and the missing role")]
      public void AnAssignmentNobodyCanPlaceTakesTheWiderAdminLevelWithIt()
      {
         hMailServer.Account account = _domain.Accounts.get_ItemByAddress(_address);
         account.AdminLevel = hMailServer.eAdminLevel.hAdminLevelServerAdmin;
         account.Save();

         Response assigned = Rest("PUT", "/api/v1/role-assignments/" + _address,
            BasicAuth("Administrator", AdminPassword), "{\"role\":\"helpdesk\",\"domains\":\"\"}");
         Assert.AreEqual(200, assigned.Status, "Body: " + assigned.Body);

         RunBackup();

         // The archive, edited the way a person would: the assignment now names a
         // role no server has.
         RenameTheArchivedRole("helpdesk", "no-such-role");

         DeleteAndRestore();

         Response session = Rest("GET", "/api/v1/session", User());
         Assert.AreEqual(200, session.Status, "Body: " + session.Body);
         StringAssert.Contains("\"administrative\":false", session.Body,
            "The account came back with administrative authority. With its assignment dropped and its admin level "
            + "left at server administrator, it holds the whole server the help desk role had narrowed it away from. "
            + "Body: " + session.Body);
         StringAssert.DoesNotContain("administer_server", session.Body);

         string domainName = _address.Substring(_address.IndexOf('@') + 1);
         hMailServer.Account restored = _application.Domains.get_ItemByName(domainName).Accounts.get_ItemByAddress(_address);
         Assert.AreEqual(hMailServer.eAdminLevel.hAdminLevelNormal, restored.AdminLevel,
            "The account's admin level was not lowered in place of the assignment that could not be restored.");

         Response listed = Rest("GET", "/api/v1/role-assignments", BasicAuth("Administrator", AdminPassword));
         Assert.AreEqual(200, listed.Status, "Body: " + listed.Body);
         StringAssert.DoesNotContain(_address, listed.Body, "An assignment naming a role nobody has was written.");

         string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
         StringAssert.Contains("The backup gives " + _address + " the role no-such-role", log, "Log: " + log);
         StringAssert.Contains("its admin level is now that of an ordinary account", log, "Log: " + log);
         StringAssert.Contains("(server administrator)", log, "The level that was not given back is named. Log: " + log);

         // And in the error log, where an administrator watching a restore that
         // ends "completed successfully" is told that an account lost its
         // authority; consumed here, since the next fixture asserts the log empty.
         CustomAsserts.AssertReportedError("HM6129", _address, "no-such-role");
      }

      [Test]
      [Description("hm_roleassignments: a restore that fails after the account's row is saved leaves the account no wider "
                   + "than its role - the admin level the archive carries is withheld until the assignment is back, so an "
                   + "account narrowed from server administrator to help desk does not come back the server administrator "
                   + "when the restore stops between the two")]
      public void AFailedRestoreLeavesTheAccountNoWiderThanItsRole()
      {
         hMailServer.Account account = _domain.Accounts.get_ItemByAddress(_address);
         account.AdminLevel = hMailServer.eAdminLevel.hAdminLevelServerAdmin;
         account.Save();

         Response assigned = Rest("PUT", "/api/v1/role-assignments/" + _address,
            BasicAuth("Administrator", AdminPassword), "{\"role\":\"helpdesk\",\"domains\":\"\"}");
         Assert.AreEqual(200, assigned.Status, "Body: " + assigned.Body);

         RunBackup();

         // The archive, edited so that the restore fails exactly where it matters:
         // after the account's row is saved and before its assignment is written.
         // A second copy of the assignment row is refused by the unique index on
         // the account, and the restore stops there - with the account's row in
         // place and, until 15 September 2026, its archived admin level in it.
         DuplicateTheArchivedAssignment();

         string log = DeleteAndRestoreExpectingFailure();
         StringAssert.Contains("Restore of domains failed", log, "Log: " + log);

         string domainName = _address.Substring(_address.IndexOf('@') + 1);
         hMailServer.Account restored = _application.Domains.get_ItemByName(domainName).Accounts.get_ItemByAddress(_address);
         Assert.AreEqual(hMailServer.eAdminLevel.hAdminLevelNormal, restored.AdminLevel,
            "The account's row was saved with the archive's admin level before the restore failed, so it is the server "
            + "administrator with nothing to say otherwise. The level must be withheld until the assignment is back.");

         Response session = Rest("GET", "/api/v1/session", User());
         Assert.AreEqual(200, session.Status, "Body: " + session.Body);
         StringAssert.DoesNotContain("administer_server", session.Body,
            "The account holds the server administrator's authority after a failed restore. Body: " + session.Body);

         // The refused row's report and the failed restore's, both consumed here.
         CustomAsserts.AssertReportedError("HM6128", "HM5014");
      }

      [Test]
      [Description("hm_roles: a custom role the restoring server already has is kept as it is here rather than overwritten "
                   + "with the archive's wider definition - every credential holding it keeps what this server gives it, "
                   + "the backup log names the difference, and the assignment that names the role is restored")]
      public void AServerRoleOfTheSameNameIsKeptAndNotWidened()
      {
         const string role = "storestest-narrowed";

         try
         {
            Response created = Rest("POST", "/api/v1/roles", BasicAuth("Administrator", AdminPassword),
               "{\"name\":\"" + role + "\",\"description\":\"Runs the queue\",\"permissions\":\"view_queue,manage_queue\"}");
            Assert.AreEqual(201, created.Status, "Body: " + created.Body);

            Response assigned = Rest("PUT", "/api/v1/role-assignments/" + _address,
               BasicAuth("Administrator", AdminPassword), "{\"role\":\"" + role + "\",\"domains\":\"*\"}");
            Assert.AreEqual(200, assigned.Status, "Body: " + assigned.Body);

            RunBackup();

            // Narrowed here after the backup was taken: what the archive carries is
            // wider than what this server's administrator decided.
            Response narrowed = Rest("PUT", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword),
               "{\"description\":\"Reads the queue\",\"permissions\":\"view_queue\"}");
            Assert.AreEqual(200, narrowed.Status, "Body: " + narrowed.Body);

            DeleteAndRestore();

            Response kept = Rest("GET", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword));
            Assert.AreEqual(200, kept.Status, "Body: " + kept.Body);

            using (JsonDocument document = JsonDocument.Parse(kept.Body))
            {
               Assert.AreEqual("view_queue", document.RootElement.GetProperty("permissions").GetString(),
                  "The restore overwrote this server's role with the archive's wider definition. Body: " + kept.Body);
               Assert.AreEqual("Reads the queue", document.RootElement.GetProperty("description").GetString());
            }

            Response session = Rest("GET", "/api/v1/session", User());
            Assert.AreEqual(200, session.Status, "Body: " + session.Body);
            StringAssert.Contains("\"role\":\"" + role + "\"", session.Body, "The assignment naming the kept role was not restored. Body: " + session.Body);
            StringAssert.DoesNotContain("manage_queue", session.Body, "The account holds a permission the archive's role had and this server's does not. Body: " + session.Body);

            string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            StringAssert.Contains("The backup carries the custom role " + role + ", which this server already has. This server's definition has been kept", log, "Log: " + log);
            StringAssert.Contains("permissions 'view_queue' here and 'view_queue,manage_queue' in the backup", log, "Log: " + log);
            StringAssert.Contains("description 'Reads the queue' here and 'Runs the queue' in the backup", log, "Log: " + log);
            Assert.IsTrue(Regex.IsMatch(log, @"Restored 0 custom role\(s\)\. 1 role\(s\) the backup carries were already on this server"), "Log: " + log);
         }
         finally
         {
            Rest("DELETE", "/api/v1/roles/" + role, BasicAuth("Administrator", AdminPassword));
         }
      }

      // --------------------------------------------------- a message's labels

      [Test]
      [Description("messagekeywords: a message comes back from a restore with its labels and its second star, "
                   + "which Message::XMLStore never wrote")]
      public void AMessageComesBackWithItsLabelsAndItsSecondStar()
      {
         SmtpClientSimulator.StaticSend(_address, _address, "Labelled", "This one is filed.");
         Pop3ClientSimulator.AssertMessageCount(_address, UserPassword, 1);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(_address, UserPassword));
         Assert.IsTrue(imap.SelectFolder("INBOX"));

         // A label of the reader's own, the second star and a follow-up: exactly
         // the three the webmail keeps here, none of them a system flag. The stars
         // above the first are the keywords $Star2 and $Star3 on top of \Flagged,
         // and a follow-up is $FollowUp beside them.
         Assert.IsTrue(imap.SetFlagOnMessage(1, true, "Invoices $Star2 $FollowUp"));
         Assert.IsTrue(imap.SetFlagOnMessage(1, true, "\\Seen"));

         string before = imap.GetFlags(1);
         StringAssert.Contains("Invoices", before);
         imap.Disconnect();

         BackupDeleteAndRestore();

         var after = new ImapClientSimulator();
         Assert.IsTrue(after.ConnectAndLogon(_address, UserPassword));
         Assert.IsTrue(after.SelectFolder("INBOX"));

         string flags = after.GetFlags(1);
         after.Disconnect();

         StringAssert.Contains("Invoices", flags,
            "The restored message lost its label, so messagekeywords is not in the archive. FETCH said: " + flags);
         StringAssert.Contains("$Star2", flags,
            "The restored message lost its second star. FETCH said: " + flags);
         StringAssert.Contains("$FollowUp", flags,
            "The restored message lost its follow-up flag. FETCH said: " + flags);
         StringAssert.Contains("\\Seen", flags, "The system flags must still be there too. FETCH said: " + flags);
      }

      // ------------------------------------------------- the backup's own word

      [Test]
      [Description("The verified restore reconciles the per-account rows it wrote against the archive it reads "
                   + "back, so a store that went missing would fail the backup rather than pass quietly")]
      public void TheVerifiedRestoreCountsThePerAccountRows()
      {
         Response added = Rest("POST", "/api/v1/me/contacts", User(),
            "{\"name\":\"Ada Lovelace\",\"address\":\"ada@example.com\"}");
         Assert.AreEqual(201, added.Status, "Body: " + added.Body);

         RunBackup();

         string log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
         StringAssert.Contains("per-account store", log,
            "The verified restore said nothing about the per-account stores, so it is not checking them. Log: " + log);
      }

      // ------------------------------------------------------------- the cycle

      private void BackupDeleteAndRestore()
      {
         RunBackup();
         DeleteAndRestore();
      }

      private void DeleteAndRestore()
      {
         // The disaster, exactly as a restore assumes it: everything gone. Every one
         // of these tables cascades from hm_accounts, so this is also what deletes
         // them - which is why a restore from an archive that does not carry them
         // destroys them.
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         string startTime = _application.Status.StartTime;

         FileInfo[] archives = new DirectoryInfo(_backupDirectory).GetFiles("*.7z");
         Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + _backupDirectory);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = true;
         backup.RestoreSettings = false;
         backup.StartRestore();

         WaitForRestore(startTime);
      }

      /// <summary>
      ///    The same disaster and the same restore, from an archive edited so that
      ///    the restore cannot finish. A failed restore reinitialises nothing, so it is
      ///    awaited through the backup log rather than through a restart, and the log
      ///    is returned for the test to read what stopped it.
      /// </summary>
      private string DeleteAndRestoreExpectingFailure()
      {
         while (_application.Domains.Count > 0)
            _application.Domains[0].Delete();

         FileInfo[] archives = new DirectoryInfo(_backupDirectory).GetFiles("*.7z");
         Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + _backupDirectory);

         CustomAsserts.AssertDeleteFile(_application.Settings.Backup.LogFile);

         var backup = _application.BackupManager.LoadBackup(archives[0].FullName);
         backup.RestoreDomains = true;
         backup.RestoreMessages = true;
         backup.RestoreSettings = false;
         backup.StartRestore();

         for (int attempt = 0; attempt < 480; attempt++)
         {
            string log = "";

            try
            {
               log = TestSetup.ReadExistingTextFile(_application.Settings.Backup.LogFile);
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            if (log.Contains("BACKUP ERROR:"))
               return log;

            if (log.Contains("Restore completed successfully"))
               Assert.Fail("The restore was expected to fail and completed instead, so the edited archive did not do what it was meant to. Log: " + log);

            Thread.Sleep(250);
         }

         Assert.Fail("The restore neither failed nor completed within two minutes.");
         return "";
      }

      /// <summary>
      ///    Edits the one archive in the backup directory: the role an archived
      ///    assignment names is renamed, in the index, and the index is put back into
      ///    the archive in place of the one the backup wrote. What a person would do
      ///    with 7-Zip and a text editor, and the only way to have an archive name a
      ///    role that is in neither the archive nor the server.
      /// </summary>
      private void RenameTheArchivedRole(string from, string to)
      {
         EditTheArchivedIndex(xml =>
         {
            var reference = new Regex("(Column=[\"']raroleid[\"']\\s+Name=[\"'])" + Regex.Escape(from) + "([\"'])");
            Assert.AreEqual(1, reference.Matches(xml).Count,
               "Expected exactly one archived assignment naming " + from + " in the index.");

            return reference.Replace(xml, "${1}" + to + "${2}");
         });
      }

      /// <summary>
      ///    Edits the archive so that the account's assignment appears twice. The
      ///    second row is refused by the unique index on the account, which is the one
      ///    way to make a restore fail exactly between the account's row and its
      ///    assignment, on every database this server runs on.
      /// </summary>
      private void DuplicateTheArchivedAssignment()
      {
         EditTheArchivedIndex(xml =>
         {
            var store = new Regex("(<Store Name=[\"']hm_roleassignments[\"']>)(\\s*<Row\\b.*?</Row>)(\\s*</Store>)", RegexOptions.Singleline);
            Assert.AreEqual(1, store.Matches(xml).Count, "Expected exactly one archived assignment store in the index.");

            return store.Replace(xml, "${1}${2}${2}${3}");
         });
      }

      private void EditTheArchivedIndex(Func<string, string> edit)
      {
         FileInfo[] archives = new DirectoryInfo(_backupDirectory).GetFiles("*.7z");
         Assert.AreEqual(1, archives.Length, "Expected exactly one archive in " + _backupDirectory);

         string staging = Paths.Combine(_backupDirectory, "edited");
         Directory.CreateDirectory(staging);

         Run7Za("e \"" + archives[0].FullName + "\" hMailServerBackup.xml -o\"" + staging + "\" -y", staging);

         string index = Paths.Combine(staging, "hMailServerBackup.xml");
         Assert.IsTrue(File.Exists(index), "7za.exe did not extract the backup's index.");

         // settings-store-exempt: reads and rewrites the backup archive's index, not hMailServer.ini.
         string xml = edit(File.ReadAllText(index));

         // UTF-16 with a byte-order mark, which is what the server writes the index in
         // and what its reader detects.
         // settings-store-exempt: writes the backup archive's index, not hMailServer.ini.
         File.WriteAllText(index, xml, Encoding.Unicode);
         File.SetLastWriteTimeUtc(index, DateTime.UtcNow.AddMinutes(1));

         // A bare file name from the staging directory, so that the entry replaces
         // the archive's own "hMailServerBackup.xml" rather than being added beside
         // it under a path.
         Run7Za("u \"" + archives[0].FullName + "\" hMailServerBackup.xml", staging);

         Directory.Delete(staging, true);
      }

      private void Run7Za(string arguments, string workingDirectory)
      {
         string programDirectory = _application.Settings.Directories.ProgramDirectory;

         // A registered install resolves to {InstallLocation}\Bin, while a developer
         // build that is not registered runs from the directory it was built into -
         // the same two candidates BackupRestoreSafety looks in.
         string[] candidates =
         {
            Paths.Combine(programDirectory, "7za.exe"),
            Paths.Combine(programDirectory, "Bin", "7za.exe"),
         };

         string sevenZip = candidates.FirstOrDefault(File.Exists);
         if (sevenZip == null)
            Assert.Fail("Could not find 7za.exe next to the server. Looked in " + string.Join(" and ", candidates) + ".");

         var startInfo = new ProcessStartInfo(sevenZip, arguments)
         {
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
         };

         using (var process = Process.Start(startInfo))
         {
            Assert.IsNotNull(process, "7za.exe could not be started.");

            string output = process.StandardOutput.ReadToEnd();
            process.WaitForExit();

            Assert.AreEqual(0, process.ExitCode, "7za.exe " + arguments + " failed: " + output);
         }
      }

      /// <summary>
      ///    The first-contact note is a domain setting, and once a restore has run the
      ///    domain is no longer the object _domain holds - so it is turned off on
      ///    whatever domains there are rather than through _domain. The next SetUp
      ///    replaces the domain anyway; this is so that nothing a test here turned on
      ///    outlives it, whether or not it reached its last line.
      /// </summary>
      private void TurnTheFirstContactNoteOff()
      {
         try
         {
            var domains = _application.Domains;

            for (int index = 0; index < domains.Count; index++)
            {
               var domain = domains[index];

               if (domain.FirstContactTip)
               {
                  domain.FirstContactTip = false;
                  domain.Save();
               }
            }
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            // A restore that failed half way can leave domains nothing can save.
            // Failing TearDown over that would replace the failure that matters.
            Console.WriteLine("Could not turn the first-contact note off: " + tidying.Message);
         }
      }

      /// <summary>
      ///    The digest's address is a server setting a restore with RestoreSettings off
      ///    leaves alone, so it is put back here rather than left to the next SetUp,
      ///    which replaces the domain and nothing else.
      /// </summary>
      private void TurnTheDigestAddressOff()
      {
         try
         {
            dynamic settings = _settings;
            settings.QuarantineDigestBaseUrl = "";
         }
         catch (Exception tidying) when (!ExceptionPolicy.IsFatal(tidying))
         {
            Console.WriteLine("Could not clear the quarantine digest address: " + tidying.Message);
         }
      }

      private void RunBackup()
      {
         var settings = _application.Settings.Backup;
         settings.BackupDomains = true;
         settings.BackupMessages = true;
         settings.BackupSettings = false;
         settings.CompressDestinationFiles = true;
         settings.Destination = _backupDirectory;

         CustomAsserts.AssertDeleteFile(settings.LogFile);

         _application.BackupManager.StartBackup();

         for (int attempt = 0; attempt < 120; attempt++)
         {
            try
            {
               string log = TestSetup.ReadExistingTextFile(settings.LogFile);

               if (log.Contains("BACKUP ERROR:"))
                  Assert.Fail("The backup failed. Log: " + log);

               if (log.Contains("Backup completed successfully"))
                  return;
            }
            catch (Exception sharing) when (!ExceptionPolicy.IsFatal(sharing))
            {
               // The server is writing the file; read it again in a moment.
            }

            Thread.Sleep(250);
         }

         Assert.Fail("The backup did not finish within 30 seconds.");
      }

      private void WaitForRestore(string lastStartTime)
      {
         for (int attempt = 0; attempt < 600; attempt++)
         {
            try
            {
               string startTime = _application.Status.StartTime;

               if (startTime.Length > 0 && startTime != lastStartTime)
               {
                  // The listeners come back with the reinitialisation; give them the
                  // moment they need before the next request, which would otherwise
                  // spend its retries on a port that is between binds.
                  Thread.Sleep(500);
                  return;
               }
            }
            catch (Exception talking) when (!ExceptionPolicy.IsFatal(talking))
            {
               // The COM server is reinitialising.
            }

            Thread.Sleep(100);
         }

         Assert.Fail("The restore did not finish within a minute.");
      }

      // ------------------------------------------------------------- the wires

      private string AddressBook
      {
         get { return "/dav/addressbooks/" + _address + "/contacts/"; }
      }

      private string Calendar
      {
         get { return "/dav/calendars/" + _address + "/calendar/"; }
      }

      private string User()
      {
         return BasicAuth(_address, UserPassword);
      }

      private static string BasicAuth(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private Response Rest(string method, string path, string authorization, string body = null,
                            string contentType = "application/json")
      {
         return Raw(RestPort, method, path, authorization, body, contentType, null);
      }

      /// <summary>
      ///    A request to the DAV listener, as a TLS-terminating proxy would forward
      ///    it: CalDAV and CardDAV refuse Basic over plain HTTP, and the listener is
      ///    plain HTTP on this bench.
      /// </summary>
      private Response Dav(string method, string path, string body, string contentType,
                           string extraHeaders = null)
      {
         return Raw(WebServicesPort, method, path, BasicAuth(_address, UserPassword), body,
            contentType ?? "application/xml; charset=utf-8",
            "X-Forwarded-Proto: https\r\n" + (extraHeaders ?? ""));
      }

      private static string Card(string formattedName, string structuredName, string email)
      {
         return string.Join("\r\n", new[]
         {
            "BEGIN:VCARD",
            "VERSION:3.0",
            "PRODID:-//A phone//EN",
            "UID:backup-card-0001",
            "FN:" + formattedName,
            "N:" + structuredName,
            "EMAIL;TYPE=INTERNET:" + email,
            "END:VCARD",
            ""
         });
      }

      private static string Event(string uid, string start, string end, string summary)
      {
         return string.Join("\r\n", new[]
         {
            "BEGIN:VCALENDAR",
            "VERSION:2.0",
            "PRODID:-//A phone//EN",
            "BEGIN:VEVENT",
            "UID:" + uid,
            "DTSTAMP:20260101T000000Z",
            "DTSTART:" + start,
            "DTEND:" + end,
            "SUMMARY:" + summary,
            "SEQUENCE:0",
            "END:VEVENT",
            "END:VCALENDAR",
            ""
         });
      }

      private static string Between(string text, string after, string before)
      {
         int start = text.IndexOf(after, StringComparison.Ordinal);
         Assert.Greater(start, -1, "Expected " + after + " in: " + text);
         start += after.Length;

         int end = text.IndexOf(before, start, StringComparison.Ordinal);
         Assert.Greater(end, -1, "Expected " + before + " after " + after + " in: " + text);

         return text.Substring(start, end - start);
      }

      private class Response
      {
         public int Status;
         public string Body = "";
         public string HeaderText = "";
         public readonly System.Collections.Generic.Dictionary<string, string> Headers =
            new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : null;
         }
      }

      private static Response Raw(int port, string method, string path, string authorization, string body,
                                  string contentType, string extraHeaders)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 40; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", port);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(250);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var head = new StringBuilder();
               head.Append(method + " " + path + " HTTP/1.0\r\n");
               head.Append("Host: 127.0.0.1:" + port + "\r\n");
               if (authorization != null)
                  head.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  head.Append(extraHeaders);

               byte[] bodyBytes = body == null ? new byte[0] : Encoding.UTF8.GetBytes(body);
               if (body != null)
               {
                  head.Append("Content-Type: " + contentType + "\r\n");
                  head.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               head.Append("Connection: close\r\n\r\n");

               byte[] headBytes = Encoding.ASCII.GetBytes(head.ToString());
               stream.Write(headBytes, 0, headBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               string raw = Encoding.UTF8.GetString(memory.ToArray());
               var response = new Response();

               int separator = raw.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               response.HeaderText = separator >= 0 ? raw.Substring(0, separator) : raw;
               response.Body = separator >= 0 ? raw.Substring(separator + 4) : "";

               string[] lines = response.HeaderText.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               for (int index = 1; index < lines.Length; index++)
               {
                  int colon = lines[index].IndexOf(':');
                  if (colon > 0)
                     response.Headers[lines[index].Substring(0, colon).Trim()] =
                        lines[index].Substring(colon + 1).Trim();
               }

               return response;
            }
         }
      }
   }
}
